// Clip Atlas — the update check's timers, the saved check time, and what happens to each answer. update-checker.hpp
// says what the checker does; this file says how.
//
// An answer comes back on the HTTP thread. That thread only copies it and hops to the UI thread with run_on_ui(). The
// hop locks a std::weak_ptr to the shared state, so the last reference is always released on the UI thread. A check
// whose answer arrives after stop() finds the weak_ptr expired, or `stopped` set, and does nothing.
#include "update-checker.hpp"

#include "http.hpp"
#include "ui-thread.hpp"

#include <obs-data.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QObject>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace {

constexpr const char *kCheckFile = "update-check.json";
constexpr const char *kLastCheckKey = "last_check";
constexpr const char *kTempExt = "tmp";
constexpr const char *kBackupExt = "bak";

// The first automatic check waits this long, so the request never slows OBS's start-up.
constexpr int kFirstCheckDelayMs = 60 * 1000;
// Automatic checks are considered once an hour. update_check_due() decides whether one runs, so a failed check is
// retried at the next tick without any other timer.
constexpr int kTickIntervalMs = 60 * 60 * 1000;

using BmemString = std::unique_ptr<char, void (*)(void *)>;
using DataPtr = std::unique_ptr<obs_data_t, void (*)(obs_data_t *)>;
using ArrayPtr = std::unique_ptr<obs_data_array_t, void (*)(obs_data_array_t *)>;

// The wall clock in Unix seconds. The saved check time and update_check_due() use the same clock.
std::int64_t current_time()
{
	return static_cast<std::int64_t>(std::time(nullptr));
}

// The string under `name`, or "" when there is none. A release may lack a field, or hold another type there. libobs
// answers "" for both; the NULL check is for a build that answers NULL instead.
std::string string_value(obs_data_t *data, const char *name)
{
	const char *value = obs_data_get_string(data, name);
	return value ? value : "";
}

// A result for a failure found on this side of the request, when there is no answer to judge.
UpdateResult failed_result(std::string error)
{
	UpdateResult result;
	result.outcome = UpdateOutcome::failed;
	result.error = std::move(error);
	return result;
}

// The release in an answer's body: nullopt when the body is not JSON or has no tag. evaluate_update() turns that into a
// failure, so an unreadable answer is never mistaken for "up to date".
std::optional<ReleaseInfo> parse_release(const std::string &body)
{
	DataPtr data(obs_data_create_from_json(body.c_str()), obs_data_release);
	if (!data)
		return std::nullopt;
	ReleaseInfo release;
	release.tag_name = string_value(data.get(), "tag_name");
	release.html_url = string_value(data.get(), "html_url");
	release.draft = obs_data_get_bool(data.get(), "draft");
	release.prerelease = obs_data_get_bool(data.get(), "prerelease");
	if (release.tag_name.empty())
		return std::nullopt;
	// NULL when there is no "assets" array. find_installer() then finds nothing, and the release page is offered.
	ArrayPtr assets(obs_data_get_array(data.get(), "assets"), obs_data_array_release);
	const std::size_t count = assets ? obs_data_array_count(assets.get()) : 0;
	for (std::size_t i = 0; i < count; ++i) {
		DataPtr item(obs_data_array_item(assets.get(), i), obs_data_release);
		if (!item)
			continue;
		ReleaseAsset asset;
		asset.name = string_value(item.get(), "name");
		asset.download_url = string_value(item.get(), "browser_download_url");
		asset.size = static_cast<std::int64_t>(obs_data_get_int(item.get(), "size"));
		asset.state = string_value(item.get(), "state");
		asset.digest = string_value(item.get(), "digest");
		release.assets.push_back(std::move(asset));
	}
	return release;
}

// The time of the last check that got an answer, from update-check.json. nullopt when the file is missing, cannot be
// read, or has no last_check. Each of those means "never checked", so an automatic check is due.
std::optional<std::int64_t> read_last_check()
{
	BmemString path(obs_module_config_path(kCheckFile), bfree);
	if (!path || !os_file_exists(path.get()))
		return std::nullopt;
	DataPtr data(obs_data_create_from_json_file(path.get()), obs_data_release);
	if (!data) {
		obs_log(LOG_WARNING, "%s could not be read; the update check is treated as never run", path.get());
		return std::nullopt;
	}
	// Written with obs_data_set_int(), which makes a user value. A file without one holds nothing to use.
	if (!obs_data_has_user_value(data.get(), kLastCheckKey))
		return std::nullopt;
	return static_cast<std::int64_t>(obs_data_get_int(data.get(), kLastCheckKey));
}

// Saves the time of the last check that got an answer. Returns false after logging why not. The time is also kept in
// memory, so even a failed save spaces the next automatic check out for the rest of this session.
bool write_last_check(std::int64_t when)
{
	BmemString folder(obs_module_config_path(""), bfree);
	BmemString path(obs_module_config_path(kCheckFile), bfree);
	if (!folder || !path) {
		obs_log(LOG_ERROR, "no config folder, update check time not saved");
		return false;
	}
	// The folder is created on first use, as settings.json's is.
	if (os_mkdirs(folder.get()) == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "could not create %s, update check time not saved", folder.get());
		return false;
	}
	DataPtr data(obs_data_create(), obs_data_release);
	obs_data_set_int(data.get(), kLastCheckKey, static_cast<long long>(when));
	if (!obs_data_save_json_safe(data.get(), path.get(), kTempExt, kBackupExt)) {
		obs_log(LOG_ERROR, "could not write %s, update check time not saved", path.get());
		return false;
	}
	return true;
}

// The one log line that ends each check: INFO for an answer, WARNING with the reason when there was none.
void log_outcome(const UpdateResult &result)
{
	switch (result.outcome) {
	case UpdateOutcome::up_to_date:
		obs_log(LOG_INFO, "update check: up to date (latest %s, this %s)", result.latest_version.c_str(),
			PLUGIN_VERSION);
		break;
	case UpdateOutcome::update_available:
		obs_log(LOG_INFO, "update check: update available (latest %s, this %s)", result.latest_version.c_str(),
			PLUGIN_VERSION);
		break;
	case UpdateOutcome::no_release:
		// There is no release, so there is no latest version to print.
		obs_log(LOG_INFO, "update check: no release published yet (this %s)", PLUGIN_VERSION);
		break;
	case UpdateOutcome::failed:
		obs_log(LOG_WARNING, "update check failed: %s", result.error.c_str());
		break;
	}
}

// The public methods run inside OBS and Qt calls, where an exception must not unwind into C code or the event loop. It
// is logged here and the next call carries on.
template<typename Fn> void run_guarded(const char *where, Fn &&fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "update check: %s failed: %s", where, e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "update check: %s failed: unknown exception", where);
	}
}

} // namespace

// What the checks and the timers share. UpdateChecker owns it through a shared_ptr. The timers and the HTTP answers
// hold weak references, and lock them only on the UI thread.
struct UpdateChecker::Impl : public std::enable_shared_from_this<UpdateChecker::Impl> {
	explicit Impl(SettingsStore &store);

	// The calls UpdateChecker forwards, with the same meaning as in the header.
	void start();
	void stop();
	void check_now(Callback done);
	void set_update_callback(Callback callback);

	// Connects the timers to this object. Called once, after the shared_ptr exists, because the slots capture a
	// weak reference to it.
	void connect_timers();
	// A timer fired: starts an automatic check when the setting is on, no check is running and the last one is due.
	void on_timer();
	// Sends the request for the latest release. `automatic` says whether a timer started it. Returns at once.
	void begin_check(bool automatic);
	// The answer to the running check. Called on the UI thread.
	void finish(const HttpResponse &response);
	// Ends the running check with `result`: logs it, saves the time when there was an answer, and calls the
	// callers.
	void complete(const UpdateResult &result);
	// The saved check time. It is read from the file the first time it is needed.
	std::optional<std::int64_t> stored_last_check();

	SettingsStore &settings;
	bool started = false;                // start() has armed the timers
	bool stopped = false;                // set by stop(); nothing starts or completes after it
	bool checking = false;               // a request is in flight
	bool running_automatic = false;      // the request in flight was started by a timer
	std::vector<Callback> waiting;       // manual callers, answered with the running check's result
	Callback update_callback;            // told about each automatic check's update_available
	std::unique_ptr<QTimer> first_check; // single-shot, kFirstCheckDelayMs after start()
	std::unique_ptr<QTimer> tick;        // repeats every kTickIntervalMs
	bool last_check_loaded = false;
	std::optional<std::int64_t> last_check; // Unix seconds of the last check that got an answer
};

UpdateChecker::Impl::Impl(SettingsStore &store) : settings(store)
{
	first_check = std::make_unique<QTimer>();
	first_check->setSingleShot(true);
	tick = std::make_unique<QTimer>();
}

void UpdateChecker::Impl::connect_timers()
{
	// Each slot holds only the weak reference, so a timer can never keep the state alive. The timers run on the UI
	// thread, where the reference is locked.
	const std::weak_ptr<Impl> weak = weak_from_this();
	QObject::connect(first_check.get(), &QTimer::timeout, [weak] {
		if (const std::shared_ptr<Impl> impl = weak.lock())
			impl->on_timer();
	});
	QObject::connect(tick.get(), &QTimer::timeout, [weak] {
		if (const std::shared_ptr<Impl> impl = weak.lock())
			impl->on_timer();
	});
}

void UpdateChecker::Impl::start()
{
	// Once only. Arming again would restart the 60 s countdown.
	if (started || stopped)
		return;
	started = true;
	first_check->start(kFirstCheckDelayMs);
	tick->start(kTickIntervalMs);
}

void UpdateChecker::Impl::stop()
{
	if (stopped)
		return;
	stopped = true;
	first_check->stop();
	tick->stop();
	// Dropped, not called. A check that is still running keeps its callers in its own list, and complete() stops
	// calling them as soon as `stopped` is set.
	waiting.clear();
	update_callback = nullptr;
}

void UpdateChecker::Impl::check_now(Callback done)
{
	if (stopped)
		return;
	waiting.push_back(std::move(done));
	// A check already in flight answers this caller too. A manual request made during an automatic check therefore
	// gets the same result, and the automatic check's update callback runs as usual.
	if (!checking)
		begin_check(false);
}

void UpdateChecker::Impl::set_update_callback(Callback callback)
{
	update_callback = std::move(callback);
}

void UpdateChecker::Impl::on_timer()
{
	try {
		// The setting is read at every tick, so turning it off stops the next automatic check.
		if (stopped || checking || !settings.get().check_for_updates)
			return;
		if (!update_check_due(stored_last_check(), current_time()))
			return;
		begin_check(true);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "update check could not start: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "update check could not start: unknown exception");
	}
}

void UpdateChecker::Impl::begin_check(bool automatic)
{
	// Set before the request. run_on_ui() on the UI thread runs its task at once, so the answer can arrive inside
	// get(), and the check must already be marked as running by then.
	checking = true;
	running_automatic = automatic;
	const std::weak_ptr<Impl> weak = weak_from_this();
	try {
		http_client().get(kLatestReleaseApiUrl, std::string("clip-atlas/") + PLUGIN_VERSION,
				  [weak](const HttpResponse &response) {
					  // On the HTTP thread, which only copies the answer across to the UI thread.
					  run_on_ui([weak, response] {
						  if (const std::shared_ptr<Impl> impl = weak.lock()) {
							  if (!impl->stopped)
								  impl->finish(response);
						  }
					  });
				  });
	} catch (const std::exception &e) {
		// The request never went out, so no answer is coming. The callers still need a result.
		complete(failed_result(std::string("the request could not be sent: ") + e.what()));
	} catch (...) {
		complete(failed_result("the request could not be sent"));
	}
}

void UpdateChecker::Impl::finish(const HttpResponse &response)
{
	UpdateResult result;
	try {
		std::optional<ReleaseInfo> release;
		// Only a 200 has a release to read. evaluate_update() judges every other answer from its status alone.
		if (response.error.empty() && response.status == 200)
			release = parse_release(response.body);
		result = evaluate_update(response.error, response.status, release, PLUGIN_VERSION);
	} catch (const std::exception &e) {
		result = failed_result(std::string("the answer could not be read: ") + e.what());
	} catch (...) {
		result = failed_result("the answer could not be read");
	}
	complete(result);
}

void UpdateChecker::Impl::complete(const UpdateResult &result)
{
	const bool automatic = running_automatic;
	// Cleared first, so a callback may start the next check. A dialog's event loop can let a timer run in the
	// meantime.
	checking = false;
	log_outcome(result);
	if (result.outcome != UpdateOutcome::failed) {
		const std::int64_t now = current_time();
		last_check = now;
		last_check_loaded = true;
		try {
			write_last_check(now);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "could not save the update check time: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "could not save the update check time: unknown exception");
		}
	}
	// Only an automatic check reaches the update callback, so a manual check never shows the banner by itself. A
	// copy of the callback is called: stop() clears the member, and that must not destroy the function while it
	// runs.
	if (automatic && result.outcome == UpdateOutcome::update_available && !stopped && update_callback) {
		try {
			const Callback notify = update_callback;
			notify(result);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "update notice failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "update notice failed: unknown exception");
		}
	}
	// Moved out first. A caller may call check_now(), which adds to `waiting`, and that must not change the list
	// this loop walks.
	std::vector<Callback> callers;
	callers.swap(waiting);
	for (const Callback &caller : callers) {
		// stop() promises that nothing is called after it, so a caller that stops the checker ends the loop.
		if (stopped)
			break;
		if (!caller)
			continue;
		try {
			caller(result);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "update check callback failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "update check callback failed: unknown exception");
		}
	}
}

std::optional<std::int64_t> UpdateChecker::Impl::stored_last_check()
{
	if (!last_check_loaded) {
		last_check = read_last_check();
		last_check_loaded = true;
	}
	return last_check;
}

UpdateChecker::UpdateChecker(SettingsStore &settings) : impl_(std::make_shared<Impl>(settings))
{
	impl_->connect_timers();
}

UpdateChecker::~UpdateChecker()
{
	stop();
}

void UpdateChecker::start()
{
	run_guarded("start", [this] { impl_->start(); });
}

void UpdateChecker::stop()
{
	run_guarded("stop", [this] { impl_->stop(); });
}

void UpdateChecker::check_now(Callback done)
{
	run_guarded("check", [this, &done] { impl_->check_now(std::move(done)); });
}

void UpdateChecker::set_update_callback(Callback callback)
{
	run_guarded("setting the update callback",
		    [this, &callback] { impl_->set_update_callback(std::move(callback)); });
}

} // namespace clip_atlas

// Clip Atlas — the installer download's state, and the launch at exit. update-installer.hpp says what the installer
// does; this file says how.
//
// A download's answer comes back on the HTTP thread. That thread only copies it and hops to the UI thread with
// run_on_ui(). The hop locks a std::weak_ptr to the shared state, so the last reference is always released on the UI
// thread. Each prepare() that starts a download takes a new generation number, and an answer for an older one is
// dropped: a newer release found while an older one downloads replaces it.
#include "update-installer.hpp"

#include "http.hpp"
#include "installer-launch.hpp"
#include "ui-thread.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace clip_atlas {

namespace {

// Below the plugin's config folder, next to settings.json. The folder belongs to the downloads: http-win.cpp removes
// earlier installers from it.
constexpr const char *kDownloadFolder = "updates";

using BmemString = std::unique_ptr<char, void (*)(void *)>;

// The public methods run inside OBS and Qt calls, where an exception must not unwind into C code or the event loop. It
// is logged here and the next call carries on.
template<typename Fn> void run_guarded(const char *where, Fn &&fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "update install: %s failed: %s", where, e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "update install: %s failed: unknown exception", where);
	}
}

} // namespace

struct UpdateInstaller::Impl : public std::enable_shared_from_this<UpdateInstaller::Impl> {
	// The calls UpdateInstaller forwards, with the same meaning as in the header.
	bool supported();
	bool prepare(const UpdateResult &result);
	void stop();
	bool launch_ready();

	// The answer to download `generation`. Called on the UI thread.
	void finish(std::uint64_t generation, const DownloadResult &result);
	// Moves to `next` and tells the state callback.
	void change(InstallerStatus next);

	bool stopped = false;
	std::optional<bool> location_ok; // supported(), once worked out
	InstallerStatus current;
	std::string ready_path;       // UTF-8 path of the checked installer while `current` is ready
	std::uint64_t generation = 0; // of the download that is running, or ran last
	StateCallback state_callback;
};

bool UpdateInstaller::Impl::supported()
{
	if (location_ok)
		return *location_ok;
	const std::string module = own_module_path_utf8();
	const std::string program_data = program_data_folder_utf8();
	location_ok = is_installer_location(module, program_data);
	if (*location_ok)
		obs_log(LOG_INFO, "updates can be installed when OBS closes");
	else
		obs_log(LOG_INFO, "updates are installed by hand: this copy (%s) is not where the installer puts it",
			module.empty() ? "unknown location" : module.c_str());
	return *location_ok;
}

bool UpdateInstaller::Impl::prepare(const UpdateResult &result)
{
	if (stopped || result.outcome != UpdateOutcome::update_available || !result.installer)
		return false;
	// The same version again, as from the hourly check or a manual one, keeps what is under way or done.
	if ((current.state == InstallerState::downloading || current.state == InstallerState::ready) &&
	    current.version == result.latest_version)
		return true;
	if (!supported())
		return false;

	BmemString folder(obs_module_config_path(kDownloadFolder), bfree);
	if (!folder) {
		obs_log(LOG_ERROR, "no config folder, the update can't be downloaded");
		return false;
	}
	DownloadRequest request;
	request.url = result.installer->url;
	request.user_agent = std::string("clip-atlas/") + PLUGIN_VERSION;
	request.folder_utf8 = folder.get();
	request.file_name = result.installer->file_name;
	request.size = result.installer->size;
	request.sha256 = result.installer->sha256;

	const std::uint64_t mine = ++generation;
	ready_path.clear();
	InstallerStatus next;
	next.state = InstallerState::downloading;
	next.version = result.latest_version;
	// Changed before the request: the stub answers inside download(), and that answer must find the download under
	// way.
	change(next);
	obs_log(LOG_INFO, "downloading the %s installer", result.latest_version.c_str());

	const std::weak_ptr<Impl> weak = weak_from_this();
	try {
		http_client().download(request, [weak, mine](const DownloadResult &answer) {
			// On the HTTP thread, which only copies the answer across to the UI thread.
			run_on_ui([weak, mine, answer] {
				if (const std::shared_ptr<Impl> impl = weak.lock()) {
					if (!impl->stopped)
						impl->finish(mine, answer);
				}
			});
		});
	} catch (const std::exception &e) {
		// The request never went out, so no answer is coming.
		DownloadResult failed;
		failed.error = std::string("the download could not be started: ") + e.what();
		finish(mine, failed);
	} catch (...) {
		DownloadResult failed;
		failed.error = "the download could not be started";
		finish(mine, failed);
	}
	return current.state == InstallerState::downloading || current.state == InstallerState::ready;
}

void UpdateInstaller::Impl::finish(std::uint64_t answered, const DownloadResult &result)
{
	// A later prepare() replaced this download, so its answer no longer describes the state.
	if (answered != generation || current.state != InstallerState::downloading)
		return;
	InstallerStatus next;
	next.version = current.version;
	if (result.error.empty() && !result.path_utf8.empty()) {
		ready_path = result.path_utf8;
		next.state = InstallerState::ready;
		obs_log(LOG_INFO, "the %s installer is downloaded and checked; it runs when OBS closes",
			next.version.c_str());
	} else {
		next.state = InstallerState::failed;
		next.error = result.error.empty() ? "the download gave no file" : result.error;
		obs_log(LOG_WARNING, "the %s installer could not be downloaded: %s", next.version.c_str(),
			next.error.c_str());
	}
	change(next);
}

void UpdateInstaller::Impl::change(InstallerStatus next)
{
	current = std::move(next);
	if (stopped || !state_callback)
		return;
	// A copy is called: the callback may replace the member, and that must not destroy the function while it runs.
	try {
		const StateCallback notify = state_callback;
		notify(current);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "update install notice failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "update install notice failed: unknown exception");
	}
}

void UpdateInstaller::Impl::stop()
{
	if (stopped)
		return;
	stopped = true;
	state_callback = nullptr;
	// A download still running ends its own way: http_client().shutdown() stops it, and its answer finds `stopped`.
	if (current.state == InstallerState::downloading)
		current.state = InstallerState::idle;
}

bool UpdateInstaller::Impl::launch_ready()
{
	if (current.state != InstallerState::ready || ready_path.empty())
		return false;
	const std::string path = ready_path;
	const std::string version = current.version;
	// Not ready any more, whatever happens below, so a second call can't start a second installer.
	ready_path.clear();
	current.state = InstallerState::idle;
	const std::string error = launch_installer(path, kInstallerParameters);
	if (!error.empty()) {
		obs_log(LOG_WARNING, "the %s installer did not start: %s", version.c_str(), error.c_str());
		return false;
	}
	obs_log(LOG_INFO, "started the %s installer; it installs once OBS has closed", version.c_str());
	return true;
}

UpdateInstaller::UpdateInstaller() : impl_(std::make_shared<Impl>()) {}

UpdateInstaller::~UpdateInstaller()
{
	stop();
}

bool UpdateInstaller::supported()
{
	bool ok = false;
	run_guarded("checking the plugin's location", [this, &ok] { ok = impl_->supported(); });
	return ok;
}

bool UpdateInstaller::prepare(const UpdateResult &result)
{
	bool started = false;
	run_guarded("preparing", [this, &result, &started] { started = impl_->prepare(result); });
	return started;
}

void UpdateInstaller::set_state_callback(StateCallback callback)
{
	run_guarded("setting the state callback", [this, &callback] { impl_->state_callback = std::move(callback); });
}

InstallerStatus UpdateInstaller::status() const
{
	return impl_->current;
}

void UpdateInstaller::stop()
{
	run_guarded("stop", [this] { impl_->stop(); });
}

bool UpdateInstaller::launch_ready()
{
	bool started = false;
	run_guarded("launch", [this, &started] { started = impl_->launch_ready(); });
	return started;
}

} // namespace clip_atlas

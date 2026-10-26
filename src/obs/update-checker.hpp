// Clip Atlas — checks GitHub for a newer release, once a day and on request.
//
// A check: http_client().get(kLatestReleaseApiUrl, "clip-atlas/" PLUGIN_VERSION,
// ...); the answer hops to the UI thread with run_on_ui(), guarded by a
// std::weak_ptr to shared state locked on the UI thread only (as in
// clip-filer.cpp). On status 200 the body is parsed with
// obs_data_create_from_json(body) (obs_data_t *, release with
// obs_data_release(); NULL when the JSON is invalid): tag_name =
// obs_data_get_string(d, "tag_name"), html_url = obs_data_get_string(d,
// "html_url"), draft = obs_data_get_bool(d, "draft"), prerelease =
// obs_data_get_bool(d, "prerelease"), and each object of
// obs_data_get_array(d, "assets") (obs_data_array_count / obs_data_array_item,
// each released) as a ReleaseAsset ("name", "browser_download_url", "size",
// "state", "digest"); a NULL object or an empty tag_name gives ReleaseInfo
// nullopt. Then evaluate_update(error, status, release,
// PLUGIN_VERSION). Every check is logged at INFO (outcome and versions) or
// WARNING (failed, with the error).
//
// Automatic checks:
//   - Only while Settings::check_for_updates is on (read when each check is
//     due, so turning it off stops the next one).
//   - The time of the last automatic check that got an answer (any outcome but
//     failed) is kept in obs_module_config_path("update-check.json") as
//     {"last_check": <Unix seconds>} (obs_data_set_int /
//     obs_data_save_json_safe(data, path, "tmp", "bak"), os_mkdirs for the
//     folder first; a missing or unreadable file means "never checked"). Read
//     and written on the UI thread: one tiny file, the same accepted exception
//     as settings.json.
//   - start() (at OBS_FRONTEND_EVENT_FINISHED_LOADING) arms a single-shot
//     QTimer for 60 s, so the check never slows OBS's start-up, and a repeating
//     QTimer every hour; each of them checks when update_check_due(last_check,
//     now) is true and no check is running.
//   - An update_available result calls the update callback; other outcomes
//     only log. A failed check is retried at the next hourly tick (it did not
//     update last_check).
//
// Manual checks (Tools → "Check for Clip Atlas updates…"): check_now() runs a
// check at once, whatever the setting and the last check time, and calls its
// own callback with every outcome; it updates last_check like an automatic
// one. A manual check while another is running waits for that one's answer
// and gets the same result.
//
// UI thread only (all public functions).
#pragma once

#include "settings-store.hpp"
#include "update-policy.hpp"

#include <functional>
#include <memory>

namespace clip_atlas {

class UpdateChecker {
public:
	using Callback = std::function<void(const UpdateResult &result)>;

	explicit UpdateChecker(SettingsStore &settings);
	// Calls stop().
	~UpdateChecker();

	UpdateChecker(const UpdateChecker &) = delete;
	UpdateChecker &operator=(const UpdateChecker &) = delete;

	// Arms the timers. Once only; does nothing after stop().
	void start();
	// Stops the timers and drops answers still on the way. Idempotent.
	void stop();

	// Starts a check now. `done` is called on the UI thread with its result,
	// unless stop() runs first.
	void check_now(Callback done);

	// Called on the UI thread with each automatic check's update_available
	// result.
	void set_update_callback(Callback callback);

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;
};

} // namespace clip_atlas

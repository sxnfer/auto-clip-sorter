// Clip Atlas — downloads a newer release's installer in the background, and runs it as OBS closes.
//
// prepare(result) takes an update_available UpdateResult whose `installer` is set and downloads that file with
// http_client().download() into obs_module_config_path("updates"), checked against the size and SHA-256 GitHub lists
// for it. The answer hops to the UI thread with run_on_ui(), guarded by a std::weak_ptr to shared state locked on the
// UI thread only (as in update-checker.cpp). Each change of state is passed to the state callback.
//
// Only a copy of the plugin the installer put there is updated this way: is_installer_location(own_module_path_utf8(),
// program_data_folder_utf8()). Any other copy (the zip, unpacked into OBS's own folder or the user's profile) would be
// left loaded next to a second copy, so for it supported() is false and the caller offers the release page instead.
//
// launch_ready() runs the checked installer with kInstallerParameters through launch_installer(). It is called from
// OBS_FRONTEND_EVENT_EXIT: the installer waits for OBS to finish closing (/WAITOBS), then installs with only its
// progress window. Windows asks the user's permission first, because the installer writes to ProgramData.
//
// UI thread only (all public functions).
#pragma once

#include "update-policy.hpp"

#include <functional>
#include <memory>
#include <string>

namespace clip_atlas {

enum class InstallerState {
	idle,        // nothing downloaded or downloading
	downloading, // the installer of `version` is on its way
	ready,       // the installer of `version` is downloaded, checked, and runs when OBS closes
	failed,      // the download of `version` failed; `error` says why
};

struct InstallerStatus {
	InstallerState state = InstallerState::idle;
	std::string version; // the release's version (UpdateResult::latest_version); "" while idle
	std::string error;   // English, for the log; "" unless failed
};

class UpdateInstaller {
public:
	using StateCallback = std::function<void(const InstallerStatus &status)>;

	UpdateInstaller();
	// Calls stop().
	~UpdateInstaller();

	UpdateInstaller(const UpdateInstaller &) = delete;
	UpdateInstaller &operator=(const UpdateInstaller &) = delete;

	// Whether this copy of the plugin is one the installer can update. Worked out on the first call, which logs
	// the answer once.
	bool supported();

	// Starts downloading `result`'s installer. True when it is downloading or ready afterwards; a call for the
	// version already downloading or ready changes nothing. False, and nothing starts, when the result has no
	// installer, supported() is false, or the installer is stopped.
	bool prepare(const UpdateResult &result);

	// Called on the UI thread with every change of state.
	void set_state_callback(StateCallback callback);

	// The current state.
	InstallerStatus status() const;

	// Drops answers still on the way; no state callback runs after it. Idempotent. A ready installer stays ready,
	// for launch_ready().
	void stop();

	// Starts the ready installer, if there is one, and logs the outcome. Once only: the installer is no longer ready
	// afterwards. True when it started.
	bool launch_ready();

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;
};

} // namespace clip_atlas

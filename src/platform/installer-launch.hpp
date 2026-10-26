// Clip Atlas — runs a downloaded installer once OBS is closing, and finds out whether this copy of the plugin is one
// the installer put there.
//
// Windows implementation (installer-launch-win.cpp):
//   own_module_path_utf8():   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
//                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, <an address in this DLL>), then
//                             GetModuleFileNameW, grown until the path fits.
//   program_data_folder_utf8(): SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT), the folder Inno Setup's
//                             {commonappdata} names, then CoTaskMemFree.
//   launch_installer(path, parameters): ShellExecuteExW with the verb "runas", SEE_MASK_NOASYNC (the call is made
//                             while OBS exits, so it must not return before the launch is done) and
//                             SEE_MASK_FLAG_NO_UI (a failure is logged, not shown). Windows shows its permission
//                             prompt (UAC) before the installer starts; the call returns once the user answers it.
// Paths are UTF-8, converted with path_from_utf8(), and back with path_to_utf8().
//
// The stub (installer-launch-stub.cpp) answers "" for both paths and "not supported on this platform" for a launch.
//
// Any thread. launch_installer() blocks while Windows' prompt is up, so it is called only at OBS's exit, when nothing
// else waits for the UI thread.
#pragma once

#include <string>

namespace clip_atlas {

// UTF-8 path of the plugin's own DLL; "" when Windows can't say.
std::string own_module_path_utf8();

// UTF-8 path of the ProgramData folder, without a trailing backslash; "" when Windows can't say.
std::string program_data_folder_utf8();

// Starts the installer at `path_utf8` (an absolute path) with `parameters` (UTF-8) as its command line, asking Windows
// to run it as administrator. "" once it has started; otherwise the reason, in English.
std::string launch_installer(const std::string &path_utf8, const std::string &parameters);

} // namespace clip_atlas

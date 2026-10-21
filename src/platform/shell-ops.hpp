// Clip Atlas — the dock's file actions through the Windows Shell: open a clip,
// show it in Explorer, rename it, and move it to the Recycle Bin.
//
// Every call returns at once and does its work on the ShellOps TaskQueue
// thread, in the order the calls were made, with COM initialised as a
// single-threaded apartment there (CoInitializeEx(nullptr,
// COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE) in the start hook,
// CoUninitialize() in the exit hook). The Shell functions below require it.
// Paths are UTF-8, converted with path_from_utf8(), then .wstring(). Include
// <shobjidl.h>, <shlobj.h> and <shellapi.h> (not the *_core.h headers, which
// MinGW lacks).
//
// Windows implementation (shell-ops-win.cpp):
//   open(path):      ShellExecuteW(nullptr, L"open", path, nullptr, nullptr,
//                    SW_SHOWNORMAL); a return value <= 32 is an error.
//   open_folder:     the same with the folder's path.
//   reveal(path):    SHParseDisplayName(path, nullptr, &pidl, 0, nullptr),
//                    then SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0),
//                    then CoTaskMemFree(pidl). If either fails, opens the
//                    parent folder instead (open_folder), which is not an error.
//   rename(path, new_name): MoveFileExW(path, <same folder>\new_name, 0).
//                    No MOVEFILE_REPLACE_EXISTING: an existing target fails
//                    with ERROR_ALREADY_EXISTS (or ERROR_FILE_EXISTS) and
//                    nothing changes. No MOVEFILE_COPY_ALLOWED: it is always a
//                    rename within one folder. new_name must be a bare file
//                    name (renamed_file_name() made it); a name containing a
//                    path separator is refused before any call.
//   recycle(path):   CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
//                    IID_PPV_ARGS(&op)); op->SetOperationFlags(FOF_ALLOWUNDO |
//                    FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION |
//                    FOF_WANTNUKEWARNING | FOF_SILENT | FOF_NOERRORUI |
//                    FOFX_EARLYFAILURE); SHCreateItemFromParsingName(path) →
//                    op->DeleteItem(item, nullptr); op->PerformOperations().
//                    FOF_WANTNUKEWARNING makes Windows ask before it would
//                    delete a file for good (a drive with no Recycle Bin, a
//                    file too large for it), so a clip is never destroyed
//                    without the user saying so. Success means
//                    PerformOperations() returned S_OK, GetAnyOperationsAborted()
//                    reports FALSE, and the file no longer exists; anything
//                    else is an error (the user declining the warning is
//                    "cancelled").
// Errors are English, for the log and for a message box: a short reason,
// e.g. "a file with that name already exists", followed by the Windows error
// text from FormatMessageW where there is one.
//
// The stub (shell-ops-stub.cpp) answers every call with "not supported on
// this platform" and changes nothing.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace clip_atlas {

class ShellOps {
public:
	// "" on success, otherwise the reason. Called once per call, on the
	// ShellOps thread, never inside the call that queued it; or right away on
	// the calling thread after shutdown() with "Clip Atlas is shutting down".
	// Must not throw (it is caught and dropped). Hop to the UI thread before
	// touching widgets or plugin state.
	using Done = std::function<void(const std::string &error)>;
	// As Done, plus the renamed file's full UTF-8 path on success ("" on error).
	using RenameDone = std::function<void(const std::string &error, const std::string &new_path_utf8)>;

	ShellOps();
	// Calls shutdown().
	~ShellOps();

	ShellOps(const ShellOps &) = delete;
	ShellOps &operator=(const ShellOps &) = delete;

	// Opens the clip in the program Windows uses for its type.
	void open(const std::string &path_utf8, Done done = {});
	// Opens a folder in Explorer.
	void open_folder(const std::string &folder_utf8, Done done = {});
	// Opens Explorer on the clip's folder with the clip selected.
	void reveal(const std::string &path_utf8, Done done = {});
	// Renames the clip within its folder. Never replaces another file.
	void rename(const std::string &path_utf8, const std::string &new_file_name_utf8, RenameDone done);
	// Moves the clip to the Recycle Bin. Never deletes it for good unless the
	// user confirms Windows' own warning. The caller asks first (the dock shows
	// a confirmation before calling this).
	void recycle(const std::string &path_utf8, Done done);

	// Drops queued calls (their callbacks get "Clip Atlas is shutting down"),
	// waits for the one running (an Explorer warning dialog may keep it waiting
	// for the user), and stops the thread. Idempotent. Call it from stop_all()
	// at EXIT, never from a static destructor.
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The plugin's single ShellOps, created on first use and never destroyed (as
// sound_player()). Defined in both shell-ops-win.cpp and shell-ops-stub.cpp.
ShellOps &shell_ops();

} // namespace clip_atlas

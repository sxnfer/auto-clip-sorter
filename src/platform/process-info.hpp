// Clip Atlas — asks Windows which program owns a captured window.
//
// The hooked signal only gives us the window title, class and executable file
// name. The executable's ProductName ("ELDEN RING") is often a better game
// name than either, so name-resolver uses it when available.
//
// Win32 implementation in process-info-win.cpp; process-info-stub.cpp returns
// "not found" so non-Windows builds compile. Implemented in M4 (WP4.2).
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace clip_atlas {

struct WindowQuery {
	std::string window_class; // UTF-8, from the hooked calldata "class"
	std::string window_title; // UTF-8, from the hooked calldata "title"
	std::string executable;   // UTF-8 file name, from the hooked calldata "executable"
};

struct ProcessInfo {
	std::filesystem::path image_path; // full path of the process executable
	std::string product_name;         // VERSIONINFO ProductName, UTF-8; may be empty
};

// Finds the top-level window matching class and title (FindWindowW), gets its
// process (GetWindowThreadProcessId, OpenProcess with
// PROCESS_QUERY_LIMITED_INFORMATION, QueryFullProcessImageNameW), checks the
// image file name equals `executable` case-insensitively, then reads the
// ProductName of the first translation in its version resource
// (GetFileVersionInfoW / VerQueryValueW).
//
// nullopt if the window is gone, the process can't be opened, or the
// executable doesn't match. Reads the executable file, so call it off the UI
// thread. Never throws.
std::optional<ProcessInfo> query_process_info(const WindowQuery &query);

} // namespace clip_atlas

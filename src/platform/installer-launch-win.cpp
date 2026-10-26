// Clip Atlas — the installer launch and the plugin's location, with the Win32 Shell. installer-launch.hpp says what
// each call does.
//
// Built on Windows only (CMakeLists.txt links shell32 and ole32). The stub for other platforms is
// installer-launch-stub.cpp.
// Guarded, so a build that already defines them still compiles (as in sound-player-win.cpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "installer-launch.hpp"

#include "clip-index.hpp"
#include "clip-layout.hpp"

#include <filesystem>
#include <string>

namespace clip_atlas {

namespace {

// Room for a module path, in characters, grown until it fits. Paths beyond MAX_PATH are possible with long path
// support on.
constexpr DWORD kFirstPathChars = MAX_PATH;
constexpr DWORD kMostPathChars = 32768;

// The English text for a Win32 error code, or "error <code>" when Windows has none.
std::string error_text(DWORD code)
{
	wchar_t text[512] = {};
	const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	DWORD length =
		FormatMessageW(flags, nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), text, 512, nullptr);
	if (length == 0)
		length = FormatMessageW(flags, nullptr, code, 0, text, 512, nullptr);
	while (length > 0 && (text[length - 1] == L'\r' || text[length - 1] == L'\n'))
		--length;
	const std::string number = "error " + std::to_string(static_cast<unsigned long>(code));
	if (length == 0)
		return number;
	return path_to_utf8(std::filesystem::path(std::wstring(text, length))) + " (" + number + ")";
}

} // namespace

std::string own_module_path_utf8()
{
	HMODULE module = nullptr;
	// Any address inside this DLL names it, and this function is one.
	const auto address = reinterpret_cast<LPCWSTR>(&own_module_path_utf8);
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				address, &module))
		return "";
	for (DWORD size = kFirstPathChars; size <= kMostPathChars; size *= 2) {
		std::wstring path(size, L'\0');
		const DWORD length = GetModuleFileNameW(module, path.data(), size);
		if (length == 0)
			return "";
		// A path that filled the buffer may have been cut short, so it is tried again with more room.
		if (length < size) {
			path.resize(length);
			return path_to_utf8(std::filesystem::path(path));
		}
	}
	return "";
}

std::string program_data_folder_utf8()
{
	PWSTR folder = nullptr;
	const HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &folder);
	std::string utf8;
	if (SUCCEEDED(hr) && folder)
		utf8 = path_to_utf8(std::filesystem::path(folder));
	// Freed on failure too: the call may allocate either way.
	CoTaskMemFree(folder);
	while (!utf8.empty() && (utf8.back() == '\\' || utf8.back() == '/'))
		utf8.pop_back();
	return utf8;
}

std::string launch_installer(const std::string &path_utf8, const std::string &parameters)
{
	if (path_utf8.empty() || path_utf8.find('\0') != std::string::npos ||
	    parameters.find('\0') != std::string::npos)
		return "the installer path is not valid";
	const std::filesystem::path path = path_from_utf8(path_utf8);
	if (!path.is_absolute())
		return "the installer path is not absolute";
	const std::wstring file = path.wstring();
	// Not a path, but path_from_utf8() is the one UTF-8 to UTF-16 conversion the plugin uses, and a path keeps its text
	// exactly as given.
	const std::wstring arguments = path_from_utf8(parameters).wstring();
	const std::wstring folder = path.parent_path().wstring();

	SHELLEXECUTEINFOW info{};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	// The installer asks for administrator rights itself; "runas" says so up front, so Windows prompts for it.
	info.lpVerb = L"runas";
	info.lpFile = file.c_str();
	info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
	info.lpDirectory = folder.c_str();
	info.nShow = SW_SHOWNORMAL;
	if (ShellExecuteExW(&info))
		return "";
	const DWORD code = GetLastError();
	if (code == ERROR_CANCELLED)
		return "the Windows permission prompt was declined";
	return "Windows could not start the installer: " + error_text(code);
}

} // namespace clip_atlas

// Clip Atlas — the dock's file actions through the Windows Shell. shell-ops.hpp states the contract; this file keeps it.
//
// Every call is queued on one thread that starts COM as a single-threaded apartment, so the Shell functions run on a
// thread that is ready for them and never on the UI thread. The queue runs the calls in the order they were made. Each
// call's `done` runs on that thread, except for a call made after shutdown() has begun, which is answered on the caller's
// thread.
//
// A path or name is checked before Windows is asked anything. Windows reads a path only up to its first NUL, and it
// silently drops trailing dots and spaces from a name, so either would act on, or report, a file other than the one the
// caller named.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>

#include "clip-layout.hpp"
#include "shell-ops.hpp"
#include "task-queue.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace clip_atlas {

namespace {

// What every refused call, and every call after shutdown(), reports. The header gives the wording.
constexpr const char kShuttingDown[] = "Clip Atlas is shutting down";

// What one call reports: "" and "" on success, or the reason and "". Only rename's success carries a new path.
struct Result {
	std::string error;
	std::string new_path;
};

// Every call finishes through this one shape, so the queue needs one callback type. A Done is wrapped into it.
using Finish = std::function<void(const std::string &error, const std::string &new_path)>;

Result fail(std::string error)
{
	return Result{std::move(error), std::string()};
}

// Frees the text FormatMessageW allocates, on every path out of the scope that holds it.
struct LocalFreer {
	void operator()(wchar_t *text) const { LocalFree(text); }
};
using LocalText = std::unique_ptr<wchar_t, LocalFreer>;

// Windows speaks UTF-16; the plugin speaks UTF-8.
std::string utf16_to_utf8(const std::wstring &wide)
{
	if (wide.empty())
		return std::string();
	const int length = static_cast<int>(wide.size());
	const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return std::string();
	std::string utf8(static_cast<std::size_t>(size), '\0');
	if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, utf8.data(), size, nullptr, nullptr) <= 0)
		return std::string();
	return utf8;
}

// Windows' text for `code`: English first, then the system's own language when English is not installed. "" when
// Windows has no text for the code.
std::string system_message(DWORD code)
{
	const DWORD languages[] = {static_cast<DWORD>(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)), 0};
	for (const DWORD language : languages) {
		wchar_t *raw = nullptr;
		const DWORD length = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, code, language, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
		LocalText owned(raw);
		if (length == 0 || raw == nullptr)
			continue;
		std::string text = utf16_to_utf8(std::wstring(raw, static_cast<std::size_t>(length)));
		// The text ends in CR LF, which would break the one-line message the dock shows.
		while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
			text.pop_back();
		if (!text.empty())
			return text;
	}
	return std::string();
}

// "<reason>: Access is denied. (error 5)", or "<reason>: error 5" when Windows has no text for the code.
std::string error_message(const std::string &reason, DWORD code)
{
	const std::string number = "error " + std::to_string(static_cast<unsigned long>(code));
	const std::string text = system_message(code);
	return reason + ": " + (text.empty() ? number : text + " (" + number + ")");
}

// The same for an HRESULT. A Win32 error wrapped as an HRESULT (facility 7) is looked up by its Win32 code, the one
// Windows has text for. Any other HRESULT is looked up as it is.
std::string error_message_hr(const std::string &reason, HRESULT hr)
{
	char hex[32];
	std::snprintf(hex, sizeof hex, "error 0x%08lX", static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
	const bool win32 = HRESULT_FACILITY(hr) == FACILITY_WIN32;
	const DWORD lookup = win32 ? static_cast<DWORD>(HRESULT_CODE(hr)) : static_cast<DWORD>(hr);
	const std::string text = system_message(lookup);
	return reason + ": " + (text.empty() ? std::string(hex) : text + " (" + hex + ")");
}

// Windows' "the file, or the folder it is in, is not there" codes.
bool is_not_found(DWORD code)
{
	return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

bool is_not_found_hr(HRESULT hr)
{
	return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

// Refused before Windows is called. An empty path names nothing, and a NUL cuts the path short.
// A relative path would resolve against OBS's working directory. Win32 drops trailing dots and
// spaces from the last name, and '*' and '?' are wildcards to some Shell calls.
std::string path_problem(const std::string &path_utf8)
{
	if (path_utf8.empty())
		return "no file was given";
	if (path_utf8.find('\0') != std::string::npos)
		return "that path can't be used";
	const std::size_t separator = path_utf8.find_last_of("\\/");
	const std::string last = separator == std::string::npos ? path_utf8 : path_utf8.substr(separator + 1);
	if (!last.empty() && (last.back() == '.' || last.back() == ' '))
		return "that path can't be used";
	if (last.find_first_of("*?") != std::string::npos)
		return "that path can't be used";
	if (!path_from_utf8(path_utf8).is_absolute())
		return "that path can't be used";
	return std::string();
}

// The folder is everything up to and including the last separator; the file is what follows it.
struct SplitPath {
	std::string folder;
	std::string file;
};

SplitPath split_path(const std::string &path_utf8)
{
	const std::size_t separator = path_utf8.find_last_of("\\/");
	if (separator == std::string::npos)
		return SplitPath{std::string(), path_utf8};
	return SplitPath{path_utf8.substr(0, separator + 1), path_utf8.substr(separator + 1)};
}

// A name Windows would store exactly as given. Refused: separators and ':' (a folder or a stream would be named), the
// characters Windows forbids in file names, control characters (a NUL would cut the name short in the Win32 call), and
// a trailing dot or space (Windows drops them, so the file would not get the name that was asked for).
bool usable_file_name(const std::string &name)
{
	if (name.empty() || name == "." || name == "..")
		return false;
	if (name.back() == '.' || name.back() == ' ')
		return false;
	const std::string forbidden = "\\/:*?\"<>|";
	for (const char c : name) {
		if (static_cast<unsigned char>(c) < 0x20 || forbidden.find(c) != std::string::npos)
			return false;
	}
	return true;
}

// ShellExecuteW succeeds with a value above 32. Anything else is an error, and Windows sets the last error for it.
Result shell_open(const std::string &path_utf8, const std::string &reason)
{
	const std::string problem = path_problem(path_utf8);
	if (!problem.empty())
		return fail(problem);
	const std::wstring wide = path_from_utf8(path_utf8).wstring();
	SetLastError(ERROR_SUCCESS);
	const HINSTANCE instance = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	const INT_PTR value = reinterpret_cast<INT_PTR>(instance);
	if (value > 32)
		return Result();
	const DWORD code = GetLastError();
	// Some failures set no last error. "completed successfully" would be a wrong report for one of them, so the
	// ShellExecute value is reported instead.
	if (code == ERROR_SUCCESS)
		return fail(reason + ": ShellExecute returned " + std::to_string(static_cast<long long>(value)));
	return fail(error_message(reason, code));
}

Result shell_open_clip(const std::string &path_utf8)
{
	return shell_open(path_utf8, "Windows could not open the file");
}

Result shell_open_folder(const std::string &folder_utf8)
{
	return shell_open(folder_utf8, "Windows could not open the folder");
}

// Selects the clip in Explorer. When Explorer can't select it, its folder is opened instead, and that call's result is
// the result of this one.
Result shell_reveal(const std::string &path_utf8)
{
	const std::string problem = path_problem(path_utf8);
	if (!problem.empty())
		return fail(problem);
	const std::wstring wide = path_from_utf8(path_utf8).wstring();
	// No return sits between these calls and the free, so the PIDL is released on every path.
	PIDLIST_ABSOLUTE pidl = nullptr;
	HRESULT hr = SHParseDisplayName(wide.c_str(), nullptr, &pidl, 0, nullptr);
	if (SUCCEEDED(hr))
		hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
	CoTaskMemFree(pidl);
	if (SUCCEEDED(hr))
		return Result();
	const std::size_t separator = path_utf8.find_last_of("\\/");
	if (separator == std::string::npos)
		return fail(error_message_hr("Windows could not show the clip in Explorer", hr));
	// The folder part, keeping its separator. "C:\" and "\\server\share\" are absolute as written, but "C:" and
	// "\\server\share" are not, and path_problem() refuses those.
	return shell_open_folder(path_utf8.substr(0, separator + 1));
}

// What a MoveFileExW failure means, in the words the dock shows. Windows' own text follows it.
std::string rename_reason(DWORD code)
{
	switch (code) {
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return "a file with that name already exists";
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return "the clip no longer exists";
	case ERROR_SHARING_VIOLATION:
	case ERROR_ACCESS_DENIED:
		return "the clip is in use or read-only";
	default:
		return "Windows could not rename the clip";
	}
}

// Renames the clip inside its own folder. Flags 0 on MoveFileExW are deliberate: without MOVEFILE_REPLACE_EXISTING a file
// that exists is never replaced, and without MOVEFILE_COPY_ALLOWED a rename is never a copy followed by a delete. A rename
// that can't be made fails and leaves both names as they were.
Result shell_rename_clip(const std::string &path_utf8, const std::string &new_name)
{
	const std::string problem = path_problem(path_utf8);
	if (!problem.empty())
		return fail(problem);
	if (!usable_file_name(new_name))
		return fail("that name can't be used");
	const SplitPath parts = split_path(path_utf8);
	if (parts.folder.empty())
		return fail("the clip's folder is unknown");
	// A path that ends in a separator, or whose last part is "." or "..", does not name a file.
	if (parts.file.empty() || parts.file == "." || parts.file == "..")
		return fail("no file was given");
	if (parts.file == new_name)
		return fail("the name is unchanged");
	const std::wstring from = path_from_utf8(path_utf8).wstring();
	// A folder is never renamed as a clip: refused here, because it would move everything inside it under the new name.
	const DWORD attributes = GetFileAttributesW(from.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return fail("that is a folder, not a clip");
	const std::string new_path = parts.folder + new_name;
	const std::wstring to = path_from_utf8(new_path).wstring();
	if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
		const DWORD code = GetLastError();
		return fail(error_message(rename_reason(code), code));
	}
	return Result{std::string(), new_path};
}

// Holds one COM interface and releases it on every path out of the scope. put() is for the out parameter of a creation
// call, as in IID_PPV_ARGS(holder.put()).
template<typename Interface> class ComPtr {
public:
	ComPtr() = default;
	ComPtr(const ComPtr &) = delete;
	ComPtr &operator=(const ComPtr &) = delete;
	~ComPtr()
	{
		if (ptr_)
			ptr_->Release();
	}

	Interface **put()
	{
		if (ptr_)
			ptr_->Release();
		ptr_ = nullptr;
		return &ptr_;
	}

	Interface *get() const { return ptr_; }

	Interface *operator->() const { return ptr_; }

private:
	Interface *ptr_ = nullptr;
};

// Moves the clip to the Recycle Bin and never anywhere else. A file Windows cannot recycle is left to Windows' own
// warning (FOF_WANTNUKEWARNING), and the user's answer to it decides.
Result shell_recycle_clip(const std::string &path_utf8)
{
	const std::string problem = path_problem(path_utf8);
	if (!problem.empty())
		return fail(problem);
	const std::wstring wide = path_from_utf8(path_utf8).wstring();
	const std::string reason = "Windows could not move the clip to the Recycle Bin";

	// Checked before the Shell is asked anything: a clip that is gone is reported as gone, and a folder is refused, because
	// recycling a folder would send everything inside it to the Recycle Bin too.
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		const DWORD code = GetLastError();
		if (is_not_found(code))
			return fail("the clip no longer exists");
		return fail(error_message(reason, code));
	}
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return fail("that is a folder, not a clip");

	ComPtr<IFileOperation> operation;
	HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(operation.put()));
	if (FAILED(hr))
		return fail(error_message_hr(reason, hr));
	// FOFX_RECYCLEONDELETE sends the file to the Recycle Bin. FOF_WANTNUKEWARNING keeps Windows' question for a file it
	// would delete for good. FOF_NOCONFIRMATION and the rest stop the Shell asking anything else, because the dock has
	// asked the user already.
	const DWORD flags = static_cast<DWORD>(FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION |
					       FOF_WANTNUKEWARNING | FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE);
	hr = operation->SetOperationFlags(flags);
	if (FAILED(hr))
		return fail(error_message_hr(reason, hr));

	ComPtr<IShellItem> item;
	hr = SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(item.put()));
	if (is_not_found_hr(hr))
		return fail("the clip no longer exists");
	if (FAILED(hr))
		return fail(error_message_hr(reason, hr));
	hr = operation->DeleteItem(item.get(), nullptr);
	if (FAILED(hr))
		return fail(error_message_hr(reason, hr));
	hr = operation->PerformOperations();
	if (FAILED(hr))
		return fail(error_message_hr(reason, hr));
	BOOL aborted = FALSE;
	hr = operation->GetAnyOperationsAborted(&aborted);
	if (FAILED(hr))
		return fail(error_message_hr(reason, hr));
	// Aborts can come with no dialog (FOF_NOERRORUI and FOF_NOCONFIRMATION hide the prompts), so
	// the user may not have cancelled at all.
	if (aborted)
		return fail("cancelled, or Windows could not move the clip (it may be in use)");
	// PerformOperations() can return S_OK when the item itself failed, so the file decides whether it was moved.
	if (GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES)
		return fail("the clip is still there (it may be in use)");
	const DWORD code = GetLastError();
	if (is_not_found(code))
		return Result();
	return fail(error_message(reason, code));
}

// Calls `finish` with the outcome. An exception from it is logged, not thrown: it runs on the queue thread, or on the
// caller's thread for a call that is refused at once.
void report(const Finish &finish, const Result &result)
{
	if (!finish)
		return;
	try {
		finish(result.error, result.new_path);
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "shell action callback failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_WARNING, "shell action callback failed: unknown exception");
	}
}

// A Done is called with the error only. An empty Done is simply not called.
Finish as_finish(ShellOps::Done done)
{
	return [done = std::move(done)](const std::string &error, const std::string &) {
		if (done)
			done(error);
	};
}

} // namespace

// One queued call. `work` runs on the queue thread and returns the outcome. `uses_com` makes that thread refuse the call
// when COM never started there, instead of calling into COM.
struct ShellOps::Impl {
	struct Operation {
		std::function<Result()> work;
		bool uses_com = false;
		Finish finish;
	};

	// Called on the caller's thread: queues the call, or answers it at once when shutdown() has begun.
	void submit(Operation operation);
	// Called on the queue thread: runs the oldest entry.
	void run_next();
	// Takes what is waiting and answers it, then stops the queue. Idempotent.
	void shutdown();
	// The queue thread's start and exit hooks.
	void start_com();
	void stop_com();

	std::mutex mutex;              // guards pending and shutting_down
	std::deque<Operation> pending; // accepted and not yet started, oldest first; one queued task per entry
	bool shutting_down = false;

	// Written and read only on the queue thread. Declared before `queue`, so they exist before its thread starts.
	bool com_ready = false;
	std::string com_error; // why COM is unavailable; set only when com_ready is false

	// Last, because its thread starts in its constructor and runs start_com() before the first task.
	TaskQueue queue{[this] { start_com(); },
			[this] {
				stop_com();
			}};
};

void ShellOps::Impl::submit(Operation operation)
{
	std::unique_lock<std::mutex> lock(mutex);
	if (shutting_down) {
		lock.unlock();
		report(operation.finish, Result{kShuttingDown, std::string()});
		return;
	}
	pending.push_back(std::move(operation));
	// The lock is held across post(), so shutdown() can't take this entry between the push and the post. post() fails only
	// after the queue has been shut down, and that happens only once shutting_down is set. The refusal below is a guard,
	// not a path that normally runs.
	if (queue.post([this] { run_next(); }))
		return;
	Operation refused = std::move(pending.back());
	pending.pop_back();
	lock.unlock();
	report(refused.finish, Result{kShuttingDown, std::string()});
}

void ShellOps::Impl::run_next()
{
	Operation operation;
	{
		std::lock_guard<std::mutex> lock(mutex);
		// shutdown() has taken everything that was waiting and answered it, so a task that finds nothing does nothing.
		if (pending.empty())
			return;
		operation = std::move(pending.front());
		pending.pop_front();
	}
	Result result;
	try {
		result = (operation.uses_com && !com_ready) ? Result{com_error, std::string()} : operation.work();
	} catch (const std::exception &e) {
		result = fail(std::string("Clip Atlas could not finish the request: ") + e.what());
	} catch (...) {
		result = fail("Clip Atlas could not finish the request: unknown exception");
	}
	report(operation.finish, result);
}

void ShellOps::Impl::shutdown()
{
	std::deque<Operation> dropped;
	{
		std::lock_guard<std::mutex> lock(mutex);
		shutting_down = true;
		dropped.swap(pending);
	}
	// None of these started, so each gets its one call here, on the calling thread and outside the lock.
	for (Operation &operation : dropped)
		report(operation.finish, Result{kShuttingDown, std::string()});
	dropped.clear();
	// Stops the queue: the tasks still queued for those entries are dropped (they would find nothing), the running call is
	// waited for, the exit hook runs and the thread is joined.
	queue.shutdown();
}

void ShellOps::Impl::start_com()
{
	const HRESULT hr =
		CoInitializeEx(nullptr, static_cast<DWORD>(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
	if (SUCCEEDED(hr)) {
		// S_FALSE means COM was already started on this thread. It still needs the CoUninitialize() in stop_com().
		com_ready = true;
		return;
	}
	com_error = error_message_hr("Windows could not start its Shell components", hr);
	obs_log(LOG_WARNING, "%s", com_error.c_str());
}

void ShellOps::Impl::stop_com()
{
	if (com_ready)
		CoUninitialize();
	com_ready = false;
}

ShellOps::ShellOps() : impl_(std::make_unique<Impl>()) {}

ShellOps::~ShellOps()
{
	shutdown();
}

void ShellOps::open(const std::string &path_utf8, Done done)
{
	impl_->submit(
		Impl::Operation{[path_utf8] { return shell_open_clip(path_utf8); }, true, as_finish(std::move(done))});
}

void ShellOps::open_folder(const std::string &folder_utf8, Done done)
{
	impl_->submit(Impl::Operation{[folder_utf8] { return shell_open_folder(folder_utf8); }, true,
				      as_finish(std::move(done))});
}

void ShellOps::reveal(const std::string &path_utf8, Done done)
{
	impl_->submit(
		Impl::Operation{[path_utf8] { return shell_reveal(path_utf8); }, true, as_finish(std::move(done))});
}

void ShellOps::rename(const std::string &path_utf8, const std::string &new_file_name_utf8, RenameDone done)
{
	impl_->submit(Impl::Operation{[path_utf8, new_file_name_utf8] {
					      return shell_rename_clip(path_utf8, new_file_name_utf8);
				      },
				      false, std::move(done)});
}

void ShellOps::recycle(const std::string &path_utf8, Done done)
{
	impl_->submit(Impl::Operation{[path_utf8] { return shell_recycle_clip(path_utf8); }, true,
				      as_finish(std::move(done))});
}

void ShellOps::shutdown()
{
	impl_->shutdown();
}

ShellOps &shell_ops()
{
	// Deliberately never deleted, as sound_player(): no destructor runs while Windows unloads the DLL.
	static ShellOps *ops = new ShellOps;
	return *ops;
}

} // namespace clip_atlas

// Clip Atlas — one HTTPS GET at a time with WinHTTP, for the update check and the installer download.
//
// get() queues the request and posts one task to the client's TaskQueue. Each task takes the oldest queued request and
// runs it, so requests run one at a time, in order, on the client's own thread: the UI thread never waits for the
// network. shutdown() answers whatever is still queued, so no request is left without its answer.
//
// Each request opens its session, connection and request handles in that order and closes them in reverse on every
// path (WinHttpHandle). Certificate checks and the redirect policy stay at WinHTTP's defaults, which never go from HTTPS
// to HTTP; nothing here weakens them.
//
// Built on Windows only (CMakeLists.txt links winhttp). The stub for other platforms is http-stub.cpp.
// Guarded, so a build that already defines them still compiles (as in sound-player-win.cpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include "clip-layout.hpp"
#include "http.hpp"
#include "sha256.hpp"
#include "task-queue.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace {

// The address must start with exactly this: the check is not case-insensitive.
constexpr char kHttpsPrefix[] = "https://";
constexpr std::size_t kHttpsPrefixLength = sizeof(kHttpsPrefix) - 1;

// GitHub's REST API asks for this media type.
constexpr wchar_t kAcceptHeader[] = L"Accept: application/vnd.github+json";
// A release download is the file's own bytes.
constexpr wchar_t kDownloadAcceptHeader[] = L"Accept: application/octet-stream";

// WinHTTP's "whole string" length: a null-terminated header, or a URL part that WinHttpCrackUrl should point into
// rather than copy.
constexpr DWORD kWholeString = static_cast<DWORD>(-1);

// Per operation (resolve, connect, send, receive), in milliseconds. shutdown() waits for the running request, so these
// bound each operation; kTotalDeadlineMs bounds the request as a whole.
constexpr int kTimeoutMs = 10000;

// A request still running this many milliseconds after its network work started (just before WinHttpOpen) fails at its
// next body query or read. The per-operation timeouts cannot bound the whole request: a body that trickles in gets a
// fresh timeout on every read. GetTickCount64 needs Windows Vista (_WIN32_WINNT 0x0600).
constexpr ULONGLONG kTotalDeadlineMs = 30 * 1000;

// The body is read in pieces of at most this many bytes.
constexpr DWORD kReadChunkBytes = 16 * 1024;

// Room for one Windows message, in characters. Messages are short; a longer one is reported without its text.
constexpr DWORD kMessageChars = 512;

constexpr char kShuttingDown[] = "Clip Atlas is shutting down";

// Calls `done` with `response`. An exception from it is logged and dropped: the thread it runs on has nothing above it
// that could catch the exception.
void call_done(const HttpClient::Done &done, const HttpResponse &response)
{
	if (!done)
		return;
	try {
		done(response);
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "HTTP callback failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_WARNING, "HTTP callback failed: unknown exception");
	}
}

// An answer with no complete reply: `error` set, status 0, empty body.
HttpResponse failure(const std::string &error)
{
	HttpResponse response;
	response.error = error;
	return response;
}

// UTF-8 to UTF-16. False for text that is not valid UTF-8, and for text too long for the int length the Win32 call takes
// (no real address or User-Agent is that long).
bool utf8_to_wide(const std::string &text, std::wstring &wide)
{
	wide.clear();
	if (text.empty())
		return true;
	if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		return false;
	const int length = static_cast<int>(text.size());
	const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
	if (chars <= 0)
		return false;
	wide.resize(static_cast<std::size_t>(chars));
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), chars) == chars;
}

// UTF-16 to UTF-8, or "" when the conversion fails.
std::string wide_to_utf8(const wchar_t *text, int length)
{
	if (length <= 0)
		return "";
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
	if (bytes <= 0)
		return "";
	std::string utf8(static_cast<std::size_t>(bytes), '\0');
	if (WideCharToMultiByte(CP_UTF8, 0, text, length, utf8.data(), bytes, nullptr, nullptr) != bytes)
		return "";
	return utf8;
}

// The English text for a Win32 or WinHTTP error code, without the line break at its end. WinHTTP's own messages are
// tried first, then the system's. "" when neither has one.
std::string windows_message(DWORD code)
{
	const HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
	const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS;
	wchar_t text[kMessageChars] = {};
	const DWORD english = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
	DWORD length = FormatMessageW(flags, winhttp, code, english, text, kMessageChars, nullptr);
	// No English text for this code: fall back to the user's language.
	if (length == 0)
		length = FormatMessageW(flags, winhttp, code, 0, text, kMessageChars, nullptr);
	if (length == 0)
		return "";
	while (length > 0 && (text[length - 1] == L'\r' || text[length - 1] == L'\n'))
		--length;
	return wide_to_utf8(text, static_cast<int>(length));
}

// "<what>: <Windows text> (error <code>)", the form every failure from Windows takes. The caller passes GetLastError()
// straight after the failed call, before anything else can change it.
HttpResponse windows_failure(const char *what, DWORD code)
{
	std::string text = windows_message(code);
	if (text.empty())
		text = "Windows gave no description";
	return failure(std::string(what) + ": " + text + " (error " + std::to_string(code) + ")");
}

// Owns one WinHTTP handle and closes it when it goes out of scope, so no return path can leak one.
class WinHttpHandle {
public:
	explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
	~WinHttpHandle() { reset(nullptr); }

	// Closes the handle held, if any, and takes `handle` in its place.
	void reset(HINTERNET handle)
	{
		if (handle_)
			WinHttpCloseHandle(handle_);
		handle_ = handle;
	}

	WinHttpHandle(const WinHttpHandle &) = delete;
	WinHttpHandle &operator=(const WinHttpHandle &) = delete;

	HINTERNET get() const { return handle_; }
	explicit operator bool() const { return handle_ != nullptr; }

private:
	HINTERNET handle_;
};

// True once `limit_ms` has passed since `started`. GetTickCount64 only moves forward, so the difference is never
// negative.
bool deadline_passed(ULONGLONG started, ULONGLONG limit_ms)
{
	return GetTickCount64() - started >= limit_ms;
}

// The handles of one open request, declared in the order they are opened, so they close in reverse: request,
// connection, session.
struct OpenRequest {
	WinHttpHandle session{nullptr};
	WinHttpHandle connection{nullptr};
	WinHttpHandle request{nullptr};
};

// Sends a GET for `url_utf8` with `accept` and waits for the status line and headers. On success the answer has no
// error, `open.request` is ready for the body, `status` holds the HTTP status and `started` the time the network work
// began, for the caller's deadline. Worker thread only.
HttpResponse send_get(const std::string &url_utf8, const std::string &user_agent, const wchar_t *accept,
		      OpenRequest &open, DWORD &status, ULONGLONG &started)
{
	// Checked before anything is opened, so a bad address never reaches the network.
	if (url_utf8.size() < kHttpsPrefixLength || url_utf8.compare(0, kHttpsPrefixLength, kHttpsPrefix) != 0)
		return failure("only HTTPS addresses are allowed");
	// A NUL would cut the address short in the C API calls below, and a different host or path would be requested.
	if (url_utf8.find('\0') != std::string::npos)
		return failure("the address is not valid");
	if (user_agent.empty())
		return failure("a User-Agent is required");

	std::wstring url;
	if (!utf8_to_wide(url_utf8, url))
		return failure("the address is not valid UTF-8");
	std::wstring agent;
	if (!utf8_to_wide(user_agent, agent))
		return failure("the User-Agent is not valid UTF-8");

	// The parts point into `url`, which therefore has to outlive them.
	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof(parts);
	parts.dwHostNameLength = kWholeString;
	parts.dwUrlPathLength = kWholeString;
	parts.dwExtraInfoLength = kWholeString;
	if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
		return windows_failure("the address could not be read", GetLastError());
	if (parts.nScheme != INTERNET_SCHEME_HTTPS)
		return failure("only HTTPS addresses are allowed");
	if (!parts.lpszHostName || parts.dwHostNameLength == 0)
		return failure("the address has no host name");
	const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
	// The request target is the path followed by the query. An address with no path asks for "/".
	std::wstring target;
	if (parts.lpszUrlPath)
		target.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
	if (parts.lpszExtraInfo)
		target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
	if (target.empty())
		target = L"/";
	const INTERNET_PORT port = parts.nPort;
	// Everything above needs no network, so the deadline starts here.
	started = GetTickCount64();

	open.session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				       WINHTTP_NO_PROXY_BYPASS, 0));
	if (!open.session)
		return windows_failure("could not open a WinHTTP session", GetLastError());
	if (!WinHttpSetTimeouts(open.session.get(), kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs))
		return windows_failure("could not set the timeouts", GetLastError());
	open.connection.reset(WinHttpConnect(open.session.get(), host.c_str(), port, 0));
	if (!open.connection)
		return windows_failure("could not connect to the server", GetLastError());
	open.request.reset(WinHttpOpenRequest(open.connection.get(), L"GET", target.c_str(), nullptr,
					      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (!open.request)
		return windows_failure("could not open the request", GetLastError());
	if (!WinHttpAddRequestHeaders(open.request.get(), accept, kWholeString, WINHTTP_ADDREQ_FLAG_ADD))
		return windows_failure("could not add the Accept header", GetLastError());
	if (!WinHttpSendRequest(open.request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
		return windows_failure("could not send the request", GetLastError());
	if (!WinHttpReceiveResponse(open.request.get(), nullptr))
		return windows_failure("could not receive the response", GetLastError());

	DWORD status_size = static_cast<DWORD>(sizeof(status));
	if (!WinHttpQueryHeaders(open.request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
		return windows_failure("could not read the status code", GetLastError());
	return HttpResponse{};
}

// Runs one GET and returns its answer. Worker thread only.
HttpResponse fetch(const std::string &url_utf8, const std::string &user_agent)
{
	OpenRequest open;
	DWORD status = 0;
	ULONGLONG started = 0;
	HttpResponse sent = send_get(url_utf8, user_agent, kAcceptHeader, open, status, started);
	if (!sent.error.empty())
		return sent;

	// An HTTP error status still returns its body (GitHub explains a 404 there), so the body is read for every status.
	std::string body;
	for (;;) {
		// Checked before each query and read, so a slow body is cut off at the next one.
		if (deadline_passed(started, kTotalDeadlineMs))
			return failure("the answer took too long");
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(open.request.get(), &available))
			return windows_failure("could not read the answer", GetLastError());
		if (available == 0)
			break;
		// Checked before reading, so the body never grows past the cap.
		if (static_cast<std::size_t>(available) > HttpClient::kMaxBodyBytes - body.size())
			return failure("the answer is too large");
		const DWORD chunk = std::min(available, kReadChunkBytes);
		const std::size_t old_size = body.size();
		body.resize(old_size + chunk);
		DWORD bytes_read = 0;
		if (deadline_passed(started, kTotalDeadlineMs))
			return failure("the answer took too long");
		if (!WinHttpReadData(open.request.get(), &body[old_size], chunk, &bytes_read))
			return windows_failure("could not read the answer", GetLastError());
		body.resize(old_size + bytes_read);
		// Nothing more arrived, so stop even if WinHTTP promised more: a read that returns nothing must not loop.
		if (bytes_read == 0)
			break;
	}

	HttpResponse response;
	response.status = static_cast<int>(status);
	response.body = std::move(body);
	return response;
}

DownloadResult download_failure(const std::string &error)
{
	DownloadResult result;
	result.error = error;
	return result;
}

// A name that stays inside its folder: no separator, drive colon or "..", and not empty.
bool is_bare_file_name(const std::string &name)
{
	return !name.empty() && name.find_first_of("\\/:") == std::string::npos &&
	       name.find("..") == std::string::npos && name.find('\0') == std::string::npos;
}

bool is_sha256_hex(const std::string &hex)
{
	if (hex.size() != 64)
		return false;
	for (const char c : hex) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

bool ends_with(const std::wstring &text, const std::wstring &suffix)
{
	return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Removes what earlier downloads left in `folder`: other installers, and .part files a crash or a shutdown left
// behind. Only those names, so nothing else that someone puts in the folder is touched. Failures are ignored: a file
// still in use is tried again on the next download.
void remove_stale_downloads(const std::filesystem::path &folder, const std::filesystem::path &keep)
{
	std::error_code error;
	for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
		std::error_code entry_error;
		if (!it->is_regular_file(entry_error) || it->path().filename() == keep)
			continue;
		const std::wstring name = it->path().filename().wstring();
		const bool installer = name.rfind(L"clip-atlas-", 0) == 0 && ends_with(name, L"-installer.exe");
		if (installer || ends_with(name, L".part"))
			std::filesystem::remove(it->path(), entry_error);
	}
}

// Whether `path` already holds exactly `size` bytes with SHA-256 `sha256`. Reads the file once, in pieces.
bool file_matches(const std::filesystem::path &path, std::int64_t size, const std::string &sha256)
{
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error) ||
	    std::filesystem::file_size(path, error) != static_cast<std::uintmax_t>(size) || error)
		return false;
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	Sha256 hash;
	std::vector<char> buffer(kReadChunkBytes);
	while (in) {
		in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const std::streamsize got = in.gcount();
		if (got > 0)
			hash.update(buffer.data(), static_cast<std::size_t>(got));
	}
	return in.eof() && hash.hex_digest() == sha256;
}

// Streams the body of the open request into `part`, hashing it on the way. "" once exactly `size` bytes with SHA-256
// `sha256` are on disk; otherwise the reason. The caller removes `part` on failure.
std::string write_body(HINTERNET request, const std::filesystem::path &part, std::int64_t size,
		       const std::string &sha256, ULONGLONG started, const std::atomic<bool> &cancelled)
{
	std::ofstream out(part, std::ios::binary | std::ios::trunc);
	if (!out)
		return "could not create the download file";
	Sha256 hash;
	std::vector<char> buffer(kReadChunkBytes);
	std::int64_t written = 0;
	for (;;) {
		// Checked before each read, so a shutdown or a slow server ends the download at the next one.
		if (cancelled.load())
			return kShuttingDown;
		if (deadline_passed(started, static_cast<ULONGLONG>(HttpClient::kDownloadDeadlineMs)))
			return "the download took too long";
		DWORD bytes_read = 0;
		if (!WinHttpReadData(request, buffer.data(), kReadChunkBytes, &bytes_read))
			return windows_failure("could not read the download", GetLastError()).error;
		if (bytes_read == 0)
			break;
		// Checked before writing, so a server that sends more than promised never fills the disk.
		if (static_cast<std::int64_t>(bytes_read) > size - written)
			return "the download is larger than GitHub said";
		out.write(buffer.data(), static_cast<std::streamsize>(bytes_read));
		if (!out)
			return "could not write the download file";
		hash.update(buffer.data(), bytes_read);
		written += bytes_read;
	}
	out.close();
	if (!out)
		return "could not write the download file";
	if (written != size)
		return "the download ended early";
	if (hash.hex_digest() != sha256)
		return "the download does not match its checksum";
	return "";
}

// Runs one download and returns its result. Worker thread only.
DownloadResult download_file(const DownloadRequest &request, const std::atomic<bool> &cancelled)
{
	if (!is_bare_file_name(request.file_name))
		return download_failure("the file name is not valid");
	if (request.folder_utf8.empty())
		return download_failure("no download folder");
	if (request.size <= 0)
		return download_failure("the download has no size");
	if (!is_sha256_hex(request.sha256))
		return download_failure("the download has no valid checksum");

	const std::filesystem::path folder = path_from_utf8(request.folder_utf8);
	const std::filesystem::path name = path_from_utf8(request.file_name);
	const std::filesystem::path target = folder / name;
	std::filesystem::path part = target;
	part += L".part";
	std::string target_utf8 = request.folder_utf8;
	if (target_utf8.back() != '\\' && target_utf8.back() != '/')
		target_utf8 += '\\';
	target_utf8 += request.file_name;

	std::error_code error;
	std::filesystem::create_directories(folder, error);
	if (error)
		return download_failure("could not create the download folder: " + error.message());
	remove_stale_downloads(folder, name);
	// A file from an earlier session that still checks out is used as it is.
	if (file_matches(target, request.size, request.sha256)) {
		DownloadResult result;
		result.path_utf8 = target_utf8;
		return result;
	}
	std::filesystem::remove(target, error);

	OpenRequest open;
	DWORD status = 0;
	ULONGLONG started = 0;
	const HttpResponse sent =
		send_get(request.url, request.user_agent, kDownloadAcceptHeader, open, status, started);
	if (!sent.error.empty())
		return download_failure(sent.error);
	if (status != 200)
		return download_failure("GitHub answered HTTP " + std::to_string(status));

	const std::string written =
		write_body(open.request.get(), part, request.size, request.sha256, started, cancelled);
	if (!written.empty()) {
		std::filesystem::remove(part, error);
		return download_failure(written);
	}
	// Renamed only now, so a file under the final name has always passed the checks above.
	std::filesystem::rename(part, target, error);
	if (error) {
		std::filesystem::remove(part, error);
		return download_failure("could not finish the download file");
	}
	DownloadResult result;
	result.path_utf8 = target_utf8;
	return result;
}

} // namespace

struct HttpClient::Impl {
	// A get() or a download(): exactly one of `done` and `download_done` is set.
	struct Request {
		std::string url;        // UTF-8; checked by fetch() on the client's thread
		std::string user_agent; // UTF-8
		Done done;
		DownloadRequest download;
		DownloadDone download_done;
	};

	std::mutex mutex;            // guards `pending` and `stopped`
	std::deque<Request> pending; // waiting for the client's thread, oldest first
	bool stopped = false;        // set by shutdown(); nothing is queued after it
	// Also set by shutdown(), and read without the lock by a running download, which stops at its next read.
	std::atomic<bool> cancelled{false};
	// Declared last, so it is destroyed first: its thread can use the members above until it is joined.
	TaskQueue queue;

	// Queues `request` and posts the task that runs it. False once the client is shut down: the caller answers it.
	bool enqueue(Request &request);
	// Runs on the client's thread: takes the oldest queued request and answers it.
	void run_next();
	// Answers `request` with `error`, through whichever callback it has.
	static void refuse(const Request &request, const char *error);
};

void HttpClient::Impl::refuse(const Request &request, const char *error)
{
	if (request.download_done) {
		DownloadResult result;
		result.error = error;
		try {
			request.download_done(result);
		} catch (const std::exception &e) {
			obs_log(LOG_WARNING, "download callback failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_WARNING, "download callback failed: unknown exception");
		}
		return;
	}
	call_done(request.done, failure(error));
}

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient()
{
	shutdown();
}

bool HttpClient::Impl::enqueue(Request &request)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (stopped)
		return false;
	pending.push_back(std::move(request));
	bool queued = false;
	try {
		queued = queue.post([this] { run_next(); });
	} catch (...) {
		// Not queued after all. Taken back, so no request waits for a task that was never posted.
		pending.pop_back();
		throw;
	}
	if (!queued) {
		// The queue refuses only after its shutdown(), which this client calls after setting `stopped` under this
		// lock, so this is not reached in practice. It is kept so that the request is still answered.
		request = std::move(pending.back());
		pending.pop_back();
	}
	return queued;
}

void HttpClient::get(const std::string &url_utf8, const std::string &user_agent, Done done)
{
	Impl::Request request;
	request.url = url_utf8;
	request.user_agent = user_agent;
	request.done = std::move(done);
	// Answered here, on the calling thread, once the lock is released: the client has been shut down.
	if (!impl_->enqueue(request))
		Impl::refuse(request, kShuttingDown);
}

void HttpClient::download(DownloadRequest download, DownloadDone done)
{
	Impl::Request request;
	request.download = std::move(download);
	request.download_done = std::move(done);
	// As in get().
	if (!impl_->enqueue(request))
		Impl::refuse(request, kShuttingDown);
}

void HttpClient::shutdown()
{
	Impl &impl = *impl_;
	std::deque<Impl::Request> dropped;
	{
		std::lock_guard<std::mutex> lock(impl.mutex);
		impl.stopped = true;
		dropped.swap(impl.pending);
	}
	impl.cancelled.store(true);
	// Answered outside the lock, so a callback that calls get() is refused without deadlocking.
	for (const Impl::Request &request : dropped)
		Impl::refuse(request, kShuttingDown);
	// Waits for the request that is running, then stops the thread. Tasks still queued are dropped.
	impl.queue.shutdown();
}

void HttpClient::Impl::run_next()
{
	Request request;
	{
		std::lock_guard<std::mutex> lock(mutex);
		// Empty when shutdown() has already answered the queue, which can happen before the queue itself stops.
		if (pending.empty())
			return;
		request = std::move(pending.front());
		pending.pop_front();
	}
	if (request.download_done) {
		DownloadResult result;
		try {
			result = download_file(request.download, cancelled);
		} catch (const std::exception &e) {
			result = download_failure(std::string("the download failed: ") + e.what());
		} catch (...) {
			result = download_failure("the download failed");
		}
		try {
			request.download_done(result);
		} catch (const std::exception &e) {
			obs_log(LOG_WARNING, "download callback failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_WARNING, "download callback failed: unknown exception");
		}
		return;
	}
	HttpResponse response;
	try {
		response = fetch(request.url, request.user_agent);
	} catch (const std::exception &e) {
		response = failure(std::string("the request failed: ") + e.what());
	} catch (...) {
		response = failure("the request failed");
	}
	call_done(request.done, response);
}

HttpClient &http_client()
{
	// Deliberately never deleted: a destructor would join the thread while Windows unloads the DLL (see http.hpp).
	static HttpClient *client = new HttpClient();
	return *client;
}

} // namespace clip_atlas

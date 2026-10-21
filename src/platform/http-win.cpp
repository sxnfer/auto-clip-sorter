// Clip Atlas — one HTTPS GET at a time with WinHTTP, for the update check.
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

#include "http.hpp"
#include "task-queue.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace clip_atlas {

namespace {

// The address must start with exactly this: the check is not case-insensitive.
constexpr char kHttpsPrefix[] = "https://";
constexpr std::size_t kHttpsPrefixLength = sizeof(kHttpsPrefix) - 1;

// GitHub's REST API asks for this media type.
constexpr wchar_t kAcceptHeader[] = L"Accept: application/vnd.github+json";

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
	~WinHttpHandle()
	{
		if (handle_)
			WinHttpCloseHandle(handle_);
	}

	WinHttpHandle(const WinHttpHandle &) = delete;
	WinHttpHandle &operator=(const WinHttpHandle &) = delete;

	HINTERNET get() const { return handle_; }
	explicit operator bool() const { return handle_ != nullptr; }

private:
	HINTERNET handle_;
};

// True once kTotalDeadlineMs has passed since `started`. GetTickCount64 only moves forward, so the difference is never
// negative.
bool deadline_passed(ULONGLONG started)
{
	return GetTickCount64() - started >= kTotalDeadlineMs;
}

// Runs one GET and returns its answer. Worker thread only.
HttpResponse fetch(const std::string &url_utf8, const std::string &user_agent)
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
	const ULONGLONG started = GetTickCount64();

	// Declared in the order they are opened, so they close in reverse: request, connection, session.
	WinHttpHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					  WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session)
		return windows_failure("could not open a WinHTTP session", GetLastError());
	if (!WinHttpSetTimeouts(session.get(), kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs))
		return windows_failure("could not set the timeouts", GetLastError());
	WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
	if (!connection)
		return windows_failure("could not connect to the server", GetLastError());
	WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
						 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (!request)
		return windows_failure("could not open the request", GetLastError());
	if (!WinHttpAddRequestHeaders(request.get(), kAcceptHeader, kWholeString, WINHTTP_ADDREQ_FLAG_ADD))
		return windows_failure("could not add the Accept header", GetLastError());
	if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
		return windows_failure("could not send the request", GetLastError());
	if (!WinHttpReceiveResponse(request.get(), nullptr))
		return windows_failure("could not receive the response", GetLastError());

	DWORD status = 0;
	DWORD status_size = static_cast<DWORD>(sizeof(status));
	if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
		return windows_failure("could not read the status code", GetLastError());

	// An HTTP error status still returns its body (GitHub explains a 404 there), so the body is read for every status.
	std::string body;
	for (;;) {
		// Checked before each query and read, so a slow body is cut off at the next one.
		if (deadline_passed(started))
			return failure("the answer took too long");
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request.get(), &available))
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
		if (deadline_passed(started))
			return failure("the answer took too long");
		if (!WinHttpReadData(request.get(), &body[old_size], chunk, &bytes_read))
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

} // namespace

struct HttpClient::Impl {
	struct Request {
		std::string url;        // UTF-8; checked by fetch() on the client's thread
		std::string user_agent; // UTF-8
		Done done;
	};

	std::mutex mutex;            // guards `pending` and `stopped`
	std::deque<Request> pending; // waiting for the client's thread, oldest first
	bool stopped = false;        // set by shutdown(); nothing is queued after it
	// Declared last, so it is destroyed first: its thread can use the members above until it is joined.
	TaskQueue queue;

	// Runs on the client's thread: takes the oldest queued request and answers it.
	void run_next();
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient()
{
	shutdown();
}

void HttpClient::get(const std::string &url_utf8, const std::string &user_agent, Done done)
{
	Impl &impl = *impl_;
	Impl::Request request{url_utf8, user_agent, std::move(done)};
	bool queued = false;
	{
		std::lock_guard<std::mutex> lock(impl.mutex);
		if (!impl.stopped) {
			impl.pending.push_back(std::move(request));
			try {
				queued = impl.queue.post([&impl] { impl.run_next(); });
			} catch (...) {
				// Not queued after all. Taken back, so no request waits for a task that was never posted.
				impl.pending.pop_back();
				throw;
			}
			if (!queued) {
				// The queue refuses only after its shutdown(), which this client calls after setting `stopped` under this
				// lock, so this is not reached in practice. It is kept so that the request is still answered.
				request = std::move(impl.pending.back());
				impl.pending.pop_back();
			}
		}
	}
	// Answered here, on the calling thread, once the lock is released: the client has been shut down.
	if (!queued)
		call_done(request.done, failure(kShuttingDown));
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
	// Answered outside the lock, so a callback that calls get() is refused without deadlocking.
	for (Impl::Request &request : dropped)
		call_done(request.done, failure(kShuttingDown));
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

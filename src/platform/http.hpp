// Clip Atlas — one HTTPS GET at a time, for the update check.
//
// Windows implementation (http-win.cpp) with WinHTTP (winhttp.h, links
// winhttp), on the client's own TaskQueue thread, synchronous calls there:
//   session = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
//                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
//     (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+, which OBS 31
//     requires anyway: it follows the system and WPAD proxy settings)
//   WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000)
//   WinHttpCrackUrl → host, port, path + query; the scheme must be HTTPS
//   connect = WinHttpConnect(session, host, port, 0)
//   request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
//              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
//              WINHTTP_FLAG_SECURE)
//   WinHttpAddRequestHeaders(request, L"Accept: application/vnd.github+json",
//              (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)
//   WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
//              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
//   WinHttpReceiveResponse(request, nullptr)
//   WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE |
//              WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
//              &status, &size, WINHTTP_NO_HEADER_INDEX)
//   WinHttpQueryDataAvailable / WinHttpReadData until 0 bytes
//   WinHttpCloseHandle on request, connect and session (every path).
// TLS certificate checks stay at WinHTTP's defaults (never disabled).
// Redirects follow WinHTTP's default policy, which never goes from HTTPS to
// HTTP. The body is capped at kMaxBodyBytes: a longer answer is an error.
//
// The stub (http-stub.cpp) answers every get() with the error "not supported
// on this platform".
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace clip_atlas {

struct HttpResponse {
	// English reason when there was no complete answer (bad URL, no network,
	// TLS failure, timeout, body too large, shut down); "" otherwise, even for
	// an HTTP error status.
	std::string error;
	int status = 0;   // the HTTP status code; 0 when `error` is set
	std::string body; // the raw body bytes (UTF-8 for GitHub's JSON)
};

class HttpClient {
public:
	static constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

	// Called once per get(), on the client's thread (never inside get()), or
	// right away on the calling thread after shutdown() with error "Clip Atlas
	// is shutting down". Must not throw (it is caught and dropped). Hop to the
	// UI thread before touching plugin state.
	using Done = std::function<void(const HttpResponse &response)>;

	HttpClient();
	// Calls shutdown().
	~HttpClient();

	HttpClient(const HttpClient &) = delete;
	HttpClient &operator=(const HttpClient &) = delete;

	// Fetches `url_utf8`, which must start with "https://" (anything else is an
	// error, without any network access). `user_agent` (UTF-8) is required by
	// GitHub, e.g. "clip-atlas/0.2.0". Requests run one at a time, in order.
	// Safe from any thread.
	void get(const std::string &url_utf8, const std::string &user_agent, Done done);

	// Drops queued requests (their `done` gets "Clip Atlas is shutting down"),
	// waits for the one running (at most the timeouts above), and stops the
	// thread. Idempotent. Call it from stop_all() at EXIT.
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The plugin's single client, created on first use and never destroyed (as
// sound_player()). Defined in both http-win.cpp and http-stub.cpp.
HttpClient &http_client();

} // namespace clip_atlas

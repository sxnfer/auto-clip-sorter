// Clip Atlas — one HTTPS GET at a time, for the update check and the
// installer download.
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
// download() runs the same GET with "Accept: application/octet-stream" and
// writes the body to a file instead of memory. GitHub answers a release
// download with a redirect to its file host, which WinHTTP follows (HTTPS to
// HTTPS). The body is hashed with core's Sha256 as it arrives, so the file is
// written once and never read back. It is written as "<file>.part", and only
// renamed to its final name once its size and SHA-256 match; on any failure
// the .part file is removed. The whole download may take kDownloadDeadlineMs,
// and shutdown() stops one in progress at its next read.
//
// The stub (http-stub.cpp) answers every get() and download() with the error
// "not supported on this platform".
#pragma once

#include <cstddef>
#include <cstdint>
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

// A file to download and check.
struct DownloadRequest {
	std::string url;        // UTF-8; must start with "https://"
	std::string user_agent; // UTF-8, as for get()
	// UTF-8 path of the folder the file goes in, created if missing. The folder
	// belongs to the download: before it starts, the other files in it named
	// "clip-atlas-*-installer.exe" or "*.part" (earlier downloads) are removed.
	std::string folder_utf8;
	std::string file_name; // UTF-8, a bare file name (no "\", "/", ":" or "..")
	std::int64_t size = 0; // the exact size in bytes; more or fewer is an error
	std::string sha256;    // 64 lowercase hex digits; a mismatch is an error
};

struct DownloadResult {
	// English reason when there is no checked file (bad request, no network,
	// HTTP error status, wrong size or checksum, disk error, shut down); ""
	// otherwise.
	std::string error;
	// UTF-8 path of the checked file ("<folder>\<file name>"); "" when `error`
	// is set.
	std::string path_utf8;
};

class HttpClient {
public:
	static constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
	// How long a whole download may take, from the request to the last byte.
	static constexpr int kDownloadDeadlineMs = 10 * 60 * 1000;

	// Called once per get(), on the client's thread (never inside get()), or
	// right away on the calling thread after shutdown() with error "Clip Atlas
	// is shutting down". Must not throw (it is caught and dropped). Hop to the
	// UI thread before touching plugin state.
	using Done = std::function<void(const HttpResponse &response)>;
	// As Done, for download().
	using DownloadDone = std::function<void(const DownloadResult &result)>;

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

	// Downloads `request.url` into `request.folder_utf8` and checks it. A file
	// of that name already there with the right size and SHA-256 is kept and
	// answered at once, without the network. Queued with get()'s requests, in
	// order. Safe from any thread.
	void download(DownloadRequest request, DownloadDone done);

	// Drops queued requests (their `done` gets "Clip Atlas is shutting down"),
	// stops a running download at its next read, waits for the request
	// running (at most the timeouts above), and stops the thread. Idempotent. Call it from stop_all() at EXIT.
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The plugin's single client, created on first use and never destroyed (as
// sound_player()). Defined in both http-win.cpp and http-stub.cpp.
HttpClient &http_client();

} // namespace clip_atlas

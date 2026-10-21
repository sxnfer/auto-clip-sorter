// Clip Atlas — the update check's HTTP client on platforms without WinHTTP.
//
// Nothing is fetched here. get() answers through `done` at once, on the calling thread, so the plugin's log explains why
// no update check ran. CMake compiles this file instead of http-win.cpp on every platform but Windows.
#include "http.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <exception>
#include <memory>
#include <string>

namespace clip_atlas {

struct HttpClient::Impl {};

namespace {

// Same as the Windows version: an exception from `done` is logged, not thrown.
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

} // namespace

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient()
{
	shutdown();
}

void HttpClient::get(const std::string &, const std::string &, Done done)
{
	HttpResponse response;
	response.error = "not supported on this platform";
	call_done(done, response);
}

void HttpClient::shutdown()
{
	// Nothing is queued or started here, so there is nothing to stop.
}

HttpClient &http_client()
{
	// Deliberately never deleted, as in http-win.cpp.
	static HttpClient *client = new HttpClient();
	return *client;
}

} // namespace clip_atlas

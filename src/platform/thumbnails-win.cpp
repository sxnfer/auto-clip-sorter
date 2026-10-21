// Clip Atlas — makes the dock's preview images with the Windows Shell.
//
// Images are made on the provider's own thread, with COM set up as a single-threaded apartment, because the Shell's
// thumbnail handlers are written for that model. Nothing here touches Qt: a finished image goes to `done`, and the dock
// hops to the UI thread itself.
//
// The queue is newest first. The dock asks for the rows on screen, so the newest request is the one the user is
// waiting for. Until shutdown(), every request is answered exactly once (made, dropped or cancelled). No `done` runs
// inside request() or while the mutex is held, so a callback may call back into the provider.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>

#include "clip-layout.hpp"
#include "task-queue.hpp"
#include "thumbnails.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace clip_atlas {

namespace {

// request() clamps to these, so no caller can ask for an image the queue or the Shell would struggle with.
constexpr int kMinSizePx = 16;
constexpr int kMaxSizePx = 1024;
// Past this many waiting requests the oldest is dropped, so a long scroll cannot queue work the user has already
// scrolled away from.
constexpr std::size_t kMaxWaiting = 256;
// The Shell returns an image no larger than the size asked for. A bigger bitmap is not one this code asked for.
constexpr long long kMaxImagePx = 1024;

// Calls `done`. An exception from it is logged, not thrown: this runs on the provider's thread, where nothing above
// the call could catch it.
void call_done(const ThumbnailProvider::Done &done, const std::string &path_utf8, ThumbnailStatus status,
	       Thumbnail thumbnail)
{
	if (!done)
		return;
	try {
		done(path_utf8, status, std::move(thumbnail));
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "thumbnail callback failed for %s: %s", path_utf8.c_str(), e.what());
	} catch (...) {
		obs_log(LOG_WARNING, "thumbnail callback failed for %s: unknown exception", path_utf8.c_str());
	}
}

// Releases a COM interface on every path out of make_thumbnail(), so no early return can leak it.
class ComReleaseGuard {
public:
	explicit ComReleaseGuard(IUnknown *object) : object_(object) {}
	~ComReleaseGuard()
	{
		if (object_)
			object_->Release();
	}
	ComReleaseGuard(const ComReleaseGuard &) = delete;
	ComReleaseGuard &operator=(const ComReleaseGuard &) = delete;

private:
	IUnknown *object_;
};

// Deletes a bitmap on every path out of make_thumbnail(). GetImage() hands the bitmap to the caller, so it is ours to
// delete whether or not it turns out to be usable.
class GdiObjectGuard {
public:
	explicit GdiObjectGuard(HGDIOBJ object) : object_(object) {}
	~GdiObjectGuard()
	{
		if (object_)
			DeleteObject(object_);
	}
	GdiObjectGuard(const GdiObjectGuard &) = delete;
	GdiObjectGuard &operator=(const GdiObjectGuard &) = delete;

private:
	HGDIOBJ object_;
};

// Makes the thumbnail of one file, at most size_px on each side. Runs on the provider's thread, where COM is
// initialised. Returns an empty Thumbnail when Windows has no usable image for the file.
Thumbnail make_thumbnail(const std::string &path_utf8, int size_px)
{
	try {
		// The Shell reads a path up to its first NUL, so an embedded NUL could open a different file.
		if (path_utf8.empty() || path_utf8.find('\0') != std::string::npos)
			return {};
		const std::wstring wide = path_from_utf8(path_utf8).wstring();

		IShellItemImageFactory *factory = nullptr;
		const HRESULT created = SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&factory));
		ComReleaseGuard factory_guard(factory);
		if (FAILED(created) || !factory)
			return {};

		HBITMAP bitmap = nullptr;
		const HRESULT got = factory->GetImage(SIZE{size_px, size_px}, SIIGBF_RESIZETOFIT, &bitmap);
		GdiObjectGuard bitmap_guard(bitmap);
		if (FAILED(got) || !bitmap)
			return {};

		BITMAP info{};
		if (GetObjectW(bitmap, static_cast<int>(sizeof(info)), &info) == 0)
			return {};
		// The sign of the height says which way up the rows are, not how big the image is. The sizes are measured in
		// 64 bits so that negating the height cannot overflow.
		long long width = info.bmWidth;
		long long height = info.bmHeight;
		if (height < 0)
			height = -height;
		if (width <= 0 || height <= 0 || width > kMaxImagePx || height > kMaxImagePx)
			return {};
		const int w = static_cast<int>(width);
		const int h = static_cast<int>(height);
		const auto byte_count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;

		// 32 bpp with top-down rows, the layout Thumbnail::bgra describes, so no row has to be flipped afterwards.
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = static_cast<DWORD>(sizeof(BITMAPINFOHEADER));
		bi.bmiHeader.biWidth = w;
		bi.bmiHeader.biHeight = -h;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;

		Thumbnail thumbnail;
		thumbnail.width = w;
		thumbnail.height = h;
		thumbnail.bgra.resize(byte_count);
		HDC screen = GetDC(nullptr);
		if (!screen)
			return {};
		const int lines =
			GetDIBits(screen, bitmap, 0u, static_cast<UINT>(h), thumbnail.bgra.data(), &bi, DIB_RGB_COLORS);
		ReleaseDC(nullptr, screen);
		if (lines != h)
			return {};

		// A bitmap without an alpha channel comes back with every alpha byte 0, which the dock would draw as
		// transparent. Those pixels are opaque, so the bytes are filled in. A bitmap with any alpha is left alone.
		bool has_alpha = false;
		for (std::size_t i = 3; i < byte_count && !has_alpha; i += 4)
			has_alpha = thumbnail.bgra[i] != 0;
		if (!has_alpha) {
			for (std::size_t i = 3; i < byte_count; i += 4)
				thumbnail.bgra[i] = 255;
		}
		return thumbnail;
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "thumbnail for %s failed: %s", path_utf8.c_str(), e.what());
	} catch (...) {
		obs_log(LOG_WARNING, "thumbnail for %s failed: unknown exception", path_utf8.c_str());
	}
	return {};
}

} // namespace

struct ThumbnailProvider::Impl {
	struct Request {
		std::string path; // UTF-8, byte for byte as the caller passed it
		int size_px = 0;
		ThumbnailProvider::Done done;
	};
	// Held by pointer so that a moved-from slot is empty. The queue then never destroys a request, and so never runs a
	// caller's captures, while the mutex is held.
	using RequestPtr = std::unique_ptr<Request>;

	// Set and read only on the provider's thread, by the start and exit hooks.
	bool com_initialized = false;

	std::mutex mutex;
	std::deque<RequestPtr> waiting; // newest at the back: the next one made
	// Requests dropped by replacement, overflow or cancel_pending(). Their `done` calls are still owed.
	std::deque<RequestPtr> dropped;
	// Serve tasks posted that have not started. Each one can make one waiting request.
	std::size_t serves_pending = 0;
	// A flush task is posted and has not started. One at a time is enough: it answers everything dropped so far.
	bool flush_pending = false;
	bool stopped = false;

	// Declared last, so it is destroyed first: the thread has stopped before the members it uses go.
	TaskQueue tasks;

	Impl();

	void start_com();
	void stop_com();
	void post_serve();
	void post_flush();
	void serve_one();
	void flush_dropped();
	// Answers requests that will not be made: each `done` is called once, with dropped. The caller owns the list and
	// destroys it outside the lock. A static member rather than a free function because Impl is private to
	// ThumbnailProvider, and code outside the class cannot name its types.
	static void answer_dropped(const std::deque<RequestPtr> &answered);
};

ThumbnailProvider::Impl::Impl() : tasks([this] { start_com(); }, [this] { stop_com(); }) {}

void ThumbnailProvider::Impl::answer_dropped(const std::deque<RequestPtr> &answered)
{
	for (const RequestPtr &request : answered)
		call_done(request->done, request->path, ThumbnailStatus::dropped, Thumbnail{});
}

void ThumbnailProvider::Impl::start_com()
{
	// The Shell's handlers expect a single-threaded apartment. S_FALSE means COM was already up on this thread, which
	// still needs its matching CoUninitialize(). RPC_E_CHANGED_MODE means the thread was set up in another model, which
	// must not be undone, so it is not counted as ours.
	const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	com_initialized = SUCCEEDED(hr);
	if (FAILED(hr))
		obs_log(LOG_WARNING, "thumbnails: COM did not start on the thumbnail thread (0x%08lx)",
			static_cast<unsigned long>(hr));
}

void ThumbnailProvider::Impl::stop_com()
{
	if (com_initialized)
		CoUninitialize();
}

void ThumbnailProvider::Impl::post_serve()
{
	// post() fails only after shutdown(), when nothing is made any more.
	static_cast<void>(tasks.post([this] { serve_one(); }));
}

void ThumbnailProvider::Impl::post_flush()
{
	static_cast<void>(tasks.post([this] { flush_dropped(); }));
}

void ThumbnailProvider::Impl::serve_one()
{
	std::deque<RequestPtr> owed;
	RequestPtr next;
	{
		std::lock_guard<std::mutex> lock(mutex);
		--serves_pending;
		// Answering the dropped requests here, before the next image is made, means a caller hears about a drop
		// right after the image in progress, not after every serve task queued ahead of the flush task.
		owed.swap(dropped);
		if (!waiting.empty()) {
			next = std::move(waiting.back());
			waiting.pop_back();
		}
	}
	answer_dropped(owed);
	if (!next)
		return;
	Thumbnail thumbnail = make_thumbnail(next->path, next->size_px);
	const ThumbnailStatus status = thumbnail.empty() ? ThumbnailStatus::failed : ThumbnailStatus::ok;
	call_done(next->done, next->path, status, std::move(thumbnail));
}

void ThumbnailProvider::Impl::flush_dropped()
{
	std::deque<RequestPtr> owed;
	{
		std::lock_guard<std::mutex> lock(mutex);
		flush_pending = false;
		owed.swap(dropped);
	}
	answer_dropped(owed);
}

ThumbnailProvider::ThumbnailProvider() : impl_(std::make_unique<Impl>()) {}

ThumbnailProvider::~ThumbnailProvider()
{
	shutdown();
}

void ThumbnailProvider::request(const std::string &path_utf8, int size_px, Done done)
{
	Impl &impl = *impl_;
	// Built before the lock, so a refused request's `done` is destroyed outside it.
	Impl::RequestPtr incoming = std::make_unique<Impl::Request>();
	incoming->path = path_utf8;
	incoming->size_px = std::clamp(size_px, kMinSizePx, kMaxSizePx);
	incoming->done = std::move(done);

	bool post_serve_task = false;
	bool post_flush_task = false;
	{
		std::lock_guard<std::mutex> lock(impl.mutex);
		if (impl.stopped)
			return;
		// The newest request for a path replaces the one still waiting. The dock may ask on every paint, so without
		// this the queue would hold the same image many times over.
		auto same = std::find_if(impl.waiting.begin(), impl.waiting.end(),
					 [&path_utf8](const Impl::RequestPtr &pending) {
						 return pending->path == path_utf8;
					 });
		if (same != impl.waiting.end()) {
			impl.dropped.push_back(std::move(*same));
			impl.waiting.erase(same);
		}
		impl.waiting.push_back(std::move(incoming));
		while (impl.waiting.size() > kMaxWaiting) {
			impl.dropped.push_back(std::move(impl.waiting.front()));
			impl.waiting.pop_front();
		}
		// A serve task that has not started will find this request, so another one is needed only when fewer serve
		// tasks wait than requests do. Posting one per request would pile up tasks without limit while the dock asks
		// on every paint.
		if (impl.serves_pending < impl.waiting.size()) {
			++impl.serves_pending;
			post_serve_task = true;
		}
		if (!impl.dropped.empty() && !impl.flush_pending) {
			impl.flush_pending = true;
			post_flush_task = true;
		}
	}
	if (post_flush_task)
		impl.post_flush();
	if (post_serve_task)
		impl.post_serve();
}

void ThumbnailProvider::cancel_pending()
{
	Impl &impl = *impl_;
	bool post_flush_task = false;
	{
		std::lock_guard<std::mutex> lock(impl.mutex);
		while (!impl.waiting.empty()) {
			impl.dropped.push_back(std::move(impl.waiting.front()));
			impl.waiting.pop_front();
		}
		if (!impl.dropped.empty() && !impl.flush_pending) {
			impl.flush_pending = true;
			post_flush_task = true;
		}
	}
	if (post_flush_task)
		impl.post_flush();
}

void ThumbnailProvider::shutdown()
{
	Impl &impl = *impl_;
	std::deque<Impl::RequestPtr> discarded_waiting;
	std::deque<Impl::RequestPtr> discarded_dropped;
	{
		std::lock_guard<std::mutex> lock(impl.mutex);
		impl.stopped = true;
		discarded_waiting.swap(impl.waiting);
		discarded_dropped.swap(impl.dropped);
	}
	// Destroyed outside the lock: a captured object's destructor may call back into this provider. None of these
	// `done` calls is made, because shutdown() drops what is queued.
	discarded_waiting.clear();
	discarded_dropped.clear();
	// Waits for the image being made, whose `done` is still called, and then stops the thread.
	impl.tasks.shutdown();
}

ThumbnailProvider &thumbnail_provider()
{
	// Deliberately never deleted: a destructor would join the thread while Windows unloads the DLL, which can deadlock
	// (see thumbnails.hpp).
	static ThumbnailProvider *provider = new ThumbnailProvider;
	return *provider;
}

} // namespace clip_atlas

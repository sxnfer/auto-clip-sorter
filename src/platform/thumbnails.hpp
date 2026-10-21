// Clip Atlas — preview images for the dock's clip list, from Windows' own
// thumbnail cache (the image Explorer shows for a video or picture).
//
// Windows implementation (thumbnails-win.cpp), on the provider's own
// TaskQueue thread with COM initialised as a single-threaded apartment
// (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)
// in the start hook, CoUninitialize() in the exit hook):
//   1. SHCreateItemFromParsingName(wide path, nullptr, IID_PPV_ARGS(&item))
//      gives an IShellItemImageFactory. Include <shobjidl.h> (not
//      <shobjidl_core.h>, which MinGW lacks; the SDK's shobjidl.h includes it).
//   2. item->GetImage(SIZE{size_px, size_px}, SIIGBF_RESIZETOFIT, &hbitmap)
//      returns the thumbnail, or the file type's icon when Windows has no
//      thumbnail. The bitmap is the caller's: DeleteObject() it.
//   3. GetObjectW() + GetDIBits() into a 32 bpp BI_RGB top-down
//      (negative biHeight) buffer gives premultiplied BGRA. If every alpha
//      byte is 0 (a bitmap without alpha), every alpha byte is set to 255.
// Paths are converted with path_from_utf8(), then .wstring() (UTF-16).
//
// The stub (thumbnails-stub.cpp) accepts requests and never calls `done`, so
// the dock shows its placeholder on other platforms.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace clip_atlas {

struct Thumbnail {
	int width = 0;
	int height = 0;
	// width * height * 4 bytes, rows top to bottom, each pixel B, G, R, A with
	// premultiplied alpha (QImage::Format_ARGB32_Premultiplied's memory layout
	// on little-endian Windows).
	std::vector<std::uint8_t> bgra;

	bool empty() const { return width <= 0 || height <= 0 || bgra.empty(); }
};

enum class ThumbnailStatus {
	ok,      // `thumbnail` holds the image
	failed,  // Windows has no image for the file (missing, unreadable, error)
	dropped, // the request was dropped unanswered (cancel_pending(), or the
		 // queue overflowed); ask again if it is still wanted
};

class ThumbnailProvider {
public:
	// Called once per request, on the provider's thread, never inside
	// request(). `path_utf8` is the requested path, byte for byte. Must not
	// throw (an exception is caught and dropped) and must hop to the UI thread
	// before touching widgets.
	using Done = std::function<void(const std::string &path_utf8, ThumbnailStatus status, Thumbnail thumbnail)>;

	ThumbnailProvider();
	// Calls shutdown().
	~ThumbnailProvider();

	ThumbnailProvider(const ThumbnailProvider &) = delete;
	ThumbnailProvider &operator=(const ThumbnailProvider &) = delete;

	// Asks for a thumbnail no larger than size_px on either side (clamped to
	// 16-1024). Newest first: the request most recently made is the next to be
	// served, because the dock asks for the rows the user is looking at. A
	// queued request for the same path is replaced: the older one's `done` is
	// called with dropped. At most 256 requests wait; beyond that the oldest is
	// dropped (its `done` gets dropped). Safe from any thread. After shutdown()
	// it does nothing and never calls `done`.
	void request(const std::string &path_utf8, int size_px, Done done);

	// Drops every queued request (each `done` gets dropped); one being made
	// finishes and is answered. Safe from any thread.
	void cancel_pending();

	// Drops queued requests without calling their `done`, waits for the one
	// being made (its `done` is still called), and stops the thread.
	// Idempotent. Call it from stop_all() at EXIT, never from a static
	// destructor (joining a thread while Windows unloads the DLL can deadlock).
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The plugin's single provider, created on first use and never destroyed (a
// heap object deliberately not deleted, like sound_player()). Defined in both
// thumbnails-win.cpp and thumbnails-stub.cpp.
ThumbnailProvider &thumbnail_provider();

} // namespace clip_atlas

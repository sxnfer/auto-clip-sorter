// Clip Atlas — the dock's preview images on platforms without the Windows Shell.
//
// Nothing is made here. request() takes the request and never calls its `done`, so the dock keeps its placeholder.
// CMake compiles this file instead of thumbnails-win.cpp on every platform but Windows.
#include "thumbnails.hpp"

#include <memory>

namespace clip_atlas {

struct ThumbnailProvider::Impl {};

ThumbnailProvider::ThumbnailProvider() : impl_(std::make_unique<Impl>()) {}

ThumbnailProvider::~ThumbnailProvider()
{
	shutdown();
}

// Nothing is queued here, so there is nothing to answer.
void ThumbnailProvider::request(const std::string &, int, Done) {}

void ThumbnailProvider::cancel_pending() {}

void ThumbnailProvider::shutdown() {}

ThumbnailProvider &thumbnail_provider()
{
	// Deliberately never deleted, as in sound-player-stub.cpp.
	static ThumbnailProvider *provider = new ThumbnailProvider;
	return *provider;
}

} // namespace clip_atlas

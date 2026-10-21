// Clip Atlas — the save sound on platforms without Windows audio.
//
// Nothing plays here. play() says so through `done`, so the plugin's log explains why the sound is silent. CMake
// compiles this file instead of sound-player-win.cpp on every platform but Windows.
#include "sound-player.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <exception>
#include <memory>
#include <string>

namespace clip_atlas {

struct SoundPlayer::Impl {};

namespace {

// Same as the Windows version: an exception from `done` is logged, not thrown.
void call_done(const SoundPlayer::Done &done, const std::string &error)
{
	if (!done)
		return;
	try {
		done(error);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "save sound callback failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "save sound callback failed: unknown exception");
	}
}

} // namespace

SoundPlayer::SoundPlayer() : impl_(std::make_unique<Impl>()) {}

SoundPlayer::~SoundPlayer()
{
	shutdown();
}

void SoundPlayer::play(const std::string &, int, Done done)
{
	call_done(done, "sound playback is only available on Windows");
}

void SoundPlayer::shutdown()
{
	// Nothing is queued or started here, so there is nothing to stop.
}

SoundPlayer &sound_player()
{
	// Deliberately never deleted, as in sound-player-win.cpp.
	static SoundPlayer *player = new SoundPlayer;
	return *player;
}

} // namespace clip_atlas

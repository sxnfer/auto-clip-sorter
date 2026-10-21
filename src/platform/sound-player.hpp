// Clip Atlas — plays the optional save sound through Windows.
//
// The sound goes straight to Windows' default playback device with winmm's
// PlaySoundW, not through an OBS source, so nothing is added to the user's
// scenes. The catch, which the settings help text states: Desktop Audio
// captures the default device, so the sound can be heard on stream and in
// later recordings and replays.
//
// One background thread reads the file and scales its volume
// (core/wav-volume), so the UI thread never does file I/O. Playback is
// asynchronous: PlaySoundW(image, nullptr, SND_MEMORY | SND_ASYNC |
// SND_NODEFAULT) with the scaled file image, which must stay alive until the
// sound ends or is replaced. A new play() stops a sound that is still playing
// (PlaySoundW(nullptr, nullptr, 0)) before its image is freed.
//
// Win32 implementation in sound-player-win.cpp (links winmm); the stub in
// sound-player-stub.cpp plays nothing so non-Windows builds compile.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace clip_atlas {

class SoundPlayer {
public:
	// Called once per play() with "" on success or an English reason (file
	// missing or unreadable, not a usable .wav, PlaySoundW failed, the player
	// was shut down, or not supported on this platform). Runs on the player's
	// thread, or on the calling thread when play() can answer at once (after
	// shutdown(), and always in the stub). Hop to the UI thread before touching
	// plugin state or widgets. Must not throw; an exception is caught and logged.
	using Done = std::function<void(const std::string &error)>;

	SoundPlayer();
	// Calls shutdown().
	~SoundPlayer();

	SoundPlayer(const SoundPlayer &) = delete;
	SoundPlayer &operator=(const SoundPlayer &) = delete;

	// Queues `path_utf8` (a .wav, UTF-8, converted with path_from_utf8) to
	// play at `volume_percent` (clamped to 0-100, see volume_gain()). Only the
	// newest request waits: a request still queued when another arrives is
	// dropped and its `done` gets "replaced by a newer sound". Safe from any
	// thread. Returns at once.
	void play(const std::string &path_utf8, int volume_percent, Done done = {});

	// Stops any sound that is playing, drops queued requests (their `done`
	// gets an error), and joins the thread. Idempotent. Call it from
	// stop_all() at EXIT, never from a static destructor: joining a thread
	// while Windows unloads the DLL can deadlock.
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The plugin's single player, created on first use and never destroyed (a
// heap object that is deliberately not deleted), so no destructor runs while
// Windows unloads the DLL. Defined in both sound-player-win.cpp and
// sound-player-stub.cpp.
SoundPlayer &sound_player();

} // namespace clip_atlas

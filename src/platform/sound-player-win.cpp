// Clip Atlas — plays the optional save sound through Windows' winmm.
//
// PlaySoundW has no volume parameter, so the file is read into memory and its
// samples are scaled (core/wav-volume). The scaled image is then played with
// SND_MEMORY. Windows reads that image while the sound plays, so it is kept
// here until the next sound replaces it or shutdown() stops playback.
//
// A worker thread does the file I/O, so the caller never waits on the disk.
// Only the newest request waits: a sound still queued when another arrives is
// dropped with "replaced by a newer sound".
//
// Built on Windows only (CMakeLists.txt links winmm). The stub for other
// platforms is sound-player-stub.cpp.
// Guarded, so a build that already defines them (MSVC warns C4005 on a redefinition, and warnings are errors) still
// compiles.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include "clip-layout.hpp"
#include "sound-player.hpp"
#include "wav-volume.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace {

// Calls `done` with `error`. An exception from it is logged rather than thrown, because this runs on the player's
// thread, or on the caller's thread for a request that is refused at once.
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

// Reads the whole file into `bytes`. The size is checked before the file is read, so a wrong file choice never loads
// a huge file into memory. Returns "" or the reason, in English because it goes to the log.
std::string read_wav_file(const std::string &path_utf8, std::vector<std::uint8_t> &bytes)
{
	const std::filesystem::path path = path_from_utf8(path_utf8);
	const std::string prefix = "could not read " + path_utf8 + ": ";
	std::error_code ec;
	const std::uintmax_t size = std::filesystem::file_size(path, ec);
	if (ec)
		return prefix + "the file was not found or cannot be read";
	if (size > kMaxSaveSoundBytes)
		return prefix + "the file is larger than the save sound limit";
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return prefix + "the file cannot be opened";
	bytes.resize(static_cast<std::size_t>(size));
	in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (in.gcount() != static_cast<std::streamsize>(bytes.size()))
		return prefix + "the file cannot be read in full";
	return "";
}

} // namespace

struct SoundPlayer::Impl {
	struct Request {
		std::string path; // UTF-8
		int volume = 0;   // percent
		Done done;
	};

	std::mutex mutex;
	std::condition_variable wake;
	std::optional<Request> pending; // the one request waiting for the worker
	bool stopping = false;          // set by shutdown(); nothing is started after it
	std::thread worker;             // started by the first play(); shutdown() moves it out to join it
	// The image PlaySoundW plays from. Only the worker replaces it, and shutdown() frees it after the join.
	std::vector<std::uint8_t> playing;

	void run();
	std::string play_request(const Request &request);
};

SoundPlayer::SoundPlayer() : impl_(std::make_unique<Impl>()) {}

SoundPlayer::~SoundPlayer()
{
	shutdown();
}

void SoundPlayer::play(const std::string &path_utf8, int volume_percent, Done done)
{
	Impl &impl = *impl_;
	Impl::Request request{path_utf8, volume_percent, std::move(done)};
	std::optional<Impl::Request> replaced;
	std::string refusal; // set when this request can't be queued; its `done` then gets it
	{
		std::lock_guard<std::mutex> lock(impl.mutex);
		if (impl.stopping) {
			refusal = "the sound player was shut down";
		} else {
			replaced = std::move(impl.pending);
			impl.pending = std::move(request);
			try {
				if (!impl.worker.joinable())
					impl.worker = std::thread([&impl] { impl.run(); });
			} catch (const std::exception &) {
				// Without a thread nothing would take this request, so it is taken back and refused.
				request = std::move(*impl.pending);
				impl.pending.reset();
				refusal = "could not start the sound thread";
			}
		}
	}
	impl.wake.notify_one();
	if (replaced)
		call_done(replaced->done, "replaced by a newer sound");
	if (!refusal.empty())
		call_done(request.done, refusal);
}

void SoundPlayer::shutdown()
{
	Impl &impl = *impl_;
	std::optional<Impl::Request> dropped;
	std::thread worker;
	{
		std::lock_guard<std::mutex> lock(impl.mutex);
		impl.stopping = true;
		dropped = std::move(impl.pending);
		impl.pending.reset();
		worker = std::move(impl.worker);
	}
	impl.wake.notify_all();
	if (dropped)
		call_done(dropped->done, "the sound player was shut down");
	if (worker.joinable()) {
		// A done callback running on the worker may call shutdown() itself, and a thread can't join itself.
		if (worker.get_id() == std::this_thread::get_id())
			worker.detach();
		else
			worker.join();
	}
	PlaySoundW(nullptr, nullptr, 0);
	std::vector<std::uint8_t>().swap(impl.playing);
}

// The worker thread. Takes one request at a time, plays it and reports the result. Done callbacks run outside the
// lock, so a callback can call play() without deadlocking.
void SoundPlayer::Impl::run()
{
	for (;;) {
		std::optional<Request> request;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [this] { return stopping || pending.has_value(); });
			if (stopping)
				return;
			request = std::move(pending);
			pending.reset();
		}
		std::string error;
		try {
			error = play_request(*request);
		} catch (const std::exception &e) {
			error = std::string("could not play the sound: ") + e.what();
		} catch (...) {
			error = "could not play the sound: unknown exception";
		}
		call_done(request->done, error);
	}
}

// Worker thread only. Reads and scales the file, then plays it. Returns "" or the reason.
std::string SoundPlayer::Impl::play_request(const Request &request)
{
	std::vector<std::uint8_t> file;
	std::string error = read_wav_file(request.path, file);
	if (!error.empty())
		return error;
	PreparedWav prepared = scale_wav_volume(file, request.volume);
	if (!prepared.error.empty())
		return prepared.error;
	// The sound that is playing reads the old image, so it stops before that image is freed.
	PlaySoundW(nullptr, nullptr, 0);
	playing = std::move(prepared.bytes);
	if (!PlaySoundW(reinterpret_cast<LPCWSTR>(playing.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
		return "Windows could not play the sound";
	return "";
}

SoundPlayer &sound_player()
{
	// Deliberately never deleted: a destructor would join the worker while Windows unloads the DLL, which can
	// deadlock (see sound-player.hpp).
	static SoundPlayer *player = new SoundPlayer;
	return *player;
}

} // namespace clip_atlas

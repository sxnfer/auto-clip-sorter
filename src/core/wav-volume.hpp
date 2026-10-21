// Clip Atlas — scales the volume of a .wav file held in memory.
//
// Windows' PlaySoundW has no volume control, so the save sound plays a copy of
// the file whose samples have been scaled, with SND_MEMORY. This file does the
// parsing and scaling so it can be tested without Windows.
//
// Pure C++17 with no OBS dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clip_atlas {

// Files larger than this are refused, so a wrong file choice can't take
// hundreds of megabytes. A few seconds of CD-quality audio is well under 1 MB.
constexpr std::size_t kMaxSaveSoundBytes = 10 * 1024 * 1024;

// The gain applied for a volume percentage: (volume / 100)^2, with volume
// clamped to [0, 100]. Squaring makes the slider feel even, because loudness
// is perceived roughly logarithmically: 50 % gives a quarter of the amplitude
// (about -12 dB) rather than a barely quieter half.
double volume_gain(int volume_percent);

struct PreparedWav {
	std::vector<std::uint8_t> bytes; // a complete RIFF/WAVE file image; empty on error
	std::string error;               // empty on success; an English reason for the log
};

// Returns `file` with every sample in its "data" chunk multiplied by
// volume_gain(volume_percent), or an error.
//
// Accepted: a RIFF/WAVE image ("RIFF", size, "WAVE", then chunks) whose "fmt "
// chunk comes before its "data" chunk. Chunks are walked by their declared
// sizes, with the pad byte after an odd-sized chunk, and unknown chunks are
// kept as they are. A "data" chunk that claims more bytes than the file has is
// clamped to what is there (players accept truncated files).
//
// Sample formats that can be scaled:
//   - format tag 1 (PCM): 8-bit (unsigned, centred on 128), 16-, 24- and 32-bit
//     signed little-endian
//   - format tag 3 (IEEE float): 32-bit
//   - format tag 0xFFFE (WAVE_FORMAT_EXTENSIBLE) whose sub-format GUID starts
//     with 1 (PCM) or 3 (float) and is otherwise the standard
//     xxxxxxxx-0000-0010-8000-00aa00389b71: the same as the tag it names
// Scaled integer samples are rounded to nearest and clamped to the type's
// range; float samples are multiplied as they are (no clamping is needed with
// a gain of at most 1). A trailing partial sample frame is left unchanged.
//
// volume_percent >= 100 returns the file unchanged after checking only the
// RIFF/WAVE header, so a format this file can't scale still plays at full
// volume. Errors: larger than kMaxSaveSoundBytes, not RIFF/WAVE, no "fmt " or
// "data" chunk, a "fmt " chunk too short for its tag, or a format or bit depth
// not listed above (when volume_percent < 100).
PreparedWav scale_wav_volume(const std::vector<std::uint8_t> &file, int volume_percent);

} // namespace clip_atlas

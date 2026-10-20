// Clip Atlas — predicts the file OBS's automatic remux will write.
//
// Right after OBS saves a replay or finishes a recording it may remux it
// (e.g. "Replay.mkv" -> "Replay.mp4") in the same folder, reading the
// original while it writes the copy. Moving the original before that is done
// would break the remux, so the sorter needs to know which file to wait for.
//
// Mirrors OBSBasic::AutoRemux() in OBS 31.0 (UI/window-basic-main.cpp).
//
// Pure C++17 with no OBS dependency. All strings are UTF-8.
#pragma once

#include <filesystem>
#include <string>

namespace clip_atlas {

// Values read from the OBS profile config (obs_frontend_get_profile_config()).
struct RemuxConfig {
	bool auto_remux = false;      // [Video] AutoRemux
	bool simple_mode = true;      // [Output] Mode is "Simple" or missing
	bool advanced_ffmpeg = false; // [AdvOut] RecType is "FFmpeg" (case-insensitive)
	std::string recording_format; // [SimpleOutput] or [AdvOut] RecFormat2, e.g. "mkv",
				      // "fragmented_mp4"
};

// Path OBS will remux `saved` into, or an empty path if no remux will happen:
//   - auto_remux off                         -> empty
//   - advanced mode with FFmpeg output        -> empty
//   - extension "avi" (any case), or none     -> empty (OBS skips lossless avi)
//   - recording_format starts "fragmented"   -> "<dir>/<stem>.remuxed.<ext>"
//   - otherwise                               -> "<dir>/<stem>.mp4"
//   - result equal to `saved`, comparing the
//     extension ASCII case-insensitively       -> empty (already .mp4)
// ProRes (".mov" output) is macOS-only and not handled.
std::filesystem::path expected_remux_path(const std::filesystem::path &saved, const RemuxConfig &config);

} // namespace clip_atlas

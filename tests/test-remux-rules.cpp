#include "remux-rules.hpp"

#include "clip-layout.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace clip_atlas;
namespace fs = std::filesystem;

namespace {

// Auto-remux on, simple output, MKV recording: the usual case where OBS writes "<stem>.mp4" beside the MKV.
RemuxConfig remux_mkv()
{
	RemuxConfig config;
	config.auto_remux = true;
	config.simple_mode = true;
	config.recording_format = "mkv";
	return config;
}

} // namespace

TEST_CASE("expected_remux_path: nothing is remuxed while AutoRemux is off")
{
	RemuxConfig config = remux_mkv();
	config.auto_remux = false;
	CHECK(expected_remux_path("Replay.mkv", config).empty());
}

TEST_CASE("expected_remux_path: advanced mode with FFmpeg output is not remuxed")
{
	RemuxConfig config = remux_mkv();
	config.simple_mode = false;
	config.advanced_ffmpeg = true;
	CHECK(expected_remux_path("Replay.mkv", config).empty());
}

TEST_CASE("expected_remux_path: advanced mode with another output type is remuxed")
{
	RemuxConfig config = remux_mkv();
	config.simple_mode = false;
	config.advanced_ffmpeg = false;
	CHECK(expected_remux_path("Replay.mkv", config) == fs::path("Replay.mp4"));
}

TEST_CASE("expected_remux_path: simple mode ignores the advanced FFmpeg flag")
{
	RemuxConfig config = remux_mkv();
	config.simple_mode = true;
	config.advanced_ffmpeg = true;
	CHECK(expected_remux_path("Replay.mkv", config) == fs::path("Replay.mp4"));
}

TEST_CASE("expected_remux_path: an MKV becomes an MP4 with the same stem")
{
	CHECK(expected_remux_path("Replay 2026-10-20 19-02-44.mkv", remux_mkv()) ==
	      fs::path("Replay 2026-10-20 19-02-44.mp4"));
}

TEST_CASE("expected_remux_path: the extension is matched without regard to case")
{
	const RemuxConfig config = remux_mkv();
	CHECK(expected_remux_path("Replay.MKV", config) == fs::path("Replay.mp4"));
	CHECK(expected_remux_path("Replay.Mkv", config) == fs::path("Replay.mp4"));
}

TEST_CASE("expected_remux_path: AVI recordings are never remuxed")
{
	RemuxConfig config = remux_mkv();
	CHECK(expected_remux_path("Replay.avi", config).empty());
	CHECK(expected_remux_path("Replay.AVI", config).empty());

	// The fragmented rule must not rescue an AVI: OBS checks the extension first.
	config.recording_format = "fragmented_mp4";
	CHECK(expected_remux_path("Replay.AVI", config).empty());
}

TEST_CASE("expected_remux_path: an MP4 is not remuxed onto itself")
{
	RemuxConfig config = remux_mkv();
	config.recording_format = "mp4";
	CHECK(expected_remux_path("Replay.mp4", config).empty());
	CHECK(expected_remux_path("Replay.MP4", config).empty());
}

TEST_CASE("expected_remux_path: fragmented MP4 gets a .remuxed sibling, even when already .mp4")
{
	RemuxConfig config = remux_mkv();
	config.recording_format = "fragmented_mp4";
	CHECK(expected_remux_path("C:/v/Replay 2026-10-20 19-02-44.mp4", config) ==
	      fs::path("C:/v/Replay 2026-10-20 19-02-44.remuxed.mp4"));
	// The original extension keeps its case, as in OBS's "remuxed." + suffix.
	CHECK(expected_remux_path("Replay.MP4", config) == fs::path("Replay.remuxed.MP4"));
}

TEST_CASE("expected_remux_path: fragmented MOV gets a .remuxed sibling")
{
	RemuxConfig config = remux_mkv();
	config.recording_format = "fragmented_mov";
	CHECK(expected_remux_path("Replay.mov", config) == fs::path("Replay.remuxed.mov"));
}

TEST_CASE("expected_remux_path: only a format that starts with fragmented counts")
{
	RemuxConfig config = remux_mkv();
	// OBS compares with strncmp, which is case-sensitive.
	config.recording_format = "Fragmented_mp4";
	CHECK(expected_remux_path("Replay.mkv", config) == fs::path("Replay.mp4"));

	config.recording_format = "mp4_fragmented";
	CHECK(expected_remux_path("Replay.mkv", config) == fs::path("Replay.mp4"));
}

TEST_CASE("expected_remux_path: a file without an extension is not remuxed")
{
	RemuxConfig config = remux_mkv();
	CHECK(expected_remux_path("Replay", config).empty());

	config.recording_format = "fragmented_mp4";
	CHECK(expected_remux_path("Replay", config).empty());
}

TEST_CASE("expected_remux_path: the directory is kept and never mistaken for an extension")
{
	const RemuxConfig config = remux_mkv();
	CHECK(expected_remux_path("D:/Clips/Game A/Replay.mkv", config) == fs::path("D:/Clips/Game A/Replay.mp4"));
	CHECK(expected_remux_path("C:/clips.v2/Replay.mkv", config) == fs::path("C:/clips.v2/Replay.mp4"));
}

TEST_CASE("expected_remux_path: UTF-8 file names keep their characters")
{
	RemuxConfig config = remux_mkv();
	CHECK(expected_remux_path(path_from_utf8("D:/ポケモン/リプレイ 日本語.mkv"), config) ==
	      path_from_utf8("D:/ポケモン/リプレイ 日本語.mp4"));

	config.recording_format = "fragmented_mp4";
	CHECK(expected_remux_path(path_from_utf8("D:/ポケモン/リプレイ 日本語.mp4"), config) ==
	      path_from_utf8("D:/ポケモン/リプレイ 日本語.remuxed.mp4"));
}

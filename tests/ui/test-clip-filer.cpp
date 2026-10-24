#include "activity-status.hpp"
#include "clip-filer.hpp"
#include "clip-layout.hpp"
#include "host-stubs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QElapsedTimer>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>
#include <stdexcept>

using namespace clip_atlas;
namespace fs = std::filesystem;

namespace {
bool eventually(const std::function<bool()> &condition, int limit = 8000)
{
	QElapsedTimer timer;
	timer.start();
	while (!condition() && timer.elapsed() < limit)
		QTest::qWait(10);
	return condition();
}

void write(const fs::path &path)
{
	std::ofstream stream(path, std::ios::binary);
	stream << "recording bytes";
	REQUIRE(stream.good());
}
} // namespace

TEST_CASE("ClipFiler emits actual wait, worker start and completion for a grouped remux save", "[activity-filer]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const fs::path root = path_from_utf8(directory.path().toStdString());
	test_clips_folder = (root / "sorted").u8string();
	const auto original = root / "replay.mkv";
	const auto remuxed = root / "replay.mp4";
	write(original);
	SettingsStore settings;
	FileMover mover;
	ClipFiler filer(settings, mover);
	ActivityModel model;
	std::vector<ActivityEvent> events;
	std::vector<ClipSorted> clips;
	filer.set_activity_callback([&](const ActivityEvent &event) {
		events.push_back(event);
		REQUIRE(model.apply(event));
	});
	filer.set_clip_sorted_callback([&](const ClipSorted &clip) { clips.push_back(clip); });
	REQUIRE(filer.file(ClipKind::replay, {{original, remuxed}}, {"cs2.exe", "Counter-Strike 2"}));
	REQUIRE(events.size() == 1);
	CHECK(model.current()->state == ActivityState::waiting);
	CHECK(model.current()->files.size() == 2);
	CHECK(model.current()->files[1].remuxed);
	QTest::qWait(50);
	CHECK(clips.empty());
	CHECK(fs::exists(original));
	write(remuxed);
	REQUIRE(eventually([&] { return clips.size() == 2; }));
	REQUIRE(model.current());
	CHECK(model.current()->state == ActivityState::completed);
	CHECK(clips[0].save_id == clips[1].save_id);
	CHECK(clips[0].save_id == model.current()->save_id);
	CHECK_FALSE(fs::exists(original));
	CHECK_FALSE(fs::exists(remuxed));
	for (std::size_t index = 0; index < 2; ++index) {
		std::vector<ActivityPhase> phases;
		for (const auto &event : events) {
			if (!event.accepted && event.file_index == index)
				phases.push_back(event.phase);
		}
		CHECK(phases == std::vector<ActivityPhase>{ActivityPhase::queued, ActivityPhase::moving,
							   ActivityPhase::completed});
		CHECK(fs::exists(clips[index].final_path));
	}
	filer.stop();
	mover.shutdown(false);
}

TEST_CASE("Split files and overlapping saves remain distinct and report real move failures", "[activity-filer]")
{
	QTemporaryDir directory;
	const fs::path root = path_from_utf8(directory.path().toStdString());
	const auto blocked = root / "not-a-directory";
	write(blocked);
	test_clips_folder = blocked.u8string();
	const auto first = root / "part1.mkv", second = root / "part2.mkv";
	write(first);
	write(second);
	SettingsStore settings;
	FileMover::Options options;
	options.retry_budget = std::chrono::milliseconds(1);
	FileMover mover(options);
	ClipFiler filer(settings, mover);
	ActivityModel model;
	std::vector<ClipSorted> clips;
	filer.set_activity_callback([&](const ActivityEvent &event) { model.apply(event); });
	filer.set_clip_sorted_callback([&](const ClipSorted &clip) { clips.push_back(clip); });
	REQUIRE(filer.file(ClipKind::recording, {{first, {}}, {second, {}}}, {}));
	REQUIRE(filer.file(ClipKind::screenshot, {{first, {}}}, {}));
	REQUIRE(eventually([&] { return clips.size() == 3; }));
	REQUIRE(model.activities().size() == 2);
	CHECK(model.activities()[0].files.size() == 2);
	CHECK(model.activities()[0].state == ActivityState::failed);
	CHECK(model.activities()[1].state == ActivityState::failed);
	CHECK(clips[0].save_id == clips[1].save_id);
	CHECK(clips[2].save_id != clips[0].save_id);
	CHECK(fs::exists(first));
	CHECK(fs::exists(second));
	CHECK_FALSE(model.activities()[0].files[0].detail.empty());
}

TEST_CASE("Filing survives a destroyed widget and a throwing observer", "[activity-filer]")
{
	QTemporaryDir directory;
	const fs::path root = path_from_utf8(directory.path().toStdString());
	test_clips_folder = (root / "sorted").u8string();
	const auto source = root / "replay.mkv";
	write(source);
	SettingsStore settings;
	FileMover mover;
	ClipFiler filer(settings, mover);
	QPointer<ActivityStatus> widget = new ActivityStatus;
	int after_destroy = 0;
	filer.set_activity_callback([&](const ActivityEvent &event) {
		if (widget)
			widget->on_activity(event);
		else
			++after_destroy;
		throw std::runtime_error("observer failure");
	});
	bool sorted = false;
	filer.set_clip_sorted_callback([&](const ClipSorted &clip) { sorted = clip.status == MoveStatus::moved; });
	REQUIRE(filer.file(ClipKind::replay, {{source, {}}}, {}));
	delete widget.data();
	REQUIRE(eventually([&] { return sorted; }));
	CHECK(after_destroy == 3);
}

TEST_CASE("Filer shutdown cancels waiting and drops deferred results", "[activity-filer]")
{
	QTemporaryDir directory;
	const fs::path root = path_from_utf8(directory.path().toStdString());
	test_clips_folder = (root / "sorted").u8string();
	const auto source = root / "replay.mkv";
	write(source);
	SettingsStore settings;
	FileMover mover;
	int events = 0, outcomes = 0;
	{
		ClipFiler filer(settings, mover);
		filer.set_activity_callback([&](const ActivityEvent &) { ++events; });
		filer.set_clip_sorted_callback([&](const ClipSorted &) { ++outcomes; });
		REQUIRE(filer.file(ClipKind::replay, {{source, root / "missing.mp4"}}, {}));
		filer.stop();
		filer.stop();
		CHECK_FALSE(filer.file(ClipKind::replay, {{source, {}}}, {}));
	}
	QTest::qWait(30);
	CHECK(events == 1);
	CHECK(outcomes == 0);
	CHECK(fs::exists(source));
}

#include "activity-model.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace clip_atlas;
using namespace std::chrono_literals;

namespace {
const auto start = ActivityClock::time_point(100s);

ActivityEvent accepted(std::uint64_t id = 1, std::size_t count = 1)
{
	ActivityEvent event;
	event.save_id = id;
	event.accepted = true;
	event.game = "Counter-Strike 2";
	event.files.resize(count);
	event.at = start;
	return event;
}

ActivityEvent phase(ActivityPhase value, std::size_t index = 0, std::uint64_t id = 1, int seconds = 1)
{
	ActivityEvent event;
	event.save_id = id;
	event.phase = value;
	event.file_index = index;
	event.at = start + std::chrono::seconds(seconds);
	return event;
}
} // namespace

TEST_CASE("Activity follows real wait, queue, move and completion", "[activity]")
{
	ActivityModel model;
	CHECK(model.current() == nullptr);
	CHECK(model.activities().empty());
	REQUIRE(model.apply(accepted()));
	CHECK(model.current()->state == ActivityState::waiting);
	CHECK(model.current()->started_at == start);
	// A delayed remux or a queued wait is still waiting, however much time passes.
	auto ready = phase(ActivityPhase::queued, 0, 1, 600);
	ready.stability_confirmed = true;
	REQUIRE(model.apply(ready));
	CHECK(model.current()->files[0].stability_confirmed);
	CHECK(model.current()->state == ActivityState::working);
	REQUIRE(model.apply(phase(ActivityPhase::moving, 0, 1, 601)));
	REQUIRE(model.apply(phase(ActivityPhase::completed, 0, 1, 605)));
	CHECK(model.current()->state == ActivityState::completed);
	CHECK(model.current()->ended_at == start + 605s);
	CHECK(model.current()->started_at == start);
}

TEST_CASE("Remux and split parts finish as one save, including partial failure", "[activity]")
{
	ActivityModel model;
	auto event = accepted(1, 4);
	event.kind = ClipKind::recording;
	event.files[1].remuxed = true;
	event.files[3].remuxed = true;
	REQUIRE(model.apply(event));
	REQUIRE(model.apply(phase(ActivityPhase::completed)));
	REQUIRE(model.apply(phase(ActivityPhase::completed, 1)));
	auto failure = phase(ActivityPhase::failed, 2);
	failure.detail = "permission denied";
	REQUIRE(model.apply(failure));
	CHECK(model.current()->state == ActivityState::waiting);
	REQUIRE(model.apply(phase(ActivityPhase::completed, 3, 1, 2)));
	CHECK(model.current()->state == ActivityState::failed);
	CHECK(model.current()->files[2].detail == "permission denied");
	CHECK(model.activities().size() == 1);
	CHECK(model.current()->kind == ClipKind::recording);
}

TEST_CASE("Timeout fallback never claims stability or sorts a missing remux", "[activity]")
{
	ActivityModel model;
	REQUIRE(model.apply(accepted(1, 2)));
	auto missing = phase(ActivityPhase::unavailable, 1);
	missing.notice = ActivityNotice::remux_unavailable;
	REQUIRE(model.apply(missing));
	auto queued = phase(ActivityPhase::queued);
	queued.notice = ActivityNotice::stability_timeout;
	REQUIRE(model.apply(queued));
	REQUIRE(model.apply(phase(ActivityPhase::moving, 0, 1, 2)));
	auto done = phase(ActivityPhase::completed, 0, 1, 3);
	done.notice = ActivityNotice::original_kept;
	done.detail = "source removal failed";
	REQUIRE(model.apply(done));
	CHECK(model.current()->state == ActivityState::completed);
	CHECK_FALSE(model.current()->files[0].stability_confirmed);
	CHECK(model.current()->files[0].notice == ActivityNotice::original_kept);
	CHECK(model.current()->files[1].notice == ActivityNotice::remux_unavailable);
	CHECK(model.current()->files[1].phase == ActivityPhase::unavailable);
}

TEST_CASE("Overlapping saves retain their identity and oldest live work has priority", "[activity]")
{
	ActivityModel model;
	REQUIRE(model.apply(accepted(1, 2)));
	REQUIRE(model.apply(accepted(2)));
	REQUIRE(model.apply(phase(ActivityPhase::moving, 0, 2)));
	CHECK(model.current()->save_id == 2);
	REQUIRE(model.apply(phase(ActivityPhase::completed, 0, 2)));
	CHECK(model.current()->save_id == 1);
	REQUIRE(model.apply(phase(ActivityPhase::completed, 0)));
	CHECK(model.current()->save_id == 1);
	REQUIRE(model.apply(phase(ActivityPhase::completed, 1)));
	CHECK(model.current()->save_id == 2);
	CHECK(model.activities().size() == 2);
}

TEST_CASE("Duplicate and stale activity events cannot regress or resurrect work", "[activity]")
{
	ActivityModel model;
	CHECK_FALSE(model.apply(accepted(0)));
	auto empty = accepted();
	empty.files.clear();
	CHECK_FALSE(model.apply(empty));
	CHECK_FALSE(model.apply(phase(ActivityPhase::moving)));
	REQUIRE(model.apply(accepted()));
	CHECK_FALSE(model.apply(accepted()));
	CHECK_FALSE(model.apply(phase(ActivityPhase::moving, 9)));
	REQUIRE(model.apply(phase(ActivityPhase::moving, 0, 1, 10)));
	CHECK_FALSE(model.apply(phase(ActivityPhase::queued, 0, 1, 11)));
	CHECK_FALSE(model.apply(phase(ActivityPhase::completed, 0, 1, 2)));
	REQUIRE(model.apply(phase(ActivityPhase::completed, 0, 1, 12)));
	CHECK_FALSE(model.apply(phase(ActivityPhase::failed, 0, 1, 13)));
	CHECK(model.current()->ended_at == start + 12s);
}

TEST_CASE("Activity history bounds finished saves without evicting active work", "[activity]")
{
	ActivityModel model;
	REQUIRE(model.apply(accepted()));
	for (std::uint64_t id = 2; id <= 40; ++id) {
		REQUIRE(model.apply(accepted(id)));
		REQUIRE(model.apply(phase(ActivityPhase::completed, 0, id)));
	}
	CHECK(model.activities().size() == 13);
	CHECK(model.current()->save_id == 1);
	CHECK_FALSE(model.apply(accepted(2)));
	CHECK_FALSE(model.apply(phase(ActivityPhase::completed, 0, 2)));
	model.stop(start + 100s);
	CHECK(model.activities().size() == 12);
	CHECK(model.current()->save_id == 1);
	CHECK(model.current()->state == ActivityState::cancelled);
}

TEST_CASE("Cancellation and shutdown freeze outcomes and ignore late delivery", "[activity]")
{
	ActivityModel model;
	REQUIRE(model.apply(accepted(1, 2)));
	REQUIRE(model.apply(accepted(2)));
	REQUIRE(model.apply(phase(ActivityPhase::completed, 0, 2)));
	REQUIRE(model.apply(phase(ActivityPhase::completed, 0)));
	model.stop(start + 5s);
	model.stop(start + 10s);
	CHECK(model.activities().front().state == ActivityState::cancelled);
	CHECK(model.activities().front().files[0].phase == ActivityPhase::completed);
	CHECK(model.activities().front().files[1].phase == ActivityPhase::cancelled);
	CHECK(model.activities().front().ended_at == start + 5s);
	CHECK(model.activities().back().state == ActivityState::completed);
	CHECK_FALSE(model.apply(phase(ActivityPhase::completed, 1)));
	CHECK_FALSE(model.apply(accepted(3)));
}

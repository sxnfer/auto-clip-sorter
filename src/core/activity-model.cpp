#include "activity-model.hpp"

#include <algorithm>

namespace clip_atlas {
namespace {

bool terminal(ActivityPhase phase)
{
	return phase >= ActivityPhase::completed;
}

void refresh(Activity &activity, ActivityClock::time_point at)
{
	bool live = false, moving = false, failed = false, cancelled = false;
	for (const auto &file : activity.files) {
		live |= !terminal(file.phase);
		moving |= file.phase == ActivityPhase::queued || file.phase == ActivityPhase::moving;
		failed |= file.phase == ActivityPhase::failed;
		cancelled |= file.phase == ActivityPhase::cancelled;
	}
	activity.state = live        ? (moving ? ActivityState::working : ActivityState::waiting)
			 : failed    ? ActivityState::failed
			 : cancelled ? ActivityState::cancelled
				     : ActivityState::completed;
	if (!live)
		activity.ended_at = std::max(at, activity.started_at);
}

} // namespace

bool ActivityModel::apply(const ActivityEvent &event)
{
	if (stopped_ || !event.save_id)
		return false;
	if (event.accepted) {
		// Save ids are allocated monotonically on the UI thread across both filers.
		if (event.save_id <= newest_id_ || event.files.empty())
			return false;
		newest_id_ = event.save_id;
		Activity activity;
		activity.save_id = event.save_id;
		activity.kind = event.kind;
		activity.game = event.game;
		activity.files = event.files;
		activity.started_at = event.at;
		for (auto &file : activity.files) {
			file.phase = ActivityPhase::waiting;
			file.changed_at = event.at;
		}
		activities_.push_back(std::move(activity));
	} else {
		auto found = std::find_if(activities_.begin(), activities_.end(),
					  [&](const Activity &a) { return a.save_id == event.save_id; });
		if (found == activities_.end() || event.file_index >= found->files.size())
			return false;
		auto &file = found->files[event.file_index];
		if (terminal(file.phase) || event.phase <= file.phase || event.at < file.changed_at)
			return false;
		file.phase = event.phase;
		file.changed_at = event.at;
		file.stability_confirmed |= event.stability_confirmed;
		if (event.notice != ActivityNotice::none)
			file.notice = event.notice;
		if (!event.detail.empty())
			file.detail = event.detail;
		refresh(*found, event.at);
	}
	trim_history();
	return true;
}

const std::deque<Activity> &ActivityModel::activities() const
{
	return activities_;
}

const Activity *ActivityModel::current() const
{
	for (const auto &activity : activities_) {
		if (activity.state == ActivityState::working)
			return &activity;
	}
	for (const auto &activity : activities_) {
		if (activity.state == ActivityState::waiting)
			return &activity;
	}
	const Activity *latest = nullptr;
	for (const auto &activity : activities_) {
		if (!latest || activity.ended_at >= latest->ended_at)
			latest = &activity;
	}
	return latest;
}

void ActivityModel::stop(ActivityClock::time_point at)
{
	if (stopped_)
		return;
	stopped_ = true;
	for (auto &activity : activities_) {
		for (auto &file : activity.files) {
			if (!terminal(file.phase)) {
				file.phase = ActivityPhase::cancelled;
				file.changed_at = at;
			}
		}
		if (activity.state == ActivityState::working || activity.state == ActivityState::waiting)
			refresh(activity, at);
	}
	trim_history();
}

void ActivityModel::trim_history()
{
	// Active work is never evicted. Only finished history is bounded.
	auto finished = [](const Activity &a) {
		return a.state != ActivityState::waiting && a.state != ActivityState::working;
	};
	while (std::count_if(activities_.begin(), activities_.end(), finished) > 12) {
		auto oldest = activities_.end();
		for (auto it = activities_.begin(); it != activities_.end(); ++it) {
			if (finished(*it) && (oldest == activities_.end() || it->ended_at < oldest->ended_at))
				oldest = it;
		}
		activities_.erase(oldest);
	}
}

} // namespace clip_atlas

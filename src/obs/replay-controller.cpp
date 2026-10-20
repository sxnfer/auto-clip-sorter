// Clip Atlas — starts and stops OBS's Replay Buffer as games come and go.
//
// replay-policy decides. This file gathers the inputs, makes the OBS calls and keeps the state the
// policy needs but cannot see: whether a request is in flight, whether the buffer is ours, and whether
// the user has overruled us for the current game.
//
// OBS answers start and stop later, and sometimes never. A request therefore sets `pending` before it
// calls OBS, so an answer delivered inside that call is still matched to it, and a timer gives up on
// silence. A buffer counts as ours only when STARTED answers our own start request, so a buffer the
// user started is never stopped.
//
// UI thread only.
#include "replay-controller.hpp"

#include "replay-policy.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QObject>
#include <QTimer>

#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace clip_atlas {

namespace {

// Long enough that a slow answer isn't mistaken for silence, short enough that the state settles quickly.
// Silence can mean OBS is showing an error dialog (an invalid output path, say), which is why a start that
// goes silent is not retried.
constexpr int kPendingTimeoutMs = 10000;

// QTimer::start() takes an int, and the policy allows up to a year. Waking early is harmless because
// evaluate() asks again, so the wait is capped rather than overflowed.
constexpr std::chrono::milliseconds kMaxTimerWait{(std::numeric_limits<int>::max)()};

enum class PendingRequest {
	none,
	start,
	stop,
};

// Keeps exceptions away from the frontend callback and the Qt timer slots. Unwinding through either
// would take OBS down, so the failure is logged and the next event carries on.
template<typename Fn> void run_guarded(const char *where, Fn &&fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Replay Buffer control: %s failed: %s", where, e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "Replay Buffer control: %s failed: unknown exception", where);
	}
}

// The same test GameState uses for "the current game changed": the capture source and its window.
bool same_game(const std::optional<CurrentGame> &a, const std::optional<CurrentGame> &b)
{
	if (!a || !b)
		return a.has_value() == b.has_value();
	return a->source_id == b->source_id && a->info == b->info;
}

// Logs name the program only. Window titles can carry private text.
std::string game_label(const std::optional<CurrentGame> &game)
{
	if (!game)
		return "no game";
	return game->info.executable.empty() ? std::string("a program with no name") : game->info.executable;
}

// True when Output settings enable the Replay Buffer. The output reference is released at once, because
// nothing here keeps it.
bool replay_output_exists()
{
	obs_output_t *output = obs_frontend_get_replay_buffer_output();
	if (!output)
		return false;
	obs_output_release(output);
	return true;
}

const char *status_label(ReplayBufferStatus status)
{
	switch (status) {
	case ReplayBufferStatus::disabled_in_obs:
		return "disabled in OBS Output settings";
	case ReplayBufferStatus::inactive:
		return "off";
	case ReplayBufferStatus::active_by_us:
		return "running, started by Clip Atlas";
	case ReplayBufferStatus::active_by_user:
		return "running, started by you";
	}
	return "unknown";
}

long long ms_between(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
	return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
}

} // namespace

struct ReplayController::Impl {
	explicit Impl(const GameState &game_state_ref);

	// OBS takes a plain function pointer, so the Impl comes back through private_data.
	static void on_frontend_event(obs_frontend_event event, void *private_data);

	void start(const Settings &initial);
	void stop();
	void handle_event(obs_frontend_event event);
	void on_obs_started();
	void on_obs_stopped();
	void on_pending_timeout();
	void on_game_changed();
	void evaluate();
	void request_start();
	void request_stop(const ReplayPolicyInput &input);
	void arm_recheck(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point at);
	void update_status();
	ReplayBufferStatus compute_status() const;

	const GameState &game_state;
	Settings settings;
	std::optional<CurrentGame> last_game; // the game on_game_changed() last saw
	PendingRequest pending = PendingRequest::none;
	std::chrono::steady_clock::time_point pending_since{};
	bool we_started = false;      // the active buffer answered our own start request
	bool suppressed = false;      // the user stopped the buffer during this game, so don't restart it
	bool stop_unanswered = false; // our stop has not been answered, even if the timeout has passed
	bool registered = false;      // the frontend callback is registered
	bool closed = false;          // stop() or OBS_FRONTEND_EVENT_EXIT: nothing may call OBS any more
	ReplayBufferStatus status = ReplayBufferStatus::inactive; // the last status given to the callback
	ReplayController::StatusCallback status_callback;
	QTimer recheck_timer; // a pending stop waiting out the grace period
	QTimer pending_timer; // gives up on a start or stop that OBS has not answered
};

ReplayController::Impl::Impl(const GameState &game_state_ref) : game_state(game_state_ref)
{
	recheck_timer.setSingleShot(true);
	// A coarse timer may fire up to 5% early, which only re-arms the wait and logs it twice.
	recheck_timer.setTimerType(Qt::PreciseTimer);
	pending_timer.setSingleShot(true);
	QObject::connect(&recheck_timer, &QTimer::timeout,
			 [this] { run_guarded("grace re-check", [this] { evaluate(); }); });
	QObject::connect(&pending_timer, &QTimer::timeout,
			 [this] { run_guarded("request timeout", [this] { on_pending_timeout(); }); });
}

void ReplayController::Impl::on_frontend_event(obs_frontend_event event, void *private_data)
{
	Impl *self = static_cast<Impl *>(private_data);
	run_guarded("frontend event", [self, event] { self->handle_event(event); });
}

void ReplayController::Impl::start(const Settings &initial)
{
	settings = initial;
	if (closed) {
		obs_log(LOG_WARNING, "Replay Buffer control was already stopped and does not restart");
		return;
	}
	if (registered) {
		evaluate();
		return;
	}

	// A buffer already running when we start was started by the user, so it is not ours to stop.
	we_started = false;
	last_game = game_state.current();
	obs_frontend_add_event_callback(&Impl::on_frontend_event, this);
	registered = true;
	obs_log(LOG_INFO, "Replay Buffer control on: auto start %s, auto stop %s, grace %d s",
		settings.replay_auto_start ? "on" : "off", settings.replay_auto_stop ? "on" : "off",
		settings.grace_period_seconds);
	evaluate();
}

void ReplayController::Impl::stop()
{
	closed = true;
	recheck_timer.stop();
	pending_timer.stop();
	if (!registered)
		return;

	// Never stops the buffer. A buffer we started keeps running and is then the user's to stop.
	// The frontend callback is left registered and goes inert (handle_event checks `closed`): OBS
	// dispatches events with an index loop, so removing a callback during EXIT would make another
	// callback run twice or be skipped. OBS drops every callback right after EXIT anyway. The owner
	// must keep this object alive until obs_module_unload(), after that point.
	registered = false;
	obs_log(LOG_INFO, "Replay Buffer control off; the Replay Buffer keeps running as it is");
}

void ReplayController::Impl::handle_event(obs_frontend_event event)
{
	if (closed)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		on_obs_started();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		on_obs_stopped();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		// The Replay Buffer output may have been created or removed since the last check.
		evaluate();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// No stop() here. OBS is still walking its frontend callback list, so removing ours now would
		// change it underneath OBS. Nothing runs after this flag is set, and stop() at unload removes
		// the callback.
		obs_log(LOG_INFO, "OBS is exiting; Clip Atlas no longer controls the Replay Buffer");
		closed = true;
		recheck_timer.stop();
		pending_timer.stop();
		break;
	default:
		// STARTING and STOPPING change nothing. Only STARTED and STOPPED answer a request.
		break;
	}
}

void ReplayController::Impl::on_obs_started()
{
	pending_timer.stop();
	const PendingRequest answered = pending;
	pending = PendingRequest::none;
	we_started = answered == PendingRequest::start;
	if (we_started)
		obs_log(LOG_INFO, "Replay Buffer started by Clip Atlas, %lld ms after the request",
			ms_between(pending_since, std::chrono::steady_clock::now()));
	else
		obs_log(LOG_INFO, "Replay Buffer started by you; Clip Atlas will not stop it");
	evaluate();
}

void ReplayController::Impl::on_obs_stopped()
{
	pending_timer.stop();
	const PendingRequest answered = pending;
	pending = PendingRequest::none;
	we_started = false;

	// A stop we asked for is ours, even when OBS answered after our timeout. With a game still running,
	// any other stop is the user's choice, or OBS dropped our start. Either way, don't restart it until
	// the game changes.
	const bool game_running = game_state.current().has_value();
	if (!stop_unanswered && game_running) {
		suppressed = true;
		if (answered == PendingRequest::start)
			obs_log(LOG_WARNING,
				"OBS stopped the Replay Buffer before it started; not retrying until the game changes");
		else
			obs_log(LOG_INFO,
				"Replay Buffer stopped while a game runs; not restarting until the game changes");
	} else if (stop_unanswered) {
		obs_log(LOG_INFO, "Replay Buffer stopped by Clip Atlas");
	} else {
		obs_log(LOG_INFO, "Replay Buffer stopped");
	}
	stop_unanswered = false;
	evaluate();
}

void ReplayController::Impl::on_pending_timeout()
{
	if (pending == PendingRequest::none)
		return;
	const PendingRequest silent = pending;
	pending = PendingRequest::none;
	if (silent == PendingRequest::start) {
		suppressed = true;
		obs_log(LOG_WARNING,
			"OBS did not start the Replay Buffer within %d s (an error dialog may be open in OBS); "
			"not retrying until the game changes",
			kPendingTimeoutMs / 1000);
	} else {
		obs_log(LOG_WARNING, "OBS did not stop the Replay Buffer within %d s", kPendingTimeoutMs / 1000);
	}
	evaluate();
}

void ReplayController::Impl::on_game_changed()
{
	if (closed)
		return;
	std::optional<CurrentGame> current = game_state.current();
	if (!same_game(current, last_game)) {
		last_game = std::move(current);
		suppressed = false;
		obs_log(LOG_INFO, "Current game is now %s", game_label(last_game).c_str());
	}
	evaluate();
}

void ReplayController::Impl::evaluate()
{
	if (!registered || closed)
		return;

	const auto now = std::chrono::steady_clock::now();
	ReplayPolicyInput input;
	input.now = now;
	input.auto_start = settings.replay_auto_start;
	input.auto_stop = settings.replay_auto_stop;
	input.grace_period = std::chrono::seconds(settings.grace_period_seconds);
	input.game_current = game_state.current().has_value();
	input.no_game_since = game_state.no_game_since();
	input.buffer_enabled = replay_output_exists();
	input.buffer_active = obs_frontend_replay_buffer_active();
	input.we_started = we_started;
	input.request_pending = pending != PendingRequest::none;
	input.suppressed = suppressed;

	const ReplayDecision decision = decide_replay_action(input);

	// A re-check only matters while the answer is "wait", so any other decision cancels it.
	recheck_timer.stop();
	switch (decision.action) {
	case ReplayAction::start:
		request_start();
		break;
	case ReplayAction::stop:
		request_stop(input);
		break;
	case ReplayAction::none:
		if (decision.recheck_at)
			arm_recheck(now, *decision.recheck_at);
		break;
	}
	update_status();
}

void ReplayController::Impl::request_start()
{
	// Set before the call, so an answer delivered inside the call is matched to this request.
	pending = PendingRequest::start;
	pending_since = std::chrono::steady_clock::now();
	obs_log(LOG_INFO, "Replay Buffer start requested: %s is running and the buffer is off",
		game_label(game_state.current()).c_str());
	obs_frontend_replay_buffer_start();
	if (pending == PendingRequest::start)
		pending_timer.start(kPendingTimeoutMs);
}

void ReplayController::Impl::request_stop(const ReplayPolicyInput &input)
{
	pending = PendingRequest::stop;
	pending_since = std::chrono::steady_clock::now();
	stop_unanswered = true;
	if (input.no_game_since)
		obs_log(LOG_INFO, "Replay Buffer stop requested: no game for %lld ms, past the %d s grace period",
			ms_between(*input.no_game_since, input.now), settings.grace_period_seconds);
	else
		obs_log(LOG_INFO, "Replay Buffer stop requested: no game is running");
	obs_frontend_replay_buffer_stop();
	if (pending == PendingRequest::stop)
		pending_timer.start(kPendingTimeoutMs);
}

void ReplayController::Impl::arm_recheck(std::chrono::steady_clock::time_point now,
					 std::chrono::steady_clock::time_point at)
{
	// Rounded up, so the wake-up never comes before the deadline.
	std::chrono::milliseconds wait = std::chrono::ceil<std::chrono::milliseconds>(at - now);
	if (wait < std::chrono::milliseconds::zero())
		wait = std::chrono::milliseconds::zero();
	if (wait > kMaxTimerWait)
		wait = kMaxTimerWait;
	recheck_timer.start(static_cast<int>(wait.count()));
	obs_log(LOG_INFO, "Replay Buffer stop waits for the grace period (%lld ms left)",
		static_cast<long long>(wait.count()));
}

void ReplayController::Impl::update_status()
{
	const ReplayBufferStatus now_status = compute_status();
	if (now_status == status)
		return;
	status = now_status;
	obs_log(LOG_INFO, "Replay Buffer is %s", status_label(status));
	if (status_callback)
		status_callback(status);
}

ReplayBufferStatus ReplayController::Impl::compute_status() const
{
	if (!replay_output_exists())
		return ReplayBufferStatus::disabled_in_obs;
	if (!obs_frontend_replay_buffer_active())
		return ReplayBufferStatus::inactive;
	return we_started ? ReplayBufferStatus::active_by_us : ReplayBufferStatus::active_by_user;
}

ReplayController::ReplayController(const GameState &state) : impl_(std::make_unique<Impl>(state)) {}

ReplayController::~ReplayController()
{
	stop();
}

void ReplayController::start(const Settings &settings)
{
	run_guarded("start", [this, &settings] { impl_->start(settings); });
}

void ReplayController::stop()
{
	run_guarded("stop", [this] { impl_->stop(); });
}

void ReplayController::apply_settings(const Settings &settings)
{
	run_guarded("settings change", [this, &settings] {
		impl_->settings = settings;
		impl_->evaluate();
	});
}

void ReplayController::on_game_changed()
{
	run_guarded("game change", [this] { impl_->on_game_changed(); });
}

ReplayBufferStatus ReplayController::status() const
{
	// After stop() OBS may be tearing down, so give the last known status and make no OBS calls.
	return impl_->closed ? impl_->status : impl_->compute_status();
}

void ReplayController::set_status_callback(StatusCallback callback)
{
	impl_->status_callback = std::move(callback);
}

} // namespace clip_atlas

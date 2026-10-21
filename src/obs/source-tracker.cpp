// Clip Atlas — finds capture sources and turns their signals into game-state events.
//
// Threads. OBS calls us from several threads, and each call is handled where it arrives:
//   - source_create, source_destroy and source_rename run on the thread that created, destroyed or renamed the
//     source. source_destroy runs on OBS's destruction thread, before the source's own destroy.
//   - hooked and unhooked come from the graphics thread: win-capture emits them from its video tick, and
//     obs_source_update() defers video sources to that tick too. activate and deactivate come from the same
//     tick, so they never run on the UI thread.
//   - obs_enum_sources() calls us while OBS holds its sources mutex.
//   - GameState, the UI map and the settings are touched only on the UI thread. Every other thread posts its work
//     with run_on_ui().
//
// Readouts. get_hooked reads strings that win-capture reallocates on the graphics thread, so it is safe only there.
//   When the UI thread needs a source's state, it queues a graphics task (OBS_TASK_GRAPHICS). That task reads
//   get_hooked and obs_source_active, then hands the plain values and a strong reference back to the UI thread with
//   run_on_ui(). The UI thread releases the reference. The graphics thread never releases a strong reference: a last
//   release takes OBS's sources mutex, and execute_graphics_tasks() holds the video task mutex while a task runs.
//
// Locks. Impl::mutex guards the registry (Impl::contexts), the only state shared between threads. No OBS call that can
//   release a source runs under it: the last release takes OBS's sources mutex, and obs_enum_sources() holds that mutex
//   while it calls us. obs_queue_task() never runs under it either, because it can wait for the video task mutex.
//
// Ownership. A context is keyed by the source pointer, not its UUID. A new source can take the UUID of an old source
//   that is still waiting for its source_destroy, which is what a scene collection switch can do. The UI keys its
//   entries by UUID and records which context owns each entry, so events from the old one are ignored.
//
// Lifetime. A queued task can outlive the tracker. It captures a weak_ptr to an Anchor that ~Impl clears, never a raw
//   pointer to the tracker or to a context.
#include "source-tracker.hpp"

#include "ui-thread.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace {

// calldata strings are valid only during the callback and may be NULL.
std::string copy_string(const char *text)
{
	return text ? std::string(text) : std::string();
}

// Only ASCII is changed, so UTF-8 bytes pass through untouched.
std::string ascii_lower(std::string text)
{
	for (char &c : text) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return text;
}

// The unversioned id drops any version suffix. Both capture types declare hooked, unhooked and get_hooked.
bool is_capture_source(const obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && (std::strcmp(id, "game_capture") == 0 || std::strcmp(id, "window_capture") == 0);
}

HookInfo hook_info_from(const calldata_t *cd)
{
	HookInfo info;
	info.title = copy_string(calldata_string(cd, "title"));
	info.window_class = copy_string(calldata_string(cd, "class"));
	info.executable = copy_string(calldata_string(cd, "executable"));
	return info;
}

// Frees the calldata on every path, including when a copy throws.
struct CalldataGuard {
	calldata_t data{};

	CalldataGuard() { calldata_init(&data); }
	~CalldataGuard() { calldata_free(&data); }
	CalldataGuard(const CalldataGuard &) = delete;
	CalldataGuard &operator=(const CalldataGuard &) = delete;
};

struct HookReadout {
	bool hooked = false;
	HookInfo info;
};

// proc_handler_call() returns false when the source has no get_hooked proc. Then nothing is hooked.
// Graphics thread only: get_hooked reads buffers that win-capture reallocates on that thread.
HookReadout read_hook_state(obs_source_t *source)
{
	HookReadout readout;
	proc_handler_t *handler = obs_source_get_proc_handler(source);
	if (!handler)
		return readout;

	CalldataGuard cd;
	if (proc_handler_call(handler, "get_hooked", &cd.data)) {
		readout.hooked = calldata_bool(&cd.data, "hooked");
		readout.info = hook_info_from(&cd.data);
	}
	return readout;
}

// Plain values from the graphics thread, so the UI thread never reads the source's own buffers.
struct Readout {
	HookReadout hook;
	bool live = false;
};

// Owns one weak reference, so a vector of them releases every one when it is destroyed.
using WeakHandle = std::unique_ptr<obs_weak_source_t, void (*)(obs_weak_source_t *)>;

} // namespace

struct SourceTracker::Impl {
	// Shared by every queued task. ~Impl clears `impl` on the UI thread, where tasks run, so a task that runs
	// after the tracker is gone finds nothing to touch.
	struct Anchor {
		Impl *impl = nullptr;
	};

	// One per connected source. Its address is the data for the per-source callbacks, so it must not be freed
	// until they are disconnected: in the source's own source_destroy callback, or in stop().
	struct SourceContext {
		std::uint64_t serial = 0; // never reused, so a late event from an old source can't match a new one
		std::string uuid;         // set before the signals connect, so callbacks can read it without the mutex
		obs_weak_source_t *weak = nullptr; // owned. The only reference this context keeps.
		std::weak_ptr<Anchor> anchor;
	};

	// UI thread only. Keyed by UUID, which is the id GameState uses. `serial` names the context that owns it.
	struct UiSource {
		std::uint64_t serial = 0;
		std::string name;
		bool selected = false;
		bool fed = false; // GameState holds state for this UUID
		bool live = false;
		bool hooked = false; // after the ignore rule: an ignored game counts as not hooked
		HookInfo info;
	};

	struct EnumArgs {
		std::vector<WeakHandle> *found;
	};

	// What a graphics readout needs. The weak reference is the request's own, so the graphics thread never
	// touches the registry. Freed by the graphics task; if OBS never runs the task, it is not freed (a small
	// leak at exit).
	struct ReadoutRequest {
		std::weak_ptr<Anchor> anchor;
		std::uint64_t serial = 0;
		std::string uuid;
		obs_weak_source_t *weak = nullptr;
	};

	// The frontend callback's private data. OBS keeps the pointer and may call the callback after the
	// tracker is gone, so the gate is never freed and only holds a weak reference to the anchor. See
	// install_frontend_callback().
	struct FrontendGate {
		std::weak_ptr<Anchor> anchor;
	};

	Impl(GameState &game_state, ChangedCallback changed, IgnorePredicate ignored)
		: state(game_state),
		  on_changed(std::move(changed)),
		  ignore(std::move(ignored)),
		  anchor(std::make_shared<Anchor>())
	{
		anchor->impl = this;
	}

	~Impl()
	{
		// stop() has run from ~SourceTracker. Clearing impl makes any task still queued a no-op.
		if (anchor)
			anchor->impl = nullptr;
	}

	Impl(const Impl &) = delete;
	Impl &operator=(const Impl &) = delete;

	GameState &state;
	ChangedCallback on_changed;
	IgnorePredicate ignore;
	std::shared_ptr<Anchor> anchor;

	// UI thread only. `running` is set from start() to stop(): tasks and the frontend callback do nothing
	// otherwise. `connected` is set while the global signals may still be connected, so stop() can drain them
	// even when running is already false.
	bool running = false;
	bool connected = false;
	Settings settings;
	std::vector<std::string> chosen_lower; // settings.chosen_sources, lowercased
	std::map<std::string, UiSource> ui;

	// Any thread. Keyed by source pointer, see the file comment.
	std::mutex mutex;
	std::map<const obs_source_t *, std::unique_ptr<SourceContext>> contexts;
	std::uint64_t next_serial = 1; // under mutex

	// ---- Any thread -------------------------------------------------------------------------------------------

	// Queues `work` for the UI thread. It runs only if the tracker is still running when it gets there.
	static void post(const std::weak_ptr<Anchor> &weak, std::function<void(Impl &)> work)
	{
		run_on_ui([weak, work = std::move(work)] {
			const std::shared_ptr<Anchor> live = weak.lock();
			if (!live || !live->impl || !live->impl->running)
				return;
			try {
				work(*live->impl);
			} catch (const std::exception &e) {
				obs_log(LOG_ERROR, "source tracker task failed: %s", e.what());
			} catch (...) {
				obs_log(LOG_ERROR, "source tracker task failed: unknown exception");
			}
		});
	}

	// The serial of the context for `source`, creating and connecting it on first sight. 0 if the source can't be
	// tracked. Connects before the context is published, so no callback can see a half-set-up context.
	std::uint64_t register_source(obs_source_t *source)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = contexts.find(source);
		if (found != contexts.end())
			return found->second->serial;

		const char *uuid_text = obs_source_get_uuid(source);
		if (!uuid_text || !*uuid_text) {
			obs_log(LOG_WARNING, "a capture source has no UUID, so it is not tracked");
			return 0;
		}

		// Reserve the slot first: if this throws, nothing has been connected yet.
		const auto slot = contexts.emplace(source, nullptr).first;
		try {
			auto ctx = std::make_unique<SourceContext>();
			ctx->serial = next_serial++;
			ctx->uuid = uuid_text;
			ctx->anchor = anchor;
			ctx->weak = obs_source_get_weak_source(source);
			if (!ctx->weak) {
				contexts.erase(slot);
				return 0;
			}
			connect_signals(obs_source_get_signal_handler(source), ctx.get());
			const std::uint64_t serial = ctx->serial;
			slot->second = std::move(
				ctx); // nothing after the connect can throw, so the context is never half-owned
			return serial;
		} catch (...) {
			contexts.erase(slot);
			throw;
		}
	}

	// Removes and returns the context for `source`, or nullptr. Used by source_destroy.
	std::unique_ptr<SourceContext> take_context(const obs_source_t *source)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = contexts.find(source);
		if (found == contexts.end())
			return nullptr;
		std::unique_ptr<SourceContext> ctx = std::move(found->second);
		contexts.erase(found);
		return ctx;
	}

	// Serials of every registered context, copied out so the caller can work without holding the mutex.
	std::vector<std::uint64_t> registered_serials()
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<std::uint64_t> serials;
		serials.reserve(contexts.size());
		for (const auto &entry : contexts) {
			if (entry.second)
				serials.push_back(entry.second->serial);
		}
		return serials;
	}

	// Caller holds the mutex.
	SourceContext *find_by_serial_locked(std::uint64_t serial)
	{
		for (const auto &entry : contexts) {
			if (entry.second && entry.second->serial == serial)
				return entry.second.get();
		}
		return nullptr;
	}

	// Connect and disconnect use the same callbacks and data pointer, which is how OBS matches them.
	static void connect_signals(signal_handler_t *handler, SourceContext *ctx)
	{
		if (!handler)
			return;
		signal_handler_connect(handler, "hooked", &Impl::on_hooked, ctx);
		signal_handler_connect(handler, "unhooked", &Impl::on_unhooked, ctx);
		signal_handler_connect(handler, "activate", &Impl::on_activate, ctx);
		signal_handler_connect(handler, "deactivate", &Impl::on_deactivate, ctx);
	}

	// Once this returns, none of the four callbacks is running for `ctx`, and none will run again.
	static void disconnect_signals(signal_handler_t *handler, SourceContext *ctx)
	{
		if (!handler)
			return;
		signal_handler_disconnect(handler, "hooked", &Impl::on_hooked, ctx);
		signal_handler_disconnect(handler, "unhooked", &Impl::on_unhooked, ctx);
		signal_handler_disconnect(handler, "activate", &Impl::on_activate, ctx);
		signal_handler_disconnect(handler, "deactivate", &Impl::on_deactivate, ctx);
	}

	// ---- Global signal callbacks (any thread). data is the Impl. ----------------------------------------------

	static void on_source_create(void *data, calldata_t *cd)
	{
		try {
			auto *impl = static_cast<Impl *>(data);
			auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
			if (!source || !is_capture_source(source))
				return;
			const std::uint64_t serial = impl->register_source(source);
			if (serial)
				post(impl->anchor, [serial](Impl &t) { t.request_readout(serial); });
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "source_create handling failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "source_create handling failed: unknown exception");
		}
	}

	// Runs on OBS's destruction thread, before the source's own destroy. The source is still valid here, so its
	// handler can be read. Disconnecting waits for any callback still running on the graphics thread.
	static void on_source_destroy(void *data, calldata_t *cd)
	{
		try {
			auto *impl = static_cast<Impl *>(data);
			auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
			if (!source)
				return;
			std::unique_ptr<SourceContext> ctx = impl->take_context(source);
			if (!ctx)
				return;

			disconnect_signals(obs_source_get_signal_handler(source), ctx.get());
			obs_weak_source_release(ctx->weak);
			const std::string uuid = ctx->uuid;
			const std::uint64_t serial = ctx->serial;
			ctx.reset();
			post(impl->anchor, [uuid, serial](Impl &t) { t.on_removed(uuid, serial); });
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "source_destroy handling failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "source_destroy handling failed: unknown exception");
		}
	}

	static void on_source_rename(void *data, calldata_t *cd)
	{
		try {
			auto *impl = static_cast<Impl *>(data);
			auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
			if (!source)
				return;
			std::string uuid;
			std::uint64_t serial = 0;
			{
				std::lock_guard<std::mutex> lock(impl->mutex);
				const auto found = impl->contexts.find(source);
				if (found == impl->contexts.end())
					return; // not a capture source, which is most renames
				uuid = found->second->uuid;
				serial = found->second->serial;
			}
			const std::string name = copy_string(calldata_string(cd, "new_name"));
			post(impl->anchor, [uuid, serial, name](Impl &t) { t.on_renamed(uuid, serial, name); });
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "source_rename handling failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "source_rename handling failed: unknown exception");
		}
	}

	// ---- Per-source signal callbacks (graphics thread). data is the context. ----------------------------------

	static void on_hooked(void *data, calldata_t *cd)
	{
		auto *ctx = static_cast<SourceContext *>(data);
		try {
			post(ctx->anchor, [uuid = ctx->uuid, serial = ctx->serial, info = hook_info_from(cd)](Impl &t) {
				t.on_hook_event(uuid, serial, true, info);
			});
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "hooked handling failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "hooked handling failed: unknown exception");
		}
	}

	static void on_unhooked(void *data, [[maybe_unused]] calldata_t *cd)
	{
		auto *ctx = static_cast<SourceContext *>(data);
		try {
			post(ctx->anchor, [uuid = ctx->uuid, serial = ctx->serial](Impl &t) {
				t.on_hook_event(uuid, serial, false, HookInfo{});
			});
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "unhooked handling failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "unhooked handling failed: unknown exception");
		}
	}

	static void on_activate(void *data, [[maybe_unused]] calldata_t *cd) { post_live(data, true); }

	static void on_deactivate(void *data, [[maybe_unused]] calldata_t *cd) { post_live(data, false); }

	static void post_live(void *data, bool live)
	{
		auto *ctx = static_cast<SourceContext *>(data);
		try {
			post(ctx->anchor, [uuid = ctx->uuid, serial = ctx->serial, live](Impl &t) {
				t.on_live_event(uuid, serial, live);
			});
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "activate handling failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "activate handling failed: unknown exception");
		}
	}

	// ---- Enumeration ------------------------------------------------------------------------------------------

	// obs_enum_sources() holds OBS's sources mutex while it calls this, so it only keeps a weak reference.
	// Connecting happens after the enumeration returns.
	static bool on_enum_source(void *param, obs_source_t *source)
	{
		auto *args = static_cast<EnumArgs *>(param);
		try {
			if (is_capture_source(source)) {
				WeakHandle weak(obs_source_get_weak_source(source), &obs_weak_source_release);
				args->found->push_back(std::move(weak));
			}
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "enumerating sources failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "enumerating sources failed: unknown exception");
		}
		return true;
	}

	// Registers every capture source that exists now. Safe to call again: known sources are left alone.
	void register_existing()
	{
		if (!running)
			return;
		std::vector<WeakHandle> found;
		EnumArgs args{&found};
		obs_enum_sources(&Impl::on_enum_source, &args);

		for (WeakHandle &weak : found) {
			obs_source_t *source = obs_weak_source_get_source(weak.get());
			if (!source)
				continue; // destroyed since the enumeration
			try {
				register_source(source);
			} catch (...) {
				obs_source_release(source);
				throw;
			}
			obs_source_release(source);
		}
	}

	// ---- Readouts ---------------------------------------------------------------------------------------------

	// UI thread. Queues the graphics readout for one source. Nothing is read here: get_hooked is only safe on the
	// graphics thread.
	void request_readout(std::uint64_t serial)
	{
		if (!running)
			return;
		auto request = std::make_unique<ReadoutRequest>();
		obs_source_t *source = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex);
			SourceContext *ctx = find_by_serial_locked(serial);
			if (!ctx)
				return;
			request->uuid = ctx->uuid;
			// Lock-free in OBS. Under the mutex, so ctx->weak stays valid while we look.
			source = obs_weak_source_get_source(ctx->weak);
		}
		// Already at zero references: its source_destroy removes the entry, so there is nothing to read.
		if (!source)
			return;
		request->anchor = anchor;
		request->serial = serial;
		request->weak = obs_source_get_weak_source(source);
		// Released here, outside the mutex. If this was the last reference, OBS starts destroying the source
		// now, which takes its sources mutex.
		obs_source_release(source);
		// Never called under the mutex: obs_queue_task() can wait for the video task mutex.
		obs_queue_task(OBS_TASK_GRAPHICS, &Impl::graphics_readout, request.release(), false);
	}

	// Graphics thread (OBS_TASK_GRAPHICS). Reads what the UI thread needs and hands the strong reference to
	// the UI thread.
	// The request's weak reference is released here: a weak release does not touch the sources mutex.
	static void graphics_readout(void *param)
	{
		std::unique_ptr<ReadoutRequest> request(static_cast<ReadoutRequest *>(param));
		try {
			obs_source_t *source = obs_weak_source_get_source(request->weak);
			if (source) {
				Readout readout;
				readout.hook = read_hook_state(source);
				readout.live = obs_source_active(source);
				run_on_ui([anchor = request->anchor, serial = request->serial, uuid = request->uuid,
					   readout,
					   source] { hand_back_readout(anchor, serial, uuid, readout, source); });
			}
		} catch (const std::exception &e) {
			// Only bad_alloc gets here. The reference taken before the failure is leaked, not released
			// on this thread, since a last release takes OBS's sources mutex.
			obs_log(LOG_ERROR, "reading a capture source failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "reading a capture source failed: unknown exception");
		}
		if (request->weak)
			obs_weak_source_release(request->weak);
	}

	// UI thread. Applies a readout, then releases the strong reference that the graphics thread handed over. The
	// release happens here even after stop(). Only a task that run_on_ui() drops (after
	// ui_tasks_shutdown()) leaves the reference unreleased.
	static void hand_back_readout(const std::weak_ptr<Anchor> &weak, std::uint64_t serial, const std::string &uuid,
				      const Readout &readout, obs_source_t *source)
	{
		const std::shared_ptr<Anchor> live = weak.lock();
		bool changed = false;
		if (live && live->impl && live->impl->running) {
			try {
				changed = live->impl->apply_readout(serial, uuid, readout, source);
			} catch (const std::exception &e) {
				obs_log(LOG_ERROR, "applying a source readout failed: %s", e.what());
			} catch (...) {
				obs_log(LOG_ERROR, "applying a source readout failed: unknown exception");
			}
		}
		obs_source_release(source);
		if (changed && live && live->impl && live->impl->running)
			live->impl->notify();
	}

	// ---- Frontend (UI thread) ---------------------------------------------------------------------------------

	// obs_frontend_remove_event_callback() is never called. OBS dispatches its callbacks by index, so removing
	// one during a dispatch can run another callback twice or skip it. OBS also releases its frontend API at
	// EXIT (window-basic-main.cpp). The callback therefore stays registered for the life of the process. It
	// does nothing unless the tracker is running, and it still works after the tracker is destroyed, because
	// the gate holds only a weak reference to the anchor.
	static FrontendGate &frontend_gate()
	{
		// Intentionally never freed: OBS keeps the pointer.
		static FrontendGate *gate = new FrontendGate;
		return *gate;
	}

	static void install_frontend_callback(const std::shared_ptr<Anchor> &owner)
	{
		FrontendGate &gate = frontend_gate();
		gate.anchor = owner;
		static bool registered = false; // UI thread only
		if (!registered) {
			obs_frontend_add_event_callback(&Impl::on_frontend_event, &gate);
			registered = true;
		}
	}

	static void on_frontend_event(enum obs_frontend_event event, void *private_data)
	{
		auto *gate = static_cast<FrontendGate *>(private_data);
		const std::shared_ptr<Anchor> live = gate->anchor.lock();
		if (!live || !live->impl || !live->impl->running)
			return;
		Impl &impl = *live->impl;
		try {
			switch (event) {
			case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
				impl.on_collection_changing();
				break;
			case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
			case OBS_FRONTEND_EVENT_FINISHED_LOADING:
				impl.register_existing();
				impl.resync_contexts();
				break;
			default:
				break;
			}
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "frontend event handling failed: %s", e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "frontend event handling failed: unknown exception");
		}
	}

	// ---- UI thread --------------------------------------------------------------------------------------------

	void notify() const
	{
		if (on_changed)
			on_changed();
	}

	bool ignores(const HookInfo &info) const { return ignore && ignore(info); }

	void rebuild_chosen()
	{
		chosen_lower.clear();
		for (const std::string &uuid : settings.chosen_sources) {
			if (!uuid.empty())
				chosen_lower.push_back(ascii_lower(uuid));
		}
	}

	bool is_selected(const std::string &uuid) const
	{
		if (settings.source_selection != SourceSelection::chosen)
			return true;
		const std::string key = ascii_lower(uuid);
		return std::find(chosen_lower.begin(), chosen_lower.end(), key) != chosen_lower.end();
	}

	// The entry for `uuid`, owned by `serial`. A serial that doesn't match means an old source that has been
	// replaced, so its events are ignored. Returns null for an unselected source too.
	UiSource *owned_selected_entry(const std::string &uuid, std::uint64_t serial)
	{
		const auto it = ui.find(uuid);
		if (it == ui.end() || it->second.serial != serial || !it->second.selected)
			return nullptr;
		return &it->second;
	}

	// Returns the entry for `uuid` owned by `serial`, claiming it if another context owned it. A claim first
	// forgets the old owner's state in GameState, since GameState is keyed by UUID too.
	UiSource &claim(const std::string &uuid, std::uint64_t serial, SteadyClock::time_point now, bool &changed)
	{
		const auto it = ui.find(uuid);
		if (it != ui.end()) {
			if (it->second.serial == serial)
				return it->second;
			if (state.source_removed(uuid, now))
				changed = true;
		}
		UiSource &entry = ui[uuid];
		entry = UiSource{};
		entry.serial = serial;
		return entry;
	}

	// Forwards a liveness change to GameState. Returns true if GameState's current game changed.
	bool apply_live(const std::string &uuid, UiSource &entry, bool live, SteadyClock::time_point now)
	{
		if (entry.live == live)
			return false;
		entry.live = live;
		return state.source_live(uuid, live, now);
	}

	// Forwards a hook transition to GameState. A repeat of the same hook is not forwarded, because GameState
	// would count it as a new hook and move this source to the front.
	bool apply_hook(const std::string &uuid, UiSource &entry, bool hooked, const HookInfo &info,
			SteadyClock::time_point now)
	{
		if (hooked) {
			if (entry.hooked && entry.info == info)
				return false;
			HookInfo copy = info;
			entry.info = std::move(copy);
			entry.hooked = true;
			return state.hooked(uuid, entry.info, now);
		}
		if (!entry.hooked)
			return false;
		entry.hooked = false;
		entry.info = HookInfo{};
		return state.unhooked(uuid, now);
	}

	// Brings one source's state into GameState from a graphics readout. Events only report transitions, so
	// this runs when a source first appears, when settings change and after a scene collection change.
	bool sync_source(const std::string &uuid, std::uint64_t serial, obs_source_t *source, const Readout &readout)
	{
		const SteadyClock::time_point now = SteadyClock::now();
		bool changed = false;
		UiSource &entry = claim(uuid, serial, now, changed);
		entry.name = copy_string(obs_source_get_name(source));
		entry.selected = is_selected(uuid);

		if (!entry.selected) {
			if (entry.fed) {
				if (state.source_removed(uuid, now))
					changed = true;
				entry.fed = false;
				entry.live = false;
				entry.hooked = false;
				entry.info = HookInfo{};
			}
			return changed;
		}

		if (apply_live(uuid, entry, readout.live, now))
			changed = true;
		if (apply_hook(uuid, entry, readout.hook.hooked && !ignores(readout.hook.info), readout.hook.info, now))
			changed = true;
		entry.fed = true;
		return changed;
	}

	// UI thread. Applies a graphics readout. Returns whether GameState's current game changed.
	bool apply_readout(std::uint64_t serial, const std::string &uuid, const Readout &readout, obs_source_t *source)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!find_by_serial_locked(serial))
				return false; // stopped, or already gone: its removal is queued
		}
		return sync_source(uuid, serial, source, readout);
	}

	// Asks for a readout of every registered source. Does nothing unless the tracker is running.
	void resync_contexts()
	{
		if (!running)
			return;
		for (const std::uint64_t serial : registered_serials())
			request_readout(serial);
	}

	void on_hook_event(const std::string &uuid, std::uint64_t serial, bool hooked, const HookInfo &info)
	{
		UiSource *entry = owned_selected_entry(uuid, serial);
		if (!entry)
			return;
		const bool effective = hooked && !ignores(info);
		if (apply_hook(uuid, *entry, effective, info, SteadyClock::now()))
			notify();
	}

	void on_live_event(const std::string &uuid, std::uint64_t serial, bool live)
	{
		UiSource *entry = owned_selected_entry(uuid, serial);
		if (!entry)
			return;
		if (apply_live(uuid, *entry, live, SteadyClock::now()))
			notify();
	}

	void on_removed(const std::string &uuid, std::uint64_t serial)
	{
		const auto it = ui.find(uuid);
		if (it == ui.end() || it->second.serial != serial)
			return;
		ui.erase(it);
		if (state.source_removed(uuid, SteadyClock::now()))
			notify();
	}

	void on_renamed(const std::string &uuid, std::uint64_t serial, const std::string &name)
	{
		const auto it = ui.find(uuid);
		if (it != ui.end() && it->second.serial == serial)
			it->second.name = name;
	}

	void on_collection_changing()
	{
		const bool changed = state.clear(SteadyClock::now());
		ui.clear();
		if (changed)
			notify();
	}

	// Disconnects every per-source signal and frees each context, except one whose source is being destroyed.
	// Runs on the UI thread after the global signals are gone, so no source_create or source_destroy
	// callback can be waiting for the mutex.
	void disconnect_all()
	{
		std::map<const obs_source_t *, std::unique_ptr<SourceContext>> taken;
		{
			std::lock_guard<std::mutex> lock(mutex);
			taken.swap(contexts);
		}

		// A source with no strong reference is being destroyed, and its source_destroy callback has not
		// removed this record. Its handler may already be freed, so its signals cannot be disconnected,
		// and freeing the context could let a late callback reach freed memory. The record is kept instead.
		for (auto &entry : taken) {
			SourceContext *ctx = entry.second.get();
			if (!ctx)
				continue;
			obs_source_t *source = obs_weak_source_get_source(ctx->weak);
			if (!source) {
				// Kept on purpose, see above the loop.
				obs_log(LOG_WARNING,
					"a capture source was being destroyed at unload; its tracking record is kept");
				entry.second.release();
				continue;
			}
			disconnect_signals(obs_source_get_signal_handler(source), ctx);
			obs_weak_source_release(ctx->weak);
			obs_source_release(source);
		}
	}
};

// ---- SourceTracker ------------------------------------------------------------------------------------------

SourceTracker::SourceTracker(GameState &state, ChangedCallback on_changed, IgnorePredicate is_ignored)
	: impl_(std::make_unique<Impl>(state, std::move(on_changed), std::move(is_ignored)))
{
}

SourceTracker::~SourceTracker()
{
	stop();
}

void SourceTracker::start(const Settings &settings)
{
	Impl &impl = *impl_;
	if (impl.connected) {
		obs_log(LOG_WARNING, "source tracker is already running; applying the new settings");
		apply_settings(settings);
		return;
	}

	bool cleared = false;
	try {
		impl.settings = settings;
		impl.rebuild_chosen();
		impl.running = true;
		// Set before the first connect, so stop() disconnects whatever did connect if this throws part way.
		impl.connected = true;

		// Global signals first, so no source created from here on is missed. Then the enumeration
		// picks up everything that already exists, and the readouts follow.
		signal_handler_t *global = obs_get_signal_handler();
		signal_handler_connect(global, "source_create", &Impl::on_source_create, &impl);
		signal_handler_connect(global, "source_destroy", &Impl::on_source_destroy, &impl);
		signal_handler_connect(global, "source_rename", &Impl::on_source_rename, &impl);
		Impl::install_frontend_callback(impl.anchor);

		// Start empty: stop() leaves GameState alone, and nothing would remove an entry from an earlier run.
		cleared = impl.state.clear(SteadyClock::now());
		impl.register_existing();
		impl.resync_contexts();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "source tracker failed to start: %s", e.what());
		stop();
	} catch (...) {
		obs_log(LOG_ERROR, "source tracker failed to start: unknown exception");
		stop();
	}

	// Last, so plugin code runs only once everything is connected. It may call stop(), which is safe here.
	if (cleared)
		impl.notify();
}

void SourceTracker::stop()
{
	Impl &impl = *impl_;
	// No early return. running is cleared first, so queued tasks and the frontend callback do nothing from
	// here on. connected is tested on its own, so the drain still runs when running is already clear.
	impl.running = false;
	try {
		if (impl.connected) {
			// Lets pending source_destroy callbacks finish, when OBS's video and audio are running.
			// Otherwise it returns false at once, and disconnect_all() keeps any record whose
			// source is still being destroyed. Callbacks that run then find the tracker stopped.
			// They only disconnect and free their context.
			obs_wait_for_destroy_queue();
			signal_handler_t *global = obs_get_signal_handler();
			signal_handler_disconnect(global, "source_create", &Impl::on_source_create, &impl);
			signal_handler_disconnect(global, "source_destroy", &Impl::on_source_destroy, &impl);
			signal_handler_disconnect(global, "source_rename", &Impl::on_source_rename, &impl);
			impl.connected = false;
		}
		impl.disconnect_all();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "source tracker stop failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "source tracker stop failed: unknown exception");
	}
	impl.ui.clear();
}

void SourceTracker::apply_settings(const Settings &settings)
{
	Impl &impl = *impl_;
	try {
		impl.settings = settings;
		impl.rebuild_chosen();
		impl.resync_contexts(); // does nothing unless running
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "applying source settings failed: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "applying source settings failed: unknown exception");
	}
}

std::vector<TrackedSource> SourceTracker::tracked_sources() const
{
	std::vector<TrackedSource> out;
	for (const auto &entry : impl_->ui) {
		if (!entry.second.selected)
			continue;
		TrackedSource tracked;
		tracked.uuid = entry.first;
		tracked.name = entry.second.name;
		tracked.hooked = entry.second.hooked;
		tracked.live = entry.second.live;
		tracked.info = entry.second.info;
		out.push_back(std::move(tracked));
	}
	return out;
}

} // namespace clip_atlas

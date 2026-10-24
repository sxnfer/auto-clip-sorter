// Clip Atlas — the clip browser dock: the status header, the banners, the game and search filters, the clip list and
// the footer, wired to the scanner, the Shell, the file mover and the plugin's state. docs/handover/M3-dock-spec.md
// says what each part does; clip-dock.hpp says what each public function does.  Work that runs on another thread (a
// scan, a describe, a Shell call, a move) comes back through run_on_ui(). Each hop captures a std::weak_ptr to the
// dock's shared state and locks it only on the UI thread, so the last reference is released there too, and a result
// that arrives after stop() or after the dock is gone does nothing. Widget signals reach the dock the same way.
// clip-filer.cpp uses this design.
#include "clip-dock.hpp"

#include "banner-area.hpp"
#include "clip-index.hpp"
#include "clip-layout.hpp"
#include "clip-list-model.hpp"
#include "clip-list-view.hpp"
#include "clip-routing.hpp"
#include "settings-dialog.hpp"
#include "settings-model.hpp"
#include "shell-ops.hpp"
#include "status-header.hpp"
#include "task-queue.hpp"
#include "ui-thread.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QAction>
#include <QChar>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace {

// The ages ("2 min ago") change once a minute, so a minute is as fine as the list needs while the dock is visible.
constexpr int kAgesIntervalMs = 60 * 1000;

// Every string the dock shows comes from obs_module_text(), through here.
QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// OBS's main window. It owns the dock and is the parent of the dialogs the dock opens.
QWidget *main_window()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

// The replay file prefix of the current profile, read on the UI thread, where the profile lives. "" without a profile:
// then no video is taken for a replay by its name.
std::string profile_replay_prefix()
{
	config_t *profile = obs_frontend_get_profile_config(); // owned by OBS; not freed here
	if (!profile)
		return {};
	const char *prefix = config_get_string(profile, "SimpleOutput", "RecRBPrefix");
	return prefix ? prefix : "";
}

// Windows folder names ignore case, so "Valorant" and "valorant" are one folder. A move into the clip's own folder
// under another case would rename the clip onto itself.
bool same_folder(const QString &a, const QString &b)
{
	return QString::compare(a, b, Qt::CaseInsensitive) == 0;
}

// Runs fn and logs anything it throws. The dock's public functions are called from OBS and Qt, and an exception must
// not leave either of them.
template<typename Fn> void guarded(const char *what, Fn fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "%s failed: %s", what, e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "%s failed: unknown exception", what);
	}
}

} // namespace

// The dock's state. The widgets belong to the dock, which deletes them, and so does the model. Only the UI thread
// touches these. Deferred work reaches them through Shared, which is the one part that other threads hold a pointer to.
struct ClipDock::Impl {
	// Held by the dock and, weakly, by the work it queues.
	struct Shared {
		explicit Shared(ClipDock *owner) : dock(owner) {}

		// Scans and describes run here, never on the UI thread. Nothing on this thread touches a widget.
		TaskQueue queue;
		// Set by stop() on the UI thread, before the queue is joined, so every result from then on finds the
		// dock stopped.
		bool stopped = false;
		// Counts the scans. A result is applied only if its scan is still the newest, so an old folder never
		// comes back.
		std::uint64_t generation = 0;
		// Read on the UI thread only. It is reset when the dock is deleted, which is after stop().
		QPointer<ClipDock> dock;
		// The folder and options of the newest scan. A describe uses them too.
		std::filesystem::path base;
		ScanOptions options;
	};

	Impl(ClipDock *dock, const GameState &state, SettingsStore &settings, FileMover &mover);

	// The dock's Impl while the dock exists and has not been stopped, otherwise nullptr. The weak_ptr is locked
	// here, and only on the UI thread, so the last shared_ptr is never released on a worker thread.
	static Impl *live_impl(const std::weak_ptr<Shared> &weak)
	{
		const std::shared_ptr<Shared> shared = weak.lock();
		if (!shared || shared->stopped)
			return nullptr;
		ClipDock *dock = shared->dock.data();
		return dock ? dock->impl_.get() : nullptr;
	}

	// Calls fn with the live Impl, on the UI thread. An exception is logged here, so it never reaches Qt or OBS.
	template<typename Fn> static void with_live(const std::weak_ptr<Shared> &weak, const char *what, Fn fn)
	{
		try {
			if (Impl *impl = live_impl(weak))
				fn(*impl);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "%s failed: %s", what, e.what());
		} catch (...) {
			obs_log(LOG_ERROR, "%s failed: unknown exception", what);
		}
	}

	bool live() const { return !shared_->stopped; }

	// Lifecycle and the OBS events.
	void start();
	void stop();
	void on_shown();
	void on_hidden();
	void apply_settings(const Settings &settings);
	void refresh_if_changed();
	void on_replay_status(ReplayBufferStatus status);
	void update_replay_banner();
	void show_game();
	void on_clip_sorted(const ClipSorted &clip);

	// Scanning.
	void start_scan();
	void apply_scan(ScanResult &&result);
	void apply_folder_banners(bool base_exists);

	// Describing one clip and putting it in the list.
	void describe_and_upsert(const std::filesystem::path &path, ClipKind kind, bool reselect);
	void apply_described(std::optional<ClipEntry> entry, const std::filesystem::path &path, ClipKind kind,
			     bool reselect);

	// The list and its filters.
	void after_list_change();
	void rebuild_game_combo();
	void apply_filter();
	void update_empty_state();
	std::optional<ClipEntry> selected_clip() const;
	std::optional<std::filesystem::path> selected_path() const;
	void select_path(const std::optional<std::filesystem::path> &path);

	// The clip actions.
	void show_context_menu(const QPoint &pos);
	void open_clip(const ClipEntry &clip);
	void reveal_clip(const ClipEntry &clip);
	void rename_clip(const ClipEntry &clip);
	void finish_rename(const std::filesystem::path &path, ClipKind kind, const std::string &error,
			   const std::string &new_path_utf8);
	void move_clip(const ClipEntry &clip);
	void finish_move(const std::filesystem::path &path, ClipKind kind, MoveStatus status,
			 const std::filesystem::path &final_path, const std::string &error);
	void recycle_clip(const ClipEntry &clip);
	void finish_recycle(const std::filesystem::path &path, const std::string &error);
	void open_folder();
	void open_settings();
	QStringList move_targets(const std::string &own_game) const;
	ShellOps::Done report_failure(const char *what);
	void show_error(const char *key, const std::string &reason, const char *what);

	ClipDock *dock_;
	const GameState &state_;
	SettingsStore &settings_;
	FileMover &mover_;
	std::shared_ptr<Shared> shared_;

	StatusHeader *header_ = nullptr;
	BannerArea *banners_ = nullptr;
	QComboBox *game_combo_ = nullptr;
	QLineEdit *search_ = nullptr;
	QStackedWidget *stack_ = nullptr;
	ClipListView *list_ = nullptr;
	QLabel *empty_label_ = nullptr;
	QPushButton *open_folder_ = nullptr;
	QTimer *ages_timer_ = nullptr;
	ClipListModel *model_ = nullptr;

	// Set by start(), which does nothing after its first call.
	bool started_ = false;
	ReplayBufferStatus replay_status_ = ReplayBufferStatus::inactive;
	// What the newest scan used. A change to any of these needs a new scan.
	std::string scanned_base_;
	std::string scanned_prefix_;
	bool scanned_monthly_ = true;
	std::string scanned_unsorted_ = "Unsorted";
};

ClipDock::Impl::Impl(ClipDock *dock, const GameState &state, SettingsStore &settings, FileMover &mover)
	: dock_(dock),
	  state_(state),
	  settings_(settings),
	  mover_(mover),
	  shared_(std::make_shared<Shared>(dock))
{
	auto *root = new QVBoxLayout(dock_);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(4);

	header_ = new StatusHeader(dock_);
	root->addWidget(header_);
	banners_ = new BannerArea(dock_);
	root->addWidget(banners_);

	// The game combo is sized by a fixed number of characters rather than its longest name, so one long game name
	// can't widen the whole dock.
	game_combo_ = new QComboBox(dock_);
	game_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	game_combo_->setMinimumContentsLength(16);
	search_ = new QLineEdit(dock_);
	search_->setPlaceholderText(module_text("Dock.Search.Placeholder"));
	search_->setClearButtonEnabled(true);
	auto *filter_row = new QHBoxLayout;
	filter_row->addWidget(game_combo_);
	filter_row->addWidget(search_, 1);
	root->addLayout(filter_row);

	// The list shows the clips. The message takes its place when there is nothing to list.
	stack_ = new QStackedWidget(dock_);
	list_ = new ClipListView(stack_);
	model_ = new ClipListModel(dock_);
	list_->setModel(model_);
	stack_->addWidget(list_);

	empty_label_ = new QLabel(stack_);
	empty_label_->setWordWrap(true);
	empty_label_->setAlignment(Qt::AlignCenter);
	stack_->addWidget(empty_label_);
	root->addWidget(stack_, 1);

	// Footer: the folder button on the left, then Refresh and the settings button.
	open_folder_ = new QPushButton(module_text("Dock.OpenFolder"), dock_);
	// Nothing is scanned yet, so there is no folder to open.
	open_folder_->setEnabled(false);
	auto *refresh = new QToolButton(dock_);
	refresh->setIcon(dock_->style()->standardIcon(QStyle::SP_BrowserReload));
	refresh->setAutoRaise(true);
	refresh->setToolTip(module_text("Dock.Refresh"));
	refresh->setAccessibleName(module_text("Dock.Refresh"));
	auto *settings_button = new QToolButton(dock_);
	// U+2699 GEAR. A glyph rather than an icon, because the standard icons don't include a settings gear.
	settings_button->setText(QString(QChar(0x2699)));
	settings_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
	settings_button->setAutoRaise(true);
	settings_button->setToolTip(module_text("Dock.Settings"));
	settings_button->setAccessibleName(module_text("Dock.Settings"));
	auto *footer = new QHBoxLayout;
	footer->addWidget(open_folder_);
	footer->addStretch(1);
	footer->addWidget(refresh);
	footer->addWidget(settings_button);
	root->addLayout(footer);

	ages_timer_ = new QTimer(dock_);
	ages_timer_->setInterval(kAgesIntervalMs);

	const std::weak_ptr<Shared> weak = shared_;
	QObject::connect(game_combo_, &QComboBox::currentIndexChanged, dock_,
			 [weak](int) { with_live(weak, "game filter", [](Impl &impl) { impl.apply_filter(); }); });
	QObject::connect(search_, &QLineEdit::textChanged, dock_, [weak](const QString &) {
		with_live(weak, "search", [](Impl &impl) { impl.apply_filter(); });
	});
	QObject::connect(list_, &QAbstractItemView::activated, dock_, [weak](const QModelIndex &index) {
		with_live(weak, "opening a clip", [&index](Impl &impl) {
			if (const ClipEntry *clip = impl.model_->clip_at(index.row()))
				impl.open_clip(*clip);
		});
	});
	QObject::connect(list_, &QWidget::customContextMenuRequested, dock_, [weak](const QPoint &pos) {
		with_live(weak, "the clip menu", [&pos](Impl &impl) { impl.show_context_menu(pos); });
	});
	// Delete and F2 act on the selected clip while the list, or a child of it, has the focus. The shortcuts are
	// owned by the list.
	new QShortcut(
		QKeySequence::Delete, list_,
		[weak] {
			with_live(weak, "deleting a clip", [](Impl &impl) {
				if (const std::optional<ClipEntry> clip = impl.selected_clip())
					impl.recycle_clip(*clip);
			});
		},
		Qt::WidgetWithChildrenShortcut);
	new QShortcut(
		QKeySequence(Qt::Key_F2), list_,
		[weak] {
			with_live(weak, "renaming a clip", [](Impl &impl) {
				if (const std::optional<ClipEntry> clip = impl.selected_clip())
					impl.rename_clip(*clip);
			});
		},
		Qt::WidgetWithChildrenShortcut);
	QObject::connect(ages_timer_, &QTimer::timeout, dock_, [weak] {
		with_live(weak, "refreshing the ages", [](Impl &impl) { impl.model_->refresh_day(); });
	});
	QObject::connect(open_folder_, &QPushButton::clicked, dock_, [weak] {
		with_live(weak, "opening the clips folder", [](Impl &impl) { impl.open_folder(); });
	});
	QObject::connect(refresh, &QToolButton::clicked, dock_,
			 [weak] { with_live(weak, "refreshing the list", [](Impl &impl) { impl.start_scan(); }); });
	QObject::connect(settings_button, &QToolButton::clicked, dock_,
			 [weak] { with_live(weak, "opening the settings", [](Impl &impl) { impl.open_settings(); }); });

	// The switch saves the setting, and the dock is updated through apply_settings() like every other listener.
	header_->set_auto_sort_callback([weak, store = &settings_](bool on) {
		with_live(weak, "setting auto-sort", [store, on](Impl &) {
			Settings changed = store->get();
			changed.auto_sort = on;
			store->set(changed);
		});
	});
	header_->set_auto_sort(settings_.get().auto_sort);

	// The empty list and "All games" until the first scan.
	rebuild_game_combo();
	update_empty_state();
}

// --- lifecycle and OBS events ---------------------------------------------------------------------------------------

void ClipDock::Impl::start()
{
	if (!live() || started_)
		return;
	started_ = true;
	show_game();
	start_scan();
}

void ClipDock::Impl::stop()
{
	if (shared_->stopped)
		return;
	// Set first, so every result that runs from here on finds the dock stopped.
	shared_->stopped = true;
	shared_->queue.shutdown();
	ages_timer_->stop();
	model_->shutdown();
}

void ClipDock::Impl::on_shown()
{
	if (!live())
		return;
	// OBS has no event for its recording folder changing, so a dock that was hidden checks the folder when it
	// shows.
	refresh_if_changed();
	// The ages are right at once. The timer keeps them right while the dock stays visible.
	model_->refresh_day();
	ages_timer_->start();
}

void ClipDock::Impl::on_hidden()
{
	ages_timer_->stop();
}

void ClipDock::Impl::apply_settings(const Settings &settings)
{
	if (!live())
		return;
	header_->set_auto_sort(settings.auto_sort);
	update_replay_banner();
	refresh_if_changed();
}

// Scans again only when something the scan depends on changed. The folder and the prefix come from outside the
// settings (OBS's recording folder, the profile), so they are compared rather than trusted to a change event.
void ClipDock::Impl::refresh_if_changed()
{
	if (!started_ || !live())
		return;
	const Settings &settings = settings_.get();
	const bool unchanged =
		settings_.effective_clips_folder() == scanned_base_ && profile_replay_prefix() == scanned_prefix_ &&
		settings.monthly_folders == scanned_monthly_ && settings.unsorted_folder == scanned_unsorted_;
	if (!unchanged)
		start_scan();
}

void ClipDock::Impl::on_replay_status(ReplayBufferStatus status)
{
	if (!live())
		return;
	replay_status_ = status;
	header_->set_replay_status(status);
	update_replay_banner();
}

// Shown only to a user who asked for the buffer to start with a game. Without that wish, a stopped buffer is expected.
void ClipDock::Impl::update_replay_banner()
{
	if (replay_status_ == ReplayBufferStatus::disabled_in_obs && settings_.get().replay_auto_start) {
		banners_->show_banner(BannerId::replay_buffer_disabled, BannerKind::warning,
				      module_text("Dock.Banner.ReplayDisabled"));
	} else {
		banners_->hide_banner(BannerId::replay_buffer_disabled);
	}
}

void ClipDock::Impl::show_game()
{
	if (!live())
		return;
	const ClipGame game = clip_game_for(state_.current(), settings_.get());
	header_->set_game(QString::fromStdString(game.name));
}

void ClipDock::Impl::on_clip_sorted(const ClipSorted &clip)
{
	if (!live())
		return;
	// A file that was neither moved nor copied is still where the list already shows it.
	if (clip.status != MoveStatus::moved && clip.status != MoveStatus::copied_source_kept)
		return;
	describe_and_upsert(clip.final_path, clip.kind, false);
}

// --- scanning -------------------------------------------------------------------------------------------------------

void ClipDock::Impl::start_scan()
{
	if (!started_ || !live())
		return;
	const Settings &settings = settings_.get();
	const std::string base_utf8 = settings_.effective_clips_folder();
	const std::string prefix = profile_replay_prefix();
	// Recorded now, so refresh_if_changed() compares against the folder this scan lists.
	scanned_base_ = base_utf8;
	scanned_prefix_ = prefix;
	scanned_monthly_ = settings.monthly_folders;
	scanned_unsorted_ = settings.unsorted_folder;
	// A new generation makes every result still on its way from an older scan stale.
	const std::uint64_t generation = ++shared_->generation;

	if (base_utf8.empty()) {
		// No folder to list. The list is emptied, and the banner says where the folder is set.
		shared_->base.clear();
		model_->reset_clips({});
		apply_folder_banners(true);
		after_list_change();
		return;
	}

	shared_->base = path_from_utf8(base_utf8);
	ScanOptions options;
	options.replay_prefix = prefix;
	shared_->options = options;
	const std::filesystem::path base = shared_->base;
	const std::weak_ptr<Shared> weak = shared_;
	shared_->queue.post([weak, generation, base, options]() {
		ScanResult result;
		try {
			result = scan_clips_folder(base, options);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "scanning the clips folder failed: %s", e.what());
			return;
		} catch (...) {
			obs_log(LOG_ERROR, "scanning the clips folder failed: unknown exception");
			return;
		}
		run_on_ui([weak, generation, result = std::move(result)]() mutable {
			with_live(weak, "applying a scan", [&](Impl &impl) {
				if (impl.shared_->generation == generation)
					impl.apply_scan(std::move(result));
			});
		});
	});
}

void ClipDock::Impl::apply_scan(ScanResult &&result)
{
	// The scan replaces every row, so the selection is carried over by path.
	const std::optional<std::filesystem::path> selected = selected_path();
	model_->reset_clips(std::move(result.clips));
	select_path(selected);
	apply_folder_banners(result.base_exists);
	after_list_change();
}

// Banners for the folder that the newest scan lists. The "no folder" case needs no scan, so it is shown from
// start_scan() too. A missing folder is known only from the scan, because checking it would touch the disk here.
void ClipDock::Impl::apply_folder_banners(bool base_exists)
{
	const bool no_folder = scanned_base_.empty();
	if (no_folder) {
		banners_->hide_banner(BannerId::clips_folder_missing);
		const std::weak_ptr<Shared> weak = shared_;
		banners_->show_banner(
			BannerId::no_clips_folder, BannerKind::warning, module_text("Dock.Banner.NoFolder"),
			module_text("Dock.Banner.OpenSettings"),
			[weak] { with_live(weak, "opening the settings", [](Impl &impl) { impl.open_settings(); }); });
	} else if (!base_exists) {
		banners_->hide_banner(BannerId::no_clips_folder);
		banners_->show_banner(
			BannerId::clips_folder_missing, BannerKind::warning,
			module_text("Dock.Banner.FolderMissing").arg(QString::fromStdString(scanned_base_)));
	} else {
		banners_->hide_banner(BannerId::no_clips_folder);
		banners_->hide_banner(BannerId::clips_folder_missing);
	}
	open_folder_->setEnabled(!no_folder);
}

// --- describing one clip --------------------------------------------------------------------------------------------

// The file was filed, renamed or moved in, so it is described on the queue and then added to the list. The queue is
// first-in first-out, so a scan that was queued earlier still lands before this describe, and a later scan includes it.
void ClipDock::Impl::describe_and_upsert(const std::filesystem::path &path, ClipKind kind, bool reselect)
{
	// Nothing is listed until the first scan, so there is nothing to add the clip to.
	if (!live() || shared_->base.empty())
		return;
	const std::filesystem::path base = shared_->base;
	const ScanOptions options = shared_->options;
	const std::weak_ptr<Shared> weak = shared_;
	shared_->queue.post([weak, base, options, path, kind, reselect]() {
		std::optional<ClipEntry> entry;
		try {
			entry = describe_clip(base, path, options);
		} catch (const std::exception &e) {
			obs_log(LOG_ERROR, "reading %s failed: %s", path_to_utf8(path).c_str(), e.what());
			return;
		} catch (...) {
			obs_log(LOG_ERROR, "reading %s failed: unknown exception", path_to_utf8(path).c_str());
			return;
		}
		run_on_ui([weak, entry = std::move(entry), path, kind, reselect]() mutable {
			with_live(weak, "adding a clip",
				  [&](Impl &impl) { impl.apply_described(std::move(entry), path, kind, reselect); });
		});
	});
}

void ClipDock::Impl::apply_described(std::optional<ClipEntry> entry, const std::filesystem::path &path, ClipKind kind,
				     bool reselect)
{
	// Nothing to add: the file is gone, or it is no longer a clip a scan would list.
	if (!entry)
		return;
	// The caller knows the exact kind. A file's name only suggests it, and a clip filed in this session is certain.
	entry->kind = kind;
	const std::optional<std::filesystem::path> selected = selected_path();
	model_->upsert_clip(std::move(*entry));
	if (reselect)
		select_path(path);
	else
		select_path(selected);
	after_list_change();
}

// --- the list and its filters ---------------------------------------------------------------------------------------

void ClipDock::Impl::after_list_change()
{
	rebuild_game_combo();
	update_empty_state();
}

void ClipDock::Impl::rebuild_game_combo()
{
	const ClipQuery query = model_->query();
	const ClipIndex &index = model_->index();
	bool choice_gone = false;
	{
		// Refilling the combo would otherwise filter the list at every step. The choice is applied once, below.
		const QSignalBlocker blocker(game_combo_);
		game_combo_->clear();
		// "All games" has no data: an invalid QVariant means no game filter.
		game_combo_->addItem(module_text("Dock.Filter.AllGames"));
		for (const std::string &game : index.games()) {
			const QString name = QString::fromStdString(game);
			game_combo_->addItem(name, name);
		}
		// "Not sorted" filters on "": the clips directly in the folder.
		if (index.has_unfiled())
			game_combo_->addItem(module_text("Dock.Game.Unfiled"), QString());

		// The choice is kept while its game still exists. Item 0 is "All games", so the search starts at item
		// 1.
		int choice = 0;
		if (query.game_folder) {
			const QString wanted = QString::fromStdString(*query.game_folder);
			choice = -1;
			for (int i = 1; i < game_combo_->count() && choice < 0; ++i) {
				if (game_combo_->itemData(i).toString() == wanted)
					choice = i;
			}
			if (choice < 0) {
				choice = 0;
				choice_gone = true;
			}
		}
		game_combo_->setCurrentIndex(choice);
	}
	// The list still filters on the game that went away, so the filter follows the combo back to all games.
	if (choice_gone)
		apply_filter();
}

// Builds the query from the two filter widgets. The selection is carried over by path, because the filter resets the
// model.
void ClipDock::Impl::apply_filter()
{
	ClipQuery query;
	const QVariant game = game_combo_->currentData();
	if (game.isValid())
		query.game_folder = game.toString().toUtf8().toStdString();
	query.search = search_->text().toUtf8().toStdString();
	const std::optional<std::filesystem::path> selected = selected_path();
	model_->set_query(std::move(query));
	select_path(selected);
	update_empty_state();
}

void ClipDock::Impl::update_empty_state()
{
	if (model_->rowCount() > 0) {
		stack_->setCurrentWidget(list_);
		return;
	}
	// Two messages: nothing has been scanned into the index, or the filter hides everything in it.
	empty_label_->setText(model_->index().clips().empty() ? module_text("Dock.Empty.NoClips")
							      : module_text("Dock.Empty.NoMatch"));
	stack_->setCurrentWidget(empty_label_);
}

std::optional<ClipEntry> ClipDock::Impl::selected_clip() const
{
	const QModelIndex current = list_->currentIndex();
	if (!current.isValid())
		return std::nullopt;
	const ClipEntry *clip = model_->clip_at(current.row());
	if (!clip)
		return std::nullopt;
	return *clip;
}

std::optional<std::filesystem::path> ClipDock::Impl::selected_path() const
{
	const std::optional<ClipEntry> clip = selected_clip();
	if (!clip)
		return std::nullopt;
	return clip->path;
}

void ClipDock::Impl::select_path(const std::optional<std::filesystem::path> &path)
{
	if (!path)
		return;
	const int row = model_->row_of(*path);
	if (row >= 0)
		list_->setCurrentIndex(list_->model()->index(row, 0));
}

// --- the clip actions -----------------------------------------------------------------------------------------------

void ClipDock::Impl::show_context_menu(const QPoint &pos)
{
	const QModelIndex index = list_->indexAt(pos);
	if (!index.isValid())
		return;
	const ClipEntry *found = model_->clip_at(index.row());
	if (!found)
		return;
	// A copy: the menu runs its own event loop, and the model may change while it is open.
	const ClipEntry clip = *found;
	list_->setCurrentIndex(index);

	// No parent, so the menu is destroyed when this function returns and never by its parent.
	QMenu menu;
	QAction *open = menu.addAction(module_text("Dock.Action.Open"));
	QAction *reveal = menu.addAction(module_text("Dock.Action.Reveal"));
	menu.addSeparator();
	QAction *rename = menu.addAction(module_text("Dock.Action.Rename"));
	QAction *move = menu.addAction(module_text("Dock.Action.MoveToGame"));
	menu.addSeparator();
	QAction *recycle = menu.addAction(module_text("Dock.Action.Recycle"));

	const std::weak_ptr<Shared> weak = shared_;
	QAction *chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
	// The menu's event loop may have stopped or deleted the dock, so the dock is looked up again before anything
	// else.
	Impl *self = live_impl(weak);
	if (!self || !chosen)
		return;
	if (chosen == open)
		self->open_clip(clip);
	else if (chosen == reveal)
		self->reveal_clip(clip);
	else if (chosen == rename)
		self->rename_clip(clip);
	else if (chosen == move)
		self->move_clip(clip);
	else if (chosen == recycle)
		self->recycle_clip(clip);
}

void ClipDock::Impl::open_clip(const ClipEntry &clip)
{
	shell_ops().open(path_to_utf8(clip.path), report_failure("open failed"));
}

void ClipDock::Impl::reveal_clip(const ClipEntry &clip)
{
	shell_ops().reveal(path_to_utf8(clip.path), report_failure("reveal failed"));
}

void ClipDock::Impl::rename_clip(const ClipEntry &clip)
{
	if (!live())
		return;
	const std::filesystem::path path = clip.path;
	const ClipKind kind = clip.kind;
	const QString stem = QString::fromStdString(path_to_utf8(path.stem()));
	bool ok = false;
	const QString typed = QInputDialog::getText(dock_, module_text("Dock.Rename.Title"),
						    module_text("Dock.Rename.Label"), QLineEdit::Normal, stem, &ok);
	// Cancelled, or OK with the same name (spaces at the ends don't count): there is nothing to rename.
	if (!ok || typed.trimmed() == stem)
		return;
	const std::optional<std::string> new_name = renamed_file_name(path, typed.toUtf8().toStdString());
	if (!new_name) {
		QMessageBox::warning(dock_, module_text("Dock.Error.Title"), module_text("Dock.Error.BadName"));
		return;
	}
	const std::weak_ptr<Shared> weak = shared_;
	shell_ops().rename(path_to_utf8(path), *new_name,
			   [weak, path, kind](const std::string &error, const std::string &new_path_utf8) {
				   run_on_ui([weak, path, kind, error, new_path_utf8] {
					   with_live(weak, "finishing a rename", [&](Impl &impl) {
						   impl.finish_rename(path, kind, error, new_path_utf8);
					   });
				   });
			   });
}

void ClipDock::Impl::finish_rename(const std::filesystem::path &path, ClipKind kind, const std::string &error,
				   const std::string &new_path_utf8)
{
	if (!error.empty()) {
		show_error("Dock.Error.RenameFailed", error, "rename failed");
		return;
	}
	// The renamed file keeps its place in the list: the old row goes, and the new name is described and added in
	// its place.
	model_->remove_clip(path);
	after_list_change();
	describe_and_upsert(path_from_utf8(new_path_utf8), kind, true);
}

void ClipDock::Impl::move_clip(const ClipEntry &clip)
{
	if (!live())
		return;
	const std::filesystem::path path = clip.path;
	const ClipKind kind = clip.kind;
	const std::string own_game = clip.game_folder;
	// The folder is taken before the dialog, which runs its own event loop: a folder change made while it is open
	// must not redirect the move.
	const std::filesystem::path base = shared_->base;
	bool ok = false;
	const QString typed = QInputDialog::getItem(dock_, module_text("Dock.Move.Title"),
						    module_text("Dock.Move.Label"), move_targets(own_game), 0, true,
						    &ok);
	const QString name = typed.trimmed();
	// Cancelled, empty, or the clip's own folder: nothing moves.
	if (!ok || name.isEmpty() || same_folder(name, QString::fromStdString(own_game)))
		return;

	const Settings settings = settings_.get();
	const std::filesystem::path dest =
		destination_dir(base, name.toUtf8().toStdString(), year_month_of(clip.modified),
				LayoutOptions{settings.monthly_folders, settings.unsorted_folder});
	const std::weak_ptr<Shared> weak = shared_;
	mover_.move(path, dest, [weak, path, kind](const MoveResult &result) {
		// The mover calls this on its own thread, or on this one once it has shut down. Only plain values are
		// copied out.
		const MoveStatus status = result.status;
		const std::filesystem::path final_path = result.final_path;
		const std::string error = result.error;
		run_on_ui([weak, path, kind, status, final_path, error] {
			with_live(weak, "finishing a move",
				  [&](Impl &impl) { impl.finish_move(path, kind, status, final_path, error); });
		});
	});
}

void ClipDock::Impl::finish_move(const std::filesystem::path &path, ClipKind kind, MoveStatus status,
				 const std::filesystem::path &final_path, const std::string &error)
{
	switch (status) {
	case MoveStatus::moved:
		// The clip is in its new folder now. The filter decides whether it is listed, and the describe checks
		// that it is still a clip.
		model_->remove_clip(path);
		after_list_change();
		describe_and_upsert(final_path, kind, false);
		break;
	case MoveStatus::copied_source_kept:
		// The source could not be removed, so both copies are listed.
		describe_and_upsert(final_path, kind, false);
		break;
	case MoveStatus::failed:
	case MoveStatus::cancelled:
		show_error("Dock.Error.MoveFailed", error, "move failed");
		break;
	}
}

void ClipDock::Impl::recycle_clip(const ClipEntry &clip)
{
	if (!live())
		return;
	const std::filesystem::path path = clip.path;
	const QMessageBox::StandardButton answer =
		QMessageBox::question(dock_, module_text("Dock.Confirm.RecycleTitle"),
				      module_text("Dock.Confirm.Recycle").arg(QString::fromStdString(clip.file_name)),
				      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer != QMessageBox::Yes)
		return;
	const std::weak_ptr<Shared> weak = shared_;
	shell_ops().recycle(path_to_utf8(path), [weak, path](const std::string &error) {
		run_on_ui([weak, path, error] {
			with_live(weak, "finishing a recycle", [&](Impl &impl) { impl.finish_recycle(path, error); });
		});
	});
}

void ClipDock::Impl::finish_recycle(const std::filesystem::path &path, const std::string &error)
{
	if (!error.empty()) {
		show_error("Dock.Error.RecycleFailed", error, "recycle failed");
		return;
	}
	model_->remove_clip(path);
	after_list_change();
}

void ClipDock::Impl::open_folder()
{
	if (!live() || shared_->base.empty())
		return;
	shell_ops().open_folder(path_to_utf8(shared_->base), report_failure("open folder failed"));
}

void ClipDock::Impl::open_settings()
{
	if (!live())
		return;
	open_settings_dialog(settings_, main_window());
}

// The folders the move dialog offers: the games in the index and the game being played, without the clip's own folder,
// sorted without regard to case (as the game list is). An exact duplicate is listed once.
QStringList ClipDock::Impl::move_targets(const std::string &own_game) const
{
	std::vector<std::string> games = model_->index().games();
	const ClipGame current = clip_game_for(state_.current(), settings_.get());
	if (!current.unsorted())
		games.push_back(current.name);

	const QString own = QString::fromStdString(own_game);
	QStringList targets;
	for (const std::string &game : games) {
		const QString name = QString::fromStdString(game);
		if (!same_folder(name, own))
			targets << name;
	}
	std::sort(targets.begin(), targets.end(), [](const QString &a, const QString &b) {
		const int order = QString::compare(a, b, Qt::CaseInsensitive);
		return order != 0 ? order < 0 : a < b;
	});
	targets.removeDuplicates();
	return targets;
}

// The callback for a Shell call that either works or fails. It runs on the ShellOps thread, so a failure goes to the UI
// thread, and a success does nothing.
ShellOps::Done ClipDock::Impl::report_failure(const char *what)
{
	const std::weak_ptr<Shared> weak = shared_;
	return [weak, what](const std::string &error) {
		if (error.empty())
			return;
		run_on_ui([weak, what, error] {
			with_live(weak, what,
				  [&](Impl &impl) { impl.show_error("Dock.Error.ActionFailed", error, what); });
		});
	};
}

// Logs a failed action and tells the user. `key` is the locale text that comes before the reason.
void ClipDock::Impl::show_error(const char *key, const std::string &reason, const char *what)
{
	obs_log(LOG_WARNING, "%s: %s", what, reason.c_str());
	QMessageBox::warning(dock_, module_text("Dock.Error.Title"),
			     module_text(key) + QLatin1Char('\n') + QString::fromStdString(reason));
}

ClipDock::ClipDock(const GameState &state, SettingsStore &settings, FileMover &mover, QWidget *parent)
	: QWidget(parent),
	  impl_(std::make_unique<Impl>(this, state, settings, mover))
{
}

ClipDock::~ClipDock()
{
	stop();
}

void ClipDock::start()
{
	guarded("starting the clip browser", [this] { impl_->start(); });
}

void ClipDock::stop()
{
	guarded("stopping the clip browser", [this] { impl_->stop(); });
}

void ClipDock::apply_settings(const Settings &settings)
{
	guarded("applying the settings to the clip browser", [this, &settings] { impl_->apply_settings(settings); });
}

void ClipDock::on_profile_changed()
{
	guarded("profile change in the clip browser", [this] { impl_->refresh_if_changed(); });
}

void ClipDock::on_game_changed()
{
	guarded("game change in the clip browser", [this] { impl_->show_game(); });
}

void ClipDock::on_replay_status(ReplayBufferStatus status)
{
	guarded("replay status in the clip browser", [this, status] { impl_->on_replay_status(status); });
}

void ClipDock::on_clip_sorted(const ClipSorted &clip)
{
	guarded("clip sorted in the clip browser", [this, &clip] { impl_->on_clip_sorted(clip); });
}

BannerArea *ClipDock::banners()
{
	return impl_->live() ? impl_->banners_ : nullptr;
}

void ClipDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	guarded("showing the clip browser", [this] { impl_->on_shown(); });
}

void ClipDock::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	guarded("hiding the clip browser", [this] { impl_->on_hidden(); });
}

ClipDock *register_clip_dock(const GameState &state, SettingsStore &settings, FileMover &mover)
{
	try {
		auto dock = std::make_unique<ClipDock>(state, settings, mover, nullptr);
		if (!obs_frontend_add_dock_by_id("clip-atlas-dock", obs_module_text("Dock.Title"), dock.get())) {
			// The widget is still ours, so the unique_ptr deletes it.
			obs_log(LOG_WARNING, "the clip browser dock was not added: OBS refused the id clip-atlas-dock");
			return nullptr;
		}
		// OBS owns the widget from here on.
		return dock.release();
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "the clip browser dock could not be created: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "the clip browser dock could not be created: unknown exception");
	}
	return nullptr;
}

} // namespace clip_atlas

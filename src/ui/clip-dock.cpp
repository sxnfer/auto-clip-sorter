// Clip Atlas — the clip browser dock: the status header, the banners, the filters, the clip list, the preview, the toast
// and the footer, wired to the scanner, the Shell, the file mover and the plugin's state. docs/handover/M6-dock-spec.md
// says what each part does; clip-dock.hpp says what each public function does. Work that runs on another thread (a
// scan, a describe, a Shell call, a move) comes back through run_on_ui(). Each hop captures a std::weak_ptr to the
// dock's shared state and locks it only on the UI thread, so the last reference is released there too, and a result
// that arrives after stop() or after the dock is gone does nothing. Widget signals reach the dock the same way.
// clip-filer.cpp uses this design.
#include "clip-dock.hpp"

#include "banner-area.hpp"
#include "clip-display.hpp"
#include "clip-index.hpp"
#include "clip-layout.hpp"
#include "clip-list-model.hpp"
#include "clip-list-view.hpp"
#include "clip-preview.hpp"
#include "clip-routing.hpp"
#include "dock-controls.hpp"
#include "dock-layout.hpp"
#include "dock-style.hpp"
#include "dock-toast.hpp"
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
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QList>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clip_atlas {

namespace {

// The groups change at midnight, so a minute is as fine as the list needs while the dock is visible.
constexpr int kDayIntervalMs = 60 * 1000;
// The search field's icon, and the widest the field gets beside the chips in the bottom row.
constexpr int kSearchIconPx = 16;
constexpr int kRowSearchWidth = 220;
// The smallest the dock gets, in any layout.
constexpr int kMinimumWidth = 200;
constexpr int kMinimumHeight = 120;

// Every string the dock shows comes from obs_module_text(), through here.
QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// The core's strings are UTF-8 std::strings. The explicit length keeps an embedded NUL from cutting the text short.
QString utf8(const std::string &text)
{
	return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// U+203A SINGLE RIGHT-POINTING ANGLE QUOTATION MARK between spaces, between the folders of a path. A code point, so
// the source stays plain ASCII.
QString path_separator()
{
	return QStringLiteral(" ") + QChar(0x203A) + QStringLiteral(" ");
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

	// A describe for a clip that was just filed: it gets the New mark, and the toast says where it went.
	struct Filing {
		MoveStatus status = MoveStatus::moved;
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

	// Building and arranging the widgets.
	void build_widgets();
	void connect_widgets();
	void apply_layout(DockLayout layout);
	void layout_column();
	void layout_row();
	void layout_wide();
	void on_resized();
	void place_toast();
	void refresh_look();

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
	void save_replay();

	// Scanning.
	void start_scan();
	void apply_scan(ScanResult &&result);
	void apply_folder_banners(bool base_exists);

	// Describing one clip and putting it in the list.
	void describe_and_upsert(const std::filesystem::path &path, ClipKind kind, bool reselect,
				 std::optional<Filing> filing = std::nullopt);
	void apply_described(std::optional<ClipEntry> entry, const std::filesystem::path &path, ClipKind kind,
			     bool reselect, std::optional<Filing> filing);
	void toast_filed(const ClipEntry &clip, MoveStatus status);

	// The list, its filters and the preview.
	void after_list_change();
	void rebuild_chips();
	void apply_filter();
	void update_empty_state();
	void update_summary();
	void update_preview();
	void select_first_clip();
	void refresh_day();
	std::optional<ClipEntry> selected_clip() const;
	std::optional<std::filesystem::path> selected_path() const;
	void select_path(const std::optional<std::filesystem::path> &path);
	QString where_text(const std::filesystem::path &path) const;

	// The clip actions.
	void show_context_menu(const QPoint &pos);
	void show_clip_menu(const ClipEntry &clip, const QPoint &global);
	void on_row_action(const QModelIndex &index, ClipListDelegate::Action action, const QPoint &global);
	void on_preview_action(PreviewAction action);
	void open_clip(const ClipEntry &clip);
	void reveal_path(const std::filesystem::path &path);
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
	QLineEdit *search_ = nullptr;
	QAction *search_icon_ = nullptr;
	ChipBar *chips_ = nullptr;
	SegmentBar *kinds_ = nullptr;
	QStackedWidget *stack_ = nullptr;
	ClipListView *list_ = nullptr;
	EmptyState *empty_ = nullptr;
	ClipPreview *preview_ = nullptr;
	DockToast *toast_ = nullptr;
	QWidget *footer_ = nullptr;
	TextBlock *summary_ = nullptr;
	GhostButton *open_folder_ = nullptr;
	GhostButton *refresh_ = nullptr;
	GhostButton *settings_button_ = nullptr;
	HairLine *line_top_ = nullptr;
	HairLine *line_bottom_ = nullptr;
	HairLine *line_side_ = nullptr;
	QTimer *day_timer_ = nullptr;
	ClipListModel *model_ = nullptr;

	DockLayout layout_ = DockLayout::column;
	// A layout change is queued from resizeEvent and made once, after it.
	bool layout_queued_ = false;
	// What each chip filters on: nullopt for All, "" for the clips directly in the folder, else a game folder.
	std::vector<std::optional<std::string>> chip_games_;
	// Set by start(), which does nothing after its first call.
	bool started_ = false;
	// The newest scan stopped at the index's cap.
	bool truncated_ = false;
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
	// An explicit minimum, which a layout doesn't override: the column layout's own minimum is taller than the bottom
	// row, and the dock must be able to shrink into the row layout. A dock made smaller than its layout needs squeezes
	// the list first.
	dock_->setMinimumSize(kMinimumWidth, kMinimumHeight);
	build_widgets();
	connect_widgets();
	apply_layout(DockLayout::column);

	const Settings current = settings_.get();
	header_->set_auto_sort(current.auto_sort);
	header_->set_replay_auto_start(current.replay_auto_start);
	header_->set_replay_status(replay_status_);
	// The empty list and the All chip until the first scan.
	rebuild_chips();
	update_empty_state();
	update_summary();
}

// --- building and arranging -----------------------------------------------------------------------------------------

void ClipDock::Impl::build_widgets()
{
	header_ = new StatusHeader(dock_);
	banners_ = new BannerArea(dock_);

	search_ = new QLineEdit(dock_);
	search_->setPlaceholderText(module_text("Dock.Search.Placeholder"));
	search_->setClearButtonEnabled(true);
	search_icon_ = search_->addAction(dock_icon(DockIcon::search, dock_->palette(), kSearchIconPx),
					  QLineEdit::LeadingPosition);

	chips_ = new ChipBar(dock_);
	chips_->setAccessibleName(module_text("Dock.Filter.Games"));
	kinds_ = new SegmentBar(
		{
			{module_text("Dock.Kind.All"), false, DockIcon::replay},
			{module_text("Dock.Kind.Replays"), true, DockIcon::replay},
			{module_text("Dock.Kind.Recordings"), true, DockIcon::recording},
			{module_text("Dock.Kind.Screenshots"), true, DockIcon::screenshot},
		},
		dock_);
	kinds_->setAccessibleName(module_text("Dock.Filter.Kinds"));

	// The list shows the clips. The empty state takes its place when there is nothing to list.
	stack_ = new QStackedWidget(dock_);
	list_ = new ClipListView(stack_);
	model_ = new ClipListModel(dock_);
	list_->setModel(model_);
	stack_->addWidget(list_);
	empty_ = new EmptyState(stack_);
	stack_->addWidget(empty_);

	preview_ = new ClipPreview(dock_);

	// Footer: the summary on the left, then the folder, Refresh and Settings buttons.
	footer_ = new QWidget(dock_);
	summary_ = new TextBlock(QString(), TextTone::muted, 0.92, QFont::Normal, footer_);
	summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	open_folder_ = new GhostButton(DockIcon::folder, module_text("Dock.OpenFolder"), footer_);
	// Nothing is scanned yet, so there is no folder to open.
	open_folder_->setEnabled(false);
	refresh_ = new GhostButton(DockIcon::refresh, module_text("Dock.Refresh"), footer_);
	settings_button_ = new GhostButton(DockIcon::settings, module_text("Dock.Settings"), footer_);
	auto *footer_row = new QHBoxLayout(footer_);
	footer_row->setContentsMargins(12, 3, 6, 3);
	footer_row->setSpacing(2);
	footer_row->addWidget(summary_, 1);
	footer_row->addWidget(open_folder_);
	footer_row->addWidget(refresh_);
	footer_row->addWidget(settings_button_);

	line_top_ = new HairLine(Qt::Horizontal, dock_);
	line_bottom_ = new HairLine(Qt::Horizontal, dock_);
	line_side_ = new HairLine(Qt::Vertical, dock_);

	// In no layout: it floats over the list.
	toast_ = new DockToast(dock_);

	day_timer_ = new QTimer(dock_);
	day_timer_->setInterval(kDayIntervalMs);
}

void ClipDock::Impl::connect_widgets()
{
	const std::weak_ptr<Shared> weak = shared_;
	chips_->set_chosen_callback(
		[weak](int) { with_live(weak, "game filter", [](Impl &impl) { impl.apply_filter(); }); });
	kinds_->set_chosen_callback(
		[weak](int) { with_live(weak, "type filter", [](Impl &impl) { impl.apply_filter(); }); });
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
	QObject::connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, dock_,
			 [weak](const QModelIndex &, const QModelIndex &) {
				 with_live(weak, "the preview", [](Impl &impl) { impl.update_preview(); });
			 });
	// The preview borrows the list's picture until its own arrives.
	QObject::connect(model_, &QAbstractItemModel::dataChanged, dock_,
			 [weak](const QModelIndex &top, const QModelIndex &, const QList<int> &roles) {
				 if (!roles.contains(Qt::DecorationRole))
					 return;
				 with_live(weak, "the preview's picture", [&top](Impl &impl) {
					 if (impl.layout_ != DockLayout::wide || top != impl.list_->currentIndex())
						 return;
					 impl.preview_->set_fallback(
						 top.data(ClipListModel::PathRole).toString().toUtf8().toStdString(),
						 top.data(Qt::DecorationRole).value<QPixmap>());
				 });
			 });
	list_->delegate()->set_action_callback([weak](const QModelIndex &index, ClipListDelegate::Action action,
						      const QPoint &global) {
		with_live(weak, "a clip action", [&](Impl &impl) { impl.on_row_action(index, action, global); });
	});
	preview_->set_action_callback([weak](PreviewAction action) {
		with_live(weak, "a preview action", [action](Impl &impl) { impl.on_preview_action(action); });
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
	// Ctrl+F anywhere in the dock goes to the search; Escape there clears it, then leaves it.
	new QShortcut(
		QKeySequence::Find, dock_,
		[weak] {
			with_live(weak, "focusing the search", [](Impl &impl) {
				impl.search_->setFocus(Qt::ShortcutFocusReason);
				impl.search_->selectAll();
			});
		},
		Qt::WidgetWithChildrenShortcut);
	new QShortcut(
		QKeySequence(Qt::Key_Escape), search_,
		[weak] {
			with_live(weak, "leaving the search", [](Impl &impl) {
				if (!impl.search_->text().isEmpty())
					impl.search_->clear();
				else
					impl.list_->setFocus(Qt::ShortcutFocusReason);
			});
		},
		Qt::WidgetShortcut);

	QObject::connect(day_timer_, &QTimer::timeout, dock_,
			 [weak] { with_live(weak, "a new day", [](Impl &impl) { impl.refresh_day(); }); });
	QObject::connect(open_folder_, &QAbstractButton::clicked, dock_, [weak] {
		with_live(weak, "opening the clips folder", [](Impl &impl) { impl.open_folder(); });
	});
	QObject::connect(refresh_, &QAbstractButton::clicked, dock_,
			 [weak] { with_live(weak, "refreshing the list", [](Impl &impl) { impl.start_scan(); }); });
	QObject::connect(settings_button_, &QAbstractButton::clicked, dock_,
			 [weak] { with_live(weak, "opening the settings", [](Impl &impl) { impl.open_settings(); }); });

	// The switch saves the setting, and the dock is updated through apply_settings() like every other listener.
	header_->set_auto_sort_callback([weak, store = &settings_](bool on) {
		with_live(weak, "setting auto-sort", [store, on](Impl &) {
			Settings changed = store->get();
			changed.auto_sort = on;
			store->set(changed);
		});
	});
	header_->set_save_callback(
		[weak] { with_live(weak, "saving the replay", [](Impl &impl) { impl.save_replay(); }); });
}

void ClipDock::Impl::apply_layout(DockLayout layout)
{
	// The selection is carried over by path: the model regroups (a reset) when the strip comes or goes.
	const std::optional<std::filesystem::path> selected = selected_path();
	layout_ = layout;
	// The old layout goes, with its nested layouts; the widgets stay, children of the dock.
	delete dock_->layout();
	for (QWidget *widget : {static_cast<QWidget *>(header_), footer_}) {
		widget->setMinimumWidth(0);
		widget->setMaximumWidth(QWIDGETSIZE_MAX);
	}
	search_->setMaximumWidth(QWIDGETSIZE_MAX);

	switch (layout) {
	case DockLayout::column:
		layout_column();
		break;
	case DockLayout::row:
		layout_row();
		break;
	case DockLayout::wide:
		layout_wide();
		break;
	}

	const bool row = layout == DockLayout::row;
	const bool wide = layout == DockLayout::wide;
	header_->set_layout(row    ? StatusHeader::Layout::rail
			    : wide ? StatusHeader::Layout::wide
				   : StatusHeader::Layout::column);
	chips_->set_max_rows(row ? 1 : wide ? 3 : 2);
	kinds_->setVisible(!row);
	preview_->setVisible(wide);
	line_top_->setVisible(!row);
	line_bottom_->setVisible(!row);
	line_side_->setVisible(row || wide);
	list_->set_strip(row);
	model_->set_grouped(!row);
	// The type filter is hidden in the strip, so it goes back to All rather than filtering out of sight.
	if (row && kinds_->current() != 0) {
		kinds_->set_current(0);
		apply_filter();
	}
	select_path(selected);
	if (wide && !list_->currentIndex().isValid())
		select_first_clip();
	update_preview();
	// The widgets' new places are needed now, for the toast.
	if (dock_->layout())
		dock_->layout()->activate();
	place_toast();
}

void ClipDock::Impl::layout_column()
{
	banners_->setContentsMargins(10, 0, 10, 8);
	auto *filters = new QVBoxLayout;
	filters->setContentsMargins(10, 0, 10, 10);
	filters->setSpacing(8);
	filters->addWidget(search_);
	filters->addWidget(chips_);
	filters->addWidget(kinds_);

	auto *root = new QVBoxLayout(dock_);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	root->addWidget(header_);
	root->addWidget(banners_);
	root->addLayout(filters);
	root->addWidget(line_top_);
	root->addWidget(stack_, 1);
	root->addWidget(line_bottom_);
	root->addWidget(footer_);
}

void ClipDock::Impl::layout_row()
{
	// The rail is as wide as the header needs at the theme's font, within reason.
	const int rail = std::clamp(qRound(QFontMetrics(dock_->font()).height() * 14.5), 230, 320);
	header_->setFixedWidth(rail);
	footer_->setFixedWidth(rail);
	search_->setMaximumWidth(kRowSearchWidth);
	banners_->setContentsMargins(0, 0, 0, 2);

	auto *rail_column = new QVBoxLayout;
	rail_column->setContentsMargins(0, 0, 0, 0);
	rail_column->setSpacing(0);
	rail_column->addWidget(header_);
	rail_column->addStretch(1);
	rail_column->addWidget(footer_);

	auto *filters = new QHBoxLayout;
	filters->setSpacing(10);
	filters->addWidget(chips_, 1, Qt::AlignVCenter);
	filters->addWidget(search_, 0, Qt::AlignVCenter);

	auto *strip = new QVBoxLayout;
	strip->setContentsMargins(10, 8, 6, 0);
	strip->setSpacing(4);
	strip->addWidget(banners_);
	strip->addLayout(filters);
	strip->addWidget(stack_, 1);

	auto *root = new QHBoxLayout(dock_);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	root->addLayout(rail_column);
	root->addWidget(line_side_);
	root->addLayout(strip, 1);
}

void ClipDock::Impl::layout_wide()
{
	banners_->setContentsMargins(12, 0, 12, 8);
	auto *list_column = new QVBoxLayout;
	list_column->setContentsMargins(10, 10, 6, 0);
	list_column->setSpacing(8);
	list_column->addWidget(search_);
	list_column->addWidget(chips_);
	list_column->addWidget(kinds_);
	list_column->addWidget(stack_, 1);

	auto *body = new QHBoxLayout;
	body->setContentsMargins(0, 0, 0, 0);
	body->setSpacing(0);
	body->addLayout(list_column, 5);
	body->addWidget(line_side_);
	body->addWidget(preview_, 6);

	auto *root = new QVBoxLayout(dock_);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	root->addWidget(header_);
	root->addWidget(banners_);
	root->addWidget(line_top_);
	root->addLayout(body, 1);
	root->addWidget(line_bottom_);
	root->addWidget(footer_);
}

void ClipDock::Impl::on_resized()
{
	place_toast();
	if (layout_queued_ || choose_dock_layout(dock_->width(), dock_->height(), layout_) == layout_)
		return;
	// Not now: rebuilding the layout inside resizeEvent would resize the dock again from inside its own resize.
	layout_queued_ = true;
	const std::weak_ptr<Shared> weak = shared_;
	QTimer::singleShot(0, dock_, [weak] {
		with_live(weak, "changing the layout", [](Impl &impl) {
			impl.layout_queued_ = false;
			const DockLayout wanted =
				choose_dock_layout(impl.dock_->width(), impl.dock_->height(), impl.layout_);
			if (wanted != impl.layout_)
				impl.apply_layout(wanted);
		});
	});
}

void ClipDock::Impl::place_toast()
{
	// Over the bottom of the list, or of the rail in the bottom row, which has the room the strip lacks.
	QRect area(stack_->mapTo(dock_, QPoint(0, 0)), stack_->size());
	if (layout_ == DockLayout::row) {
		const int top = header_->geometry().bottom() + 1;
		const QRect rail(0, top, header_->width(), footer_->geometry().top() - top);
		if (rail.height() >= 60)
			area = rail;
	}
	toast_->set_area(area);
}

void ClipDock::Impl::refresh_look()
{
	// The accents are read again on the next paint, and the icons are pixmaps in the old colours.
	reset_theme_accents();
	search_icon_->setIcon(dock_icon(DockIcon::search, dock_->palette(), kSearchIconPx));
	header_->set_replay_status(replay_status_);
	for (QWidget *child : dock_->findChildren<QWidget *>())
		child->update();
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
	day_timer_->stop();
	model_->shutdown();
	preview_->shutdown();
	toast_->shutdown();
}

void ClipDock::Impl::on_shown()
{
	if (!live())
		return;
	// OBS has no event for its recording folder changing, so a dock that was hidden checks the folder when it
	// shows.
	refresh_if_changed();
	// The groups are right at once. The timer keeps them right while the dock stays visible.
	refresh_day();
	day_timer_->start();
}

void ClipDock::Impl::on_hidden()
{
	day_timer_->stop();
}

void ClipDock::Impl::apply_settings(const Settings &settings)
{
	if (!live())
		return;
	header_->set_auto_sort(settings.auto_sort);
	header_->set_replay_auto_start(settings.replay_auto_start);
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
	switch (clip.status) {
	case MoveStatus::moved:
	case MoveStatus::copied_source_kept:
		describe_and_upsert(clip.final_path, clip.kind, false, Filing{clip.status});
		break;
	case MoveStatus::failed: {
		// The file is still where OBS saved it: the toast says why, and Show finds it there.
		ClipEntry original;
		original.path = clip.original_path;
		original.file_name = path_to_utf8(clip.original_path.filename());
		original.kind = clip.kind;
		const std::filesystem::path path = clip.original_path;
		const std::weak_ptr<Shared> weak = shared_;
		place_toast();
		toast_->show_toast(ToastTone::warning, module_text("Dock.Toast.Failed").arg(clip_title(original, true)),
				   utf8(clip.error), [weak, path] {
					   with_live(weak, "showing a clip",
						     [&path](Impl &impl) { impl.reveal_path(path); });
				   });
		break;
	}
	case MoveStatus::cancelled:
		// OBS is closing; nothing moved.
		break;
	}
}

void ClipDock::Impl::save_replay()
{
	// The button is disabled while the buffer is stopped, but the buffer can stop between the paint and the click.
	if (obs_frontend_replay_buffer_active())
		obs_frontend_replay_buffer_save();
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
		truncated_ = false;
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
	truncated_ = result.truncated;
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
void ClipDock::Impl::describe_and_upsert(const std::filesystem::path &path, ClipKind kind, bool reselect,
					 std::optional<Filing> filing)
{
	// Nothing is listed until the first scan, so there is nothing to add the clip to.
	if (!live() || shared_->base.empty())
		return;
	const std::filesystem::path base = shared_->base;
	const ScanOptions options = shared_->options;
	const std::weak_ptr<Shared> weak = shared_;
	shared_->queue.post([weak, base, options, path, kind, reselect, filing]() {
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
		run_on_ui([weak, entry = std::move(entry), path, kind, reselect, filing]() mutable {
			with_live(weak, "adding a clip", [&](Impl &impl) {
				impl.apply_described(std::move(entry), path, kind, reselect, filing);
			});
		});
	});
}

void ClipDock::Impl::apply_described(std::optional<ClipEntry> entry, const std::filesystem::path &path, ClipKind kind,
				     bool reselect, std::optional<Filing> filing)
{
	// Nothing to add: the file is gone, or it is no longer a clip a scan would list.
	if (!entry)
		return;
	// The caller knows the exact kind. A file's name only suggests it, and a clip filed in this session is certain.
	entry->kind = kind;
	const ClipEntry clip = *entry;
	const std::optional<std::filesystem::path> selected = selected_path();
	model_->upsert_clip(std::move(*entry));
	if (reselect)
		select_path(path);
	else
		select_path(selected);
	after_list_change();
	if (filing) {
		model_->set_fresh(path);
		toast_filed(clip, filing->status);
	}
}

void ClipDock::Impl::toast_filed(const ClipEntry &clip, MoveStatus status)
{
	// Where it went, below the clips folder: "Elden Ring › 2026-10". A clip filed in the folder itself has no
	// folders below it, so its game, or "Not sorted", says where it is.
	QStringList folders;
	for (const std::string &folder : folders_below(shared_->base, clip.path))
		folders << utf8(folder);
	const QString where = folders.isEmpty() ? game_label(clip.game_folder) : folders.join(path_separator());
	const bool copied = status == MoveStatus::copied_source_kept;
	const QString title = module_text(copied ? "Dock.Toast.Copied" : "Dock.Toast.Filed").arg(where);
	const QString detail = clip_title(clip, true) + QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" ") +
			       utf8(format_size(clip.size));
	const std::filesystem::path path = clip.path;
	const std::weak_ptr<Shared> weak = shared_;
	place_toast();
	toast_->show_toast(copied ? ToastTone::warning : ToastTone::success, title, detail, [weak, path] {
		with_live(weak, "showing a clip", [&path](Impl &impl) { impl.reveal_path(path); });
	});
}

// --- the list, its filters and the preview --------------------------------------------------------------------------

void ClipDock::Impl::after_list_change()
{
	rebuild_chips();
	update_empty_state();
	update_summary();
	update_preview();
}

void ClipDock::Impl::rebuild_chips()
{
	const ClipIndex &index = model_->index();
	const int current = chips_->current();
	const std::optional<std::string> chosen = current >= 0 && current < static_cast<int>(chip_games_.size())
							  ? chip_games_[static_cast<std::size_t>(current)]
							  : std::nullopt;

	std::map<std::string, int> counts;
	for (const ClipEntry &clip : index.clips())
		counts[clip.game_folder] += 1;

	std::vector<ChipBar::Entry> entries;
	chip_games_.clear();
	entries.push_back({module_text("Dock.Filter.All"), static_cast<int>(index.clips().size())});
	chip_games_.push_back(std::nullopt);
	for (const std::string &game : index.games()) {
		entries.push_back({utf8(game), counts[game]});
		chip_games_.push_back(game);
	}
	// "Not sorted" filters on "": the clips directly in the folder.
	if (index.has_unfiled()) {
		entries.push_back({module_text("Dock.Game.Unfiled"), counts[std::string()]});
		chip_games_.push_back(std::string());
	}

	// The choice is kept while its chip still exists.
	int keep = 0;
	bool choice_gone = false;
	if (chosen) {
		const auto at = std::find(chip_games_.begin(), chip_games_.end(), chosen);
		if (at != chip_games_.end())
			keep = static_cast<int>(at - chip_games_.begin());
		else
			choice_gone = true;
	}
	chips_->set_entries(entries, keep);
	// A lone "All 0" says nothing that the empty list doesn't.
	chips_->setVisible(!index.clips().empty());
	// The list still filters on the game that went away, so the filter follows the chips back to All.
	if (choice_gone)
		apply_filter();
}

// Builds the query from the filters. The selection is carried over by path, because the filter resets the model.
void ClipDock::Impl::apply_filter()
{
	ClipQuery query;
	const int chip = chips_->current();
	if (chip >= 0 && chip < static_cast<int>(chip_games_.size()))
		query.game_folder = chip_games_[static_cast<std::size_t>(chip)];
	// The segments are All, then the kinds in ClipKind's order.
	const int kind = kinds_->current();
	if (kind > 0)
		query.kind = static_cast<ClipKind>(kind - 1);
	query.search = search_->text().toUtf8().toStdString();
	const std::optional<std::filesystem::path> selected = selected_path();
	model_->set_query(std::move(query));
	select_path(selected);
	if (layout_ == DockLayout::wide && !list_->currentIndex().isValid())
		select_first_clip();
	update_empty_state();
	update_preview();
}

void ClipDock::Impl::update_empty_state()
{
	if (model_->clip_count() > 0) {
		stack_->setCurrentWidget(list_);
		return;
	}
	// Two messages: nothing has been scanned into the index, or the filter hides everything in it.
	if (model_->index().clips().empty())
		empty_->set_texts(DockIcon::replay, module_text("Dock.Empty.Title"), module_text("Dock.Empty.Body"));
	else
		empty_->set_texts(DockIcon::search, module_text("Dock.Empty.NoMatch"),
				  module_text("Dock.Empty.NoMatchHint"));
	stack_->setCurrentWidget(empty_);
}

void ClipDock::Impl::update_summary()
{
	const ClipIndex &index = model_->index();
	if (index.clips().empty()) {
		summary_->set_text(QString());
		return;
	}
	if (truncated_) {
		summary_->set_text(
			module_text("Dock.Footer.Truncated").arg(static_cast<qulonglong>(index.clips().size())));
		return;
	}
	std::uintmax_t bytes = 0;
	for (const ClipEntry &clip : index.clips())
		bytes += clip.size;
	summary_->set_text(summary_text(index.clips().size(), bytes));
}

void ClipDock::Impl::update_preview()
{
	if (layout_ != DockLayout::wide)
		return;
	const QModelIndex current = list_->currentIndex();
	const ClipEntry *clip = current.isValid() ? model_->clip_at(current.row()) : nullptr;
	if (!clip) {
		preview_->show_nothing();
		return;
	}
	const QDateTime modified = current.data(ClipListModel::ModifiedRole).toDateTime();
	PreviewInfo info;
	info.path_utf8 = path_to_utf8(clip->path);
	info.size = clip->size;
	info.modified_ms = modified.toMSecsSinceEpoch();
	info.kind = static_cast<int>(clip->kind);
	info.title = clip_title(*clip, true);
	info.where = where_text(clip->path);
	info.game = game_label(clip->game_folder);
	info.kind_text = kind_name(clip->kind);
	info.saved = QLocale::system().toString(modified, QLocale::ShortFormat);
	info.size_text = utf8(format_size(clip->size));
	info.fallback = current.data(Qt::DecorationRole).value<QPixmap>();
	preview_->show_clip(info);
}

void ClipDock::Impl::select_first_clip()
{
	for (int row = 0; row < model_->rowCount(); ++row) {
		if (model_->clip_at(row)) {
			list_->setCurrentIndex(model_->index(row, 0));
			return;
		}
	}
}

void ClipDock::Impl::refresh_day()
{
	// A new day regroups the clips (a reset), so the selection is carried over by path.
	const std::optional<std::filesystem::path> selected = selected_path();
	if (model_->refresh_day())
		select_path(selected);
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

// The folder a clip is in, as "D:\Clips › Elden Ring › 2026-10": the clips folder in the system's form, then the
// folders below it.
QString ClipDock::Impl::where_text(const std::filesystem::path &path) const
{
	QString where = QDir::toNativeSeparators(utf8(path_to_utf8(shared_->base)));
	for (const std::string &folder : folders_below(shared_->base, path))
		where += path_separator() + utf8(folder);
	return where;
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
	show_clip_menu(clip, list_->viewport()->mapToGlobal(pos));
}

void ClipDock::Impl::show_clip_menu(const ClipEntry &clip, const QPoint &global)
{
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
	QAction *chosen = menu.exec(global);
	// The menu's event loop may have stopped or deleted the dock, so the dock is looked up again before anything
	// else.
	Impl *self = live_impl(weak);
	if (!self || !chosen)
		return;
	if (chosen == open)
		self->open_clip(clip);
	else if (chosen == reveal)
		self->reveal_path(clip.path);
	else if (chosen == rename)
		self->rename_clip(clip);
	else if (chosen == move)
		self->move_clip(clip);
	else if (chosen == recycle)
		self->recycle_clip(clip);
}

void ClipDock::Impl::on_row_action(const QModelIndex &index, ClipListDelegate::Action action, const QPoint &global)
{
	const ClipEntry *found = model_->clip_at(index.row());
	if (!found)
		return;
	// A copy, as above: the menu and the Shell calls outlive this row.
	const ClipEntry clip = *found;
	switch (action) {
	case ClipListDelegate::Action::open:
		open_clip(clip);
		break;
	case ClipListDelegate::Action::reveal:
		reveal_path(clip.path);
		break;
	case ClipListDelegate::Action::more:
		list_->setCurrentIndex(index);
		show_clip_menu(clip, global);
		break;
	}
}

void ClipDock::Impl::on_preview_action(PreviewAction action)
{
	const std::optional<ClipEntry> clip = selected_clip();
	if (!clip)
		return;
	switch (action) {
	case PreviewAction::open:
		open_clip(*clip);
		break;
	case PreviewAction::reveal:
		reveal_path(clip->path);
		break;
	case PreviewAction::rename:
		rename_clip(*clip);
		break;
	case PreviewAction::move:
		move_clip(*clip);
		break;
	case PreviewAction::recycle:
		recycle_clip(*clip);
		break;
	}
}

void ClipDock::Impl::open_clip(const ClipEntry &clip)
{
	shell_ops().open(path_to_utf8(clip.path), report_failure("open failed"));
}

void ClipDock::Impl::reveal_path(const std::filesystem::path &path)
{
	shell_ops().reveal(path_to_utf8(path), report_failure("reveal failed"));
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

void ClipDock::on_theme_changed()
{
	guarded("theme change in the clip browser", [this] {
		if (impl_->live())
			impl_->refresh_look();
	});
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

void ClipDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	guarded("resizing the clip browser", [this] {
		if (impl_ && impl_->live())
			impl_->on_resized();
	});
}

void ClipDock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	// The base class can send events while the Impl is still being made.
	if (!impl_ || (event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange))
		return;
	guarded("restyling the clip browser", [this] {
		if (impl_->live())
			impl_->refresh_look();
	});
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

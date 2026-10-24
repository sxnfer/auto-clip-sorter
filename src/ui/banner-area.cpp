// Clip Atlas — the notices above the clip list. Each shown banner is one row in a stack; banner-area.hpp says what a row
// holds and how dismissing works.
#include "banner-area.hpp"

#include "dock-controls.hpp"

#include <obs-module.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <utility>

namespace clip_atlas {

namespace {

QString module_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace

BannerArea::BannerArea(QWidget *parent) : QWidget(parent)
{
	layout_ = new QVBoxLayout(this);
	layout_->setContentsMargins(0, 0, 0, 0);
	layout_->setSpacing(4);
	// Hidden until the first banner, so an empty area takes no space in the dock.
	setVisible(false);
}

void BannerArea::show_banner(BannerId id, BannerKind kind, const QString &text, const QString &action_text,
			     Action action, bool dismissible)
{
	// A dismissed banner stays dismissed while its text is the same, so a repeated check doesn't bring back a notice
	// the user closed. New text is new information, so it shows again.
	const auto dismissed = dismissed_text_.find(id);
	if (dismissed != dismissed_text_.end() && dismissed->second != text)
		dismissed_text_.erase(dismissed);

	Banner &banner = banners_[id];
	// Only a change to what the row looks like needs a new row. A new action alone keeps the row, so focus and the
	// click target stay put: the button runs whatever action is stored when it is clicked.
	const bool same_look = banner.row != nullptr && banner.kind == kind && banner.text == text &&
			       banner.action_text == action_text && banner.dismissible == dismissible;
	banner.kind = kind;
	banner.text = text;
	banner.action_text = action_text;
	banner.action = std::move(action);
	banner.dismissible = dismissible;

	// The parameters are stored even while the banner is dismissed, so they are right when it comes back.
	if (dismissed_text_.count(id))
		retire_row(banner);
	else if (!same_look)
		refresh_row(id);
	update_visibility();
}

void BannerArea::hide_banner(BannerId id)
{
	const auto it = banners_.find(id);
	if (it == banners_.end())
		return;
	retire_row(it->second);
	// The dismissed text is kept on purpose: hiding is not dismissing, and a repeat of the dismissed text stays hidden.
	banners_.erase(it);
	update_visibility();
}

bool BannerArea::is_shown(BannerId id) const
{
	return banners_.count(id) != 0 && dismissed_text_.count(id) == 0;
}

QWidget *BannerArea::make_row(BannerId id, const Banner &banner)
{
	auto *row = new NoticeFrame(banner.kind == BannerKind::warning ? NoticeTone::warning : NoticeTone::information,
				    this);
	auto *line = new QHBoxLayout(row);
	// The frame paints its icon in the left margin.
	line->setContentsMargins(row->icon_room(), 7, 6, 7);
	line->setSpacing(8);

	auto *message = new QLabel(banner.text, row);
	// The text is never rich text: a path or a name in it must show as typed.
	message->setTextFormat(Qt::PlainText);
	message->setWordWrap(true);
	message->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	line->addWidget(message, 1);

	if (!banner.action_text.isEmpty()) {
		auto *action_button = new QPushButton(banner.action_text, row);
		connect(action_button, &QPushButton::clicked, this, [this, id] { run_action(id); });
		line->addWidget(action_button, 0, Qt::AlignVCenter);
	}

	if (banner.dismissible) {
		auto *dismiss_button = new GhostButton(DockIcon::clear, module_text("Dock.Banner.Dismiss"), row);
		connect(dismiss_button, &QAbstractButton::clicked, this, [this, id] { dismiss(id); });
		line->addWidget(dismiss_button, 0, Qt::AlignTop);
	}
	return row;
}

void BannerArea::refresh_row(BannerId id)
{
	Banner &banner = banners_.at(id);
	// The new row takes the old row's place, so the banners around it don't move.
	int index = -1;
	if (banner.row) {
		index = layout_->indexOf(banner.row);
		retire_row(banner);
	}
	if (index < 0)
		index = index_after(id);
	banner.row = make_row(id, banner);
	layout_->insertWidget(index, banner.row);
}

void BannerArea::retire_row(Banner &banner)
{
	if (!banner.row)
		return;
	layout_->removeWidget(banner.row);
	banner.row->hide();
	// Deleted later, not now: this can run inside the row's own click handler, and deleting a widget there is unsafe.
	banner.row->deleteLater();
	banner.row = nullptr;
}

int BannerArea::index_after(BannerId id) const
{
	// The stack follows BannerId order, so a new row goes in front of the first row of a later banner.
	for (auto it = banners_.upper_bound(id); it != banners_.end(); ++it) {
		if (it->second.row)
			return layout_->indexOf(it->second.row);
	}
	return layout_->count();
}

void BannerArea::run_action(BannerId id)
{
	const auto it = banners_.find(id);
	if (it == banners_.end())
		return;
	// Copied before the call: the action may show or hide this banner, which replaces or erases the stored action
	// while it is running.
	const Action action = it->second.action;
	if (action)
		action();
}

void BannerArea::dismiss(BannerId id)
{
	const auto it = banners_.find(id);
	if (it == banners_.end())
		return;
	dismissed_text_[id] = it->second.text;
	retire_row(it->second);
	update_visibility();
}

void BannerArea::update_visibility()
{
	setVisible(layout_->count() > 0);
}

} // namespace clip_atlas

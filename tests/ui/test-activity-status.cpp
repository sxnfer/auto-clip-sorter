#include "activity-status.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFrame>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

using namespace clip_atlas;

namespace {
ActivityEvent accepted()
{
	ActivityEvent event;
	event.save_id = 1;
	event.accepted = true;
	event.game = "Counter-Strike 2";
	ActivityFile file;
	file.path = "D:/Recordings/Replay 2026-10-23 19-02-44.mkv";
	event.files.push_back(file);
	return event;
}

void capture(QWidget *widget, const QString &name)
{
	const auto directory = qEnvironmentVariable("CLIP_ATLAS_CAPTURE_DIR");
	if (directory.isEmpty())
		return;
	QDir().mkpath(directory);
	QApplication::processEvents();
	QPixmap shot = widget->grab();
	if (widget->testAttribute(Qt::WA_TranslucentBackground)) {
		// The panel's shadow falls on whatever is under it, so the picture puts it on the dock's base.
		QPixmap backed(shot.size());
		backed.setDevicePixelRatio(shot.devicePixelRatio());
		backed.fill(widget->palette().color(QPalette::Base));
		QPainter painter(&backed);
		painter.drawPixmap(0, 0, shot);
		painter.end();
		shot = backed;
	}
	CHECK(shot.save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
}
} // namespace

TEST_CASE("Activity pill supports keyboard, accessible expansion and repeated toggling", "[activity-ui]")
{
	ActivityStatus status;
	status.resize(320, 36);
	status.set_reduced_motion(true);
	status.show();
	QTest::qWait(5);
	auto *pill = status.findChild<QAbstractButton *>(QStringLiteral("activityPill"));
	auto *panel = status.findChild<QFrame *>(QStringLiteral("activityPanel"));
	REQUIRE(pill);
	REQUIRE(panel);
	pill->setFocus();
	CHECK(pill->focusPolicy() == Qt::StrongFocus);
	auto *accessible = QAccessible::queryAccessibleInterface(pill);
	REQUIRE(accessible);
	CHECK(accessible->state().expandable);
	CHECK(accessible->state().collapsed);
	QTest::keyClick(pill, Qt::Key_Return);
	REQUIRE(status.is_expanded());
	CHECK(accessible->state().expanded);
	CHECK(panel->isVisible());
	QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
	CHECK_FALSE(status.is_expanded());
	CHECK(pill->hasFocus());
	for (int i = 0; i < 20; ++i) {
		QTest::keyClick(pill, Qt::Key_Space);
		CHECK(status.is_expanded());
		QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
		CHECK_FALSE(status.is_expanded());
	}
	status.set_expanded(true);
	panel->hide(); // Qt::Popup also hides this way after an outside press.
	CHECK_FALSE(status.is_expanded());
	status.set_expanded(true);
	status.stop();
	status.stop();
	status.on_activity(accepted());
	status.set_expanded(true);
	CHECK_FALSE(status.is_expanded());
	CHECK_FALSE(panel->isVisible());
}

TEST_CASE("Activity status wraps details and survives narrow resize during animation", "[activity-ui]")
{
	ActivityStatus status;
	status.resize(460, 36);
	status.show();
	status.on_activity(accepted());
	status.set_expanded(true);
	QTest::qWait(70);
	status.resize(180, 36);
	QTest::qWait(20);
	auto *panel = status.findChild<QFrame *>(QStringLiteral("activityPanel"));
	REQUIRE(panel);
	// The panel's window has room for the card's shadow; the card itself stays within the dock's width.
	CHECK(panel->contentsRect().width() <= status.width());
	CHECK(status.height() == 36);
	auto *scroll = panel->findChild<QScrollArea *>();
	REQUIRE(scroll);
	CHECK(scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
	status.set_expanded(false);
	QTest::qWait(220);
	CHECK_FALSE(panel->isVisible());
	status.set_expanded(true);
	status.hide();
	CHECK_FALSE(status.is_expanded());
}

TEST_CASE("History controls preserve keyboard escape and failures expose their detail", "[activity-ui]")
{
	ActivityStatus status;
	status.set_reduced_motion(true);
	status.resize(320, 36);
	status.show();
	QTest::qWait(5);
	status.on_activity(accepted());
	auto next = accepted();
	next.save_id = 2;
	status.on_activity(next);
	ActivityEvent failure;
	failure.save_id = 1;
	failure.phase = ActivityPhase::failed;
	failure.detail = "permission denied";
	status.on_activity(failure);
	failure.save_id = 2;
	status.on_activity(failure);
	auto *pill = status.findChild<QAbstractButton *>(QStringLiteral("activityPill"));
	CHECK(pill->accessibleName().contains(QStringLiteral("permission denied")));
	status.set_expanded(true);
	auto *panel = status.findChild<QFrame *>(QStringLiteral("activityPanel"));
	const auto controls = panel->findChildren<QAbstractButton *>(QStringLiteral("activityHistoryEntry"));
	REQUIRE(controls.size() == 2);
	controls.first()->setFocus();
	QTest::keyClick(controls.first(), Qt::Key_Escape);
	QTest::qWait(5);
	CHECK_FALSE(status.is_expanded());
	CHECK(pill->hasFocus());
	status.set_expanded(true);
	const auto history = panel->findChildren<QAbstractButton *>(QStringLiteral("activityHistoryEntry"));
	REQUIRE_FALSE(history.empty());
	QTest::mouseClick(history.first(), Qt::LeftButton);
	QTest::qWait(5);
	CHECK(status.is_expanded());
}

TEST_CASE("A destroyed activity widget safely drops context-bound late callbacks", "[activity-ui]")
{
	QPointer<ActivityStatus> status = new ActivityStatus;
	bool delivered = false;
	QTimer::singleShot(0, status, [&] { delivered = true; });
	delete status.data();
	QTest::qWait(5);
	CHECK(status.isNull());
	CHECK_FALSE(delivered);
}

TEST_CASE("Activity state previews use real widget painting in light and dark palettes", "[activity-ui]")
{
	// Away from every capture, so no control is painted as hovered.
	QCursor::setPos(QPoint(-10000, -10000));
	for (bool dark : {false, true}) {
		ActivityStatus status;
		// The roles the dock's style reads, in OBS's Yami and Yami Light colours.
		QPalette palette;
		const auto set = [&palette](QPalette::ColorRole role, const QColor &color) {
			palette.setColor(QPalette::All, role, color);
		};
		set(QPalette::Window, dark ? QColor(0x1D, 0x1F, 0x26) : QColor(0xE3, 0xE4, 0xE9));
		set(QPalette::Base, dark ? QColor(0x27, 0x2A, 0x33) : QColor(0xF7, 0xF7, 0xF9));
		set(QPalette::Button, dark ? QColor(0x3C, 0x40, 0x4D) : QColor(0xD8, 0xDA, 0xE1));
		set(QPalette::Highlight, dark ? QColor(0x28, 0x4C, 0xB8) : QColor(0x33, 0x60, 0xDE));
		set(QPalette::HighlightedText, Qt::white);
		for (QPalette::ColorRole role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
			set(role, dark ? QColor(Qt::white) : QColor(0x17, 0x18, 0x1D));
		for (QPalette::ColorRole role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
			palette.setColor(QPalette::Disabled, role,
					 dark ? QColor(153, 153, 153) : QColor(0x7D, 0x80, 0x8C));
		status.setPalette(palette);
		// On the dock's base, as in OBS, where the pill's notice fill shows as a darker capsule.
		status.setBackgroundRole(QPalette::Base);
		status.setAutoFillBackground(true);
		status.resize(380, 36);
		status.set_reduced_motion(true);
		status.show();
		QTest::qWait(5);
		const QString theme = dark ? QStringLiteral("dark-") : QStringLiteral("light-");
		capture(&status, theme + QStringLiteral("ready"));
		status.on_activity(accepted());
		capture(&status, theme + QStringLiteral("waiting"));
		ActivityEvent moving;
		moving.save_id = 1;
		moving.phase = ActivityPhase::moving;
		moving.stability_confirmed = true;
		status.on_activity(moving);
		QTest::qWait(5);
		capture(&status, theme + QStringLiteral("working"));
		status.set_expanded(true);
		QTest::qWait(5);
		capture(status.findChild<QFrame *>(QStringLiteral("activityPanel")),
			theme + QStringLiteral("expanded"));
		status.set_expanded(false);
		moving.phase = ActivityPhase::completed;
		moving.at = ActivityClock::now();
		status.on_activity(moving);
		capture(&status, theme + QStringLiteral("completed"));
		status.resize(180, 36);
		capture(&status, theme + QStringLiteral("narrow"));
		status.resize(380, 36);
		auto next = accepted();
		next.save_id = 2;
		status.on_activity(next);
		moving.save_id = 2;
		moving.phase = ActivityPhase::failed;
		moving.detail = "Permission denied";
		moving.at = ActivityClock::now();
		status.on_activity(moving);
		capture(&status, theme + QStringLiteral("failed"));
		status.set_expanded(true);
		capture(status.findChild<QFrame *>(QStringLiteral("activityPanel")),
			theme + QStringLiteral("failed-expanded"));
	}
}

int main(int argc, char **argv)
{
	QApplication application(argc, argv);
	return Catch::Session().run(argc, argv);
}

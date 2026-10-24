#include "activity-status.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QDir>
#include <QFrame>
#include <QPointer>
#include <QScrollArea>
#include <QTest>
#include <QTimer>
#include <QToolButton>
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
	CHECK(widget->grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
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
	CHECK(panel->width() <= status.width());
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
	const auto controls = panel->findChildren<QToolButton *>();
	REQUIRE(controls.size() == 2);
	controls.first()->setFocus();
	QTest::keyClick(controls.first(), Qt::Key_Escape);
	QTest::qWait(5);
	CHECK_FALSE(status.is_expanded());
	CHECK(pill->hasFocus());
	status.set_expanded(true);
	const auto history = panel->findChildren<QToolButton *>();
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
	for (bool dark : {false, true}) {
		ActivityStatus status;
		QPalette palette;
		if (dark) {
			palette.setColor(QPalette::Window, QColor(28, 29, 32));
			palette.setColor(QPalette::Base, QColor(35, 36, 40));
			palette.setColor(QPalette::Text, QColor(236, 237, 240));
			palette.setColor(QPalette::WindowText, QColor(236, 237, 240));
			palette.setColor(QPalette::PlaceholderText, QColor(154, 157, 164));
			palette.setColor(QPalette::Mid, QColor(65, 67, 74));
			palette.setColor(QPalette::Midlight, QColor(47, 49, 55));
		}
		status.setPalette(palette);
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

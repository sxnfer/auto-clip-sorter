// Clip Atlas — the dock's colours, fonts and icons. dock-style.hpp says where each colour comes from.
#include "dock-style.hpp"

#include <QApplication>
#include <QChar>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QRegularExpression>
#include <QStringList>
#include <QtMath>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <utility>

namespace clip_atlas {

namespace {

// The device pixel ratios an icon is drawn for. Windows display scaling is 100 % to 300 % in these steps; QIcon picks
// the nearest and scales the rest.
constexpr qreal kIconScales[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};

// A polyline through `points`, as one path so the joins are round.
QPainterPath polyline(std::initializer_list<QPointF> points, bool closed = false)
{
	QPainterPath path;
	bool first = true;
	for (const QPointF &point : points) {
		if (first)
			path.moveTo(point);
		else
			path.lineTo(point);
		first = false;
	}
	if (closed)
		path.closeSubpath();
	return path;
}

void line(QPainter &painter, qreal x1, qreal y1, qreal x2, qreal y2)
{
	painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
}

void dot(QPainter &painter, const QColor &color, qreal x, qreal y, qreal radius)
{
	painter.save();
	painter.setPen(Qt::NoPen);
	painter.setBrush(color);
	painter.drawEllipse(QPointF(x, y), radius, radius);
	painter.restore();
}

// The colour a label gets from the application style sheet with `css_class` set, or `fallback` when the class
// doesn't change it from `plain`, the colour of a label without a class.
QColor class_color(const char *css_class, const QColor &plain, const QColor &fallback)
{
	QLabel probe;
	probe.setProperty("class", QString::fromLatin1(css_class));
	probe.ensurePolished();
	const QColor color = probe.palette().color(QPalette::WindowText);
	return color.rgba() == plain.rgba() ? fallback : color;
}

// Set by theme_accents() and cleared by reset_theme_accents(). UI thread only, like everything here.
std::optional<ThemeAccents> g_accents;

} // namespace

QColor mix_colors(const QColor &from, const QColor &to, qreal amount)
{
	const qreal t = std::clamp(amount, 0.0, 1.0);
	const auto channel = [t](qreal a, qreal b) {
		return a + (b - a) * t;
	};
	QColor mixed;
	mixed.setRgbF(static_cast<float>(channel(from.redF(), to.redF())),
		      static_cast<float>(channel(from.greenF(), to.greenF())),
		      static_cast<float>(channel(from.blueF(), to.blueF())),
		      static_cast<float>(channel(from.alphaF(), to.alphaF())));
	return mixed;
}

QColor with_alpha(const QColor &color, qreal opacity)
{
	QColor faded = color;
	faded.setAlphaF(static_cast<float>(std::clamp(color.alphaF() * opacity, 0.0, 1.0)));
	return faded;
}

QColor muted_text(const QPalette &palette)
{
	const QColor text = palette.color(QPalette::Active, QPalette::Text);
	const QColor disabled = palette.color(QPalette::Disabled, QPalette::Text);
	if (disabled.isValid() && disabled.rgb() != text.rgb())
		return disabled;
	return mix_colors(text, palette.color(QPalette::Active, QPalette::Base), 0.45);
}

QColor hover_fill(const QPalette &palette, QPalette::ColorRole role)
{
	return mix_colors(palette.color(role), palette.color(QPalette::Text), 0.08);
}

QColor selection_fill(const QPalette &palette)
{
	// Enough of the accent to read as selected next to a hover fill, little enough that white or black text keeps
	// its contrast on it.
	return mix_colors(palette.color(QPalette::Base), palette.color(QPalette::Highlight), 0.34);
}

QColor line_color(const QPalette &palette)
{
	// A mix rather than Button itself: Yami Light's border grey is lighter than its base and nearly invisible.
	return mix_colors(palette.color(QPalette::Base), palette.color(QPalette::Text), 0.13);
}

QColor notice_fill(const QPalette &palette)
{
	const QColor base = palette.color(QPalette::Base);
	const QColor window = palette.color(QPalette::Window);
	// Closer than this and the fill would not show against the dock.
	const int distance = std::abs(base.lightness() - window.lightness());
	if (distance >= 6)
		return window;
	return mix_colors(base, palette.color(QPalette::Text), 0.06);
}

QColor elevated_fill(const QPalette &palette)
{
	if (is_light_palette(palette))
		return palette.color(QPalette::Base);
	return mix_colors(palette.color(QPalette::Base), palette.color(QPalette::Text), 0.08);
}

QColor accent_text(const QPalette &palette)
{
	const QColor accent = palette.color(QPalette::Highlight);
	if (is_light_palette(palette))
		return accent;
	return mix_colors(accent, QColor(Qt::white), 0.42);
}

bool is_light_palette(const QPalette &palette)
{
	return palette.color(QPalette::Base).lightness() > 128;
}

const ThemeAccents &theme_accents()
{
	if (g_accents)
		return *g_accents;
	QLabel plain_probe;
	plain_probe.ensurePolished();
	const QColor plain = plain_probe.palette().color(QPalette::WindowText);
	// Yami's green and amber, darker on a light base so they still read there.
	const bool light = is_light_palette(QApplication::palette());
	ThemeAccents accents;
	accents.success =
		class_color("text-success", plain, light ? QColor(0x1E, 0x85, 0x28) : QColor(0x37, 0xD2, 0x47));
	accents.warning =
		class_color("text-warning", plain, light ? QColor(0x92, 0x6E, 0x11) : QColor(0xE5, 0xAF, 0x24));
	g_accents = accents;
	return *g_accents;
}

void reset_theme_accents()
{
	g_accents.reset();
}

QFont scaled_font(const QFont &base, qreal scale, QFont::Weight weight)
{
	QFont font = base;
	// A font sized in pixels reports -1 points, and scaling that would give an invalid size.
	if (font.pointSizeF() > 0)
		font.setPointSizeF(font.pointSizeF() * scale);
	else if (font.pixelSize() > 0)
		font.setPixelSize(std::max(1, qRound(font.pixelSize() * scale)));
	font.setWeight(weight);
	return font;
}

void paint_dock_icon(QPainter &painter, DockIcon icon, const QRectF &box, const QColor &color)
{
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.translate(box.topLeft());
	painter.scale(box.width() / 24.0, box.height() / 24.0);
	QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(pen);
	painter.setBrush(Qt::NoBrush);

	switch (icon) {
	case DockIcon::folder:
		painter.drawPath(
			polyline({{3.5, 18.5}, {3.5, 6.5}, {9.0, 6.5}, {11.0, 8.5}, {20.5, 8.5}, {20.5, 18.5}}, true));
		break;
	case DockIcon::refresh: {
		// Clockwise from three o'clock to half past one, with the arrowhead at its end.
		const QRectF ring(4.0, 4.0, 16.0, 16.0);
		QPainterPath arc;
		arc.arcMoveTo(ring, 0.0);
		arc.arcTo(ring, 0.0, -315.0);
		painter.drawPath(arc);
		painter.drawPath(polyline({{20.0, 4.0}, {20.0, 9.0}, {15.0, 9.0}}));
		break;
	}
	case DockIcon::settings: {
		painter.drawEllipse(QPointF(12.0, 12.0), 6.0, 6.0);
		painter.drawEllipse(QPointF(12.0, 12.0), 2.2, 2.2);
		QPen teeth(color, 2.6, Qt::SolidLine, Qt::FlatCap);
		painter.setPen(teeth);
		line(painter, 12.0, 2.5, 12.0, 5.5);
		line(painter, 12.0, 18.5, 12.0, 21.5);
		line(painter, 2.5, 12.0, 5.5, 12.0);
		line(painter, 18.5, 12.0, 21.5, 12.0);
		line(painter, 5.3, 5.3, 7.4, 7.4);
		line(painter, 16.6, 16.6, 18.7, 18.7);
		line(painter, 5.3, 18.7, 7.4, 16.6);
		line(painter, 16.6, 7.4, 18.7, 5.3);
		break;
	}
	case DockIcon::search:
		painter.drawEllipse(QPointF(11.0, 11.0), 6.5, 6.5);
		line(painter, 15.8, 15.8, 20.0, 20.0);
		break;
	case DockIcon::open: {
		painter.setPen(Qt::NoPen);
		painter.setBrush(color);
		painter.drawPolygon(QPolygonF({{8.0, 5.5}, {18.5, 12.0}, {8.0, 18.5}}));
		break;
	}
	case DockIcon::more:
		dot(painter, color, 5.5, 12.0, 1.8);
		dot(painter, color, 12.0, 12.0, 1.8);
		dot(painter, color, 18.5, 12.0, 1.8);
		break;
	case DockIcon::replay: {
		// Counter-clockwise from nine o'clock, nearly all the way round, with an arrowhead and clock hands.
		const QRectF ring(4.0, 4.0, 16.0, 16.0);
		QPainterPath arc;
		arc.arcMoveTo(ring, 180.0);
		arc.arcTo(ring, 180.0, 315.0);
		painter.drawPath(arc);
		painter.drawPath(polyline({{4.0, 4.0}, {4.0, 8.5}, {8.5, 8.5}}));
		painter.drawPath(polyline({{12.0, 8.0}, {12.0, 12.0}, {14.5, 14.0}}));
		break;
	}
	case DockIcon::recording:
		painter.drawRoundedRect(QRectF(3.0, 6.0, 13.0, 12.0), 2.0, 2.0);
		painter.drawPath(polyline({{16.0, 10.5}, {21.0, 7.5}, {21.0, 16.5}, {16.0, 13.5}}, true));
		break;
	case DockIcon::screenshot:
		painter.drawRoundedRect(QRectF(3.5, 5.0, 17.0, 14.0), 2.0, 2.0);
		painter.drawEllipse(QPointF(9.0, 10.0), 1.6, 1.6);
		painter.drawPath(polyline({{20.5, 16.0}, {15.5, 11.0}, {7.0, 19.0}}));
		break;
	case DockIcon::check:
		painter.drawPath(polyline({{5.0, 12.5}, {9.5, 17.0}, {19.0, 7.5}}));
		break;
	case DockIcon::warning:
		painter.drawPath(polyline({{12.0, 3.5}, {21.5, 20.0}, {2.5, 20.0}}, true));
		line(painter, 12.0, 10.0, 12.0, 14.5);
		dot(painter, color, 12.0, 17.4, 1.2);
		break;
	case DockIcon::save:
		line(painter, 12.0, 4.0, 12.0, 15.0);
		painter.drawPath(polyline({{7.0, 10.0}, {12.0, 15.0}, {17.0, 10.0}}));
		line(painter, 5.0, 20.0, 19.0, 20.0);
		break;
	case DockIcon::clear:
		line(painter, 6.5, 6.5, 17.5, 17.5);
		line(painter, 17.5, 6.5, 6.5, 17.5);
		break;
	case DockIcon::gamepad:
		painter.drawRoundedRect(QRectF(3.0, 8.0, 18.0, 9.5), 4.5, 4.5);
		line(painter, 8.0, 10.6, 8.0, 14.6);
		line(painter, 6.0, 12.6, 10.0, 12.6);
		dot(painter, color, 15.6, 11.7, 1.0);
		dot(painter, color, 17.6, 13.6, 1.0);
		break;
	case DockIcon::trash:
		line(painter, 4.0, 7.0, 20.0, 7.0);
		painter.drawPath(polyline({{9.0, 7.0}, {9.0, 4.5}, {15.0, 4.5}, {15.0, 7.0}}));
		painter.drawPath(polyline({{6.5, 7.0}, {7.5, 19.5}, {16.5, 19.5}, {17.5, 7.0}}));
		break;
	case DockIcon::chevron_down:
		painter.drawPath(polyline({{7.0, 9.5}, {12.0, 14.5}, {17.0, 9.5}}));
		break;
	case DockIcon::chevron_up:
		painter.drawPath(polyline({{7.0, 14.5}, {12.0, 9.5}, {17.0, 14.5}}));
		break;
	}
	painter.restore();
}

QIcon dock_icon(DockIcon icon, const QPalette &palette, int size)
{
	const QColor normal = palette.color(QPalette::Active, QPalette::ButtonText);
	const QColor disabled = muted_text(palette);
	QIcon result;
	for (const qreal scale : kIconScales) {
		const int device = qCeil(size * scale);
		for (const auto &[mode, color] : {std::pair<QIcon::Mode, QColor>{QIcon::Normal, normal},
						  std::pair<QIcon::Mode, QColor>{QIcon::Active, normal},
						  std::pair<QIcon::Mode, QColor>{QIcon::Selected, normal},
						  std::pair<QIcon::Mode, QColor>{QIcon::Disabled, disabled}}) {
			QPixmap pixmap(device, device);
			pixmap.setDevicePixelRatio(scale);
			pixmap.fill(Qt::transparent);
			QPainter painter(&pixmap);
			paint_dock_icon(painter, icon, QRectF(0.0, 0.0, size, size), color);
			painter.end();
			result.addPixmap(pixmap, mode);
		}
	}
	return result;
}

QString initials_of(const QString &name)
{
	static const QRegularExpression separators(QStringLiteral("[ \\-:_]+"));
	QString initials;
	int taken = 0;
	for (const QString &word : name.split(separators, Qt::SkipEmptyParts)) {
		// The first letter or digit, read as a code point, so a letter outside the BMP keeps both of its halves.
		for (qsizetype i = 0; i < word.size(); ++i) {
			char32_t code = word.at(i).unicode();
			if (word.at(i).isHighSurrogate() && i + 1 < word.size() && word.at(i + 1).isLowSurrogate()) {
				code = QChar::surrogateToUcs4(word.at(i), word.at(i + 1));
				++i;
			}
			if (QChar::isLetterOrNumber(code)) {
				initials += QString::fromUcs4(&code, 1).toUpper();
				++taken;
				break;
			}
		}
		if (taken == 2)
			break;
	}
	return initials;
}

} // namespace clip_atlas

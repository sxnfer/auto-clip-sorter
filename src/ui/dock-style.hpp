// Clip Atlas — the dock's colours, fonts and icons, worked out from the OBS theme.
//
// Colours come from the palette that OBS builds from its theme (docs/handover/M6-dock-spec.md): Base is the dock,
// Window the darker fill of notices, Button the grey of inputs, chips and borders, Highlight the accent, and the
// Disabled group's Text the muted grey. Where a theme has no exact role, such as a hover fill, two roles are mixed, so
// the same code suits dark and light themes. Green and amber come from the theme's text-success and text-warning
// classes, read from probe labels polished against the application style sheet; a theme without those classes gets
// fixed colours chosen for its lightness.
//
// Icons are strokes on a 24-unit grid, painted in the colour asked for, so they follow the theme's text colour and stay
// sharp at any scale. The standard icons have no folder, gear or clip-kind glyphs.
//
// UI thread only.
#pragma once

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPalette>
#include <QString>

class QPainter;
class QRectF;

namespace clip_atlas {

// `from` moved `amount` (0-1) of the way to `to`, alpha included.
QColor mix_colors(const QColor &from, const QColor &to, qreal amount);

// `color` with its alpha multiplied by `opacity` (0-1).
QColor with_alpha(const QColor &color, qreal opacity);

// The grey for secondary text: the Disabled group's Text, which OBS themes set to their muted colour. A palette whose
// disabled text equals its normal text gets text mixed halfway towards the base instead.
QColor muted_text(const QPalette &palette);

// The fill of a row or chip under the mouse: `role` (Base or Button) moved a little towards Text.
QColor hover_fill(const QPalette &palette, QPalette::ColorRole role);

// The fill of a selected row or card: Base tinted with Highlight. Text keeps its own colour on it, so a theme's text
// stays readable whatever its accent.
QColor selection_fill(const QPalette &palette);

// Lines and outlines: Base mixed a little towards Text, which reads on dark and light bases alike.
QColor line_color(const QPalette &palette);

// The fill of the Replay Buffer pill, notices and the toast: Window, which OBS themes make a shade off Base, or Base
// mixed a little towards Text for a theme whose Window and Base are the same.
QColor notice_fill(const QPalette &palette);

// The accent as a text colour. Yami's Highlight is a deep blue that is hard to read on its dark base, so a dark base
// gets it lightened.
QColor accent_text(const QPalette &palette);

// True when the palette's Base is light, so fallback colours can be chosen to read on it.
bool is_light_palette(const QPalette &palette);

struct ThemeAccents {
	QColor success; // the theme's text-success colour
	QColor warning; // the theme's text-warning colour
};

// The theme's success and warning colours, worked out once from the application palette and style sheet and kept until
// reset_theme_accents(), because the probe labels are too costly to make on every paint. A class the theme doesn't
// define leaves its probe in the plain text colour; then a fallback for the palette's lightness is used.
const ThemeAccents &theme_accents();
// Forgets the colours above. The dock calls it when OBS changes the theme.
void reset_theme_accents();

// `base` with its size multiplied by `scale` (point or pixel size, whichever it uses) and the given weight. The dock
// paints its text with these, because OBS's style sheet overrides sizes set with setFont().
QFont scaled_font(const QFont &base, qreal scale, QFont::Weight weight = QFont::Normal);

enum class DockIcon {
	folder,
	refresh,
	settings,
	search,
	open,
	more,
	replay,
	recording,
	screenshot,
	check,
	warning,
	save,
	clear,
	gamepad,
	trash,
};

// Paints `icon` to fill the square `box`, in `color`.
void paint_dock_icon(QPainter &painter, DockIcon icon, const QRectF &box, const QColor &color);

// The icon for a QPushButton or a line edit action: ButtonText for the normal, active and selected modes, the muted
// grey for disabled, drawn at `size` logical pixels for each common device pixel ratio.
QIcon dock_icon(DockIcon icon, const QPalette &palette, int size);

// One or two letters for a game's avatar: the first letter or digit of each of the first two words ("Elden Ring"
// gives "ER", "Counter-Strike 2" gives "CS"), or of the only word. Words split on spaces, hyphens, colons and
// underscores, and a word with no letter or digit is skipped. Upper case; empty for a name with no letters or digits.
QString initials_of(const QString &name);

} // namespace clip_atlas

# Dock design explorations (October 2026)

Eleven directions for the Clip Atlas dock, from a careful refresh of 1.0 to looks that break with how OBS
docks usually look. Each one is drawn at real dock size and works: filters filter, Save saves, switches
switch.

- **Canvas:** <https://claude.ai/artifact/LNWg9TGEZWcTcCVQ3wwmxW> (private until you share it from its
  Share menu). Every artboard has Tweaks for its **state** (playing, saved, idle, problem, empty) and the
  **OBS theme** (Yami, Acri, Rachni, Grey, Light).
- **Sample data:** one Friday evening with 13 clips in five games, plus two clips left loose in the clips
  folder while auto-sort was off. The pictures are stand-in art, not game footage.
- **Status:** exploration only. Nothing here changes the plugin, and nothing here has been tried in OBS.

## What the research says

### Where the dock lives

- Docks get dragged into three kinds of place: a **tall side column** (about 300-400 x 600-900 px), the
  **bottom row** next to Scenes, Sources and the Audio Mixer (short and wide, often only about 200-260 px
  tall), or a **floating window**. The 1.0 layout assumes the side column. In the bottom row, the header,
  filter row and footer take about half the height, which leaves room for one or two clip rows.
- Many streamers have one monitor and never look at OBS while they play. For them the dock matters at
  two moments: *is the Replay Buffer armed?* before they play, and *did that clip save, and where did it
  go?* afterwards. 1.0 shows the first as a small dot and doesn't show the second at all. The new clip
  just appears in the list.

### OBS 31 theming: more room than the 1.0 rules assume

- Yami has been the default theme since OBS 28. In OBS 31, themes are `.obt` files with `.ovt` variants:
  Yami comes as Default, Acri, Classic, Grey, Light and Rachni.
- **Theme classes.** OBS 31.0's `Yami.obt` defines classes that any widget can opt into. Qt matches a
  `.name` selector against a widget's `class` dynamic property, so
  `button->setProperty("class", "icon-gear")` should give the theme's own gear icon. Defined in 31.0.0:
  `text-muted`, `text-warning`, `text-danger`, `text-success`, `text-heading`, `text-large`,
  `frame-notice`, `icon-gear`, `icon-refresh`, `icon-trash`, `icon-filter` and `icon-media-play`.
  `bg-warning` and `button-primary` arrive only in later versions. **To verify on Windows** before we rely
  on it. This would replace the ⚙ glyph that 1.0 uses because the standard icons have no gear.
- `OBS_FRONTEND_EVENT_THEME_CHANGED` exists in 31.0. It is useful for anything cached from the palette,
  such as tinted pixmaps. The 1.0 header already handles `QEvent::PaletteChange` and `StyleChange`.
- The mocks use the real palette values from `Yami.obt` and the variant files. For example, Yami's dock
  background is `#272A33`, the input background `#3C404D`, the primary `#284CB8` and muted text
  `#969696`. List hover is the bright `primary_light` (`#476BD7`), and in Rachni it is teal.

### Thumbnails

- Windows makes no Shell thumbnail for `.mkv` without a codec pack (see the 1.0 CHANGELOG). Many setups
  record replays as `.mkv`, so **many users will see file icons, not pictures**. Every
  picture-led concept needs a designed no-picture tile. The Baseline artboard shows the honest 1.0 case,
  and its `mkvThumbnails` tweak shows the codec-pack case.

### Clip tools outside OBS

- **Steam Game Recording:** a timeline with markers added by the player and the game, plus game-mode
  indicators. Time is the main axis.
- **NVIDIA App:** a gallery of thumbnails with sorting. Pictures come first.
- **Medal:** a library grouped by date, with filters for game, type and favourites, and an "Untitled"
  view for clips you haven't dealt with yet. That last view is a triage queue.
- **Outplayed:** capture modes you choose up front (highlights, full match, full session or manual).
- **Xbox Game Bar** (2025 refresh): larger targets and a compact mode for small surfaces.
- **Colour from artwork:** Material's dynamic colour takes a seed colour from a source image
  (`setContentBasedSource`). Music players commonly tint the UI from the album art.

## The concepts

Each entry gives the idea, the convention it breaks, why it helps, how it would be built in Qt Widgets,
and what it costs. "New data" means something the plugin doesn't track today.

### Baseline: the 1.0 dock

Rebuilt from `src/ui/` with the exact margins, sizes, strings and Yami values. It's the reference for
everything else.

### A · Native Plus (side column, bottom row, floating)

- **Idea:** only the OBS theme, used with more care. A game avatar and "Now playing", a status pill, an
  Auto-sort switch, a **Save replay** button, game chips with counts, a kind filter, date groups, short
  titles ("Replay 21:46:58", with the full file name as the tooltip), hover actions, and a toast:
  "Filed in Elden Ring › 2026-10".
- **Breaks:** nothing visual. It changes the layout by dock size: in the bottom row the header becomes a
  rail and the clips a sideways strip, and floating adds a preview pane with the actions as buttons.
- **Qt:** checkable `QToolButton` chips in a horizontal `QScrollArea`. Date headers as non-selectable rows
  from the model, with `setUniformItemSizes(false)`. Hover actions painted by the delegate and handled in
  `editorEvent`. The toast is an overlay `QFrame` with a `QPropertyAnimation`. The switch is a small
  `QAbstractButton` painted from the palette; OBS's own switch isn't exported to plugins. The strip is a
  `QListView` in `IconMode` with left-to-right flow and no wrapping. `resizeEvent` picks the layout
  through a pure `core/` function that can be unit-tested.
- **New data or APIs:** `obs_frontend_replay_buffer_save()` (in the 31.0 header). A kind field in
  `ClipQuery`. Day grouping in `core/`.
- **Cost:** medium. The lowest risk of all the concepts.

### B · Now Playing

- **Idea:** the current game is the hero. The latest clip, blurred, is the backdrop, and its dominant
  colour tints the Save button and the buffer ring. Below that come this session's clips as a strip, then
  the library.
- **Breaks:** colour comes from the clips, not the theme. Large display type in a utility panel.
- **Qt:** a `paintEvent` that draws the thumbnail scaled to cover and blurred once (downscale, then
  upscale smoothly, cached), with a gradient into the palette's base colour. The dominant colour is a
  pure function over the BGRA bytes we already get from the Shell, so it can be unit-tested. It needs a
  contrast check before any text sits on that colour.
- **New data:** when the current game was hooked (for "in game since 20:41"), which `GameState` doesn't
  expose.
- **Cost:** medium. **Risk:** the hero is about 250 px tall, so the bottom row needs a rail version.
  Without a picture it falls back to the theme gradient.

### C · Contact Sheet

- **Idea:** a light table of prints: two-up thumbnails, frame numbers, kind codes (RPL, REC, IMG), and
  grease-pencil marks for the new clip and for "keepers".
- **Breaks:** a grid instead of a list, and an analogue look.
- **Qt:** `QListView` in `IconMode` with a fixed grid and a delegate that paints the paper, a shadow
  from a cached nine-patch image, and the marks as a slightly jittered `QPainterPath`. Group labels don't
  exist in `IconMode`; drop them or use a custom view. A handwriting font would have to be bundled
  (OFL licence) or the marks drawn without words.
- **New data:** keepers (a favourites list in the settings that rename and move keep up to date).
- **Cost:** medium. **Risk:** with `.mkv` file icons instead of pictures, it loses its point.

### D · Atlas

- **Idea:** the product's name, taken literally. Each game is a territory sized by clip count or disk
  space (a squarified treemap). Click a territory to filter. "You are here" marks the current game, and
  clips that aren't sorted are uncharted, hatched water.
- **Breaks:** a map is the navigation. The **Disk** view shows where the space goes; in the sample data,
  one Hades II recording takes 65 %.
- **Qt:** the treemap is a pure function in `core/` (easy to test). A custom widget paints the
  territories, with contour rings rendered once per territory into a pixmap, and hit-tests clicks.
  It needs a `QAccessibleInterface`, or the game list kept as an accessible fallback.
- **Cost:** medium to large. **Risk:** with one or two games, the map says little.

### E · Session River

- **Idea:** clips are grouped into play sessions on a timeline that runs from **Now** into the past. Each
  session is coloured by its game. Breaks are shown ("1 h 18 min without clips"), and older days fold
  away.
- **Breaks:** time is the layout, as in Steam's timeline.
- **Qt:** `group_sessions(clips, gap)` in `core/`, using the `modified` times we already have. The model
  mixes session headers, clips, gaps and day rows, and the delegate paints the river from flags such as
  "line above" and "line below".
- **Cost:** medium. **Risk:** recordings are placed at the time they were saved, not when they started.

### F · Triage

- **Idea:** auto-sort files most clips, so the dock is about the rest. Clips that need a home come up one
  at a time as large cards, with suggested games ranked by how close in time they were played. File a
  clip with a click or the keys 1, 2 and 3, or undo. Below the card, a "Filed by Clip Atlas" list shows
  where each clip went.
- **Breaks:** a task to finish, inbox-style, instead of a list to browse.
- **Qt:** the card is a `QFrame`, the keys are `QShortcut`s scoped to the dock, and filing reuses the
  existing Move to game path (`FileMover`). A pure `suggest_games(clip, index)` goes in `core/`.
- **New data:** a rule for what needs a home: clips loose in the clips folder (game folder `""`) and clips
  in the Unsorted folder.
- **Cost:** small to medium.

### G · Flight Recorder

- **Idea:** a plain log of everything Clip Atlas did: games found, buffer started and stopped, each file
  filed and where, and sorting paused by the hotkey. Clip lines can be acted on. A command line at the
  bottom takes `open`, `move`, `find`, `save`, `sort off` and so on.
- **Breaks:** text and keyboard first. The automation explains itself, which builds trust and helps
  with support ("copy the log").
- **Qt:** an `EventLog` ring buffer in `core/` (pure, tested). `plugin-main` feeds it from the game,
  replay-status, sorted-clip and settings callbacks. The view is a `QListView` with a monospace delegate
  (Cascadia Mono or Consolas), and the command line is a `QLineEdit` with a `QCompleter`. Tag colours
  can come from `text-success`, `text-warning` and `text-danger`.
- **Cost:** medium. Good as a second view even if another concept is the main one.

### H · Tally Deck

- **Idea:** broadcast hardware. A tally lamp you can read from across the room (amber ARMED, red SAVED,
  dark STANDBY), a dot-matrix display with a buffer meter, chunky keys, and a printed tape log with
  dithered thumbnails.
- **Breaks:** skeuomorphism and physical motion, ignoring the theme on purpose.
- **Qt:** all `QPainter`: radial gradients for the lamp, a 5 x 7 dot-matrix glyph table painted as dots,
  keys as `QAbstractButton` subclasses with a pressed offset, and the tape as a delegate. Bahnschrift
  (ships with Windows 10 and later) for the labels.
- **New data:** the buffer length, read like the replay prefix: `RecRBTime` under `SimpleOutput` or
  `AdvOut`, depending on the output mode (checked against 31.0's `window-basic-main-outputs.cpp`).
- **Cost:** large. **Risk:** looks wrong next to the Light theme.

### I · Lower Third

- **Idea:** esports broadcast graphics. A scorebug header (LIVE, the game, session time, clip count),
  a big slanted SAVE REPLAY, match-card rows with game-colour edges, and a lower-third wipe:
  "CLIP SAVED · ELDEN RING / 2026-10".
- **Breaks:** speaks the stream's own visual language: slants, condensed capitals, motion.
- **Qt:** `QPainterPath` parallelograms (or `QTransform` shear), Bahnschrift SemiCondensed, and a
  `QVariantAnimation` driving the wipe's clip rect.
- **Cost:** medium. **Risk:** condensed capitals hurt long file names, and translations run longer.

### J · Editorial

- **Idea:** a quiet, typographic index. A serif headline with a one-line summary, numbered sections per
  game, dotted leaders to the time, and grey thumbnails that colour on hover. Night and paper editions.
- **Breaks:** whitespace and restraint in a dense app. The opposite of gamer tropes.
- **Qt:** Sitka (ships with Windows) or Georgia for the serif. Small capitals with
  `QFont::setCapitalization`, leaders with a dotted `QPen`. Greyscale copies of the thumbnails are made
  once and cached.
- **New data:** the summary sentences need plural forms per language, but `obs_module_text` has no
  plurals, so each case needs its own key.
- **Cost:** small to medium. **Risk:** the lowest density; the paper edition is loud in a dark theme.

### K · Ambient

- **Idea:** for a glance from across the room, the dock is a colour field with one word: ARMED, SAVED,
  STANDBY or CHECK OBS. A strip, compact or full layout is chosen by height. It moves only while the
  buffer runs, so stillness means nothing is being kept.
- **Breaks:** the dock as a status light, not a panel of controls.
- **Qt:** height breakpoints in `resizeEvent` (a pure function again). The field is painted, and the
  breathing animation runs only while the buffer is active and the dock is visible. The states differ
  in lightness and words, not hue alone.
- **Cost:** small to medium. **Risk:** a bright field shows on stream if the OBS window itself is
  captured.

## Side by side

Rough figures for a 360 x 720 side-column dock.

| Concept | Theme fidelity | Readable from across the room | Clips visible | Build cost | New data |
|---|---|---|---|---|---|
| Baseline | full | no | ~8 | done | none |
| A · Native Plus | full | a little (pill, toast) | ~7 | M | save API, kind filter, day groups |
| B · Now Playing | chrome only | yes (hero) | ~5 + strip | M | hook time, dominant colour |
| C · Contact Sheet | no | no | ~6 | M | keepers |
| D · Atlas | chrome only | no | ~4 + map | M-L | treemap |
| E · Session River | full | no | ~8 | M | sessions |
| F · Triage | full | no | 1 card + ~3 | S-M | suggestions, triage rule |
| G · Flight Recorder | mostly | no | ~14 log lines | M | event log |
| H · Tally Deck | no | yes (lamp) | ~8 | L | buffer length |
| I · Lower Third | no | yes (wipe) | ~5 | M | hook time |
| J · Editorial | no | no | ~6 | S-M | plural strings |
| K · Ambient | chrome only | yes (whole dock) | ~11 (full) | S-M | none |

## Ideas worth keeping whichever direction wins

1. **A confirmation when a clip is filed:** "Filed in <Game> › <YYYY-MM>", with a Show button. This is the
   biggest gap in 1.0.
2. **Save replay from the dock** (`obs_frontend_replay_buffer_save()`).
3. **Layouts by dock size**, so the bottom row gets a strip instead of one and a half rows.
4. **Short titles** for OBS's default names, with the real file name in the tooltip and in search.
5. **A designed tile for clips with no picture** (the `.mkv` case).
6. **Theme classes** for icons and notices (`icon-gear`, `frame-notice`, `text-warning`), if they work in
   a plugin on Windows.
7. **Pure `core/` helpers,** each unit-tested: day and session grouping, dominant colour, treemap layout,
   game suggestions, layout breakpoints, an event log.
8. **No way to open OBS settings:** OBS 31 has no frontend function for it, so the "Replay Buffer is off"
   notice can only explain where the setting is, not open it.

## Recommendation

Build **A · Native Plus** next, with its bottom-row and floating layouts. It fixes the real problems
(the bottom-row squeeze, no feedback on save, the ⚙ glyph, raw file names) without leaving the theme.
Add **F · Triage** for clips that need a home, and **G · Flight Recorder** as a second view, since trust
in the automation is what the dock is for. Keep one expressive look for later, as an optional dock
style: **B**'s colour from the clip art is the most distinctive while staying tasteful, and **K** solves
the across-the-room glance for streamers with one monitor.

## Sources

- OBS themes: [Yami.obt (master)](https://github.com/obsproject/obs-studio/blob/master/frontend/data/themes/Yami.obt),
  [Yami.obt (31.0.0)](https://github.com/obsproject/obs-studio/blob/31.0.0/UI/data/themes/Yami.obt), and the
  variant files in [frontend/data/themes](https://github.com/obsproject/obs-studio/tree/master/frontend/data/themes)
- OBS 31.0 frontend API: [obs-frontend-api.h](https://github.com/obsproject/obs-studio/blob/31.0.0/UI/obs-frontend-api/obs-frontend-api.h);
  buffer settings in [window-basic-main-outputs.cpp](https://github.com/obsproject/obs-studio/blob/31.0.0/UI/window-basic-main-outputs.cpp)
- Steam Game Recording: [Make Tech Easier](https://maketecheasier.com/review-gameplay-with-steam-game-recording),
  [Steam Deck HQ](https://steamdeckhq.com/tips-and-guides/how-to-use-game-recording-on-the-steam-deck-all-features-explained/)
- NVIDIA App: [release notes](https://www.nvidia.com/en-us/geforce/news/nvidia-app-download-and-features/)
- Medal: [managing clips](https://support.medal.tv/support/solutions/articles/48001164598-how-to-manage-clips-on-medal),
  [getting started on Windows](https://support.medal.tv/support/solutions/articles/48000959661-getting-started-with-medal-on-windows)
- Outplayed: [set-up guide](https://support.overwolf.com/support/solutions/articles/9000268210-outplayed-quick-set-up-guide)
- Xbox Game Bar 2025 refresh: [AlternativeTo](https://alternativeto.net/news/2025/3/microsoft-revamps-its-xbox-game-bar-ui-with-an-improved-design-for-handheld-devices/)
- Dynamic colour: [Android dynamic color](https://source.android.com/docs/core/display/dynamic-color),
  [DynamicColorsOptions.Builder](https://developer.android.com/reference/com/google/android/material/color/DynamicColorsOptions.Builder)

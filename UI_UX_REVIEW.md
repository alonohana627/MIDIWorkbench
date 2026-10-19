# MIDI Workbench: UI/UX review and redesign plan

Reviewer view: a desktop UI/UX pass over the Qt Widgets app as built in `build/debug`.
Evidence: `docs/images/main-window.png` (1400x860, demo.mid) and fresh offscreen captures of
`demo.mid` and a blank document at the offscreen default size (744x533). Offscreen captures use
Qt's fallback font, so glyph shapes (the subscript-looking digits in "Track 1", "C4") are not
what Windows shows; layout, sizes and colors are the same.

Scope rules for the plan: Qt 6 Widgets only, no new dependency. QtSvg is **not** installed
(only Core/Gui/Widgets are linked, `CMakeLists.txt:52`), so icons are painted with QPainter.
New `.cpp/.hpp` files under `src/ui` and `include/mwb/ui` are picked up by the glob in
`cmake/MwbModule.cmake:22`.

---

## 1. Verdict: what a user feels in the first minute

1. **The editor is the smallest thing on screen.** At 1400x860 the note grid gets about 470 of
   1400 px (34%): Library dock ~260 px (empty, "No library open"), track list 200 px
   (`editor_tab.cpp:229`), inspector ~380 px, keyboard gutter 56 px, plus a second horizontal
   keyboard of ~75 px under the roll (`editor_tab.cpp:214`) that lines up with nothing.
   `main_window.cpp:197` calls `resize(1400, 860)` and never `resizeDocks`, so dock widths are
   whatever the size hints give. At small sizes the inspector title truncates to "In..." and its
   labels to "Velocit", "Chann" (offscreen 744x533 capture).
2. **You cannot find how to draw a note.** The edit tools (Select/Draw/Erase/Split/Audition) exist
   only in the *Tools* menu (`main_window.cpp:308-312`, `:512`); only Draw/Select toggle has a key
   (B, `:305`). The toolbar holds Play/Stop/Loop/Metronome/Tempo/Drag MIDI and a blank 180 px label
   (`:589-593`). Snap lives at the bottom of the inspector's third group box. Note color mode
   (`piano_roll.cpp:77`, velocity/channel/scale degree) has no UI at all.
3. **No visual identity, harsh grid.** No style, palette or font is set anywhere
   (`midiworkbench_main.cpp:19-24`; zero `setStyle/setPalette/setFont` in `src/ui`). The grid draws
   every bar in `QPalette::Text` (pure black, `piano_roll.cpp:320`) and every 1/16 snap line, so the
   roll is a cage of black and grey bars; the gutter keys are hardcoded light colors
   (`piano_roll.cpp:530`) so a dark theme would be impossible today. The spec requires
   dark/light/high-contrast (spec 29.3).
4. **Selection is nearly invisible.** Notes default to `#4F8FE8` (`piano_roll.cpp:371,374`) and
   selection is a 2 px `QPalette::Highlight` outline (`:423`), which in the default palette is also
   a mid blue. Selected and unselected notes look the same at a glance; the transform tools in the
   inspector then act on "the active track" because nothing *looks* selected.
5. **Opening a file does not show the file; a blank file shows nothing.** `fit_all`
   (`piano_roll.cpp:1114-1127`) sets `top_pitch = highest + 4` and ignores the lowest note, so the
   demo's chords (C2-G3) are below the fold at the default height; a blank document opens at
   `top_pitch = 84` (C6, `:61`) with no hint at all. The status bar reads "0 selected  Snap 1/16"
   with no separators, and any `showMessage` (used ~30 times) *hides* the cursor/selection labels
   because they were added with `addWidget` (`main_window.cpp:155-157`).

Smaller but felt: unicode-glyph "icons" (▶ ■ ⟲ ♩ ⧉ ↑ ↓) depend on font coverage; M/S/R are bare
checkboxes; the inspector shows disabled spinboxes full of fake values (0, 240, 60, 100, 1) when
nothing is selected and exposes Start/Length in raw ticks and a JSON "All parameters" field
(`inspector.cpp:163-164, 297`); the command palette (Ctrl+Shift+P) is never advertised.

---

## 2. Target design

### 2.1 Layout (default and "Reset Layout", 1400x860)

```
+-------------------------------------------------------------------------------------------+
| Menu                                                                                      |
| [Main toolbar: file | undo | tools | snap | transport | position | ... | panels | search]  |
+-------------------------------------------------------------------------------------------+
| Library (dock, L) |  doc tabs                                           | Inspector (dock, R)|
|  hidden on first  |  +-tracks 168px-+-ruler 28px-------------------+    |  300 px            |
|  run; 260 px when |  |  ■ Track 1 MS|  keys 56 | note grid         |    |  collapsible       |
|  a library is     |  |  ...         |          |                   |    |  sections          |
|  open             |  |  [+][⧉][−]   |  Velocity lane 72 px         |    |                    |
+-------------------+--+--------------+------------------------------+----+--------------------+
| status: Bar 3.2.120 · E4 (64) | 3 notes | Snap 1/16 | C major     msg...   Recovery ✓ | Audio |
+-------------------------------------------------------------------------------------------+
```

- **Library dock**: left, 260 px. Hidden on first run (no library configured); shown automatically
  and remembered once a library is opened. Toolbar toggle button "Library" (Ctrl+1).
- **Inspector dock**: right, 300 px default, min 260. Toggle "Inspector" (Ctrl+2).
- **Event list dock**: bottom, hidden, 200 px tall when shown (Ctrl+3).
- **Track list**: 168 px (was 200), row height 28 px.
- **On-screen keyboard** (`KeyboardWidget`): hidden by default; shown when Playing mode is on or via
  View > On-screen Keyboard. It stays in the splitter so the spec's "bottom editor" item remains.
- **Velocity lane**: 72 px default via splitter sizes, not stretch factor 1 of 6.
- Result at 1400 wide, first run: grid ≈ 1400 − 168 − 56 − 300 − scrollbars ≈ 850 px (was ~470).
- Bump `layout_schema_version` (`main_window.cpp:60`) to 2 so old saved states do not override it.

### 2.2 Toolbars

One main toolbar (`toolbar.main`, not movable, icon size 20 px, `Qt::ToolButtonIconOnly` except
Play which is `TextBesideIcon`), left to right:

| Group | Contents |
|---|---|
| File | New (menu button with the 5 templates), Open, Save |
| History | Undo, Redo |
| Tools (exclusive `QActionGroup`, checkable) | Select **1**, Draw **2**, Erase **3**, Split **4**, Audition **5** |
| Grid | Snap combo (`1/4 1/8 1/16 1/32 1/8T 1/16T Off`), width 84 px |
| Transport | Return-to-start, Play/Pause, Record (red dot), Loop, Metronome, Tempo × spin |
| Readout | Position label, monospace, `3.2.120  ▶ Playing`, fixed 150 px, framed |
| (stretch) | |
| Export | Drag MIDI handle (existing `DragHandle`) |
| Panels | Library, Inspector toggles (the docks' `toggleViewAction()`) |
| Search | `QLineEdit` 220 px, placeholder "Search commands  Ctrl+Shift+P"; focus opens palette |

Digits 1-5 are free (the computer-keyboard player uses A-K, Z/X/C/V; `keyboard_widget.cpp:235-243`).
Tools must reflect state: the checked tool button follows `PianoRollView::set_tool` and B.

### 2.3 Theme: Fusion + QPalette + a small stylesheet

`QApplication::setStyle("Fusion")` always (native Windows style ignores QPalette for most
controls). Theme choice in View > Theme: System (follows `QStyleHints::colorScheme()`), Dark,
Light, High contrast; stored as `QSettings "ui/theme"`. Default: System.

| Role | Dark | Light | High contrast |
|---|---|---|---|
| Window | `#1E1F22` | `#F3F4F6` | `#000000` |
| WindowText / Text / ButtonText | `#E4E5E8` | `#1B1C1F` | `#FFFFFF` |
| Base | `#17181B` | `#FFFFFF` | `#000000` |
| AlternateBase | `#202226` | `#F7F8FA` | `#0F0F0F` |
| Button | `#2A2C31` | `#FBFBFC` | `#000000` |
| Mid / Midlight / Dark | `#3A3D43` / `#2F3237` / `#111214` | `#C9CCD2` / `#E3E5E9` / `#A0A4AC` | `#FFFFFF` ×3 |
| Highlight / HighlightedText | `#2F6FDB` / `#FFFFFF` | `#2563EB` / `#FFFFFF` | `#FFD400` / `#000000` |
| PlaceholderText | `#8B8E96` | `#6B6F78` | `#C0C0C0` |
| Disabled Text | `#6B6E75` | `#A3A6AD` | `#8A8A8A` |
| ToolTipBase / ToolTipText | `#2A2C31` / `#E4E5E8` | `#1B1C1F` / `#FFFFFF` | `#000000` / `#FFFFFF` |
| Link | `#6AA6FF` | `#1D4ED8` | `#00FFFF` |
| Accent "danger" (record, errors) | `#F0524B` | `#D92D20` | `#FF5050` |

Contrast (WCAG): dark text/window 13.9:1, placeholder on base 5.5:1, highlighted text 4.9:1;
light text/window 15.6:1, placeholder on base 5.2:1, highlighted text 5.2:1. All ≥ 4.5:1.

Stylesheet (keep it this small; everything else comes from the palette):

```css
QToolBar { border: 0; spacing: 2px; padding: 3px 6px; }
QToolBar::separator { width: 1px; margin: 4px 6px; background: palette(mid); }
QToolButton { padding: 4px; border-radius: 4px; }
QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); }
QDockWidget::title { padding: 6px 8px; font-weight: 600; background: palette(window); }
QStatusBar { border-top: 1px solid palette(mid); }
QStatusBar::item { border: 0; }
QGroupBox { font-weight: 600; margin-top: 14px; }
QTabBar::tab { padding: 6px 14px; }
*:focus { outline: none; }
QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QTableView:focus, QListView:focus
  { border: 2px solid palette(highlight); border-radius: 3px; }
```

### 2.4 Typography

- Family list (Windows first): `{"Segoe UI Variable Text", "Segoe UI", "Tahoma", "Noto Sans",
  "DejaVu Sans"}` via `QFont::setFamilies`. Monospace for readouts: `{"Cascadia Mono",
  "Consolas", "DejaVu Sans Mono"}` via a `mono_font()` helper (position label, cursor label,
  event-list tick columns).
- **Size bump: base point size +1 pt** (Windows default 9 pt → 10 pt), applied once in `main()`
  before any widget exists: `QFont f = QApplication::font(); f.setPointSizeF(f.pointSizeF() + 1);
  QApplication::setFont(f);`. Every widget that today uses `font()` inherits it, so relative sizes
  stay. Do not set pixel sizes anywhere; Windows text scaling keeps working.
- Two derived sizes only: section headers = base, weight 600; secondary/help text = base −1 pt in
  `PlaceholderText` color (inspector notes, library status). Piano-roll note labels keep their
  row-height-derived size (`piano_roll.cpp:394`); keyboard octave labels = base −1 pt.

### 2.5 Spacing and sizes

- 4 px grid. Dock content margins 8 px; form vertical spacing 6 px; group box inner margin 8 px.
- Minimum hit target 24 px (spec 29.3): tool buttons 28x28, track rows 28 px, M/S/R toggles 22x20.
- Ruler 28 px (was 24) so a +1 pt bar number fits; keep `keyboard_width = 56` (tests use it).
- Controller lane `left_margin_` must stay equal to `PianoRollView::keyboard_width` (it is 56 in
  `controller_lane.hpp:70`); make it reference the constant instead of a literal.

### 2.6 Icons

`resources/` has only the app icon (`midiworkbench.svg/png/ico`). No QtSvg. Add
`mwb::ui::make_icon(IconId, const QPalette&)` that paints 16/20/24/32 px pixmaps with QPainter
paths in `WindowText` color (plus a `Highlight`-colored disabled/checked variant via `QIcon::On`).
Set: play (triangle), pause (two bars), stop (square), return (bar + triangle), record (circle in
danger color), loop (two arcs), metronome (trapezoid + needle), select (arrow), draw (pencil),
erase (eraser block), split (vertical scissors line), audition (speaker), snap (magnet), new, open,
save, undo, redo, library (stacked books), inspector (sliders), add/duplicate/delete/up/down for
tracks. Rebuild icons on theme change. Where Qt has a matching `QStyle::StandardPixmap` (dialog
save/open), use it only as a fallback.

### 2.7 Piano roll visuals

All roll colors come from one `RollColors` struct built from the active theme (no literals in paint
code):

| Element | Dark | Light |
|---|---|---|
| White-key row | `#1D1F23` | `#FFFFFF` |
| Black-key row | `#18191C` | `#F1F2F5` |
| C-row divider (bottom of each C) | `#3A3D43` | `#C3C7CE` |
| Snap line (only if ≥ 8 px apart, was 6) | `#24262A` | `#EDEEF1` |
| Beat line | `#30333A` | `#D9DCE1` |
| Bar line (1 px) / every 4th bar (2 px) | `#50555E` | `#9AA0AA` |
| Scale root tint / member tint | accent α 56 / α 22 | accent α 46 / α 18 |
| Loop region (ruler + 6% fill over grid) | `#3FB37F` | `#2E9E6B` |
| Edit cursor | 1 px dashed `#9AA0AA` | 1 px dashed `#6B6F78` |
| Playhead | 2 px danger color + 8 px triangle in ruler | same |

- Notes: track color is the hue; **velocity drives lightness** in all modes:
  `L = 0.38 + 0.30 * v/127` (dark) or `0.62 − 0.22 * v/127` (light), S = 0.65. Velocity color
  mode stays as is but uses a perceptual ramp `#3B82F6 → #A855F7 → #F43F5E`.
- Corner radius 2 px, 1 px outline = fill darker(140).
- **Selected**: fill lighter(135) + 2 px outline in `WindowText` (white on dark, near-black on
  light) + 1 px inner `Base` line. Never the Highlight blue. Out-of-scale stays dashed orange and
  invalid stays dotted red (shape cues per spec).
- Sounding notes: 2 px top strip in `#FFFFFF` α 160 instead of a full wash.
- Gutter keys use `#E9EAED` / `#2B2D31` (dark) and `#FFFFFF` / `#2B2D31` (light); C labels in base
  −1 pt, black on white keys; hover row highlighted in the gutter and as a 1 px row tint in the grid.
- Ruler: `Window` background, bar numbers at every bar ≥ 28 px apart, beat ticks 4 px, bar ticks
  10 px, loop bracket as a 6 px band with 3 px end caps, region names in `PlaceholderText`.
- Empty state (active track has no notes): centered text in `PlaceholderText`, two lines:
  "Press 2 for Draw, then click to add notes · Space to play" /
  "Ctrl+O opens a MIDI file · drop .mid files anywhere · Ctrl+Shift+P for all commands".
- Initial and fit view: center the note range vertically; if it does not fit, reduce `row_height`
  down to 8 px; with no notes center C4 (pitch 60).

### 2.8 First run and empty states

- First run (no `layout/schema` key): Compose layout (2.1), blank "Untitled" document with one
  track, roll centered on C4, the roll empty-state text, status bar shows the persistent hint
  "Tip: Ctrl+Shift+P finds any command" until the first edit.
- Library dock empty state (no library open): a `QStackedWidget` page with heading "No library
  yet", one sentence ("A library indexes your MIDI files so you can search by tag, key, tempo and
  melody."), primary button "Create or Open Library…", secondary "Import a Folder…" (disabled with
  tooltip until a library is open). Table/search only appear on the second page.
- Library with zero results: centered "No matches for <query>" + "Clear search" link.
- Inspector with no selection: hide the five numeric rows (`QFormLayout::setRowVisible`) and show
  "Select notes to edit start, length, pitch and velocity."

### 2.9 Status bar

Permanent, separated sections (all `addPermanentWidget` or a left `QWidget` container that
`showMessage` cannot hide); transient messages go into a dedicated `message_label_` that clears
after its timeout:

`[cursor: 3.2.120 · E4 (64), mono, 170 px] | [3 notes selected] | [Snap 1/16] | [C major] |
[message ... stretch] | [Recovery ✓ 12:03:41] | [WASAPI 48 kHz · 10 ms  CLIP]`

Clicking Snap opens the snap menu; clicking the scale focuses the inspector scale section. Warnings
("Recovery not saved", "CLIP") use the danger color plus a "!" prefix (not color alone).

### 2.10 Discoverability

- Every toolbar tooltip: name + shortcut from the action, built from `QAction::shortcut()` so user
  rebinding is reflected: "Draw tool (2)". Re-run after the shortcut editor closes.
- Menus already show shortcuts; add the five tool shortcuts and Ctrl+1/2/3 panel toggles.
- Command palette: show the shortcut right-aligned per row and the category ("Edit", "View");
  open it from the toolbar search field.
- Disabled commands keep their explanatory tooltip (already done for undo/selection, `:728, :750`).
- Help > "Getting Started" opens a short dialog with the 8 essential keys (tools 1-5, B, Space, R,
  F, Q, Ctrl+Shift+P, F12).

### 2.11 Accessibility

- Contrast: all text pairs ≥ 4.5:1 (table 2.3); grid lines are decoration but bar lines ≥ 3:1 vs
  rows in High contrast (`#FFFFFF` on `#000000`).
- Focus ring: 2 px `Highlight` border on inputs (stylesheet) and a 2 px inset rect on the piano
  roll, drum grid and controller lanes (roll already draws 1 px at `piano_roll.cpp:292`).
- Keyboard: controller lanes `Qt::ClickFocus` (`controller_lane.cpp:59`) → `Qt::StrongFocus` with
  arrow keys moving the selected point; set a tab order: toolbar search → track list → piano roll →
  lanes → inspector. F6 / Shift+F6 cycle docks (standard Windows pane navigation).
- Accessible names for every new icon-only tool button (= the action text).
- High-contrast theme uses solid fills for selected notes (yellow `#FFD400` outline 3 px).

---

## 3. Implementation list (in order)

Each item builds and runs on its own. Run `ui_tests` after items touching `piano_roll.cpp` and
`editor_tab.cpp` (tests use `PianoRollView::keyboard_width` and the row mapping).

**Foundation**

1. **P1 Base font +1 pt and family list.** `src/ui/main/midiworkbench_main.cpp`, `main()`, right
   after `QApplication application(...)`: call new `mwb::ui::apply_base_font(application)` that
   reads `QApplication::font()`, sets families (2.4) and `pointSizeF() + 1.0`, then
   `QApplication::setFont`. Put it in new `src/ui/theme.cpp` / `include/mwb/ui/theme.hpp`.
2. **P1 Fusion + palettes.** `theme.hpp/.cpp`: `enum class Theme { system, dark, light,
   high_contrast }`, `QPalette make_palette(Theme)` with the exact values of 2.3 (set Active,
   Inactive and Disabled groups), `QString base_stylesheet()` (2.3), `void apply_theme(Theme)` which
   calls `QApplication::setStyle("Fusion")`, `setPalette`, `qApp->setStyleSheet`. For `system`,
   resolve via `QGuiApplication::styleHints()->colorScheme()` and reconnect on
   `colorSchemeChanged`. Call it from `main()` after item 1 with `QSettings "ui/theme"`.
3. **P1 Theme menu.** `main_window.cpp`, `build_actions()`: add exclusive checkable commands
   `view.theme_system/dark/light/high_contrast`; `build_menus()`: add a "Theme" submenu under View.
   On trigger: `apply_theme`, save setting, `update()` the current tab's views, rebuild icons.
4. **P1 RollColors.** `include/mwb/ui/theme.hpp`: `struct RollColors { QColor white_row,
   black_row, c_line, snap, beat, bar, bar_strong, root_tint, member_tint, loop, cursor, playhead,
   key_white, key_black, key_label; }` and `RollColors roll_colors()` for the current theme (2.7).
   Used by items 9-12.

**Layout**

5. **P1 Default dock sizes and first-run layout.** `main_window.cpp`: bump `layout_schema_version`
   to 2 (`:60`). In the constructor `else` branch (`:196-198`) after `resize(1400, 860)`: call
   `apply_layout("compose")`, then `resizeDocks({inspector_dock_}, {300}, Qt::Horizontal)`.
   `build_docks()`: `inspector_->setMinimumWidth(260)`. `reset_layout()` (`:1209`): same sizes,
   library shown only if `library_->is_open()` (add that accessor to `LibraryPanel`), width 260.
   `apply_layout("library")` (`:1197`): use 420 px instead of `width() / 2`.
6. **P1 Editor tab proportions.** `editor_tab.cpp` constructor: `horizontal->setSizes({168,
   1000})` (`:229`); after building `vertical`, `vertical->setSizes({600, 72, 0})` and
   `keyboard_->hide()`; add `EditorTab::set_keyboard_visible(bool)`. In `main_window.cpp` toggle it
   from `view.playing_mode` and a new `view.onscreen_keyboard` checkable command.
7. **P1 Fit and initial view.** `piano_roll.cpp`, `fit_all()` (`:1114`): compute low and high
   pitch; `rows = note_area().height() / row_height`; if `high - low + 4 > rows` set `row_height =
   max(8.0, note_area().height() / (high - low + 4))`; `top_pitch = clamp((low + high) / 2 + rows
   / 2, 0, 127)`. With no notes: `top_pitch = 60 + rows / 2`. In `main_window.cpp` `connect_tab` /
   `open_document`, call `fit_all()` via `QTimer::singleShot(0, ...)` so it runs after layout
   (today it runs before the tab has its real size).

**Toolbar and commands**

8. **P1 Icon factory.** New `include/mwb/ui/icons.hpp`, `src/ui/icons.cpp`: `enum class IconId`
   (2.6) and `QIcon make_icon(IconId)` painting antialiased paths at 16/20/24/32 px in
   `qApp->palette().windowText()`, checked state in `highlightedText()`. No resources needed.
9. **P1 Main toolbar.** `main_window.cpp`, replace `build_transport()` (`:563-594`) with
   `build_toolbar()` per 2.2: reuse existing command actions (`file.new`, `edit.undo`,
   `tool.*`, `transport.*`) instead of creating anonymous ones (today Play/Stop are separate
   actions from `transport.play_pause/stop`, so shortcuts and tooltips diverge). Put
   `tool.select..audition` in a `QActionGroup` (exclusive, checkable) and give them shortcuts
   `Qt::Key_1..Key_5` in `build_actions()` (`:308-312`). Keep object names `toolbar.transport` →
   rename `toolbar.main` (schema bump covers it). Keep `transport.position` label but set
   `mono_font()`, fixed width 150, `QFrame::StyledPanel`.
10. **P1 Tool state sync.** `piano_roll.hpp/.cpp`: add `signal tool_changed(EditTool)` emitted in
    `set_tool()` (`:65`). `main_window.cpp` `connect_tab` and `current_tab_changed`: check the
    matching action. B (`tool.toggle_draw`) then updates the toolbar too.
11. **P1 Snap combo on the toolbar.** `main_window.cpp` `build_toolbar()`: `QComboBox` with the
    values of the inspector's `snap_` combo; both write through the same controller call the
    inspector uses (`inspector.cpp` near `:247`); update both from `view_state_changed`.
12. **P2 Tooltips with shortcuts.** `main_window.cpp`: `refresh_tooltips()` sets
    `action->setToolTip(QString("%1 (%2)").arg(text_without_ampersand, shortcut.toString(
    QKeySequence::NativeText)))` for every toolbar action; call after `build_toolbar()`, after the
    shortcut editor is accepted (`:480-483`) and after the saved profile loads (`:143-147`). Keep the
    disabled-reason tooltips from `update_command_state()` (`:721-750`) taking precedence.
13. **P2 Command search field.** `build_toolbar()`: `QLineEdit` 220 px, placeholder "Search
    commands  Ctrl+Shift+P"; on `focusIn`/`textEdited` open `CommandPalette` prefilled
    (add `CommandPalette::set_query(QString)` in `command_palette.cpp`). Palette rows
    (`command_palette.cpp:47-52`): append the shortcut, right-aligned via a 2-column
    `QListWidgetItem` text with tab or a tiny delegate.

**Piano roll painting**

14. **P1 Grid and rows from RollColors.** `piano_roll.cpp` `paint_rows()` (`:298-314`) and
    `paint_grid()` (`:316-353`): replace `palette()` lookups with `roll_colors()`; snap threshold
    6.0 → 8.0 px; bar line every bar in `bar`, every 4th bar 2 px in `bar_strong`; draw the C
    divider with `c_line`. `paint_keyboard()` (`:522-554`): replace the literals at `:530, :534,
    :538, :548`.
15. **P1 Selection that reads.** `piano_roll.cpp` `paint_notes()` (`:380-436`): selected → fill
    `lighter(135)`, outer 2 px pen `palette().windowText()`, inner 1 px `palette().base()` rect
    adjusted by 2 px; unselected outline = `fill.darker(140)`; `drawRoundedRect(body, 2, 2)`.
    Sounding notes (`:420`): 2 px top strip instead of a wash. `controller_lane.cpp:136`: same
    selected color rule for stems.
16. **P1 Velocity as lightness.** `piano_roll.cpp` `note_color()` (`:356-375`): for track and
    channel modes take the hue/saturation from the base color and set HSL lightness per 2.7;
    replace the velocity-mode HSV sweep with the 3-stop ramp.
17. **P2 Playhead, cursor, ruler.** `paintEvent` (`:278-288`): playhead in `roll_colors().playhead`;
    `paint_ruler()` (`:556-592`): add the 8 px playhead triangle, 4 px beat ticks, loop bracket end
    caps; `ruler_height` 24 → 28 in `piano_roll.hpp:71` (check `ui_tests`), and 6% loop fill over
    the grid in `paintEvent`.
18. **P2 Empty-state overlay.** `piano_roll.cpp` `paintEvent`: if `controller_.notes().notes`
    is empty, draw the two hint lines (2.7) centered in `local`, `PlaceholderText` color, base
    font, text via `tr()`.
19. **P2 Hover row.** `mouseMoveEvent`: store `hover_pitch_`; paint 1 px tint row in grid and
    highlight the gutter key. Clear on `leaveEvent`.
20. **P3 Color mode UI.** View > Note Colors submenu (Track/Channel/Velocity/Scale degree),
    exclusive actions calling `PianoRollView::set_color_mode` (`:77`); persist in `QSettings`.

**Panels**

21. **P1 Status bar that keeps its readouts.** `main_window.cpp` constructor (`:149-159`): put
    cursor/selection/snap/scale labels in a container `QWidget` with `QHBoxLayout` and 1 px
    `QFrame::VLine` separators, add it with `addPermanentWidget` *before* recovery/device; add a
    `message_label_` (stretch) and `show_message(QString, int ms)` that sets it and a single-shot
    clear timer. Replace every `statusBar()->showMessage(` call (about 30) with `show_message(`.
    `cursor_label_` uses `mono_font()`, min width 170.
22. **P1 Library empty state.** `library_panel.cpp` constructor (`:207-227`): wrap existing
    widgets in page 1 of a `QStackedWidget`; page 0 = empty state (2.8). Switch to page 1 when a
    library opens (where `status_` is set after open, `:310-320`). Expose `bool is_open() const`.
    `main_window.cpp`: on `LibraryPanel::opened` (new signal) show the library dock at 260 px once.
23. **P1 Inspector selection section.** `inspector.cpp` `:154-170` and the refresh at `:404`:
    when nothing is selected call `selection_form->setRowVisible(row, false)` for the five numeric
    rows and show the hint label; show them again when a selection exists.
24. **P2 Collapsible inspector sections.** New small `CollapsibleSection` class in `inspector.cpp`
    (header `QToolButton` with `Qt::ToolButtonTextBesideIcon` + arrow, `setCheckable`, toggles the
    content widget). Replace the five `QGroupBox`es (`:154, :176, :225, :259, :282`). Defaults:
    Selection, Track, Scale & grid expanded; Chord, Transform collapsed; persist states in
    `QSettings "inspector/sections"`. Move "All parameters (JSON)" (`:297, :313`) behind an
    "Advanced" checkbox in Transform. Move "Refresh ports" next to Output as a 28 px icon button.
25. **P2 Musical units in the inspector.** Start/Length spin boxes: subclass `QSpinBox` as
    `TickSpinBox` overriding `textFromValue`/`valueFromText` with the controller's
    `meter_map().format()` (bar.beat.tick) and a parser; label "Start", "Length"; ticks in tooltip.
26. **P2 Track list toggles.** `editor_tab.cpp` `TrackListWidget`: `verticalHeader()->
    setDefaultSectionSize(28)`; replace M/S/R check items (`:117-125`) with a
    `QStyledItemDelegate` that paints a 22x20 rounded pill with the letter: off = `Button` with
    `Mid` border; M on = `#E0A526`, S on = `#3FB37F`, R on = danger color, black/white letter by
    luminance. Click toggles via `editorEvent`. Track color swatch 12x12 rounded in the name column.
    Buttons (`:52-56`) use `make_icon`.
27. **P2 Event list dock and lanes focus.** `controller_lane.cpp:59` → `Qt::StrongFocus`, draw a
    2 px focus inset; `controller_lane.hpp:70` initialize `left_margin_` from
    `PianoRollView::keyboard_width`; lane label in base −1 pt `PlaceholderText`.
28. **P2 Panel shortcuts and pane cycling.** `build_actions()`: Ctrl+1/2/3 toggle Library,
    Inspector, Event List (`toggleViewAction()->setShortcut`); F6/Shift+F6 cycle focus between
    track list, roll, inspector, library.
29. **P3 Tab bar polish.** `main_window.cpp:125-129`: `tabs_->setElideMode(Qt::ElideRight)`, close
    button via `QTabBar::setTabButton` with an icon from `make_icon(IconId::close)` shown on hover
    (or stylesheet `QTabBar::close-button` with a painted pixmap saved to a `QTemporaryDir`), and
    a leading `•` in the tab title when the document is dirty.
30. **P3 Getting Started dialog.** Help menu command `help.getting_started` with a 2-column table
    of the essential keys (2.10); offered once on first run as a status-bar link, never modal.
31. **P3 Docs screenshot.** Regenerate `docs/images/main-window.png` with
    `--screenshot` after the above, once per theme (`main-window-dark.png`, `-light.png`).

**Done when:** at 1400x860 first run the grid is ≥ 800 px wide, demo.mid shows all notes without
scrolling, a new user can draw a note using only what is visible on screen, selected notes are
distinguishable in a grayscale screenshot, and all text pairs meet 4.5:1 in both themes.

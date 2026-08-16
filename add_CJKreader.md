# TASK: Add an Optional CJK Vertical Reader Mode to CrossPoint v1.5.0

## Context & Hardware Constraints
- **Target Platform:** Xteink X4 E-Reader (ESP32-C3 microcontroller, 16MB Flash, ~380KB SRAM, no PSRAM).
- **Base Codebase:** CrossPoint v1.5.0 (C++ / PlatformIO), maintained as a personal fork that tracks upstream `develop`.
- **Problem Statement:** The stock Xteink firmware handles CJK reading noticeably better than CrossPoint today, and CrossPoint has no vertical (`vertical-rl`) reading mode at all.
- **Goal:** Add a second, opt-in reader mode for CJK vertical reading that the user can switch to (and back from) via a setting, while touching as little of the shared/mainstream codebase as possible so future `git merge`/rebase against upstream stays clean.

## Guiding Principle: Isolate the Diff, Not the Architecture
The original draft of this plan isolated by inventing a parallel architecture
(`IReaderEngine` interface, `ReaderFactory`, refactoring the existing reader
into `StandardEpubReader`, a hand-rolled SD font format, direct framebuffer
writes, a bespoke JSON config loader). That approach actually works against
the real goal: it touches and restructures the exact files upstream changes
most often, so every future rebase would conflict there, and it re-implements
things the codebase already does well (and does safely w.r.t. the SD-card
mutex).

The revised approach instead:
- Adds new code only in new files, under a dedicated folder.
- Touches existing/shared files at the smallest number of stable call sites
  possible: one dispatch branch in `ReaderActivity`, plus the menu-shell
  additions every settings screen already requires — never the content of
  an existing screen (e.g. Text Settings stays untouched).
- Reuses existing infrastructure (`lib/Epub` parsing, `SdCardFont`,
  `GfxRenderer`, `PersistableStore`, the HAL) instead of duplicating it.

---

## Architectural Requirements

### 1. New Activity, Not a New Engine Interface
- No `IReaderEngine` / `ReaderFactory` refactor. `ReaderActivity` already
  dispatches to format-specific activities (`EpubReaderActivity`,
  `XtcReaderActivity`, `TxtReaderActivity`); add one more:
  `CjkVerticalReaderActivity`.
- The **only** edit to `src/activities/reader/ReaderActivity.h/.cpp` is a
  branch in the existing EPUB dispatch (`onGoToEpubReader`): if
  `CjkReaderSettings::getInstance().enabled` is true, construct
  `CjkVerticalReaderActivity` instead of `EpubReaderActivity`. Everything
  else about `ReaderActivity` (XTC/TXT/BMP handling, library nav, resume
  logic) is untouched.
- Only one reader activity is ever instantiated at a time (same lifetime
  discipline the existing activities already follow), so there's no added
  steady-state RAM cost from having two reader implementations in the tree.

### 2. Isolated Code & Directory Structure
All new files live under `src/activities/reader/cjk/`, to keep the diff
self-contained and easy to carry across rebases:
- `CjkVerticalReaderActivity.h / .cpp` — the activity: page/column
  navigation, input handling, render loop.
- `CjkVerticalLayout.h / .cpp` — pagination: walks extracted chapter text and
  lays it into vertical columns using font metrics (no framebuffer or SD
  access of its own).
- `CjkKinsoku.h / .cpp` — kinsoku shori rule tables and the line/column-break
  decision logic.
- `CjkReaderSettings.h / .cpp` — persisted config for this mode (see §3).
- `CjkReaderSettingsActivity.h / .cpp` — the settings submenu UI (see §5).

### 3. Configuration & Storage — Own Store, Shared State Stays Shared
- **Do not add fields to `src/CrossPointSettings.h`.** That struct is
  read far outside the reader (`main.cpp`, `BaseTheme`/`UITheme`,
  `SleepActivity`, the other format readers) and is the highest-churn file
  in the repo for settings-related upstream changes — editing it is exactly
  the kind of touch that causes rebase pain.
- Instead, define `CjkReaderSettings : public PersistableStore<CjkReaderSettings>`,
  following the same pattern already used by `RecentBooksStore`,
  `OpdsServerStore`, `WifiCredentialStore`, and `KOReaderCredentialStore` —
  each of those is its own small class with its own JSON file, using the
  same CRTP base as `CrossPointSettings` but fully independent of it. This
  gets mutex-safe, HAL-backed SD reads/writes for free, without a bespoke
  parser and without touching the shared struct.
- `CjkReaderSettings::getFilePath()` → `/.crosspoint/cjk_reader_settings.json`.
- Fields owned here (new, CJK-only, nothing else in the firmware reads
  them): `enabled` (engine on/off), `verticalRL` (vs. horizontal-LR),
  `cjkFontFamilyName` + point size, `columnSpacing` (small/medium/large),
  `kinsokuEnabled`.
  - `cjkFontFamilyName` is deliberately separate from
    `SETTINGS.sdFontFamilyName`: a user may want a Latin serif for normal
    EPUB reading and a dedicated CJK family only in vertical mode, so this
    is mode-specific choice, not shared system state. The picker UI reuses
    `SdCardFontRegistry::discover()/getFamilies()` (the same enumeration the
    existing Settings → Reader → Font Family screen uses) — no new SD-scan
    code needed, just a second place that stores the selection.
- Fields **read from `SETTINGS` (`CrossPointSettings`), never duplicated**:
  `orientation`, `uiTheme`, status bar composition, `screenMargin`. These are
  genuinely system-wide — the CJK reader should stay visually consistent
  with the rest of the device, the same way `TxtReaderActivity` and
  `XtcReaderActivity` already read them without owning them.

### 4. Core CJK Reader Implementation — Reuse the Existing Text/Font Pipeline
- **Text extraction:** keep using `lib/Epub` (container/OPF/TOC/chapter HTML
  parsing) to get plain chapter text. Do not re-implement EPUB parsing.
- **Font loading & glyph lookup:** use the existing `lib/EpdFont/SdCardFont`
  + `GfxRenderer::registerSdCardFont`. `SdCardFont` already does
  interval-based codepoint→glyph lookup from SD with the anti-fragmentation
  caching (`miniData`/kept-if-fits buffers) the project spent real effort
  building — no new binary font format, no raw `Unicode_ID * Bytes_Per_Glyph`
  offsetting.
  - **Confirmed, not just assumed:** `lib/EpdFont/scripts/fontconvert_sdcard.py`
    already ships `cjk` (CJK Unified Ideographs + Hiragana + Katakana +
    Fullwidth) and `hangul` Unicode interval presets, and `docs/sd-card-fonts.md`
    documents an existing CJK-fallback feature built on this exact path — so
    CJK glyph coverage through `.cpfont` is already proven working code, not
    a gap to fill.
  - **Skip kerning/ligatures for this reader.** CJK bitmap glyphs are
    fixed-pitch; the vertical step is a constant `glyph_height + spacing`.
    Don't call `SdCardFont`'s kerning-matrix path or trigger
    `loadStyleKernLigatureData` — it's pure overhead for text that never
    needs it.
  - **Prewarm per chapter, not per page.** `SdCardFont::prewarm()` /
    `buildAdvanceTable()` accept arbitrary-length UTF-8 text; feed a whole
    chapter at once so repeated Han characters hit the persistent advance
    cache instead of re-triggering the expensive per-glyph SD-miss path
    (`SdCardFont::onGlyphMiss`, `lib/GfxRenderer/GfxRenderer.cpp:33`).
- **Vertical layout (`CjkVerticalLayout`):** columns advance right-to-left
  (X -= column_width), glyphs advance top-to-bottom within a column
  (Y += glyph_advance), using `GfxRenderer::getTextAdvanceX` for metrics —
  same calls the horizontal layout path uses, just walked in a different
  order (kerning skipped, see above).
- **Vertical punctuation:** nudge, don't rotate. Small marks (`、`, `。`,
  `，`, `．`, brackets, colons) just need an `(dx, dy)` pixel offset toward
  the glyph's top-right corner to read correctly in tategaki — no need for
  `GfxRenderer::drawTextRotated90CW`'s full rotated draw path. This is
  cheaper and is exactly the technique the `aBER0724/crosspoint-reader-cjk`
  fork's `LayoutEngine::getVerticalPunctuationOffset()` uses.
- **Kinsoku shori (`CjkKinsoku`):** forbid line-start characters
  (`。`, `、`, `」`, `』`, `）`, `！`, `？`) at the top of a column and
  line-end characters (`「`, `『`, `（`) at the bottom. Port this logic
  directly from `aBER0724/crosspoint-reader-cjk`'s `src/reader/LayoutEngine.cpp`
  (`isProhibitedLineStart`/`isProhibitedLineEnd`/`chooseLineBreak`) — it's a
  small, pure codepoint-table module with zero SD/framebuffer/font
  dependency, so it's directly portable regardless of which font/rendering
  substrate it sits on. See the reference-fork section below for why only
  this piece is being adopted from that codebase.
- **Rendering:** draw exclusively through `GfxRenderer` (`drawText`,
  `drawTextRotated90CW`, `getGlyphBitmap` + existing blit helpers), which
  already routes through `HalDisplay`. No direct framebuffer writes, no
  bypassing the HAL — direct SPI/SD access from outside `lib/hal` races the
  mutex-serialized access path and can panic FreeRTOS on this hardware.

### 5. UI Integration — Its Own Menu Entry, Not Nested in Text Settings
- `CjkReaderSettingsActivity` gets its **own row** in the Settings → Reader
  tab, as a sibling of "Text Settings" / "Manage Fonts" / "Customise Status
  Bar" — not an option added inside the existing Text Settings screen. That
  screen (`TextSettingsActivity.cpp`, the one that actually holds font
  size/spacing/alignment/margins today) is not touched at all.
- Concretely, that means the same three-part pattern every existing sibling
  screen already uses, entirely confined to `SettingsActivity.h`/`.cpp`
  (the top-level menu shell) — verified against how `StatusBarSettingsActivity`
  is wired in:
  1. One new `SettingAction` enum value (`SettingsActivity.h`).
  2. One `readerSettings.push_back(SettingInfo::Action(StrId::STR_CJK_READER,
     SettingAction::CjkReaderSettings))` in `rebuildSettingsLists()`
     (`SettingsActivity.cpp`, next to the existing
     `STR_CUSTOMISE_STATUS_BAR` line).
  3. One `case SettingAction::CjkReaderSettings:` in the action-dispatch
     switch that enters `CjkReaderSettingsActivity`.
  All three are additive lines in the two files every settings screen in
  this app already registers through — there's no third option that avoids
  touching *some* shared file to make a new screen reachable, but this
  keeps the touch to the generic menu shell instead of any screen's actual
  content, so it carries the same low rebase risk as when upstream itself
  adds a new settings screen.
- **One screen, not two.** Enabling the engine and configuring it both live
  in `CjkReaderSettingsActivity`, reached through the single row above — no
  separate "on/off" entry elsewhere (e.g. under System). Splitting enable
  and configure across two menu rows would mean a second `SettingAction`
  enum value, a second row registration, a second dispatch case, and a
  second translated string for no functional benefit: falling back to the
  standard reader is exactly as fast (one tap into this same screen,
  toggle off) either way.
- `CjkReaderSettingsActivity` itself is new code in `cjk/`, and everything
  in it reads/writes `CjkReaderSettings` only — never `SETTINGS`. Rows, in
  order:
  1. **Enable CJK Reader** — `[Off / On]`, default **Off**. Master switch;
     when off, `ReaderActivity` always uses the standard `EpubReaderActivity`
     regardless of every other value below. This is the one field that
     `ReaderActivity`'s dispatch branch actually reads.
  2. **Font Family** — picker listing `SdCardFontRegistry::getFamilies()`
     (the same SD-scanned family list the existing Settings → Reader →
     Font Family picker uses; no new discovery code). Stores the chosen
     family name into `CjkReaderSettings::cjkFontFamilyName`. Empty/default
     shows "Not set" and the engine refuses to enable until a family is
     picked, since there's no built-in CJK font to fall back to.
  3. **Font Size** — the point sizes the *selected* family actually ships
     on SD (mirrors how the existing reader Font Size row only offers sizes
     the active family has `.cpfont` files for, via
     `SdCardFontFamilyInfo::availableSizes()`). Not a fixed 16/24px pair —
     whatever the installed font provides.
  4. **Reading Mode** — `[Vertical-RL / Horizontal-LR]`, default
     **Vertical-RL** (the actual point of this engine; horizontal is there
     mainly for comparing against the standard reader without switching
     screens).
  5. **Column Spacing** — `[Small / Medium / Large]`, default **Medium**.
     Fixed pixel gaps between columns, defined as constants in
     `CjkVerticalLayout`, not computed from anything in `SETTINGS`.
  6. **Kinsoku Shori** — `[On / Off]`, default **On**. Gates whether
     `CjkKinsoku::chooseLineBreak` applies the line-start/line-end rules
     at all; off just breaks at the column edge.
  - All six bound directly to `CjkReaderSettings::load()/save()` — one
    `saveToFile()` call when leaving the screen, same pattern
    `TextSettingsActivity` already uses for its own settings.

---

## Reference Fork: What to Reuse vs. Avoid from `aBER0724/crosspoint-reader-cjk`

A real, shipped CJK fork exists at `aBER0724/crosspoint-reader-cjk`
(cloned locally for review). It's useful as a second data point on what a
CJK reader for this hardware actually needs — and as a concrete illustration
of why this plan avoids the "fully isolated engine" approach it took.

**What it built:** a `BinFontEngine` (`src/reader/BinFontEngine.cpp`) using
a flat `.bin` format with `offset = codepoint * bytesPerGlyph` lookup — the
same scheme this plan's original draft proposed — plus a second, parallel
EPUB pipeline (`src/reader/StreamEpubParser.cpp`) separate from `lib/Epub`,
and direct edits to the shared `CrossPointSettings.h` (a `readerLayoutMode`
field plus a whole new "CJK-specific settings" block inserted inline).

**Why this plan doesn't copy that part:**
- `BinFontEngine::openFont()` opens its file via `Storage.openFileForRead()`
  (HAL-compliant), but then keeps the raw `FsFile` handle and calls
  `.seek()`/`.read()` on it directly on every glyph draw thereafter
  (`BinFontEngine.cpp:161-166`) — bypassing the HAL mutex for the actual
  per-glyph reads. On this codebase's own hardware/concurrency model that's
  the exact pattern that races the SD SPI state machine against any other
  task touching storage (web server, background scans).
- `BinFontEngine::drawChar()` writes straight into a raw `uint8_t*
  frameBuffer` (`BinFontEngine.cpp:145-221`), bypassing `GfxRenderer`/
  `HalDisplay` entirely.
- `StreamEpubParser` duplicates EPUB parsing and pagination that `lib/Epub`
  already does, doubling the amount of book-parsing code to maintain.
- The direct `CrossPointSettings.h` edits are exactly the shared-file churn
  this plan is trying to stay out of.

This confirms the isolated-engine approach is buildable and does ship — it's
not a strawman — but it pays for its isolation with real HAL-safety risk and
a second reading pipeline. Reusing `.cpfont`/`SdCardFont`/`lib/Epub`/HAL (as
this plan does) gets the same "own code in `cjk/`, minimal shared-file
touch" outcome without either cost.

**What it built that's genuinely worth taking:** `src/reader/LayoutEngine.cpp`.
It has no SD, framebuffer, or font dependency at all — just codepoint tables
and a kinsoku break-point search over a small `uint32_t` array. That's
exactly the shape of code this plan's `CjkKinsoku` module should be, and
it's already proven in a shipped build, so port it rather than
re-deriving it from scratch.

---

## Deliverables
1. New files only, under `src/activities/reader/cjk/`:
   `CjkVerticalReaderActivity`, `CjkVerticalLayout`, `CjkKinsoku` (kinsoku
   rules ported from `aBER0724/crosspoint-reader-cjk`'s `LayoutEngine.cpp`),
   `CjkReaderSettings`, `CjkReaderSettingsActivity`.
2. Edits to existing/shared files, all additive, none touching a screen's
   actual content:
   - `ReaderActivity`'s EPUB dispatch: branch to `CjkVerticalReaderActivity`
     when `CjkReaderSettings::getInstance().enabled`.
   - `SettingsActivity.h`/`.cpp`: one `SettingAction` enum value, one new
     row registration, one dispatch case — the same pattern
     `StatusBarSettingsActivity` etc. already use. `TextSettingsActivity.cpp`
     (the actual Reader Settings content) is not touched.
3. `src/CrossPointSettings.h` unmodified.
4. No new heap allocation patterns beyond what `SdCardFont`/`GfxRenderer`
   already do; no direct SD (`SdFat`/`FsFile`) or framebuffer access outside
   `lib/hal` — this is a deliberate departure from the reference fork's
   `BinFontEngine`, which does both (see above).
5. Fallback is just flipping `enabled` back off — `EpubReaderActivity` and
   the rest of the reading pipeline are never modified, so the stock
   behavior is always intact underneath.

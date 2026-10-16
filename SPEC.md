# Muon Bench E-Reader: shared spec

Standalone side project (not part of Muon Sortes). ESP32-S3 +
Good Display GDEH0576T81 e-paper + BME280 + 5-way d-pad (4 directions + center
click).
Power model: external battery plugged in briefly to turn pages, then unplugged;
e-ink retains the image. Therefore boot-to-page must be fast and reading position
must be persisted on every page turn.

## Hardware

- Display: Good Display GDEH0576T81, 5.76 inch, 920x680 B/W, SSD2677
  controller, on a DESPI-C02 adapter. GxEPD2 class `GxEPD2_576_GDEH0576T81`
  (auto-included by `GxEPD2_BW.h`). Set the adapter's RESE switch to 0.47.
  **The UI is portrait.** `ui::begin()` calls `display.setRotation(DISPLAY_ROTATION)`
  with `DISPLAY_ROTATION = 1` from `include/config.h`, so the logical canvas is
  680 wide by 920 tall and every layout constant is written against that.
  Rotation 1 puts the panel FPC tail on the left edge of the portrait page;
  rotation 3 puts it on the right and flips the image 180 degrees. If the first
  frame comes up upside down on the bench, change that one constant to 3 and
  reflash. Nothing else in the firmware or the host tools cares which of the
  two it is.
  The adapter has no panel power enable line, so there is nothing to drive in
  setup(): VCC to 3V3 and the panel is live.
  The page buffer is the full panel height (920*680/8 = 78200 bytes), so a
  frame is composed in one firstPage/nextPage pass. Reset pulse is 50 ms
  (`display.init(115200, true, 50, false)`); the 2 ms pulse the smaller
  Waveshare panel accepted does not bring the SSD2677 up reliably. This is the
  same panel and the same init the Muon firmware drives in
  `src/drivers/display.cpp`.
- Sensor: BME280 on I2C, address 0x76 (0x77 fallback).
- Input: 5 momentary buttons to GND, internal pull-ups, active LOW. Debounce in
  firmware (~20ms), edge-triggered on press.

## Pin map (ESP32-S3, lolin_s3_mini board definition)

| Signal | GPIO | Notes |
|---|---|---|
| EINK_MOSI | 17 | adapter SDI |
| EINK_SCK  | 18 | adapter SCK |
| EINK_CS   | 8  | adapter CS |
| EINK_DC   | 9  | adapter D/C |
| EINK_RST  | 10 | adapter RES |
| EINK_BUSY | 46 | adapter BUSY (strapping pin, idles LOW at reset) |
| EINK_MISO | none | panel is write-only; SPI.begin() gets -1 |
| EINK_VCC  | 3V3 | adapter VCC |
| EINK_GND  | GND | adapter GND |
| BME_SDA   | 12 | optional sensor, Wire.begin(12, 13) |
| BME_SCL   | 13 | |
| BTN_UP    | 40 | |
| BTN_DOWN  | 41 | |
| BTN_CENTER| 42 | |
| BTN_LEFT  | 1  | |
| BTN_RIGHT | 2  | |

Deliberately avoided: 0/3/45 (strapping; 46 carries BUSY, which idles LOW so
download mode still works), 19/20 (native USB), 33 through 37 (octal PSRAM on
N16R8 style boards), 43/44 (UART0), 15 (dead on the founder's board). All pins live in
`include/pins.h`, the single source of truth.

## Build environment

PlatformIO, Arduino on espressif32:

```ini
[env:esp32s3]
platform = espressif32
board = lolin_s3_mini
framework = arduino
board_build.filesystem = littlefs
board_build.partitions = partitions.csv
build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1
```

`partitions.csv` is an 8 MB map with a single app slot (no OTA) and a 6.09 MB
filesystem partition for books.

Libraries: GxEPD2, Adafruit GFX, U8g2_for_Adafruit_GFX (fonts), Adafruit BME280 + Adafruit
Unified Sensor, ArduinoJson 7 (the daily numbers file). LittleFS (bundled with
the core) holds books, uploaded with `pio run -e esp32s3 -t uploadfs` from
`data/`.

## Text grids (pagination contract, shared by firmware and converter)

Monospace grids so host-side pagination and on-device rendering agree exactly.
A grid is identified by a `font_id`, stored per variant in an MPG2 book. The
numbers live in `include/config.h` and `tools/pdf2book.py` `GRIDS` and must
match exactly. Advance widths are the `max_char_width` byte of the u8g2 font
headers (`U8g2_for_Adafruit_GFX/src/u8g2_fonts.c`).

Text grids are computed per panel from the portrait canvas size and the font
advance and pitch, not tuned by hand. A grid is the triple (font id, cols,
rows); a book carries its grid, and old books with no grid info are assumed to
be the panel 0 grids below. When a book's grid does not match the active panel,
the device re-paginates it from its own text on first open ("Formatting for
this screen"). `tools/pdf2book.py` and `tools/txt2book.py` take `--panel {576,750}`
(default 576; output unchanged apart from a 9 byte GRID extension). Panels: 0 = GDEH0576T81 (5.76 inch,
680x920 portrait canvas), 1 = Waveshare 7.5 inch V2 (480x800 portrait canvas),
chosen at runtime (NVS `ui`/`panel`, missing = 0). The contract below is the
panel 0 grid and is unchanged.

Both panel 0 grids are portrait 680x920 with a 16px left/right margin, so 648px of
text width, and the status rule sits at y=896 (24px above the bottom edge) with
the status baseline at y=912.

| font_id | Font | Advance | Grid | First baseline | Pitch | Last baseline |
|---|---|---|---|---|---|---|
| 0 | `u8g2_font_profont22_mf` | 12x22 | 54 x 37 | 26 | 24 | 890 (ink ends 894) |
| 1 | `u8g2_font_profont29_mf` | 16x29 | 40 x 27 | 32 | 32 | 864 (ink ends 869) |

Column arithmetic: 648/12 = 54 exactly for font 0, and 648/16 = 40.5 floored to
40 for font 1 (8px of unused width at the right edge). Row arithmetic: font 0
baseline n is 26 + (n-1)*24, so row 37 is 890 with a 4px descender ending at
894 (a 38th would be 914, past the rule); font 1 baseline n is 32*n, so row 27
is 864 with a 5px descender ending at 869 (a 28th would land on the rule at
896). Both clear the rule at 896.

Font 1's first baseline of 32 gives the same ~8px of ink margin at the top as
font 0's 26. The bottom strip (below the last row) is a status line in a small
font: book title, page N/M.

The host side keeps these numbers in one place: `tools/pdf2book.py` derives
`GRIDS`, the margins, the status strip and the figure targets from `PANEL_W` /
`PANEL_H`. `tools/txt2book.py` and `tools/mpg1_dump.py` import them, and
`tools/extract_figures.swift` mirrors them at the top of its own file.

The converter guarantees every line fits its grid's column count and every page
fits its row count; firmware renders lines verbatim, no wrapping on device.

## Book file format: MPG1 (.pgs), little-endian

Path on device: `/books/<slug>.pgs`

- magic: 4 bytes `MPG1`
- u32 page_count
- u16 title_len, then title UTF-8 (display title)
- u32 offsets[page_count]: absolute file offset of each page blob
- page blobs: raw UTF-8, lines joined with `\n` (no trailing `\n` required).
  Page p spans offset[p] to offset[p+1] (or EOF for the last page).

ASCII plus common Latin-1 punctuation only (converter transliterates curly
quotes, dashes, ellipsis; profont22 has no exotic glyphs).

### Image pages

A page blob whose first four bytes are `0x01 'I' 'M' 'G'` is a bitmap page, not
text. Text pages never start with `0x01`, so books without figures are
unchanged and older files keep working.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x01 0x49 0x4D 0x47` |
| 4 | 2 | u16 LE width (stored bitmap) |
| 6 | 2 | u16 LE height (stored bitmap) |
| 8 | 1 | flags, bit0 = pixel double on render (each stored pixel drawn 2x2) |
| 9 | 1 | reserved, write 0 |
| 10 | ceil(width/8)*height | raster |

Raster rows run top to bottom, MSB first inside each byte, bit 1 = black ink.
That is exactly binary PBM (P4) packing, so the converter copies the raster
from the extractor's PBM files verbatim.

Image pages are ordinary pages: they count toward N, jump lands on them, and
the saved position can point at one. The firmware centers the bitmap in the
680x896 content area above the status strip and always uses a FULL refresh for
an image page and for the page that follows one, because dithered art ghosts
badly under partial refresh.

The extractor targets that content area: 680x896 with `--full-res`, or a
reduced 272x358 box plus `MPG1_IMAGE_FLAG_DOUBLE` by default (34 bytes per row
times 358 = 12172 raster bytes at the box limit, drawn 2x2 on device, so a
plate covers up to 544x716 of the page and is centred with a margin).
`MPG1_MAX_IMAGE_BYTES` in `include/config.h` is 77000, the ceiling for a full
res 680x896 raster (85 bytes per row times 896 = 76160).

The reduced box is 2/5 of the content area rather than 1/2 (`IMAGE_HALF_*` in
`tools/pdf2book.py`). 1/2 makes a doubled plate fill the page exactly, which is
what the landscape build did, but portrait plates at 1/2 cost 892 KB for the
52 figures of the Marathon Monks book and pushed `data/books` past what the
4 MB era filesystem could spare alongside a ~600 KB news sync transient. The
filesystem is 6.09 MB since 2026-09-01, so this is headroom, not a ceiling.
2/5 costs 64% of that. Raise it back to `// 2` for a book with few figures.

A crop whose orientation does not match the page box is rotated 90 degrees CCW
so it fills the page (the reader turns the device a quarter turn to view it).
With the portrait page that means LANDSCAPE crops rotate and portrait crops fit
directly, the opposite of the old landscape build. The extractor derives this
from its target box, so it needs no edit if the panel orientation changes
again.

Figures are produced by a separate extractor into `<outdir>/figs/*.pbm` plus
`<outdir>/manifest.json` and `<outdir>/review/*.png`. The reviewer's veto is
the review PNG: delete `review/<id>.png` and `txt2book.py --figures` leaves that
figure out. Each figure is inserted right after the text of the source PDF page
it came from.

## Structure recovery for a scanned book

Vision (`tools/ocr_pdf.swift`) returns lines with no geometry, so a page of OCR
arrives as an undifferentiated run and `pages_to_paragraphs` joins the lot into
one paragraph. `tools/ocr_paragraphs.py` puts the structure back: it reads
`pdftotext -bbox-layout` for the same PDF, classifies each line from its left
edge in points, and transfers the resulting breaks onto the Vision text by fuzzy
matching the first four words of a line (candidates restricted to Vision line
starts, so a break can never land mid line).

Line classes, all measured against the page's modal left margin (the modal is
taken over full measure lines only, so indented first lines cannot pull it):

- **para**: inset 6 to 34 pt. An ordinary first line indent.
- **verse**: inset 36 pt or more and shorter than 0.62 of the text measure.
  Emitted as a paragraph of its own so the line break survives pagination.
- **block**: three or more consecutive inset lines whose right edges agree
  within 6 pt. A set-off quotation is inset about as far as a first line indent,
  so depth alone cannot tell them apart; the run and the shared measure can. The
  block's own left edge then becomes the baseline for paragraph starts inside
  it, and the body line after the block always opens a paragraph. The right
  edge test is what stops the indented body paragraph that follows a block from
  being read as its last line.
- **speaker**: a line opening `Name:` starts a paragraph. Interview turns are
  set with a hanging indent, so geometry cannot see them. This test runs on the
  Vision text, not the oracle, because the scanner's own OCR renders `Jen:` as
  `fen:`, `]en:` and `~en:`.

Running heads that carry the page number are unique per page, so
`find_running_lines` (which wants a line repeated on half the pages) cannot see
them. Name them with `txt2book.py --strip-regex` instead, one pattern per head
form, and allow for the OCR reading the bullet as `D` or `O` and `50` as `SO`.

## Book file format: MPG2 (.pgs), little-endian

MPG2 holds the same book paginated for more than one text size, so the reader
can switch size on the device. Firmware sniffs the 4-byte magic and supports
both formats; MPG1 books behave as a single variant.

- magic: 4 bytes `MPG2`
- u8 variant_count
- u16 title_len, then title UTF-8
- per variant, repeated variant_count times:
  - u8 font_id (see the text grid table above)
  - u32 page_count
  - page table: page_count entries of `{ u32 offset, u32 length, u32 anchor }`,
    12 bytes each. `offset` is an absolute file offset, `length` is the blob
    length in bytes.
- then the blob region.

Page blobs are encoded exactly as in MPG1 (raw UTF-8 lines joined with `\n`, or
an image page starting `0x01 'I' 'M' 'G'`). Each figure's image blob is written
once and every variant's table points at the same offset and length. That is
why the table carries an explicit length instead of deriving the span from the
next offset.

`anchor` is the cumulative character count into the book's logical text stream
at the start of that page. The stream is the cleaned paragraphs concatenated
with one separator character each, so it is identical across variants and equal
anchors mean the same place in the book. An image page takes the anchor of the
text position it was inserted at. Anchors are non-decreasing within a variant.

Switching size maps the position as: `A` = anchor of the current page in the
old variant, new page = the largest index `p` in the new variant with
`anchor[p] <= A`.

Build a dual size book with `tools/txt2book.py --dual` (variant 0 on the 54x37
profont22 grid, variant 1 on the 40x27 profont29 grid). Inspect either format,
including anchor mapping, with `tools/mpg1_dump.py` (`--variant N`, `--map P`).

## Reading state

`/books/<slug>.pos`: u32 LE page index, then u32 LE variant index. A legacy
4-byte file reads as variant 0. Written on every page turn and on every text
size switch (page turns are seconds apart, wear is negligible). `/current.txt`: slug of the open book.
On boot: read current book + position, render that page immediately.

## UX

Reading mode:
- LEFT = next page, RIGHT = previous page (the founder holds the device so LEFT
  is the natural forward tilt).
- DOWN = next page, UP = previous page. The portrait page runs top to bottom,
  so the vertical axis reads as "further on" / "back"; both axes turn pages
  while reading. The menu keeps UP/DOWN for cursor movement, so this second
  mapping lives only in the reading handler.
- Full GxEPD2 refresh per turn is acceptable; fast partial is used where the
  panel supports it, with a full refresh every 10 partials to clear ghosting.
- CENTER = open menu.

Menu (partial refresh for cursor moves): a two column grid of tiles, 6 rows
per screen, laid out row major (tiles 1 and 2 are the first row). Layout
constants are at the top of `src/ui.cpp` (`MENU_*`): a 44 px solid black
header band, tiles of 318x126 px with a 12 px gutter from y=64 to y=880, then
the ordinary status strip. Each tile shows its 1-based index as a large
numeral (fub42) top right, a short accent hairline, a bold label (helvB24) and
a small state line (7x14) bottom left; a label wider than the tile wraps
onto a second line (the state line moves down to make room) and anything
still too wide is cut with `..`. The tile at the cursor is filled black with white content. With more
tiles than fit, rows scroll so the cursor's row stays on screen and the strip
reads `rows a-b of n`.
- Header band shows `MENU` and the live BME readings: temperature C, humidity
  %, pressure hPa, then the watchdog reboot count if nonzero, then
  `synced YYYY-MM-DD HH:MM` if the news brief has ever been synced.
- Tiles, in order: Resume (state = open book title), Speed read (`<n>
  words, <wpm> wpm`, opens the speed read book picker below), one tile per .pgs found
  (title, `p. N/M` saved position), Daily numbers (`synced <stamp>` or `never
  synced`, opens the numbers view below), Text size (Normal/Large, only for a
  book with more than one variant), Sync feeds (last stamp), WiFi setup
  (stored network count), Check WiFi, Books and games (WiFi) (`N books, M
  roms`, starts the books and games server below), Auto sync (on/off), Jump
  to page (current page), Flip screen (`as built` / `turned 180`), Button
  test.
- Daily numbers exists only when `METRICS_TOKEN` is set at compile time; Sync
  feeds and Auto sync only when `NEWS_TOKEN` or `METRICS_TOKEN` is set
  (`FEEDS_CONFIGURED` in `include/config.h`). A public build with the
  `CHANGE_ME` placeholders has neither, never auto syncs at boot, and skips
  any feed whose own token is unset.
- Flip screen toggles the panel rotation between 1 and 3 (both portrait, 180
  degrees apart), stores it in NVS (`ui`/`rot`) and redraws with a full
  refresh. While the rotation differs from `DISPLAY_ROTATION` the pad is
  turned with the picture: `src/input.cpp` reports UP as DOWN and LEFT as
  RIGHT (and back) in every event and mask, so every mapping below still
  means the same direction to the reader. No stored value means
  `DISPLAY_ROTATION`.
- Check WiFi (`src/wifi_check.*`) scans once, then tries to join every stored
  network the scan saw, 15 s each, and reports one line per stored network:
  `ssid: ok -52 dBm`, `ssid: wrong password / no join`, `ssid: not in range`.
  It does not stop at the first success: the point is to find the stale
  credentials, not to get online. The radio is off when it returns and the
  result screen is held until a button press.
- Text size toggles between Normal and Large, remaps the reading position
  through the page anchors, saves it, leaves the menu, and renders the page.
- Navigation with the three working buttons: DOWN = next row in the same
  column (wraps to the top), LEFT (the founder's "right") = next column in the
  row (the last column wraps to the first column of the same row), CENTER =
  pick. UP and RIGHT are the reverse moves when present, never required. There
  is no back button: the menu is left via the Resume tile.
- Jump to page, by silkscreen on the back of the unit: up +1 (BTN_LEFT),
  right +10 (BTN_UP), left -10 (BTN_DOWN), down -1 (BTN_RIGHT, dead on this
  unit), CENTER go. Wraps at both ends (below page 1 lands on the last page,
  past the end on page 1). Holding a pad past JUMP_HOLD_MS repeats its step
  every JUMP_REPEAT_MS.

### Speed read

RSVP over a book, in `src/rsvp.*`, ported from the rsvp_eink proof of concept
(tuning constants `RSVP_*` in `include/config.h`).

- The Speed read tile opens a book picker: the menu grid with one tile per
  book (title, `p. N/M` saved position), cursor on the open book, same
  navigation. Picking a book opens it as its menu tile would (saved page,
  saved variant) and starts at the first word of that page. Picking the book
  already open starts at the page being read, and at the word it was left on
  if speed read was last exited on that same page (RAM only). Two settings
  tiles follow the books: Words (`<n> per flash`, CENTER cycles 1, 2, 3) and
  Speed (`<wpm> wpm`, CENTER adds 25 and wraps to 100 past the top speed for
  that word count: 300, 500, 600). Both are stored in NVS (`ui`/`rsvpwords`,
  `ui`/`rsvpwpm`, defaults 2 and 250).
- The panel runs landscape (rotation 1 -> 0, 3 -> 2), SPI at 40 MHz and the
  partial LUT forced to 40 C (`setForcedTemp` in the vendored driver). All
  three are put back on exit, which returns to the reading view on the current
  page with a full refresh.
- Waveform cut (`setCutMs`): a partial refresh is stopped RSVP_CUT_MS (60 ms)
  after it starts by resetting the controller (200 us pulse), which is then
  re-initialised with the partial LUT (~90 ms, 83 of it the 0x04 power on).
  Full refreshes are never cut. The cut is chosen per flash: none when the
  flash's hold (interval x dwell) is at least RSVP_UNCUT_MIN_MS (450 ms, the
  full 54 ms SPI + 367 ms drive plus margin). The cut must never be combined
  with the controller partial window (0x90/0x91): after the reset the RAM is
  undefined, the window is not honoured, and the whole panel flashes noise.
- Words per flash as set (fewer at a sentence or paragraph end), left aligned
  at a fixed edge between two static hairline rules, in 40 pt Gelasio,
  stepping down (34/28 pt) only when a chunk would run off the right edge. Status line at the bottom: `<wpm> wpm   p. N/M
  <title>`, plus `PAUSED` or `END` and a one line hint while paused. Latin-1
  is transliterated to ASCII for the 7 bit fonts. Image pages are skipped.
- Interval per flash = words * 60000 / wpm (480 ms at 2 words, 250 wpm),
  held 1.5x at a sentence end and 1.2x at a clause break, scheduled from the
  previous due time, never bursting to catch up. Entry is a full refresh;
  after that, a ghost clearing full refresh to a blank band lands at the first
  sentence end once RSVP_FULL_AFTER (30) partials have run, and in any case at
  RSVP_FULL_MAX (40), after that chunk's full interval.
- Paragraph breaks: a blank line inside a page. A page end counts as one when
  the page ends in a blank line, the next text page is not the next page (an
  image between), or the next page's first word would have fitted after the
  page's last line (the wrap is greedy, so the paragraph must have ended).
- Position: whenever the chunk on glass starts on a new page, that page is
  saved with the active variant, exactly like a page turn.
- Playing: LEFT (or RIGHT) +25 wpm, DOWN (or UP) -25 wpm, 100 to the top
  speed for the word count, stored in NVS; CENTER pauses. Paused: LEFT plays,
  DOWN steps back 10 chunks and shows that chunk, CENTER exits to
  the reading view. At the end of the book the mode pauses on `End`.
- Presses made while a flash holds the loop are taken from the pin latch
  (`input::takeLatched`), so the pad works during playback.

### Numbers view

`src/numbers_view.*`, `AppState::NUMBERS`. The metrics hub's daily figures
drawn as a grid of cards in the same family as the menu, one screen per
project, in one of four time windows at a time (yesterday, 7 days, 30 days,
total). The data is `/numbers.json` (`NUMBERS_FILE`), the body of
`GET /metrics/ereader.json` stored verbatim by the feed sync; every string in
it arrives already formatted (ASCII, labels at most 14 characters, values at
most 9 with `12.3M` / `45k` compact forms, deltas signed, at most 10 metrics
per project), so the device parses (ArduinoJson) and draws, nothing else.
Each metric carries its value, foot line and delta once per window as
parallel arrays (`v`, `a`, `d`), each project one headline per window, and
the top level a `windows` list of `{name, range}`; the view holds at most
`MAX_WINDOWS` (4). A file with plain strings instead of arrays (the feed
before 2026-09-12) still loads, as a single "Yesterday" window.

Screen layout, 680x920, constants at the top of `numbers_view.cpp` with the
band, gutter, tile width, pad and accent width shared with the menu through
`ui.h` (`ui::HEADER_H`, `ui::GRID_GUTTER`, `ui::TILE_W`, `ui::TILE_PAD`,
`ui::TILE_ACCENT_W`):

- 44 px black header band: project name in helvB24 white, the window and its
  range (`7 days  4 Sep to 10 Sep`, `Total  since 9 Sep`) in 7x14 right.
- headline line in profont22 at y=80 (`Visits up 144%`, `Quiet day.`, or for
  the total window `Everything since Wed 9 Sep.`).
- 2 x 5 grid of hairline cards from y=100 to y=880, 318x146 px with a 12 px
  gutter, one metric each: label (profont22) with a 36 px accent hairline
  under it, the value large (fub42 digits; a trailing `M`/`k` in helvB24 on
  the same baseline since fub42 has no letters; the whole value in helvB24
  when it does not fit), the foot line bottom left (`7d avg 9`, `prev 117`,
  `avg 15/day`, `was 1,064`, `since 9 Sep`) and the delta bottom right in
  7x14 (bold). The foot line is cut before it can run into the delta.
- status strip: `daily numbers  2/5  08:07` left (screen, and the Worker's
  ICT render time), `DOWN project   RIGHT window   CENTER menu` right.

When the feed carries notes (`ig skipped`, `reddit failed`) one more screen,
"Sync notes", lists them with the generation time. A feed with no projects
draws a single "No data in the last week." screen. Without a
`/numbers.json` at all (never synced, or a parse failure) a status screen
says "No numbers yet, run Sync feeds." and CENTER is the only way out.

A fifth window, Charts, follows Total: one full screen line chart per
metric over the feed's `history` axis (90 days ending on the reporting day,
values from the metric's `h` string, `NAN` for a missing day). Header band
`Charts  last 90 days`, the metric and its yesterday value in the headline
slot, then the plot from x=112 to 660 and y=112 to 836: a value column on
the left with min, mid and max labels (dotted rules at mid and max), date
ticks every 14 days along the bottom, a 5 px dot per day and a 2 px line
between neighbouring days that both have a value. The y axis starts at zero
unless the series goes negative. In this window DOWN steps to the next
metric and spills into the next project after the last one; the strip reads
`daily numbers  2/5  chart 3/10`. Another press of the right pad wraps back
to Yesterday.

Buttons: DOWN = next project screen (next metric in Charts), wrapping; LEFT
(the founder's "right") = next time window, Yesterday through Charts and
back to Yesterday; UP and RIGHT = the reverse moves when present; CENTER =
back to the menu. The screen index and the window survive leaving
the view, so reopening lands on the project and window last looked at (both
reset to the first if the feed shrank). The data is loaded from
the file on every open and freed on leaving; the file is a few KB.

Refresh policy is the menu's: `ui::frameBegin(false)` for screen changes
(partial), full when the panel still shows an image page or the ration says
so. The view draws through the `ui::` primitives (`setFont`, `printAt`,
`fitText`, `gfx()`, `frameBegin`/`frameNext`/`frameEnd`) rather than
reaching into the panel object.

The layout was tuned on a PIL mock first (`numbers_mock.py`, output
`numbers_mock.png`, in the session scratchpad); the constants in
`numbers_view.cpp` are the mock's.

## Networking

The radio is off in normal reading and stays off. It comes up for exactly two
things, both entered from the menu, and both put it back down before returning:
"WiFi setup" and "Sync feeds". There is no server on the device, no MQTT, and
nothing listens while a book is open.

### Credential store

`src/wifi_store.*`, backed by NVS (Preferences), not LittleFS: `pio run -t
uploadfs` rewrites the whole filesystem partition, and WiFi credentials must
survive that. Up to `WIFI_STORE_MAX_NETWORKS` (8) entries, newest first;
saving an SSID that is already stored replaces it in place rather than eating a
slot. The size lives in `include/wifi_config.h` and supersedes the older
`WIFI_MAX_NETWORKS` (4) still sitting in `include/config.h`.

### NVS keys

| Namespace | Key | Type | Meaning |
|---|---|---|---|
| `wifi` | `n` | u8 | number of stored networks, 0 to 8 |
| `wifi` | `ssid0` .. `ssid7` | String | SSID of entry N |
| `wifi` | `pw0` .. `pw7` | String | passphrase of entry N |
| `news` | `etag` | String | ETag of the brief currently on the device |
| `news` | `stamp` | String | `YYYY-MM-DD HH:MM` UTC of the last brief sync |
| `news` | `etag2` | String | ETag of the `/numbers.json` on the device |
| `news` | `stamp2` | String | `YYYY-MM-DD HH:MM` UTC of the last numbers sync |
| `news` | `auto` | bool | sync once at boot, default false, all feeds |
| `news` | `bootsync` | bool | set while a boot sync is running, see the boot loop guard |
| `ui` | `rot` | u8 | panel rotation from Flip screen, 1 or 3; absent = `DISPLAY_ROTATION` |

### WiFi setup (captive portal)

`src/wifi_setup.*`. Scans first (a soft AP pins the radio to one channel and
makes the scan worse), then brings up an open soft AP `ereader-setup` in
AP+STA mode, a catch-all DNSServer on port 53 and a one page WebServer on
192.168.4.1. The page lists the scanned SSIDs, takes a manual SSID and a
password, and posts to `/save`. It also lists the stored networks, each with a
`remove` link to `GET /forget?ssid=`. Any other path is answered with a 302 to
the portal, which is what makes a phone pop its "sign in to network" sheet.

The form has two submit buttons, both posting to `/save` with a `go` field:

- `Save and connect` stores the credentials and then verifies them for real:
  the device joins the network as a station while the AP stays up, so the phone
  is not dropped mid setup, and the result is reported both on the page and on
  the panel. The join runs from the main loop, not inside the request handler,
  so the portal stays responsive while it waits.
- `Save and add another` only writes the store and redirects back to the
  portal, no join. That is how a batch of networks goes in during one visit
  without waiting out a join for each one. Use `Check WiFi` from the menu
  afterwards to find out which of them actually work.

The panel shows the AP name, the URL, the saved network list, the result of the
last join attempt, and `Press center to exit`. CENTER tears the AP down and
powers the radio off. Status redraws go through `ui::renderStatusScreen()`,
which uses the normal partial window policy, so setup does not burn a full
refresh every few seconds.

The AP is open on purpose: it is up only while the user is standing at the
device, it serves one page, and the one secret on it is torn down seconds
later. Set `WIFI_SETUP_AP_PASSWORD` in `include/config.h` to 8 characters or
more to make it WPA2 instead.

### Connect helper

`wifi_connect()` in `src/net.*` puts the radio in STA mode, scans, and joins
the strongest network it holds credentials for, falling through to the next
best on failure with a 15 s budget each. A hidden network never shows in a
scan, so an empty match list falls back to trying the store in order.
`wifi_off()` disconnects and sets `WIFI_OFF`.

`net_time_sync()` asks SNTP for the time once, with a 5 s budget, right after
associating. The reader has no RTC and does not need the time, but mbedtls
rejects every certificate as "not yet valid" while the clock still reads 1970,
so TLS does need it.

### Feed sync

`src/news_sync.*`. The device syncs a table of feeds, `NEWS_FEEDS` in
`include/config.h`, in one WiFi session. Each row has a `kind`:

| # | kind | slug | title | endpoint | NVS keys |
|---|---|---|---|---|---|
| 0 | BOOK | `news` | News brief | `/brief/latest.txt?k=NEWS_TOKEN` | `etag`, `stamp` |
| 1 | NUMBERS | `numbers` | Daily numbers | `/metrics/ereader.json?k=METRICS_TOKEN` | `etag2`, `stamp2` |

Both are on `api.muonsortes.com`. A BOOK feed is plain text and becomes a
book. A NUMBERS feed is the metrics hub's daily cross project figures as
compact JSON (see `docs/metrics-hub.md`, section "E-reader feed"), stored as
is at `/numbers.json` (`NUMBERS_FILE`) for the numbers view to draw; it is
never paginated and never a book. (Until 2026-09-11 it was a 40 column text
page paginated into `/books/numbers.pgs`; the sync and the boot now delete
that file and its `.pos`, and `books::list()` skips the `numbers` slug in
case one is still there.)

Adding a feed is one row in that table plus a key pair in NVS. Nothing else in
the firmware changes for a BOOK feed: `news_sync.cpp` walks the table, and the
library scan already picks up any `.pgs` in `/books`.

Steps: connect, set the clock, then for each feed an HTTPS GET with
`If-None-Match` set to that feed's stored ETag (and `Accept: text/plain` or
`application/json` by kind), and

- 304: report "up to date", nothing else happens for that feed.
- 200, BOOK: stream the body to the feed's temp file (plain text, UTF-8, at
  most `NEWS_MAX_BYTES` = 200 KB), paginate it from that file into
  `/books/<slug>.pgs` on both text grids, rewrite `/books/<slug>.pos` to page 1
  of the large variant, and store the new ETag plus the `YYYY-MM-DD HH:MM` stamp
  parsed out of the response's `Date` header. No NTP is needed for the stamp.
- 200, NUMBERS: stream the body to the temp file under the same 200 KB cap
  (it is a few KB), rename it over `/numbers.json` in one step, remove any
  stale `/books/numbers.pgs` and `.pos`, and store the ETag and stamp the same
  way.

The radio comes up once for the whole table and goes off when the last feed is
done. A feed that fails does not stop the ones after it: a stale numbers page is
no reason to go without the news. The aggregate result is `OK` if any feed
downloaded (with the page counts summed over the BOOK feeds and `numbers` set
if the NUMBERS feed was among them), `NOT_MODIFIED` if every feed answered
304, and otherwise the first failure with its slug in the message.

Progress is drawn on the panel as "connecting", then per feed
"<title>: downloading", then "<title>: paginating" and "<title>: N pages" for
a book or "<title>: saved" for the numbers, and finally "done" (with "daily
numbers updated" when that feed was new) or "error: <slug>: <reason>".

`news_sync::lastStamp()` is feed 0's stamp, which is what the menu header
shows; `news_sync::numbersStamp()` is feed 1's, shown on the Daily numbers
menu tile.

Each BOOK feed is written as an **MPG2** book with `NEWS_VARIANT_COUNT` (2) variants:
variant 0 on the 54x37 profont22 grid, variant 1 on the 40x27 profont29 grid,
the same order and the same anchors `tools/txt2book.py --dual` produces. The
temp file is paginated once per variant, so "Text size" in the menu works on a
synced feed exactly as it does on a host built dual size book. There are no
image pages in a feed, so no blob is shared between variants.

`NEWS_DEFAULT_VARIANT` (1, the large text) is the variant a synced feed opens
in. A sync rewrites `/books/<slug>.pos` as page 0 of that variant rather than
deleting it, because a missing `.pos` reads back as variant 0.

The body is never held in RAM. It is streamed to a temp file, paginated from
that file a 512 byte block at a time, and each finished page blob is appended to
a second temp file (`/<slug>.blob`), because the MPG2 page tables sit ahead of
the blobs and their size is only known once every page of every variant exists.
The final pass writes the header, both page tables and then copies the blob file
in. The only per page RAM cost is 8 bytes (length plus anchor) in the page table
vector, per variant, capped at `MAX_PAGES` (3000) pages per variant. The task
watchdog is fed throughout: from the socket read loop, from the pagination loop,
from the page table writer and from the copy loop.

Temp file names are per feed, so a failure part way through one feed leaves the
others alone. Paginating twice roughly doubles what a sync costs on the
filesystem: at peak the partition holds the feed's `.tmp`, its `.blob` with both
paginations, and the finished `/books/<slug>.pgs`, so a full size 200 KB brief
wants about 1 MB free rather than the ~600 KB the single variant writer needed.
The feeds are synced one after another, so that peak is per feed and not summed.

HTTPClient parses the response headers but leaves chunked framing in the
stream, and `writeToStream()` gives no place to feed the watchdog, so
`news_sync.cpp` reads the body itself and handles both identity (with or
without Content-Length) and chunked bodies.

A synced book is an ordinary book: it shows up in the library, opens, turns
pages, switches text size, and takes a saved position like any other. Its title
is written as `<feed title>  <stamp>`, and `books.cpp` maps the `news` slug to
"News brief" for the case where a file is left behind with no title.
`books::list()` reopens and reparses every `.pgs` on each scan, so a feed
replaced while another book is open is picked up correctly; if the replaced feed
was the open book, the firmware reopens it after the sync.

### TLS

`src/ca_roots.h` bundles two public self signed roots as one concatenated PEM
string (mbedtls parses a bundle): **GTS Root R4**, which is what
`api.muonsortes.com` chains to today through Google Trust Services' WE1
intermediate, and **ISRG Root X1** (Let's Encrypt) for the day the Cloudflare
edge issuer changes. Both carry their SHA-256 fingerprint in a comment and the
command that re-derives them.

`NEWS_TLS_ALLOW_INSECURE_FALLBACK` (default 1) retries once with
`setInsecure()` if the validated connection fails at the transport level. That
turns certificate validation off completely: the link is still encrypted, but
anything on the path can impersonate the endpoint and hand the reader whatever
text it likes, along with the `NEWS_TOKEN` in the query string. It is a
fallback, not the normal path. If it is what makes syncing work, the real fix
is to put the right root in `ca_roots.h`.

### Auto sync

A menu toggle stored in NVS, default off. The reader has no clock between
boots, so "older than 12 hours" is not a question it can answer; the toggle is
the whole policy. When it is on and at least one network is stored, boot
renders the saved reading page first (boot-to-page stays as fast as it was),
then runs a sync, then redraws the page.

#### Boot loop guard

A boot sync that kills the device is a reboot loop the reader cannot escape,
since the menu (and so the toggle) is only reachable once the sync returns.
Three guards in `src/main.cpp` cover that:

- **NVS flag.** `news` / `bootsync` (bool) is written just before the boot sync
  and removed the moment it returns, whatever the outcome. Finding it set at
  boot means the last boot sync never returned, for any reason: watchdog,
  brownout, panic, or the battery unplugged mid-transfer. The flag is cleared
  and the sync is skipped for that boot.
- **Reset reason.** `ESP_RST_TASK_WDT` / `ESP_RST_WDT` and `ESP_RST_BROWNOUT`
  also skip. WiFi TX peaks near 500 mA on a battery that is only plugged in
  briefly, so a brownout is a likely way for the sync to die.
- **Grace window.** With the page already on glass, the boot sync waits up to
  `AUTO_SYNC_GRACE_MS` (8 s, defined at the top of `main.cpp`) for a button. A
  press skips the sync for that boot: the reader wanted the page, not a sync.

Any skip sets an in-RAM flag for the life of the boot, and the menu header
appends `auto sync skipped` after the `wdt N` count.

### Games server

`src/games.*`, reached from the "Books and games (WiFi)" tile. It is also how
books get onto a finished reader, see "Books over WiFi" below. The reader hosts NES and Game Boy
(Color) ROMs plus a browser emulator page, and stores the save files, so a
laptop or a phone plays in its browser and either device can pick up where
the other left off. The reader never emulates anything; e-ink cannot do 60
frames a second and the S3 has no RAM to spare next to the shadow buffer.

- Radio: joins the strongest stored network like a sync (`wifi_connect`) and
  raises `GAMES_AP_SSID` (WPA2, `GAMES_AP_PASSWORD`) alongside it, so the
  page works at home over the LAN and anywhere else over the reader's own AP.
  With a station link up the reader also announces `GAMES_MDNS_HOST.local`.
  The status screen shows both URLs, the ROM count, the request count and
  the idle time. CENTER exits; so does `GAMES_IDLE_TIMEOUT_MS` (15 min)
  without a request. The page pings once a minute while a game is loaded, so
  an open game keeps the server up and a closed tab lets it stop. The task
  watchdog runs on the 60 s sync window while the server is up because
  streaming a script to a phone cannot feed it.
- Files: ROMs under `GAMES_ROM_DIR`, saves under `GAMES_SAVE_DIR`, asset packs
  under `GAMES_AUX_DIR`. Names are
  `[A-Za-z0-9._-]`, lowercase, at most `GAMES_NAME_MAX` (40) characters, with
  extension `.nes`, `.gb`, `.gbc` or `.pack`; an upload is renamed to fit, and
  a `.pack` is routed to the aux folder instead of the ROM folder (cap
  `GAMES_MAX_AUX_BYTES`, 3 MB). Three save
  files per ROM, all opaque bytes to the firmware: `<rom>.sav` (cartridge
  battery RAM, Game Boy only, uploaded by the page when it changes),
  `<rom>.state` (the manual Save button) and `<rom>.auto` (autosave every
  minute and on page hide). Uploads and saves are written to a `.tmp` and
  renamed, so a dropped connection never leaves a half file.
- Page: `www/index.html` + `www/app.js`, with jsnes (`www/vendor/nes.js`)
  and WasmBoy (`www/vendor/gb.js`). `tools/build_www.sh` gzips the four into
  `www/dist/`, and `board_build.embed_files` links them into the app image.
  Never ship the page through the filesystem: `uploadfs` rewrites the whole
  LittleFS partition and would take the ROMs, saves, books and synced feeds
  with it. `tools/www_stub_server.py` is a laptop stand-in for the device
  (same routes against a folder) for working on the page.
- Routes (all on port 80, no auth beyond the AP password):
  - `GET /`, `/app.js`, `/nes.js`, `/gb.js`: the embedded page and scripts,
    `Content-Encoding: gzip`, `ETag` from length and build time, `no-cache`,
    `304` on a matching `If-None-Match`.
  - `GET /api/roms`: `[{"name","size","sav","state","auto"}]`, the three
    booleans say which save files exist.
  - `GET /roms/<name>`: the ROM bytes.
  - `POST /api/upload`: multipart, field `rom`, one file; `303` to `/` or
    `400` with the reason (bad extension, over the size cap, full). A `.pack`
    goes to `GAMES_AUX_DIR` and reopens the pack reader on the spot.
  - `POST /api/delete`: form field `name`; removes the ROM and its saves.
  - `GET /saves/<rom>.sav|.state|.auto`: the bytes, `404` when absent.
  - `POST /saves/<rom>.sav|.state|.auto`: raw body up to
    `GAMES_MAX_SAVE_BYTES`, answers `{"ok":true,"size":N}`.
  - `POST /api/wram`: raw body of exactly 8192 bytes (`GAMES_WRAM_BYTES`), the
    Game Boy work RAM from GB address `0xC000` up. Answers
    `{"ok":true,"size":8192,"n":<snapshot count>}`, or `400` with
    `{"ok":false,"want":8192}` for any other length. The body is never written
    to the filesystem: it lands in the static buffer in `src/pokemon_state.*`.
  - `GET /api/wram`: the last snapshot back out, `404` before the first one.
    Debugging only, so a decoder can be run against a real save on a host.
  - `GET /api/ping`: `{"ok":true,"free":<bytes>,"uptime":<s>,"roms":N,
    "pack":<bool>,"wram":<bool>,"total":<bytes>,"reserve":<bytes>,
    "txtFactor":N,"txtMax":<bytes>,"books":N}`, and resets the idle timer like
    every other request. `pack` says whether `GAMES_PACK_PATH` opened, `wram`
    whether a work RAM snapshot has arrived; the book fields are the space rule
    below, so the page can refuse a book before sending it.
- ROM uploads stop with `400 not enough free space on the reader` before they
  would leave less than `BOOK_UPLOAD_MIN_FREE_BYTES` free: a full LittleFS
  panics inside littlefs instead of failing the write.

#### Books over WiFi

The same page has a Books section above the games. Routes:

- `GET /api/books`: `[{"slug","title","pages","page","sizes","size"}]`,
  from `books::list()`: pages of variant 0, saved page, variant count, file
  bytes.
- `POST /api/books/upload?size=<bytes>&title=<text>`: multipart, one file,
  `.txt` or `.pgs`. The slug is the file stem through the host slug rule
  (`tools/pdf2book.py slugify`, 24 characters); `news` and `numbers` get
  `_book` appended so a feed can never overwrite an upload. Answers
  `{"ok":true,"slug","title","pages"}` or `400 {"ok":false,"error"}`.
  - Space: at the first byte the handler takes the free space once and
    refuses the upload when `size` (the page's claim) would not fit, then
    counts the bytes that really arrive against the same budget. A `.txt`
    may use `(free - BOOK_UPLOAD_MIN_FREE_BYTES) / BOOK_TXT_SPACE_FACTOR`
    (256 KB reserve, factor 5: raw text, the blobs of both grids and the
    finished book are on flash at once), capped at `BOOK_MAX_TXT_BYTES`
    (1 MB, which keeps the page tables small next to the WiFi stack). A
    `.pgs` may use `free - BOOK_UPLOAD_MIN_FREE_BYTES`, capped at
    `BOOK_MAX_PGS_BYTES`.
  - `.txt`: streamed to `BOOK_UPLOAD_TMP_FILE`, then paginated by
    `news_sync::writeTextBook()`, the news brief's path (`src/text_paginate.*`,
    both grids, MPG2) into the staging book `/books/.upload.pgs`. UTF-8 in,
    same normalization as the brief and the host converter; bytes that are
    not valid UTF-8 are dropped. The title is the `title` query, else the
    file stem with `_` and `-` as spaces, control characters dropped, at most
    60 characters. The panel shows "Adding a book" while it paginates.
  - `.pgs`: streamed straight to the staging book.
  - Either way the staging book must open with `Book::open()` before it
    replaces `/books/<slug>.pgs`; the old `.pos` goes with it, so the new
    book opens at page 1 in the normal size. The staging and temp files are
    removed on every failure path and again when the server starts, and
    `books::list()` skips any slug that starts with a dot.
- `POST /api/books/delete`: form field `slug`; removes the `.pgs` and `.pos`,
  `404` when there was no such book.
- When the server returns, `main.cpp` rescans the library and reopens the open
  book from its file, so a replaced book is never read through a stale page
  table and a deleted one closes.

#### Pokemon companion screen

While a Game Boy game runs, the page mirrors the emulator's work RAM to the
reader every second (`WORK_RAM_LOCATION` out of the WasmBoy wasm memory,
hashed with the same FNV-1a as the battery poll so an unchanged 8 KB costs no
request; one upload in flight at a time), and the reader draws a companion screen from it instead of the
server status screen. The page knows nothing about Pokemon; all of the game
knowledge is in firmware:

- `src/pokemon_state.*`: the snapshot buffer and the typed decoders (player
  name and the Gen 1 charmap, party, money, coins, badges, map and position,
  play time, Pokedex counts, bag, battle and enemy mon, repel steps, PC box,
  day care, OT names and ids, player facing direction, the active battle slot,
  both 29 byte battle structs, the trainer class and enemy party, the PC item
  list, the box mons, and the two item pickup flag arrays). Addresses come from
  pokered's `wram.asm`, walked from the `$CBFC` base of `SECTION "WRAM"`.
- `src/pack.*`: reader for the `EPK1` asset container at `GAMES_PACK_PATH`
  (`/games/aux/pokered.pack`), built on a host by `tools/pokered_pack.py` from
  a pokered checkout. Magic `EPK1`, u32 LE section count, then entries of
  `{char name[16], u32 offset, u32 length}` with offsets from the start of the
  file. Only the table of contents is held in RAM; section bytes are read from
  the open `File` on demand. The repo ships the generator, never the assets.
- `src/gbgfx.*`: 2 bpp Game Boy tiles and the Gen 1 font on the 1 bit panel.
  Shades dither at scale 2 and up (0 white, 1 one dot in four, 2 checker,
  3 black) and threshold at scale 1. `printGb` draws from the pack's `font`
  section (256 1 bpp tiles in charmap order, cached in RAM) and falls back to
  the reader's own font when no pack is present, so the screen still reads.
  `drawBox` uses the nine `border` tiles.

- `src/pokemon_views.*`: the four views, drawn to the pixel contract in
  `docs/pokemon-aux-display-layout.md`. Two tile grids share one origin at
  (4, 24), so 2x (16 px) and 3x (24 px) tiles land on the same lattice; the
  reader's own chrome is a black top bar (view name, player name, play time,
  and the reader clock at the last accepted snapshot) and a black hint band
  carrying the pad legend, both in the game font, white on black.
  - **Home**: the in-game Town Map at 2x with the current cell boxed and the
    game's cursor sprite over it, a location box under it, the trainer card
    split left (name, id, money, coins, rival) and right (Pokedex counts, play
    time, repel steps, map and step coordinates, bag count), a six row party
    box (icon, nickname, level, status, HP bar, HP numbers, types, species and
    OT line), and the eight badges: the badge art when earned, the gym leader's
    face dimmed one shade when not.
  - **Inventory**: the bag at 3x in one column of twenty rows (name, quantity,
    Mart price), then the PC item list on the left and money, coins, the
    current box, the day care and the box's mons on the right.
  - **Terrain**: the current map drawn from its real tileset, blockset and
    block layout, a ten by eight block window at 2x clamped around the player
    (a map smaller than the window is centred and the rest filled with the
    map's own border block), Red's frame by facing direction with right
    mirrored from left, then wild encounters with the game's fixed slot odds
    (20 20 15 10 10 10 5 5 4 1 percent, merged by species) and the map's placed
    and hidden items with their step coordinates, taken ones struck through
    from the pickup flag arrays. An interior with neither table gets a taller
    window instead of the two boxes.
  - **Battle**: the enemy's front sprite at 3x, name, level, status, HP bar and
    numbers, types, catch rate or the trainer class and how many mons are left,
    dex number and its owned / seen / new state, base stats; our back sprite
    and the same block for the active mon; the four moves with type, PP, power,
    accuracy, STAB and an inverted effectiveness badge (`x2`, `x4`, `1/2`,
    `1/4`, `x0`, nothing when neutral) from the pack's type chart against both
    of the enemy's types; the party strip with the active mon's name inverted;
    and the enemy's own moves scored against our mon.

  Pack sections are read on demand into small buffers rather than loaded whole.
  The current tileset and blockset are cached by id (4 KB each), along with the
  town map, the 2 bpp battle font page and the type chart; one Pokemon picture
  exists at a time. That is about 11 KB of static RAM on top of the 8 KB
  snapshot and gbgfx's 2 KB font page.

CENTER exits. Silk right (`BTN_UP`) is the next page and silk left
(`BTN_DOWN`) the previous, wrapping over Home and Inventory; silk up
(`BTN_LEFT`) toggles Terrain over whichever page is current. A battle takes
the screen automatically while the in battle byte is nonzero, whatever the
pads say, and the view that was up comes back by itself when the fight ends.
The screen redraws when the displayed part of the snapshot changed
(`viewSignature()`, an FNV-1a over the WRAM ranges the views read) or a pad
changed the view, at most every `GAMES_AUX_REDRAW_MS` (300 ms): a partial
refresh when the same view redraws with new data, a full one when the view
changes or every `GAMES_AUX_FULL_MS` (60 s). Pad presses are latched in an
interrupt (`input::takeLatched`, `BUTTON_LATCH_MS`) so a press during a
refresh is not lost. The raw POST body is read one byte at a time
(`-DHTTP_RAW_BUFLEN=1` in platformio.ini) because the stock buffer read blocks
up to 5 s on a short last chunk; save uploads are staged in RAM before the
filesystem write. The embedded page assets carry an ETag and
`Cache-Control: no-cache`, so a reflash is picked up on the next load. With no pack on
the reader every view falls back to one line, "pokered.pack missing, upload it
on the games page", in the reader's own font.

`tools/pack_check.py` is the host side check: it prints a pack's section table
with offsets and lengths and says whether the reader would accept it.

### Config to fill in

`include/config.h`:

- `NEWS_TOKEN`: ships as `"CHANGE_ME"`. The shared secret in the brief URL's
  query string. Lives in `include/secrets.h` (gitignored, copy
  `include/secrets.h.example`).
- `METRICS_TOKEN`: same file, the read token for the metrics feed
  (`METRICS_READ_TOKEN` on the Worker side). A `secrets.h` written before the
  metrics feed existed has none, so `config.h` falls back to `"CHANGE_ME"`.
- A token left at `"CHANGE_ME"` hides its feed: the feed is skipped by a
  sync, and the menu leaves out Daily numbers (metrics) and, with both unset,
  Sync feeds and Auto sync. This is a compile time check
  (`NEWS_FEED_CONFIGURED`, `METRICS_FEED_CONFIGURED`, `FEEDS_CONFIGURED`).
- `NEWS_URL` / `METRICS_URL`: `https://api.muonsortes.com/brief/latest.txt?k=`
  and `https://api.muonsortes.com/metrics/ereader.json?k=` by default.
- `WIFI_SETUP_AP_SSID` / `WIFI_SETUP_AP_PASSWORD`: portal AP name and, if you
  want WPA2, a passphrase of 8 characters or more.

The server side of the brief (the routine that writes `latest.txt`, its ETag
and its token check) is out of scope here.

## On-device pagination

A brief downloaded by the device has to be paginated by the device, on exactly
the grid and with exactly the rules `tools/pdf2book.py` uses, or a book made on
the device would not match one made on the host. `src/text_paginate.cpp` is the
C++ twin of `normalize_text`, `pages_to_paragraphs`, `wrap_paragraph_spans` and
`paginate_spans` for the case txt2book handles with a plain text input: no form
feeds, no figures, and therefore no running header detection (which returns an
empty set for fewer than four pages anyway).

Character model. Input is UTF-8. Normalization transliterates the same
codepoints the host does (the `TRANSLIT` table is mirrored entry for entry),
keeps ASCII plus the Latin-1 supplement, and drops everything else. The result
is held one byte per character, which is what makes a column mean the same
thing here as it does in Python, where a column is a codepoint and not a byte.
Page blobs are encoded back to UTF-8 on the way out, so the bytes in the file
are the bytes the host writer would have produced.

The file is Arduino free on purpose so the host can compile it. That is how the
two are kept in sync:

```
python3 tools/check_paginator.py            # built-in sample
python3 tools/check_paginator.py brief.txt  # your own text
```

It builds `tools/paginate_test.cpp` against `src/text_paginate.cpp` with the
system `c++`, runs both pipelines over the same bytes on both grids (54x37 and
40x27) and diffs the pages line by line. Run it after touching either side.
It currently passes on the built-in sample and on 180 KB of real book text.

One deliberate divergence: a paragraph longer than
`textpage::PARAGRAPH_MAX_CHARS` (32 KB) is flushed early instead of buffered
without limit, and `Pipeline::truncated()` says so. Real prose never gets
close; this only fires on a file with no blank line in it.

## Style rules

No em-dashes anywhere (code comments, docs, README, strings). Use periods,
commas, colons, parentheses.

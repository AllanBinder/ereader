# ereader

A homemade e-reader: an ESP32-S3 board, a 5.76 inch e-paper panel and a
five-way button. It reads books, keeps your page when the power goes, and
turns into a small web server when you want it to: from a phone you can add
books, delete them, and play Game Boy and NES games in the browser while the
reader keeps the saves. Apache-2.0 code, CC BY 4.0 docs, see `NOTICE`.
Contract and internals: `SPEC.md`.

This is a standalone side project by the people behind
[Muon Sortes](https://muonsortes.com), not part of it. A build guide with
photos lives at
[muonsortes.com/build-a-wifi-ereader](https://muonsortes.com/build-a-wifi-ereader).

## What you need

| Part | What to buy | Notes |
|---|---|---|
| Board | ESP32-S3 dev board with **8 MB or 16 MB** flash (N8R2, N16R8 and similar) | 4 MB boards are not supported by the ready-made image. USB-C. |
| Panel | Good Display GDEH0576T81 (5.76 inch, 920x680, black and white) | Buy it as a kit with the adapter below. |
| Panel adapter | Good Display DESPI-C02 | Set its RESE switch to 0.47. Runs at 3.3 V. |
| Buttons | Five-way navigation switch module (up, down, left, right, center) | Five signal legs plus one common leg. |
| Capacitors | 470 uF electrolytic plus 100 nF ceramic | Across the adapter's VCC and GND. Not optional. |
| Cable | USB-C data cable | For installing and for power. |
| Optional | BME280 breakout | Temperature, humidity and pressure in the menu header. |

Power can be USB only (plug in, read, unplug: the page stays on the glass), or
a battery; the build guide above covers the options.

Case: the laser-cut acrylic and 3D print case files are sent by email when
you sign up on the [build guide](https://muonsortes.com/build-a-wifi-ereader).

## Wiring

This table is `include/pins.h`, which is the single source of truth. Solder
every joint; Dupont jumpers let the panel rail sag during a refresh, and that
eventually damages the panel controller.

| From | To ESP32-S3 | Notes |
|---|---|---|
| Adapter SDI | GPIO 17 | SPI data |
| Adapter SCK | GPIO 18 | SPI clock |
| Adapter CS | GPIO 8 | |
| Adapter D/C | GPIO 9 | |
| Adapter RES | GPIO 10 | |
| Adapter BUSY | GPIO 46 | |
| Adapter VCC | 3V3 | 3.3 V, never 5 V. Put the 470 uF and 100 nF capacitors right here, across VCC and GND. |
| Adapter GND | GND | |
| Button UP | GPIO 40 | "up" as you hold the reader in portrait |
| Button DOWN | GPIO 41 | |
| Button LEFT | GPIO 1 | |
| Button RIGHT | GPIO 2 | |
| Button CENTER | GPIO 42 | the center click |
| Button common | GND | all five share it |
| BME280 SDA (optional) | GPIO 12 | |
| BME280 SCL (optional) | GPIO 13 | |
| BME280 VCC / GND (optional) | 3V3 / GND | |

The panel has no MISO line; nothing goes there. The button module's own
silkscreen does not matter: wire each switch by the direction it points when
you hold the reader, and the firmware takes care of the rest. Modules that
switch to 3.3 V instead of to ground work too, since the firmware samples the
idle level at boot (so do not hold a button while it boots).

## Install without building anything

1. On a computer, open
   [muonsortes.com/build-a-wifi-ereader#flash](https://muonsortes.com/build-a-wifi-ereader#flash)
   in **Chrome or Edge** (other browsers cannot talk to USB devices).
2. Plug the board into the computer with the USB-C cable. On boards with two
   USB-C ports, use the one marked USB (not UART or COM) if one fails.
3. Click **Install**, pick the board's port, and confirm. **This erases
   everything on the board.** It takes about two minutes.

If the port never shows up or the install fails to start, hold the board's
BOOT button, tap RST, release BOOT, and click Install again.

The image is built for 8 MB of flash. It also runs on 16 MB boards (the
extra 8 MB just goes unused). It does not fit a 4 MB board.

## First boot

The reader opens *Alice's Adventures in Wonderland*, which comes with the
install, so you can check the panel and the buttons straight away. If the
picture is upside down for the way you hold it, press center for the menu and
pick **Flip screen**: it turns the picture 180 degrees and turns the buttons
with it, so up is still up. The choice is kept across power cycles and
firmware updates (an install from the web page, which erases the board,
resets it).

## Set up WiFi from a phone

1. Press center for the menu and pick **WiFi setup**.
2. On your phone, join the WiFi network **ereader-setup** (no password). Most
   phones then open the setup page by themselves; if not, open
   `http://192.168.4.1`.
3. Pick your home network, type its password, press Save. The reader checks
   it and says whether it worked, on the page and on the panel.
4. Press center on the reader to finish.

Up to eight networks are remembered. **Check WiFi** in the menu tests every
stored network and tells you which ones work. You can skip this step
entirely: adding books also works through the reader's own WiFi.

## Add books from a phone

1. Press center for the menu and pick **Books and games (WiFi)**. The panel
   shows the addresses to open:
   - on your home WiFi: `http://ereader.local/` or the `http://192.168.x.x/`
     address it shows;
   - anywhere else: join the WiFi **ereader-games** (password `ereader1`)
     and open `http://192.168.4.1/`.
2. On that page, under **Books**, pick a file and tap **Add book**.
   - A **.txt** file (UTF-8 or plain ASCII) is laid out into pages on the
     reader itself, in both text sizes. It takes a few seconds per 100 KB.
     Up to 1 MB of text per book; for longer books use a .pgs.
   - A **.pgs** file made by the converter in `tools/` is stored as is.
     This is also the way to get books with pictures onto the reader.
3. Delete a book with its Delete button (tap twice). The page shows how much
   space is left, and refuses a book that would not fit before sending it.
4. Press center on the reader when you are done (it also stops by itself
   after 15 minutes with the page closed). The new books are in the menu.

Free public domain books in plain text: [Project Gutenberg](https://www.gutenberg.org)
(pick "Plain Text UTF-8").

The same page plays games: upload your own `.nes`, `.gb` or `.gbc` dumps
and play on the phone with the touch pad; the reader keeps the saves. See
"Games server and the Pokemon companion display" below.

## Controls

Five buttons. Names below are the wires in the table above (with **Flip
screen** on, the firmware swaps them for you).

Reading:

- LEFT or RIGHT: next page.
- UP or DOWN: previous page.
- CENTER: menu.

Menu (a grid of tiles, two columns):

- DOWN: next row. UP: previous row.
- LEFT: next column. RIGHT: previous column.
- CENTER: pick. Leave the menu with the Resume tile.

Menu items: Resume, Speed read, one tile per book, Text size (Normal or Large, for books
that have both), WiFi setup, Check WiFi, Books and games (WiFi), Jump to
page, Flip screen, Button test.

Speed read: pick a book and it flashes one, two or three words at a time on
the landscape screen, from the page you are on in that book (two words at 250
words a minute to start). The picker ends with two settings tiles: Words
(1, 2 or 3 per flash) and Speed (+25 wpm a press, wrapping back to 100). While
it plays, LEFT is faster and DOWN is slower (25 wpm a press, remembered),
CENTER pauses. Paused, LEFT plays again, DOWN steps back a sentence or two,
CENTER goes back to the normal page, which is kept in step with what you speed
read.

How it gets fast on the GDEH0576T81: SPI at 40 MHz, the partial waveform
picked for a forced 40 C (the shortest the panel stores, 367 ms), and a
waveform cut. When a flash has to come faster than that waveform allows, the
driver resets the panel controller 60 ms into the refresh, which stops the
drive, and re-initialises it (about 90 ms, mostly the charge pump restart).
Words are already solid black at 60 ms, so a flash drops from about 420 ms to
about 205 ms: roughly 290 wpm at one word a flash. The cut is automatic and
per flash: any flash that stays up 450 ms or more gets the full waveform. A
ghost clearing full refresh runs at the first sentence end after 30 flashes
(always by 40). The cut adds some ghosting, and a truncated waveform is
probably not DC balanced, so treat it as experimental on your panel: if
ghosts survive a full refresh, stop. The cut length and the re-init cost are
specific to this panel; see `RSVP_*` in `include/config.h` and `setCutMs` in
the vendored driver.

Jump to page: LEFT +1, RIGHT -1, UP +10, DOWN -10, CENTER goes there. Hold a
button to repeat. The target wraps around at both ends.

**Button test** shows which GPIO each press actually pulls, which is the
quickest way to find a miswired or bridged switch. If a direction feels
backwards for the way you hold it, swap those two wires, or change the
mapping in `src/main.cpp` (see "Make it yours").

## Make it yours

Everything above works as is, no code needed. But the firmware is small, plain
C++, and commented throughout, so it is easy to change, and the easiest way
is to let [Claude Code](https://claude.com/claude-code) do it:

1. `git clone https://github.com/AllanBinder/ereader` and open the folder
   with Claude Code.
2. Describe what you want in plain words. For example:
   - "I wired the buttons to GPIO 4, 5, 6, 7 and 15, update the pins."
   - "My panel is a Waveshare 7.5 inch V2, port the display code to it."
   - "Add a menu item that shows my calendar from this URL."
   - "Pull a daily text file from my server into the library, like the news
     brief does."
   - "Make LEFT go back a page instead of forward."
3. It edits the code and builds it with PlatformIO; plug the board in and
   ask it to flash.

`include/pins.h` holds every pin, `include/config.h` every tunable, and
`SPEC.md` describes how the pieces fit together, so a change usually touches
one or two files. Or read on and build it yourself.

# Developer guide

## Building from source

Install [PlatformIO](https://platformio.org) (the VS Code extension or the
`pio` command line), then from the project root:

```
pio run -e esp32s3 -t upload        # build and flash the firmware only
pio run -e esp32s3 -t uploadfs      # replace the filesystem with data/ (wipes books, ROMs, saves)
pio device monitor                  # log over USB
```

`-t upload` writes the app only and never touches books, saves or WiFi
settings. The web page the reader serves is gzipped into the app image from
`www/`; after editing `www/index.html` or `www/app.js`, run
`sh tools/build_www.sh` before building. `python3 tools/www_stub_server.py`
serves the same page and routes from a folder on your computer for working on
the page without the reader.

**The ready-made image.** The single file the install page flashes is the
bootloader, partition table, app and a filesystem holding one public domain
book, merged at offset 0. To reproduce it: build without `include/secrets.h`,
convert a book into a folder with `tools/txt2book.py --dual --out <dir>/books`,
make the filesystem with PlatformIO's `mklittlefs -c <dir> -s 0x5F0000 -p 256
-b 4096 littlefs.bin`, then

```
esptool.py --chip esp32s3 merge_bin -o ereader-merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 .pio/build/esp32s3/bootloader.bin 0x8000 .pio/build/esp32s3/partitions.bin \
  0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/esp32s3/firmware.bin 0x210000 littlefs.bin
```

## Games server and the Pokemon companion display

Pick the Books and games (WiFi) tile and the reader joins your WiFi (or raises its own access
point, `ereader-games`), announces `http://ereader.local/`, and serves a
browser page with an NES emulator (jsnes) and a Game Boy / Game Boy Color
emulator (WasmBoy). Upload your own ROM dumps from the page, play on a phone
or a laptop, and the reader keeps the saves so either device can continue
where the other stopped. The page has a touch pad, keyboard controls (arrows or
WASD, X or E for A, Z or R for B, Enter for Start, Shift for Select) and 1x/2x/4x
speed.

While Pokemon Red or Blue runs, the page posts the emulator's 8 KB work RAM
to the reader once a second and the e-ink turns into a companion dashboard
built entirely from the game's own tiles, font and sprites:

- **Home**: the Kanto town map with your position, trainer card, money,
  Pokedex counts, play time, the party with HP bars, levels, status and
  types, and the eight badges.
- **Inventory**: the bag with Mart prices, PC items, the day care and the
  current box.
- **Terrain**: the current map drawn from its real tileset around you, the
  wild encounters with their odds, and the placed and hidden items with
  taken ones struck through.
- **Battle**: front and back sprites, HP, types, base stats, catch rate, your
  four moves scored against the enemy's types, and the enemy's moves against
  you.

The repo ships the generator (`tools/pokered_pack.py`), which builds the
asset pack from your own checkout of the
[pret/pokered](https://github.com/pret/pokered) disassembly; no game assets
or ROMs are in the repo. Upload the resulting `pokered.pack` from the games
page. Design docs: `docs/pokemon-aux-display-plan-2026-09-12.md`,
`docs/pokemon-aux-display-layout.md`, `docs/pokered-pack-format.md`.

Want the same for another game? `docs/companion-dashboard-game-candidates-2026-09-13.md`
ranks games with documented RAM maps and full disassemblies (Pokemon
Gold/Silver/Crystal, Link's Awakening, FireRed, Harvest Moon, Dragon Warrior
IV and more) with the addresses, repos and emulator memory access per system.

## Hardware notes

Pins are declared in `include/pins.h`, the single source of truth.

The wiring table is at the top of this file and in `include/pins.h`, which
is the single source of truth. Pins deliberately left alone: 0, 3 and 45 are
strapping pins (46 carries BUSY, which idles LOW at reset so download mode
still works), 19 and 20 are native USB, 33 through 37 are claimed by octal
PSRAM on N16R8 style boards, and 43/44 are UART0. GPIO 11 is spare, and 15 is
avoided because it is dead on the original unit.

**Panel.** Good Display GDEH0576T81, 5.76 inch, 920x680, 1 bit, SSD2677
controller, driven through a DESPI-C02 adapter. **The reader runs the panel in
portrait.** `ui::begin()` rotates it so the logical page is 680 wide by 920
tall, which is what every layout number in `include/config.h` is written
against. The rotation is the single constant `DISPLAY_ROTATION` in
`include/config.h`: 1 (the default) puts the panel's FPC tail on the left edge
of the portrait page, 3 puts it on the right and turns the image 180 degrees.
If the first frame comes up upside down, use **Flip screen** in the menu
(stored in NVS, and it turns the buttons too), or set the constant to 3 and
reflash. The adapter's header is labelled
BUSY, RES, D/C, CS, SCK, SDI, GND, VCC, which is the row the wiring table wires.
Set the adapter's RESE switch to 0.47. There is no panel power enable line on
this adapter, so nothing has to be driven before the first SPI access.

Wire the adapter with soldered joints, not Dupont crimps. Resistive crimps
sagged the panel rail under refresh current on the retired RP2040 build and
eventually damaged that controller. Put 470uF plus 100nF right across the
adapter's VCC/GND and confirm with a DMM that adapter VCC reads within 0.03 V of
the board's 3V3 at idle.

**Refresh regime.** The panel is not driven by GxEPD2's stock
`GxEPD2_576_GDEH0576T81` class. That driver declares
`hasFastPartialUpdate = false` and runs the full ghost-clearing waveform even
when you ask for a partial window, so every page turn and every menu cursor
move costs a multi-second white flash. The reader instead links
`src/drivers/GxEPD2_576_T81_Fast.{h,cpp}`, vendored from the Muon Sortes
firmware (`Muon_Decider/src/drivers/GxEPD2_576_GDEH0576T81.{h,cpp}`). It loads
its own partial LUT and keeps a shadow copy of the frame currently on glass.
The panel's wire format is 2 bits per pixel, `[old_value, new_value]`, and the
partial LUT drives only the pixels whose two bits differ, so a partial refresh
touches just the ink that actually changed: no flash, a few hundred
milliseconds.

The class is deliberately named differently from the stock one rather than
shadowing its header. `GxEPD2_BW.h` includes every panel header it can find and
the library's own `.cpp` is compiled either way, so a same-name copy would
depend on include order and on the linker preferring our object over the
archive member. With a distinct name the stock class is still compiled but
nothing instantiates it, and the linker drops it (confirmed: zero
`GxEPD2_576_GDEH0576T81` symbols in `firmware.elf`).

`ui::beginFrame()` in `src/ui.cpp` picks the waveform per frame:

- Partial: `display.epd2.setFastRefresh(true)` then `setPartialWindow()` over
  the whole screen. The driver ships a full frame and ignores the window
  rectangle, so the window must stay full-screen and the `GxEPD2_BW` page
  height must stay the full panel height.
- Full: `setFastRefresh(false)` then `setFullWindow()`. Forced every
  `PARTIALS_BEFORE_FULL` frames (`include/config.h`, 10), and always on the
  frame after an image page, which would otherwise ghost through.
- The shadow stays coherent by itself: a full frame goes out through
  `writeImageForFullRefresh()`, which rewrites the shadow from the same bitmap
  it just sent.

The shadow is about 78 KB, `malloc`ed on the first write and never freed, on
top of the 78 KB static page buffer. If that allocation ever fails the driver
refuses fast mode and stays on full refreshes, because a partial frame with no
shadow would drive no pixels at all and leave the stale image up.
`ui::begin()` prints the free heap and largest free block after display init so
you can check the margin against the roughly 50 KB a news sync needs for TLS.

**Buttons.** Five momentary switches, each with one leg to its GPIO and the
other leg to a common GND rail. The firmware uses internal pull-ups and
samples each pin's idle level at boot, so either polarity works and no
external resistors are needed. Debounce is about 20 ms in firmware,
edge-triggered on press, and two lines going active within 150 ms count as
one press (a bridged pair of switches then cannot double a page turn).

**Sensor.** BME280 on I2C at address 0x76, with 0x77 as the fallback. Boards
sold as "BMP280" will not report humidity.

## Books from a computer (converter and uploadfs)

Everything below runs from the project root.

1. Drop text-based PDFs into `books_src/`.
2. Convert them:

   ```
   python3 tools/pdf2book.py
   ```

   That writes one `data/books/<slug>.pgs` per PDF and prints a per-book
   summary plus the running total against the 6.09 MB LittleFS budget. You can
   also pass explicit paths and override the title:

   ```
   python3 tools/pdf2book.py books_src/moby.pdf --title "Moby-Dick"
   python3 tools/pdf2book.py books_src/moby.pdf --out /tmp/preview
   ```

3. Check the pagination without hardware:

   ```
   python3 tools/mpg1_dump.py data/books/moby.pgs      # header and limit checks
   python3 tools/mpg1_dump.py data/books/moby.pgs 12   # render page 12 as text
   ```

4. Either add each `.pgs` from the phone page (see "Add books from a
   phone"), or upload the whole `data/` folder to the device filesystem:

   ```
   cd ereader && pio run -e esp32s3 -t uploadfs
   ```

   This rewrites the whole filesystem partition, so it also wipes the `.pos`
   reading positions, `/current.txt`, and every ROM and game save. Every book
   reopens at page 1 after an `uploadfs`. The phone page does not have this
   problem.

5. Build and upload the firmware:

   ```
   cd ereader && pio run -e esp32s3 -t upload
   ```

Firmware and books are uploaded separately. Adding a book needs only step 2 and
step 4; changing firmware needs only step 5, which never touches the books.

### Converting a scanned book

`pdf2book.py` needs a real text layer. A scan has none worth using (Acrobat's
own Paper Capture layer runs words together and misreads whole letters), so a
scanned book goes through four steps instead of step 2. The Bill Porter *Road
to Heaven* conversion of 2026-09-02 is the worked example:

```
BOOK="books_src/road_to_heaven.pdf"

# 1. OCR with Vision. --no-spread keeps output pages 1:1 with PDF pages, which
#    the figure manifest depends on.
swift tools/ocr_pdf.swift --no-spread "$BOOK" > build/road.txt

# 2. Restore paragraphs, block quotes, verse and interview turns. Vision gives
#    the best characters but no geometry; pdftotext -bbox-layout gives the
#    geometry. This merges the two.
python3 tools/ocr_paragraphs.py build/road.txt "$BOOK" build/road_to_heaven.txt

# 3. Extract the plates. --full-res stores them at the full 680x896 content
#    area; drop it for the 2/5 reduced box if the filesystem is tight.
#    --force-pages emits a whole page with no detection, for cover art and for
#    line art the ink detector will not see (here, a two page map spread).
swift tools/extract_figures.swift "$BOOK" build/figures --full-res --force-pages 1
swift tools/extract_figures.swift "$BOOK" build/maps --full-res --pages 10-11 \
      --force-pages 10,11        # then merge the two manifests

# 4. Build the book. Running heads that carry a page number are unique per
#    page, so the automatic repeat detector cannot see them: name them with
#    --strip-regex (one per head form, verso and recto).
python3 tools/txt2book.py build/road_to_heaven.txt --title "Road to Heaven" \
      --dual --figures build/figures/manifest.json \
      --strip-regex '[0-9SOol]{1,4}[.,]?\s*[-*DO0oa]?\s*Road to Heaven' \
      --strip-regex '(?:Road to Heaven|Hermit Heaven|...)\s*[-*DO0oa]\s*[0-9SOol]{1,4}[.,]?'
```

Review the figures before step 4: delete any `build/figures/review/<id>.png`
and `txt2book.py --figures` leaves that figure out.

Full res plates cost about 76 KB each (85 bytes per row times 896), so 44 of
them are 2.87 MB. That is affordable on the 6.09 MB filesystem for one such
book at a time, not for two.

**If the upload does not start.** The bench ESP32-S3 boards do not always
auto-reset into the ROM downloader. Hold BOOT, tap RST, release BOOT, then run
the upload again. USB-CDC is on (`ARDUINO_USB_MODE=1`,
`ARDUINO_USB_CDC_ON_BOOT=1`), so the serial device disappears and comes back
around a flash; if `pio device monitor` cannot find the port, replug.

### Converter dependencies

The converter needs a PDF text extraction library. `pypdf` is preferred and is
what this project is tested against; `pdfminer.six` works as a fallback.

```
python3 -m pip install -r tools/requirements.txt
```

If pip is broken or blocked for your default `python3` (common with Homebrew
Python builds), use a virtual environment instead:

```
/usr/bin/python3 -m venv .venv
.venv/bin/pip install -r tools/requirements.txt
.venv/bin/python tools/pdf2book.py
```

## Personal feeds (optional): news brief and daily numbers

The radio is off while you read. It comes up only for the WiFi menu items and
the feed sync, and each turns it back off before it returns.

The feeds are the founder's own: a news brief and a metrics dashboard pulled
from a private server. They are compiled out of the menu unless you set a
token (see below), so a stock build, and the ready-made image, show neither
Sync feeds, Auto sync nor Daily numbers. Point `NEWS_URL` at your own server
to use the same machinery for a feed of your own.

**WiFi setup.** Pick it from the menu and the reader puts up an open access
point called `ereader-setup`. Join it from a phone or a laptop and open
`http://192.168.4.1` (most phones offer the page by themselves, since the
reader answers every DNS query with its own address). Pick your network from
the list or type the name, enter the password, and press Save. The reader
stores it, then joins the network for real to check it and reports back on both
the web page and the panel. Press the center button on the reader to finish.
Up to eight networks are remembered. They live in NVS, not in the filesystem, so
`pio run -t uploadfs` does not wipe them.

**Sync feeds.** Fetches a plain text brief over HTTPS, paginates it on the
device with the same rules the host converter uses, and writes it to
`/books/news.pgs`. A sync refuses to start with less than `NEWS_MIN_FREE_BYTES`
(640 KB) free on the filesystem and reports "storage full" instead; the
bundled littlefs panics rather than returning an error when a write runs out
of space, so the check has to happen up front. It then shows up in the library as "News brief" with the
time it was fetched, and reads like any other book. An unchanged brief costs
one 304 and no download, because the previous response's ETag is sent back.
The brief always opens at page 1 after a sync, since the old reading position
points into text that is gone.

The device paginates the brief on both text grids and writes it as a dual size
MPG2 book, so "Text size" in the menu switches the brief between Normal and
Large the same way it does for a host built dual size book. The brief opens in
the large text after every sync.

**Auto sync: on/off.** When on, the reader syncs once at every boot, after it
has drawn the reading page. Default is off. There is no clock between boots, so
there is no "sync if older than 12 hours" rule to have: the toggle is the whole
policy.

To turn the feeds on, copy `include/secrets.h.example` to `include/secrets.h`
(gitignored) and fill in `NEWS_TOKEN` (and `METRICS_TOKEN` for the numbers
view). Both ship as `"CHANGE_ME"`; a token left at that value hides its feed
at compile time. `NEWS_TOKEN` is the shared secret in the brief URL's query
string.
`NEWS_URL` defaults to
`https://api.muonsortes.com/brief/latest.txt?k=<NEWS_TOKEN>`. Setting
`WIFI_SETUP_AP_PASSWORD` to 8 characters or more makes the setup AP WPA2
instead of open.

A sync needs room on the filesystem for the raw body and the page blobs at the
same time, and the brief is paginated twice (once per text size), so keep
roughly 1 MB free in the books partition if you expect full size briefs.

**If a sync fails.** The panel prints `error: <reason>`.

- `no saved network`: run WiFi setup first.
- `wifi`: none of the stored networks would associate.
- `connect -1`: the TLS connection did not come up. Usually the certificate
  chain moved; see `src/ca_roots.h` and the `NEWS_TLS_ALLOW_INSECURE_FALLBACK`
  note in SPEC.md.
- `http 401` / `http 404`: check `NEWS_TOKEN` and `NEWS_URL`.
- `brief too large`: the response was over 200 KB (`NEWS_MAX_BYTES`).

## Keeping the two paginators in sync

The device paginates a downloaded brief and every uploaded .txt book itself,
and that C++ has to produce
exactly what `tools/txt2book.py` would have produced for the same text. This is
the check:

```
python3 tools/check_paginator.py            # built-in sample
python3 tools/check_paginator.py brief.txt  # your own text
```

It compiles `src/text_paginate.cpp` natively with the system `c++`, runs both
pipelines over the same bytes on both text grids, and diffs the pages line by
line. Run it after touching either `src/text_paginate.cpp` or the cleaning and
pagination functions in `tools/pdf2book.py`.

## File formats

**`/books/<slug>.pgs`, MPG1.** The book container, little-endian. Magic `MPG1`
(4 bytes), then u32 page count, then u16 title length and that many bytes of
UTF-8 title, then u32 absolute file offsets for each page, then the page blobs
back to back. A page blob is raw UTF-8 with its lines joined by `\n`; page p
runs from `offsets[p]` to `offsets[p+1]`, or to end of file for the last page.
Every line is at most 54 characters and every page at most 37 lines, so the
firmware renders lines verbatim. Text is ASCII plus common Latin-1 punctuation
only, since the profont22 font has no exotic glyphs.

**`/books/<slug>.pgs`, MPG2.** The same book paginated on more than one text
grid, which is what "Text size" switches between. Magic `MPG2`, then u8 variant
count, u16 title length and the title, then per variant a u8 font id, a u32
page count and a page table of `{u32 offset, u32 length, u32 anchor}` entries,
then the blob region. Anchors map a reading position from one text size to the
other. `tools/txt2book.py --dual` writes this, and so does a news sync. The
full field by field description is in SPEC.md.

**`/books/<slug>.pos`.** A u32 little-endian page index (0 based), rewritten on
every page turn. Page turns are seconds apart, so flash wear is negligible.

**`/current.txt`.** The slug of the book that is currently open, as plain text.
On boot the firmware reads this, reads the matching `.pos`, and renders that
page immediately.

Slugs come from the PDF filename stem: lowercased, every run of non-alphanumeric
characters replaced with `_`, truncated to 24 characters.

## Troubleshooting

**The panel is blank and nothing was ever drawn.** Check the DESPI-C02 RESE
switch is on 0.47, that the FPC is fully seated and latched, and that RES
(GPIO 10) and BUSY (GPIO 46) are connected. This failure mode is silent: SPI
succeeds, GxEPD2 returns no error, and the screen simply stays blank. Also
confirm the adapter VCC is on 3V3, not 5V. BUSY should read 0 at idle and swing
to about 3.3 V during a refresh; a mid-rail BUSY means a damaged controller.

**Sensor readings show as n/a.** That is normal with no BME280 fitted. With
one, check the I2C wiring on GPIO 12 (SDA) and GPIO 13 (SCL), that the BME280 has power and ground, and the I2C address. The firmware
tries 0x76 first and falls back to 0x77; some breakouts ship strapped the other
way. A part that is really a BMP280 will not return humidity.

**A book does not appear in the menu.** If you added it from the phone page,
check that the page said "Added"; the reader rescans when you leave the
books and games screen. For `uploadfs`, confirm the `.pgs` file is in
`data/books/` on the host, then re-run `pio run -e esp32s3 -t uploadfs`. Uploading firmware
with `pio run -e esp32s3 -t upload` does not touch the filesystem, so a newly converted
book will not appear until the filesystem is uploaded again.

**The converter produces an empty or near-empty book.** The converter only
handles PDFs that carry a real text layer. A scanned PDF is a stack of page
images with no text, so extraction yields nothing. Run OCR on it first with a
tool such as `ocrmypdf` and convert the OCRed output. Building OCR into the
converter is out of scope for this project. Images and figures in any PDF are
skipped, since only text is extracted.

**`pio run -e esp32s3 -t uploadfs` fails or the device runs out of space.** The
filesystem partition is 6.09 MB, defined by the `spiffs` row in
`partitions.csv` (the image is LittleFS; the row is named `spiffs` because that
is the name the tooling looks for). The converter prints the running total for
`data/books/` and warns past about 1.76 MB. Remove a book and re-run, or on an 8
or 16 MB module extend that row to the end of flash.

## Brief delivery (server side)

The brief lives in Cloudflare KV behind the `muonsortes-api` Worker (`api/`):

- `POST https://api.muonsortes.com/brief`, `Authorization: Bearer <BRIEF_POST_TOKEN>`,
  plain-text body up to 200 KB. Written by a Claude Code routine (cloud, network
  access Custom with `api.muonsortes.com` allowed, token in the environment vars).
- `GET https://api.muonsortes.com/brief/latest.txt?k=<BRIEF_READ_TOKEN>`, returns
  the text with an `ETag`; the device sends `If-None-Match` and gets 304 when unchanged.

Both tokens are Worker secrets (`wrangler secret put`) and are recorded in
`.muonsortes-secrets.txt`. The device's copy of the read token is
`include/secrets.h` (gitignored; template in `secrets.h.example`).

Routine prompt tail:

    Write the brief to brief.txt (plain ASCII text, no Markdown, headline lines
    in CAPS, blank line between items, under 200 KB), then run:
    curl -sS -X POST https://api.muonsortes.com/brief \
      -H "Authorization: Bearer $BRIEF_POST_TOKEN" -H "Content-Type: text/plain" \
      --data-binary @brief.txt
    and confirm the response contains "ok":true.

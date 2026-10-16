#pragma once

#include <stdint.h>

// Input.
static const uint32_t BUTTON_DEBOUNCE_MS = 20;
static const uint32_t BUTTON_POLL_MS = 5;
static const uint32_t BUTTON_LATCH_MS = 40;  // held this long = a real press

// Coincidence lockout. Two GPIOs going active inside this window are one
// press, not two: on this build the d-pad's GPIO 5 and GPIO 6 lines are tied
// together somewhere in the wiring, so a single press on either of those two
// switches reports both. Firmware cannot tell those two switches apart (that
// needs the short cleared), but it can refuse to act on the same press twice,
// which is what turned a page turn into a forward-then-back cycle. The window
// is wide enough to cover the debounce of the second line and far shorter than
// any deliberate two-button press a reader would make.
static const uint32_t BUTTON_COINCIDENCE_MS = 150;

// Task watchdog timeout. Fed every loop() pass and from the e-ink busy
// callback during refreshes, so it only has to outlast a single full panel
// refresh (GxEPD2 gives up on BUSY at 10 s, but the callback keeps feeding
// through that wait).
static const uint32_t WDT_TIMEOUT_MS = 8000;

// Display refresh policy: force a full refresh every Nth partial to clear
// ghosting on the T81 panel.
static const uint16_t PARTIALS_BEFORE_FULL = 10;

// Panel geometry. The panel is chosen at runtime (src/panel.*, NVS key "panel"
// in NVS_NS_UI), so every size below that depends on it is a global filled by
// panel::applyGeometry() before the first frame is drawn, not a constant. The
// names are unchanged, so call sites read them exactly as before. Values are
// written out for both panels:
//   0  Good Display GDEH0576T81, 5.76 inch, 920x680 native, on a DESPI-C02
//      adapter: portrait canvas 680 wide by 920 tall.
//   1  Waveshare 7.5 inch V2, 800x480 native: portrait canvas 480 by 800.
// The UI is PORTRAIT on every panel: the native frame is landscape and an odd
// rotation turns it. Everything below is derived from the canvas width and
// height. The initial values are panel 0's, so a file scope initializer that
// copies one of these before setup() sees the 5.76 inch layout; code that
// must follow the active panel reads the global at draw time instead.
extern int SCREEN_W;  // 680 | 480
extern int SCREEN_H;  // 920 | 800

// Rotation passed to GxEPD2's setRotation(), copied from the active panel's
// row in the table in src/panel.cpp. The panel's native frame is landscape,
// so an odd rotation is what makes the canvas portrait.
//   1 = 90 degrees clockwise: on the 5.76 inch panel the FPC tail exits on
//       the LEFT edge of the portrait page (native top edge becomes the left).
//   3 = 270 degrees: the opposite edge, image flipped 180 degrees relative to
//       rotation 1.
// Default 1 on every panel. If a panel's first portrait frame comes up upside
// down on the bench, change its row in the table to 3 and reflash; nothing
// else in the firmware cares.
extern uint8_t DISPLAY_ROTATION;

// "Flip screen" in the menu turns the picture 180 degrees at runtime (rotation
// 1 <-> 3) for anyone running a prebuilt image who cannot edit the table. The
// choice lives in NVS, so it survives reflashing and uploadfs. Until it is
// used, DISPLAY_ROTATION is what the panel runs at. While the picture is
// turned away from DISPLAY_ROTATION the pad is turned with it (UP swaps with
// DOWN, LEFT with RIGHT), so "up" stays up for the reader.
#define NVS_NS_UI "ui"
#define NVS_KEY_UI_ROTATION "rot"
// Panel id (u8, see src/panel.h). Missing = 0, the GDEH0576T81.
#define NVS_KEY_UI_PANEL "panel"

// Status strip at the bottom of every screen. Same proportions as the earlier
// landscape build: the rule sits STATUS_STRIP_H px above the bottom edge and
// the status baseline STATUS_BASELINE_INSET px above that edge. On panel 0
// that is 920 - 24 = 896 and 920 - 8 = 912; on panel 1, 776 and 792.
static const int STATUS_STRIP_H = 24;
static const int STATUS_BASELINE_INSET = 8;
extern int STATUS_RULE_Y;      // SCREEN_H - STATUS_STRIP_H: 896 | 776
extern int STATUS_BASELINE_Y;  // SCREEN_H - STATUS_BASELINE_INSET: 912 | 792

// Content area: full width, everything above the status rule.
extern int CONTENT_W;  // SCREEN_W: 680 | 480
extern int CONTENT_H;  // STATUS_RULE_Y: 896 | 776

// Text grid (shared pagination contract with the host converter). These must
// match tools/pdf2book.py GRIDS exactly, or host pagination and on-glass
// rendering disagree.
//
// The margin, first baseline, pitch, advance and descender are properties of
// the font and stay constant; columns and rows are computed from the canvas
// by panel::applyGeometry():
//   cols = (SCREEN_W - 2*TEXT_MARGIN_X) / advance
//   rows = (STATUS_RULE_Y - 1 - descent - firstBaseline) / pitch + 1
// which is the most lines whose last descender ends at least one pixel above
// the status rule.
//
// Font 0, u8g2_font_profont22_mf, 12 px advance, 4 px descender. Panel 0:
// usable width 680 - 2*16 = 648 px, 648/12 = 54 columns exactly. First
// baseline 26, pitch 24: baseline n is 26 + (n-1)*24, and (896 - 1 - 4 - 26)
// / 24 = 36 (865/24), so 37 rows: the 37th baseline is 26 + 36*24 = 890 and
// its descender ends at 894, just clear of the rule at 896. A 38th would land
// at 914, past the rule. Panel 1: 448/12 = 37 columns, (776 - 1 - 4 - 26)/24
// = 31 (745/24), so 32 rows.
extern int TEXT_COLS;  // 54 | 37
extern int TEXT_ROWS;  // 37 | 32
static const int TEXT_MARGIN_X = 16;
static const int TEXT_FIRST_BASELINE_Y = 26;
static const int TEXT_LINE_PITCH = 24;
static const int TEXT_ADVANCE = 12;
static const int TEXT_DESCENT = 4;

// Font 1, u8g2_font_profont29_mf, 16 px advance, 5 px descender. Panel 0:
// 648/16 = 40.5, floored to 40 columns (8 px of unused width at the right
// edge). First baseline 32 keeps the same ~8 px of ink margin at the top;
// pitch 32 puts baseline n at 32*n, and (896 - 1 - 5 - 32)/32 = 26 (858/32),
// so 27 rows: the 27th is 864 and its descender ends at 869, clear of 896. A
// 28th baseline would land at 896, on the rule itself. Panel 1: 448/16 = 28
// columns, (776 - 1 - 5 - 32)/32 = 23 (738/32), so 24 rows.
extern int TEXT_LARGE_COLS;  // 40 | 28
extern int TEXT_LARGE_ROWS;  // 27 | 24
static const int TEXT_LARGE_FIRST_BASELINE_Y = 32;
static const int TEXT_LARGE_LINE_PITCH = 32;
static const int TEXT_LARGE_ADVANCE = 16;
static const int TEXT_LARGE_DESCENT = 5;

// Font ids as stored in an MPG2 variant header.
static const uint8_t BOOK_FONT_PROFONT22 = 0;
static const uint8_t BOOK_FONT_PROFONT29 = 1;

// Sensor.
static const uint8_t BME280_ADDR_PRIMARY = 0x76;
static const uint8_t BME280_ADDR_SECONDARY = 0x77;

// MPG2 books hold the same text paginated on more than one grid. Two is all
// the converter emits today (normal and large), so cap the parser well above
// that rather than trusting the file.
static const uint8_t MPG2_MAX_VARIANTS = 4;

// MPG1 image pages. A page blob starting with 0x01 'I' 'M' 'G' is a bitmap,
// not UTF-8 text. Header: magic(4), u16 width, u16 height, u8 flags, u8
// reserved, then ceil(width/8)*height raster bytes, rows top to bottom, MSB
// first inside each byte, bit 1 = black.
static const uint32_t MPG1_IMAGE_HEADER_LEN = 10;
static const uint8_t MPG1_IMAGE_FLAG_DOUBLE = 0x01;  // draw each pixel as 2x2
// A full page bitmap is the whole portrait content area, 680x896 on panel 0:
// the row stride is ceil(680/8) = 85 bytes, so 85*896 = 76160 bytes. Anything
// larger than this cannot be a page of ours, so reject it rather than
// allocate. The converter emits half resolution figures with
// MPG1_IMAGE_FLAG_DOUBLE by default (340x448, stride 43, 19264 bytes), so this
// ceiling only bites on --full-res books. A book made for one panel can be
// read on another (an oversize page is scaled down to fit at draw time), so
// the ceiling is the largest content bitmap of ANY panel in the table, not the
// active one, rounded up to the next 1000 bytes for headroom: 77000 today
// (panel 1's 60*776 = 46560 is smaller). Filled by panel::applyGeometry().
extern uint32_t MPG1_MAX_IMAGE_BYTES;

// Filesystem layout.
#define BOOKS_DIR "/books"
#define CURRENT_FILE "/current.txt"

// Jump-to-page step sizes.
static const int JUMP_STEP_SMALL = 1;
static const int JUMP_STEP_LARGE = 10;
// Jump view hold to repeat: a pad held this long keeps applying its own
// step at this cadence. The cadence leaves room for a
// partial refresh between steps.
static const uint32_t JUMP_HOLD_MS = 600;
static const uint32_t JUMP_REPEAT_MS = 450;

// ---------------------------------------------------------------- networking
//
// The radio is off in normal reading. It comes up only for "Sync news" and
// "WiFi setup", and both put it back down before returning.

// NVS namespaces and keys. NVS survives a `pio run -t uploadfs`, which
// rewrites the whole LittleFS partition, so credentials and the sync state
// live here and not in a file.
//   namespace "wifi": "n" (u8 count), "ssid0".."ssid3", "pw0".."pw3"
//   namespace "news": "etag", "stamp", "etag2", "stamp2", "auto"
// The unsuffixed pair belongs to feed 0 (the news brief) and is spelled without
// an index for the sake of the readers already in the field: renaming it would
// throw away a stored ETag and cost one needless download. Feed N > 0 uses
// "etag<N+1>" / "stamp<N+1>". NVS keys are capped at 15 characters.
#define NVS_NS_WIFI "wifi"
#define NVS_KEY_WIFI_COUNT "n"
#define NVS_NS_NEWS "news"
#define NVS_KEY_NEWS_ETAG "etag"
#define NVS_KEY_NEWS_STAMP "stamp"
#define NVS_KEY_NEWS_AUTO "auto"
#define NVS_KEY_METRICS_ETAG "etag2"
#define NVS_KEY_METRICS_STAMP "stamp2"

// Credential store depth. Bumping this needs no other change; the keys are
// generated from the index.
static const uint8_t WIFI_MAX_NETWORKS = 4;

// Per network association budget in wifi_connect().
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// Captive portal AP. Open on purpose: it is only up while the reader is in
// setup and the user is standing in front of it, it serves exactly one page,
// and the one secret it carries (the home WiFi password the user types) is
// typed on a link that is torn down seconds later. Set a passphrase of 8
// characters or more here to make it WPA2 instead.
#define WIFI_SETUP_AP_SSID "ereader-setup"
#define WIFI_SETUP_AP_PASSWORD ""
#define WIFI_SETUP_AP_IP_TEXT "192.168.4.1"

// Games server (src/games.*): the reader serves NES and Game Boy ROMs plus a
// browser emulator page, and stores the save files, so a laptop or a phone
// can play and pick up where the other left off. It joins the stored WiFi
// like a sync does and also raises its own AP, so it works away from home.
// The AP is WPA2 because the page accepts uploads; anyone on it can write to
// the ROM folder. The server shuts down after GAMES_IDLE_TIMEOUT_MS with no
// request (the page pings every minute while a game is loaded) or on CENTER.
#define GAMES_DIR "/games"
#define GAMES_ROM_DIR "/games/roms"
#define GAMES_SAVE_DIR "/games/saves"
// Auxiliary assets the reader itself draws with, not anything the page plays.
// Today that is the Pokemon companion screen's EPK1 pack (src/pack.*), built
// on a host by tools/pokered_pack.py and uploaded through the games page's
// file picker like a ROM.
#define GAMES_AUX_DIR "/games/aux"
#define GAMES_PACK_PATH GAMES_AUX_DIR "/pokered.pack"
#define GAMES_AP_SSID "ereader-games"
#define GAMES_AP_PASSWORD "ereader1"
#define GAMES_AP_IP_TEXT "192.168.4.1"
#define GAMES_MDNS_HOST "ereader"  // http://ereader.local/
static const uint32_t GAMES_IDLE_TIMEOUT_MS = 15UL * 60UL * 1000UL;
static const uint32_t GAMES_MAX_SAVE_BYTES = 1024UL * 1024UL;
static const uint32_t GAMES_MAX_ROM_BYTES = 2048UL * 1024UL;
static const int GAMES_NAME_MAX = 40;
static const uint32_t GAMES_SCREEN_REDRAW_MS = 10000;
// Companion screen: redraw this soon after a changed snapshot, and force a
// full refresh at least this often so partial ghosting never accumulates.
static const uint32_t GAMES_AUX_REDRAW_MS = 300;
static const uint32_t GAMES_AUX_FULL_MS = 60000;
// Companion screen: the page posts the Game Boy work RAM (8 KB, GB 0xC000 up)
// to POST /api/wram whenever it changed, and src/pokemon_state.* decodes it.
static const uint32_t GAMES_MAX_AUX_BYTES = 3072UL * 1024UL;
static const size_t GAMES_WRAM_BYTES = 8192;

// Books over WiFi. The same server takes a plain text book (.txt, UTF-8) or a
// ready made book (.pgs from tools/pdf2book.py or tools/txt2book.py) from the
// page, and deletes books. A .txt is streamed to BOOK_UPLOAD_TMP_FILE and then
// paginated on the device with the news brief's paginator (src/text_paginate.*)
// on both text grids, so it becomes a dual size MPG2 book like a synced brief.
// Free space is checked before the first byte is written and again as bytes
// arrive, because a full LittleFS panics instead of failing a write (see
// news_sync.cpp). Pagination needs the raw text, the page blobs of both grids
// and the finished book on flash at the same time, so a .txt needs about
// BOOK_TXT_SPACE_FACTOR times its own size; a .pgs needs its own size. Either
// way BOOK_UPLOAD_MIN_FREE_BYTES must still be free afterwards.
#define BOOK_UPLOAD_TMP_FILE "/book.tmp"     // raw upload, deleted afterwards
#define BOOK_UPLOAD_BLOB_FILE "/book.blob"   // page blobs while assembling
// Where a new book is assembled and checked before it replaces anything. The
// leading dot keeps it out of the upload name space (a slug never starts with
// a dot) and books::list() skips it.
#define BOOK_UPLOAD_STAGE_SLUG ".upload"
static const uint32_t BOOK_UPLOAD_MIN_FREE_BYTES = 256UL * 1024UL;
static const uint32_t BOOK_TXT_SPACE_FACTOR = 5;
static const uint32_t BOOK_MAX_TXT_BYTES = 1024UL * 1024UL;
static const uint32_t BOOK_MAX_PGS_BYTES = 6144UL * 1024UL;
static const int BOOK_SLUG_MAX = 24;  // same as tools/pdf2book.py SLUG_MAX
static const int BOOK_TITLE_MAX = 60;

// Sync endpoints. NEWS_TOKEN and METRICS_TOKEN are the shared secrets in the
// query strings; fill them in before the first sync, they ship as placeholders.
// include/secrets.h defines NEWS_TOKEN and METRICS_TOKEN; it is gitignored,
// copy include/secrets.h.example. A fresh checkout has no secrets.h, and a
// secrets.h written before the metrics feed existed has no METRICS_TOKEN; a
// missing token should not stop the reader building. The placeholders make
// the feeds fail with an HTTP 401 instead, which is what it looks like anyway.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef NEWS_TOKEN
#define NEWS_TOKEN "CHANGE_ME"
#endif
#ifndef METRICS_TOKEN
#define METRICS_TOKEN "CHANGE_ME"
#endif
// A build with a placeholder token has no feed to talk to, so its menu leaves
// out everything that only exists for the feeds: "Sync feeds" and "Auto sync"
// unless at least one token is set, "Daily numbers" unless METRICS_TOKEN is
// set. Decided at compile time; a build with include/secrets.h is unchanged.
constexpr bool configStrEq(const char *a, const char *b) {
  return *a == *b && (*a == '\0' || configStrEq(a + 1, b + 1));
}
static constexpr bool NEWS_FEED_CONFIGURED = !configStrEq(NEWS_TOKEN, "CHANGE_ME");
static constexpr bool METRICS_FEED_CONFIGURED =
    !configStrEq(METRICS_TOKEN, "CHANGE_ME");
static constexpr bool FEEDS_CONFIGURED =
    NEWS_FEED_CONFIGURED || METRICS_FEED_CONFIGURED;

#define NEWS_URL "https://api.muonsortes.com/brief/latest.txt?k=" NEWS_TOKEN
#define METRICS_URL \
  "https://api.muonsortes.com/metrics/ereader.json?k=" METRICS_TOKEN

// Body ceiling. The brief is streamed to a LittleFS temp file, never held in
// RAM, but a runaway response should still not fill the books partition.
static const uint32_t NEWS_MAX_BYTES = 200UL * 1024UL;
// Free space a sync insists on before it downloads anything: the raw brief,
// its paginated variants and the numbers JSON together, with headroom for
// littlefs metadata. Below this the sync reports "storage full" instead of
// writing, because a full filesystem panics inside littlefs (see news_sync.cpp).
static const uint32_t NEWS_MIN_FREE_BYTES = 640UL * 1024UL;
static const uint32_t NEWS_HTTP_TIMEOUT_MS = 20000;

// The task watchdog window while a sync is running. HTTPClient::GET() blocks
// inside DNS, the TLS handshake and the wait for the first response byte with
// no way to feed the watchdog from this task, and on a slow AP that is easily
// longer than the normal 8 s WDT_TIMEOUT_MS: the reader then reboots mid
// download, comes back up, auto syncs, and reboots again. Widen the window for
// the duration of the sync (see news_sync.cpp) and put it back afterwards.
static const uint32_t NEWS_WDT_TIMEOUT_MS = 60000;

// Where a synced feed lands. The slug is what the library scan sees.
#define NEWS_SLUG "news"
#define NEWS_TITLE "News brief"
#define NEWS_TMP_FILE "/news.tmp"    // raw body, deleted after pagination
#define NEWS_BLOB_FILE "/news.blob"  // page blobs, deleted after assembly

// The daily numbers feed is JSON, not a book: the sync stores the body as is
// at NUMBERS_FILE and the native grid view (src/numbers_view.*) draws it. The
// slug survives so a numbers.pgs left behind by the earlier text version can
// be found and removed.
#define METRICS_SLUG "numbers"
#define METRICS_TITLE "Daily numbers"
#define METRICS_TMP_FILE "/numbers.tmp"
#define NUMBERS_FILE "/numbers.json"

// What a feed turns into on the device.
enum NewsFeedKind : uint8_t {
  NEWS_FEED_BOOK,     // plain text, paginated into /books/<slug>.pgs
  NEWS_FEED_NUMBERS,  // JSON, stored verbatim at NUMBERS_FILE
};

// The feed table. One sync session fetches every row in it over the same WiFi
// association, in this order. A BOOK row becomes its own book; a NUMBERS row
// becomes NUMBERS_FILE. Adding a feed is one entry here plus its NVS key pair
// above; news_sync.cpp walks the table and needs no change. Temp file names
// are per feed so a failure part way through one leaves the others alone.
struct NewsFeed {
  NewsFeedKind kind;
  const char *slug;      // library slug, so /books/<slug>.pgs
  const char *title;     // book title, with the sync stamp appended
  const char *url;       // endpoint, token already in the query
  const char *etagKey;   // NVS key in NVS_NS_NEWS holding the stored ETag
  const char *stampKey;  // NVS key holding "YYYY-MM-DD HH:MM" of the last sync
  const char *tmpFile;   // raw body while it downloads
  const char *blobFile;  // page blobs while a book is assembled (BOOK only)
  bool configured;       // token set at compile time; an unset feed is skipped
};

static const uint8_t NEWS_FEED_COUNT = 2;

static const NewsFeed NEWS_FEEDS[NEWS_FEED_COUNT] = {
    {NEWS_FEED_BOOK, NEWS_SLUG, NEWS_TITLE, NEWS_URL, NVS_KEY_NEWS_ETAG,
     NVS_KEY_NEWS_STAMP, NEWS_TMP_FILE, NEWS_BLOB_FILE, NEWS_FEED_CONFIGURED},
    {NEWS_FEED_NUMBERS, METRICS_SLUG, METRICS_TITLE, METRICS_URL,
     NVS_KEY_METRICS_ETAG, NVS_KEY_METRICS_STAMP, METRICS_TMP_FILE, nullptr,
     METRICS_FEED_CONFIGURED},
};

// The device paginates a synced brief on both grids and writes an MPG2 book,
// so the "Text size" menu item works on the news brief like any other book.
// Variant 0 is the profont22 grid, variant 1 the profont29 grid, in that
// order, which matches what tools/txt2book.py --dual writes.
static const uint8_t NEWS_VARIANT_COUNT = 2;

// The brief is short and read at arm's length, so it opens in the large text
// by default. A sync rewrites /books/news.pos to page 0 of this variant, since
// the old position points into text that is gone anyway.
static const uint8_t NEWS_DEFAULT_VARIANT = 1;

// TLS. The roots in src/ca_roots.h cover the current api.muonsortes.com chain
// (Google Trust Services) and Let's Encrypt. If a chain change ever breaks
// validation on the bench, flip this to 1 to fall back to an unauthenticated
// connection rather than losing the feature. Read the comment in
// src/news_sync.cpp before doing that.
#define NEWS_TLS_ALLOW_INSECURE_FALLBACK 1

// Certificate validity checks need a clock. There is no RTC and no NTP in the
// reader's normal life, so a sync asks SNTP once, briefly, after associating.
#define NET_SNTP_SERVER_1 "pool.ntp.org"
#define NET_SNTP_SERVER_2 "time.nist.gov"
static const uint32_t NET_SNTP_TIMEOUT_MS = 5000;
// Footer clock. The status strip shows local HH:MM once the clock has been
// set, which happens during a news sync (SNTP, UTC) and then survives on the
// ESP32's RTC for as long as the board keeps power. Offset from UTC in
// minutes: 420 = UTC+7, Indochina Time. There is no DST handling and none is
// needed at this offset.
static const int CLOCK_UTC_OFFSET_MINUTES = 420;

// Any epoch past 2025-01-01 means the clock has really been set.
static const uint32_t NET_TIME_SANE_EPOCH = 1735689600UL;

// Speed read (src/rsvp.*): RSVP, one word at a time flashed in one fixed spot
// on the landscape panel. Tuned on this panel in the rsvp_eink proof of
// concept: 40 MHz SPI, the partial LUT picked for a forced 40 C, and that
// waveform cut at 60 ms (RSVP_CUT_MS) give solid black words in ~205 ms a
// flash (54 ms SPI + 60 ms drive + ~90 ms controller re-init), a ~290 wpm
// ceiling. A 60 ms cut is the most ghosting judged fine to ship; 100 ms ghosts
// less at ~245 wpm. Normal reading keeps 10 MHz, the internal sensor and the
// full waveform; only the mode switches.
// All of this is tuned for panel 0. A panel whose row in src/panel.cpp says
// speed read is unsupported never gets the fast drive (ui::setSpeedDrive and
// ui::setSpeedCut do nothing there) and the menu should hide the mode.
// Words per flash is a setting (1-3, picker tile, NVS); 2 is the default.
// The cut is automatic: a flash is only cut when the time it stays on glass
// (interval x dwell) is shorter than RSVP_UNCUT_MIN_MS, the full uncut flash
// (54 ms SPI + 367 ms drive) plus margin. So slow settings, and the longer
// sentence-end holds at mid speeds, run the complete waveform.
static const int RSVP_WORDS_DEFAULT = 2;
static const uint32_t RSVP_UNCUT_MIN_MS = 450;
// Hold multipliers for a chunk that ends a sentence (or paragraph) and one
// that ends on a comma, semicolon, colon or dash (Spritz-style pacing).
static const float RSVP_DWELL_SENTENCE = 1.5f;
static const float RSVP_DWELL_CLAUSE = 1.2f;
static const int RSVP_WPM_DEFAULT = 250;
static const int RSVP_WPM_MIN = 100;
// Top speed by words per flash (index 1-3): about the panel ceiling at a 60 ms
// cut (~205 ms a flash) for 1 word, and kept readable for 2 and 3.
static const int RSVP_WPM_MAX_BY_WORDS[4] = {0, 300, 500, 600};
static const int RSVP_WPM_STEP = 25;
// Ghost clearing full refresh at the first sentence (or paragraph) end once
// this many partials have run since the last full, and unconditionally at
// RSVP_FULL_MAX so a run-on sentence cannot stretch the cycle. 2 words at
// 425 wpm ghosted noticeably by the end of a 45-flash paragraph cycle.
static const int RSVP_FULL_AFTER = 30;
static const int RSVP_FULL_MAX = 40;
// Chunks the paused "back" press steps back (a sentence or two).
static const int RSVP_REWIND_CHUNKS = 10;
static const uint32_t RSVP_SPI_HZ = 40000000;
static const int RSVP_LUT_TEMP = 40;
static const int RSVP_CUT_MS = 60;
// Words per minute, NVS namespace NVS_NS_UI.
#define NVS_KEY_RSVP_WPM "rsvpwpm"
// Words per flash, NVS namespace NVS_NS_UI.
#define NVS_KEY_RSVP_WORDS "rsvpwords"

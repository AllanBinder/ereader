#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "books.h"
#include "config.h"
#include "games.h"
#include "input.h"
#include "news_sync.h"
#include "numbers_view.h"
#include "panel.h"
#include "pins.h"
#include "rsvp.h"
#include "ui.h"
#include "wifi_check.h"
#include "wifi_setup.h"
#include "wifi_store.h"

namespace {

// How long the boot sync waits for the reader to claim the device before it
// starts. Lives here rather than in config.h so the guard is one file.
const uint32_t AUTO_SYNC_GRACE_MS = 8000;

// NVS key (namespace NVS_NS_NEWS, "news") holding the "a boot sync is running
// right now" flag. Keys are capped at 15 characters.
const char *NVS_KEY_BOOT_SYNC = "bootsync";

enum class AppState {
  READING,
  MENU,
  JUMP,
  EMPTY,
  NUMBERS,
  SPEED_PICK,
  SPEED_READ,
  SCREEN_PICK,
};

AppState state = AppState::EMPTY;

Book book;
// Reused across page turns so the raster vector keeps its capacity instead of
// reallocating up to 45 KB on every image page.
PageData pageData;
std::vector<BookEntry> library;
uint32_t currentPage = 0;

int menuCursor = 0;
uint32_t jumpTarget = 1;

// Cursor in the speed read book picker (one tile per library book).
int speedCursor = 0;

// Cursor in the "Screen" list (one tile per supported panel, then Back).
int screenCursor = 0;

// Daily numbers view. The data is loaded from /numbers.json every time the
// view is opened (it is a few KB) and dropped when it is left. The screen
// index is kept across visits so reopening lands where the reader left off.
numbers_view::Data numbersData;
bool numbersLoaded = false;
int numbersScreen = 0;
int numbersWindow = 0;  // time window index into numbersData.windows; == windows.size() is the chart window
int numbersChart = 0;   // metric index in the chart window

Adafruit_BME280 bme;
bool bmeOk = false;

// Lifetime count of watchdog-caused reboots, persisted to /wdt.cnt. Shown in
// the menu header when nonzero so crash frequency is visible without serial.
uint32_t wdtRebootCount = 0;

void logf(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

// Arms the ESP32 task watchdog on the Arduino loop task. The core already
// starts a TWDT during boot, so this reconfigures it to our timeout rather
// than trying to create a second one. panic = true so a hang reboots instead
// of only logging.
void wdtBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true);
#endif
  esp_task_wdt_add(nullptr);
}

// True when the previous boot ended in a watchdog reset. ESP_RST_TASK_WDT is
// the task watchdog; ESP_RST_WDT covers the interrupt and RTC watchdogs, which
// fire from the same class of hang.
bool wdtCausedReboot() {
  esp_reset_reason_t r = esp_reset_reason();
  return r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

// True when the previous boot ended in a brownout. The reader runs off a
// battery that is only plugged in briefly, and WiFi TX peaks near 500 mA, so a
// sync is exactly the workload that can drop the rail. A brownout reset is not
// a watchdog reset, so it needs its own skip.
bool brownoutCausedReboot() {
  return esp_reset_reason() == ESP_RST_BROWNOUT;
}

// The boot sync arms this flag in NVS before it starts and clears it as soon as
// the sync returns, whatever the outcome. Finding it still set at boot means
// the previous boot sync never returned: watchdog, brownout, panic, or a pulled
// plug. That is the general form of the reset-reason checks, and the one that
// actually breaks a boot loop.
bool bootSyncFlagSet() {
  Preferences p;
  if (!p.begin(NVS_NS_NEWS, true)) return false;
  bool v = p.getBool(NVS_KEY_BOOT_SYNC, false);
  p.end();
  return v;
}

void setBootSyncFlag(bool on) {
  Preferences p;
  if (!p.begin(NVS_NS_NEWS, false)) return;
  if (on) {
    p.putBool(NVS_KEY_BOOT_SYNC, true);
  } else {
    p.remove(NVS_KEY_BOOT_SYNC);
  }
  p.end();
}

// Set for the life of the boot when the boot sync was skipped by the guard, so
// the menu header can say so. Not persisted: the next boot either syncs or sets
// it again.
bool autoSyncSkipped = false;

void showEmptyLibrary(const String &reason) {
  state = AppState::EMPTY;
  std::vector<String> lines;
  // Lines stay within 37 columns, the narrowest text grid in the panel table
  // (the 7.5 inch panel), so the screen reads the same on every panel.
  lines.push_back("No book to read.");
  lines.push_back("");
  if (reason.length()) {
    lines.push_back(reason);
    lines.push_back("");
  }
  lines.push_back("To add one from a phone or a");
  lines.push_back("computer:");
  lines.push_back("");
  lines.push_back("  1. Press center for the menu.");
  lines.push_back("  2. Pick \"Books and games (WiFi)\".");
  lines.push_back("  3. Open the address it shows and");
  lines.push_back("     upload a .txt or .pgs book.");
  lines.push_back("");
  lines.push_back("WiFi setup in the same menu joins");
  lines.push_back("your network; without one, join");
  lines.push_back("the reader's own WiFi.");
  lines.push_back("");
  lines.push_back("Or convert on a computer with");
  lines.push_back("tools/ and run");
  lines.push_back("    pio run -e esp32s3 -t uploadfs");
  ui::renderMessage("muon reader", lines);
}

// "Flip screen": the rotation lives in NVS (NVS_NS_UI), 1 or 3, and the pad
// turns with the picture whenever it differs from DISPLAY_ROTATION. Nothing
// stored means DISPLAY_ROTATION, so a build that never flips is unchanged.
uint8_t storedRotation() {
  Preferences p;
  if (!p.begin(NVS_NS_UI, true)) return DISPLAY_ROTATION;
  uint8_t r = p.getUChar(NVS_KEY_UI_ROTATION, DISPLAY_ROTATION);
  p.end();
  return (r == 1 || r == 3) ? r : DISPLAY_ROTATION;
}

// Forgets the stored flip. Called whenever the panel choice changes: the new
// glass may sit in its case either way up, so it starts as built and the
// reader flips it from the menu if needed.
void clearStoredRotation() {
  Preferences p;
  if (!p.begin(NVS_NS_UI, false)) return;
  p.remove(NVS_KEY_UI_ROTATION);
  p.end();
}

// DISPLAY_ROTATION comes from the active panel's row in src/panel.cpp (set by
// ui::begin / ui::beginPanel), so read it here at call time, never cache it.
void applyRotation(uint8_t r) {
  ui::setRotation(r);
  input::setTurned(ui::rotation() != DISPLAY_ROTATION);
}

void flipScreen() {
  uint8_t next = (ui::rotation() == 1) ? 3 : 1;
  Preferences p;
  if (p.begin(NVS_NS_UI, false)) {
    p.putUChar(NVS_KEY_UI_ROTATION, next);
    p.end();
  }
  applyRotation(next);
  logf("flip screen: rotation %u, pad %s\n", (unsigned)next,
       input::turned() ? "turned" : "as wired");
}

void renderCurrentPage(bool forceFull = false) {
  if (!book.isOpen()) {
    showEmptyLibrary("");
    return;
  }
  if (!book.loadPage(currentPage, pageData)) {
    pageData.clear();
    pageData.text = "(this page could not be read)";
  }
  if (pageData.isImage) {
    ui::renderImagePage(book.title(), pageData.bits(), pageData.width,
                        pageData.height, pageData.flags, currentPage,
                        book.pageCount());
    return;
  }
  String text = pageData.text;
  if (!text.length()) text = "(this page is empty)";
  ui::renderPage(book.title(), text, book.fontId(), currentPage,
                 book.pageCount(), forceFull);
}

void setPage(uint32_t page, bool forceFull = false) {
  if (!book.isOpen()) return;
  if (page >= book.pageCount()) page = book.pageCount() - 1;
  currentPage = page;
  books::savePosition(book.slug(), currentPage, book.activeVariant());
  renderCurrentPage(forceFull);
}

// A book whose stored grid does not match the active panel (made on another
// panel, or by a converter run for another --panel) is re-paginated on the
// device the first time it is opened, with a status screen on glass while it
// runs. books::reformat() swaps the file in place and maps the saved reading
// position, so the book is reopened afterwards to pick up the new page table.
// If the reformat fails (storage full, unreadable text) the old book stays
// open on the wrong grid: lines clip, but the reader is not locked out.
// Every path that opens a book (boot, menu, speed read, library reload) goes
// through openBook(), so this is the single place it runs.
bool prepareBookForPanel(Book &b, const String &slug) {
  if (!b.needsReformat()) return true;
  String title = b.title().length() ? b.title() : slug;
  b.close();
  auto show = [&](uint32_t done, uint32_t total) {
    std::vector<String> lines;
    lines.push_back(title);
    lines.push_back("");
    if (total) {
      lines.push_back("page " + String((unsigned long)done) + " of " +
                      String((unsigned long)total));
    }
    ui::renderStatusScreen("Formatting for this screen", lines);
  };
  String err;
  bool ok = books::reformat(slug, show, &err);
  if (!ok) logf("reformat %s failed: %s\n", slug.c_str(), err.c_str());
  library = books::list();
  return b.open(slug);
}

bool openBook(const String &slug) {
  if (!book.open(slug)) {
    logf("book open failed: %s (%s)\n", slug.c_str(), book.errorText().c_str());
    return false;
  }
  if (!prepareBookForPanel(book, slug)) {
    logf("book reformat failed: %s\n", slug.c_str());
    book.close();
    return false;
  }
  uint8_t variant = 0;
  currentPage = books::loadPosition(slug, &variant);
  if (variant >= book.variantCount()) variant = 0;
  book.setVariant(variant);
  if (currentPage >= book.pageCount()) currentPage = 0;
  books::saveCurrentSlug(slug);
  return true;
}

// Menu header: live sensor readings, the watchdog reboot count when there is
// one, and the last news sync stamp when there is one.
String sensorHeader() {
  String s;
  if (!bmeOk) {
    s = "sensor n/a";
  } else {
    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0f;
    char buf[80];
    snprintf(buf, sizeof(buf), "%.1f C   %.1f %%   %.1f hPa", t, h, p);
    s = String(buf);
  }
  if (wdtRebootCount > 0) {
    s += "   wdt " + String((unsigned long)wdtRebootCount);
  }
  if (autoSyncSkipped) s += "   auto sync skipped";
  String stamp = news_sync::lastStamp();
  if (stamp.length()) s += "   synced " + stamp;
  return s;
}

// Re-reads every saved position so the menu shows current page numbers. The
// open book's page count can change with its variant, so take that from the
// book itself.
void refreshLibrarySaved() {
  for (size_t i = 0; i < library.size(); i++) {
    uint8_t variant = 0;
    library[i].savedPage = books::loadPosition(library[i].slug, &variant);
    library[i].savedVariant = variant;
    if (book.isOpen() && book.slug() == library[i].slug) {
      library[i].pageCount = book.pageCount();
    }
    if (library[i].savedPage >= library[i].pageCount) library[i].savedPage = 0;
  }
}

// The books and games server can add, replace and delete books, including the
// one that is open. Rescan, and reopen the open book from its file so a
// replaced book is not read through a stale page table; a deleted one closes.
void reloadLibraryAfterServer() {
  library = books::list();
  if (book.isOpen()) {
    String slug = book.slug();
    if (!openBook(slug)) {
      book.close();
      if (!library.empty()) openBook(library[0].slug);
    }
  } else if (!library.empty()) {
    openBook(library[0].slug);
  }
  refreshLibrarySaved();
}

// The menu is built as a list of rows carrying what to do, not just a label:
// the book list is variable length and the text size row only exists for a
// book with more than one variant, so hard coded indices do not survive.
enum class MenuAction {
  RESUME,
  SPEED_READ,
  OPEN_BOOK,
  NUMBERS,
  TEXT_SIZE,
  SYNC_NEWS,
  WIFI_SETUP,
  WIFI_CHECK,
  GAMES,
  AUTO_SYNC,
  JUMP,
  FLIP_SCREEN,
  SCREEN,
  BUTTON_TEST,
};

struct MenuRow {
  String label;
  String sub;     // small state line under the label on the tile, or empty
  MenuAction action;
  int bookIndex;  // index into `library`, or -1

  MenuRow(const String &l, MenuAction a, int idx)
      : label(l), action(a), bookIndex(idx) {}
  MenuRow(const String &l, const String &s, MenuAction a, int idx)
      : label(l), sub(s), action(a), bookIndex(idx) {}
};

bool hasTextSizeItem() {
  return book.isOpen() && book.variantCount() > 1;
}

std::vector<MenuRow> menuRows() {
  std::vector<MenuRow> rows;
  // Each row is a tile: a bold label plus a small state line. Book tiles carry
  // the saved position; action tiles carry their current state where there is
  // one. The tile renderer cuts anything too wide with "..".
  rows.push_back(MenuRow("Resume", book.isOpen() ? book.title() : String("no book"),
                         MenuAction::RESUME, -1));
  // Speed read is only offered on glass it has been tuned on (the panel
  // table's speedReadSupported); elsewhere the tile does not exist.
  if (panel::speedReadSupported()) {
    rows.push_back(MenuRow("Speed read", String(rsvp::words()) + " word" +
                               (rsvp::words() > 1 ? "s" : "") + ", " +
                               String(rsvp::wpm()) + " wpm",
                           MenuAction::SPEED_READ, -1));
  }
  for (size_t i = 0; i < library.size(); i++) {
    String pos = "p. " + String((unsigned long)(library[i].savedPage + 1)) +
                 "/" + String((unsigned long)library[i].pageCount);
    rows.push_back(MenuRow(library[i].title, pos, MenuAction::OPEN_BOOK, (int)i));
  }
  // The feed tiles only exist in a build with a token for their feed (see
  // FEEDS_CONFIGURED in config.h); a public build has nowhere to sync from.
  if (METRICS_FEED_CONFIGURED) {
    String stamp = news_sync::numbersStamp();
    rows.push_back(MenuRow(METRICS_TITLE,
                           stamp.length() ? "synced " + stamp : String("never synced"),
                           MenuAction::NUMBERS, -1));
  }
  if (hasTextSizeItem()) {
    String size = (book.fontId() == BOOK_FONT_PROFONT29) ? "Large" : "Normal";
    rows.push_back(MenuRow("Text size", size, MenuAction::TEXT_SIZE, -1));
  }
  if (FEEDS_CONFIGURED) {
    String stamp = news_sync::lastStamp();
    rows.push_back(MenuRow("Sync feeds",
                           stamp.length() ? "synced " + stamp : String("never synced"),
                           MenuAction::SYNC_NEWS, -1));
  }
  {
    size_t n = wifi_store::list().size();
    String nets = String((unsigned long)n) + (n == 1 ? " network" : " networks");
    rows.push_back(MenuRow("WiFi setup", nets, MenuAction::WIFI_SETUP, -1));
  }
  rows.push_back(MenuRow("Check WiFi", MenuAction::WIFI_CHECK, -1));
  {
    // The way books get onto a finished reader, so the name says so.
    size_t n = games::romCount();
    size_t nb = library.size();
    rows.push_back(MenuRow("Books and games (WiFi)",
                           String((unsigned long)nb) + (nb == 1 ? " book, " : " books, ") +
                               String((unsigned long)n) + (n == 1 ? " rom" : " roms"),
                           MenuAction::GAMES, -1));
  }
  if (FEEDS_CONFIGURED) {
    rows.push_back(MenuRow("Auto sync",
                           news_sync::autoSyncEnabled() ? "on" : "off",
                           MenuAction::AUTO_SYNC, -1));
  }
  rows.push_back(MenuRow("Jump to page",
                         book.isOpen() ? "page " + String((unsigned long)(currentPage + 1)) +
                                             "/" + String((unsigned long)book.pageCount())
                                       : String(""),
                         MenuAction::JUMP, -1));
  rows.push_back(MenuRow("Flip screen",
                         input::turned() ? "turned 180" : "as built",
                         MenuAction::FLIP_SCREEN, -1));
  rows.push_back(MenuRow("Screen", panel::shortName(panel::active()),
                         MenuAction::SCREEN, -1));
  rows.push_back(MenuRow("Button test", "pad diagnostics", MenuAction::BUTTON_TEST, -1));
  return rows;
}

// Rows to tiles for the renderer. The name survives from the one column list
// so every call site stays as it was.
std::vector<ui::MenuTile> labelsOf(const std::vector<MenuRow> &rows) {
  std::vector<ui::MenuTile> out;
  out.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); i++) {
    out.push_back(ui::MenuTile(rows[i].label, rows[i].sub));
  }
  return out;
}

// Switches text size and keeps the reading position: the new page is the one
// whose anchor is at or before the anchor of the page being read.
void toggleTextSize() {
  if (!hasTextSizeItem()) return;
  uint8_t next = (uint8_t)((book.activeVariant() + 1) % book.variantCount());
  uint32_t mapped = book.mapPage(currentPage, next);
  if (!book.setVariant(next)) return;
  currentPage = mapped;
  if (currentPage >= book.pageCount()) currentPage = 0;
  books::savePosition(book.slug(), currentPage, book.activeVariant());
  refreshLibrarySaved();
  logf("text size: variant %u, page %lu/%lu\n", (unsigned)book.activeVariant(),
       (unsigned long)(currentPage + 1), (unsigned long)book.pageCount());
  state = AppState::READING;
  renderCurrentPage(true);
}

void openMenu() {
  state = AppState::MENU;
  menuCursor = 0;
  refreshLibrarySaved();
  ui::renderMenu(sensorHeader(), labelsOf(menuRows()), menuCursor);
}

// One frame of the numbers view: the grid in the current window, or the
// chart when the window index sits one past the feed's windows.
void renderNumbers() {
  if (numbersWindow >= (int)numbersData.windows.size()) {
    numbers_view::renderChart(numbersData, numbersScreen, numbersChart);
  } else {
    numbers_view::render(numbersData, numbersScreen, numbersWindow);
  }
}

// Opens the daily numbers view on the screen it was last left at. Without a
// numbers file (never synced, or the sync failed) a status screen says so and
// CENTER is the only way out.
void openNumbers() {
  state = AppState::NUMBERS;
  numbersLoaded = false;
  if (!numbers_view::available()) {
    numbers_view::renderMissing("");
    return;
  }
  String err;
  if (!numbers_view::load(numbersData, &err)) {
    logf("numbers load failed: %s\n", err.c_str());
    numbers_view::renderMissing(err);
    return;
  }
  numbersLoaded = true;
  if (numbersScreen >= numbersData.screens()) numbersScreen = 0;
  if (numbersWindow > (int)numbersData.windows.size()) numbersWindow = 0;
  renderNumbers();
}

void leaveNumbers() {
  numbersData = numbers_view::Data();
  numbersLoaded = false;
  openMenu();
}

void handleNumbers(Button b) {
  if (!numbersLoaded) {
    if (b == BTN_CENTER) leaveNumbers();
    return;
  }
  const int total = numbersData.screens();
  // The feed's windows plus the chart window, which the right pad reaches
  // last and wraps back from to the first window.
  const int windows = (int)numbersData.windows.size() + 1;
  const bool charting = numbersWindow >= windows - 1;
  switch (b) {
    // DOWN = next project (in the chart window, next metric, spilling into
    // the next project), the founder's "right" (BTN_LEFT) = next time
    // window, both wrapping; UP and RIGHT are the reverse moves when present,
    // never required.
    case BTN_DOWN:
      if (charting && numbersChart + 1 <
                          numbers_view::metricsOnScreen(numbersData, numbersScreen)) {
        numbersChart++;
      } else {
        numbersScreen = (numbersScreen + 1) % total;
        numbersChart = 0;
      }
      renderNumbers();
      break;
    case BTN_UP:
      if (charting && numbersChart > 0) {
        numbersChart--;
      } else {
        numbersScreen = (numbersScreen + total - 1) % total;
        numbersChart =
            charting ? numbers_view::metricsOnScreen(numbersData, numbersScreen) - 1
                     : 0;
        if (numbersChart < 0) numbersChart = 0;
      }
      renderNumbers();
      break;
    case BTN_LEFT:
      numbersWindow = (numbersWindow + 1) % windows;
      if (numbersWindow == windows - 1) numbersChart = 0;
      renderNumbers();
      break;
    case BTN_RIGHT:
      numbersWindow = (numbersWindow + windows - 1) % windows;
      if (numbersWindow == windows - 1) numbersChart = 0;
      renderNumbers();
      break;
    case BTN_CENTER:
      leaveNumbers();
      break;
    default:
      break;
  }
}

// Waits for a button or a timeout, whichever comes first, feeding the
// watchdog. Used to hold a result screen up long enough to read, and to give
// the boot sync a grace window the reader can cancel. Returns true if a button
// ended the wait.
bool waitForButtonOr(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    Button b;
    if (input::poll(b)) return true;
    delay(BUTTON_POLL_MS);
  }
  return false;
}

// Runs a news sync with progress on the panel, then folds the result back into
// the library. Returns true if the brief on the device changed.
bool runNewsSync() {
  bool changed = false;
  auto progress = [](const String &line) {
    std::vector<String> lines;
    lines.push_back("Sync feeds");
    lines.push_back("");
    lines.push_back(line);
    ui::renderStatusScreen("feed sync", lines);
  };

  news_sync::Outcome out = news_sync::sync(progress);

  std::vector<String> lines;
  lines.push_back("Sync feeds");
  lines.push_back("");
  switch (out.result) {
    case news_sync::Result::OK:
      if (out.pages > 0) {
        lines.push_back(String((unsigned long)out.pages) + " pages");
      }
      if (out.numbers) lines.push_back("daily numbers updated");
      lines.push_back("done");
      changed = true;
      break;
    case news_sync::Result::NOT_MODIFIED:
      lines.push_back("up to date");
      break;
    case news_sync::Result::NO_NETWORKS:
      lines.push_back("error: no saved network");
      lines.push_back("");
      lines.push_back("Run WiFi setup first.");
      break;
    default:
      lines.push_back("error: " + out.message);
      break;
  }
  String stamp = news_sync::lastStamp();
  if (stamp.length()) {
    lines.push_back("");
    lines.push_back("last sync " + stamp);
  }
  ui::renderStatusScreen("feed sync", lines);
  waitForButtonOr(3000);

  if (changed) {
    // The brief was rewritten under us. Reopen it if it is the book on screen,
    // and rescan so the library entry picks up the new title and page count.
    if (book.isOpen() && book.slug() == NEWS_SLUG) {
      openBook(NEWS_SLUG);
    }
    library = books::list();
    refreshLibrarySaved();
  }
  return changed;
}

// Live pad state on glass, because the panel is the only diagnostic channel
// that works when the USB CDC console is not being read. Shows which lines are
// active right now plus the last few distinct combinations, so a single tap
// that pulls two GPIOs is visible as "L R" on one history line rather than as
// two separate events nobody can tell apart afterwards.
//
// Exits on its own once the pad has been left alone, since every button is
// under test and none of them can be spent on "done".
void runButtonTest() {
  const char *names[BTN_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "CENTER"};
  // With the screen flipped the pad is turned too, so each name reads the
  // GPIO that now means that direction.
  int gpios[BTN_COUNT];
  for (int i = 0; i < BTN_COUNT; i++) gpios[i] = input::gpioFor((Button)i);
  std::vector<String> history;
  uint8_t lastMask = 0;
  uint32_t lastActivity = millis();
  const uint32_t kIdleExitMs = 12000;

  auto maskText = [&](uint8_t m) {
    String s;
    for (int i = 0; i < BTN_COUNT; i++) {
      if (m & (1 << i)) {
        if (s.length()) s += " ";
        s += names[i];
      }
    }
    return s.length() ? s : String("(none)");
  };

  auto draw = [&](uint32_t secsLeft) {
    std::vector<String> lines;
    lines.push_back("Button test");
    lines.push_back("");
    // Within 37 columns, so it fits the narrowest panel's text grid.
    lines.push_back("Press each direction once. Every");
    lines.push_back("line that goes active is listed, so");
    lines.push_back("a press that pulls two pins shows");
    lines.push_back("both names on the same history row.");
    lines.push_back("");
    String pins = "gpio";
    for (int i = 0; i < BTN_COUNT; i++) {
      // Two lines: all five pairs do not fit 37 columns.
      if (i == 3) {
        lines.push_back(pins);
        pins = "    ";
      }
      pins += "  " + String(names[i]) + " " + String(gpios[i]);
    }
    lines.push_back(pins);
    lines.push_back("");
    lines.push_back("now:  " + maskText(input::liveMask()));
    lines.push_back("");
    lines.push_back("history, newest last:");
    for (size_t i = 0; i < history.size(); i++) {
      lines.push_back("  " + history[i]);
    }
    lines.push_back("");
    lines.push_back("Leaves this screen after " + String((unsigned long)secsLeft) +
                    "s");
    lines.push_back("with the pad untouched.");
    ui::renderStatusScreen("button test", lines);
  };

  draw(kIdleExitMs / 1000);

  while (true) {
    esp_task_wdt_reset();
    uint8_t m = input::liveMask();
    if (m != lastMask) {
      // Only presses are interesting; a release is the same combination going
      // away and would double every history entry.
      if (m != 0) {
        // Watch the pad for a moment before recording: a second line pulled by
        // the same press does not necessarily go active on the same
        // microsecond, and the redraw below is blocking, so anything not
        // gathered here is lost.
        uint32_t watchStart = millis();
        while (millis() - watchStart < 250) {
          esp_task_wdt_reset();
          m |= input::liveMask();
          delay(BUTTON_POLL_MS);
        }
        String entry = String((unsigned long)history.size() + 1) + ". " + maskText(m);
        if (history.empty() || history.back() != entry) history.push_back(entry);
        if (history.size() > 12) history.erase(history.begin());
      }
      lastMask = m;
      lastActivity = millis();
      draw(kIdleExitMs / 1000);
    }
    uint32_t idle = millis() - lastActivity;
    if (idle >= kIdleExitMs) break;
    delay(BUTTON_POLL_MS);
  }

  input::flush();
}

void enterJump() {
  if (!book.isOpen()) {
    openMenu();
    return;
  }
  state = AppState::JUMP;
  jumpTarget = currentPage + 1;
  ui::renderJump(book.title(), jumpTarget, book.pageCount());
}

void handleReading(Button b) {
  switch (b) {
    // Founder holds the device so LEFT is the natural "forward" tilt. With the
    // portrait UI the page also runs top to bottom, so DOWN reads as "further
    // on" and is a second next-page; UP is a second previous-page. The menu
    // still owns UP/DOWN for cursor movement, which is why this mapping lives
    // here and not in the shared handler.
    // 2026-09-10: only the GPIO 1 (BTN_LEFT) and GPIO 41 (BTN_DOWN) pads
    // survived the case build, so those two must cover both directions:
    // BTN_LEFT is the founder's "right" = forward, BTN_DOWN = back.
    case BTN_LEFT:
    case BTN_RIGHT:
      if (book.isOpen() && currentPage + 1 < book.pageCount()) {
        setPage(currentPage + 1);
      }
      break;
    case BTN_DOWN:
    case BTN_UP:
      if (book.isOpen() && currentPage > 0) setPage(currentPage - 1);
      break;
    case BTN_CENTER:
      openMenu();
      break;
    default:
      break;
  }
}

// Grid navigation, shared by the menu and the speed read book picker. The
// tiles are laid out row major in `cols` columns, so row = cursor / cols and
// column = cursor % cols. Only DOWN, LEFT (the founder's "right") and CENTER
// work on this unit; UP and RIGHT are the reverse moves and are never
// required. Returns true when `b` was a move (the caller redraws).
bool moveGridCursor(Button b, int &cursor, int count) {
  const int cols = ui::menuColumns();
  const int col = cursor % cols;
  switch (b) {
    case BTN_DOWN:
      // Next row in the same column, wrapping to the top of that column.
      cursor += cols;
      if (cursor >= count) cursor = (col < count) ? col : 0;
      return true;
    case BTN_UP: {
      // Previous row in the same column, wrapping to the last row that has
      // a tile in that column.
      cursor -= cols;
      if (cursor < 0) {
        int last = ((count - 1 - col) / cols) * cols + col;
        cursor = (last >= 0 && last < count) ? last : 0;
      }
      return true;
    }
    case BTN_LEFT:
      // Next column within the row; the last column wraps to the first
      // column of the same row (an odd tail row has only one column).
      if (col + 1 < cols && cursor + 1 < count) {
        cursor += 1;
      } else {
        cursor -= col;
      }
      return true;
    case BTN_RIGHT:
      // Previous column within the row, wrapping to the last tile of the row.
      if (col > 0) {
        cursor -= 1;
      } else {
        int last = cursor - col + cols - 1;
        if (last >= count) last = count - 1;
        cursor = last;
      }
      return true;
    default:
      return false;
  }
}

// ------------------------------------------------------------ speed read
//
// Menu tile -> book picker -> RSVP (src/rsvp.*) -> reading view. The picker
// is the menu grid with one tile per book, each carrying its saved position;
// picking one opens it exactly as its menu tile would and starts at the first
// word of the saved page. Picking the book already open starts at the page
// being read, mid page if speed read was last left on that same page.
// Two settings tiles follow the books: Words (1/2/3 per flash) and Speed
// (steps up by RSVP_WPM_STEP, wrapping); CENTER on one changes it in place.

std::vector<ui::MenuTile> speedTiles() {
  std::vector<ui::MenuTile> out;
  for (size_t i = 0; i < library.size(); i++) {
    out.push_back(ui::MenuTile(
        library[i].title, "p. " + String((unsigned long)(library[i].savedPage + 1)) +
                              "/" + String((unsigned long)library[i].pageCount)));
  }
  out.push_back(ui::MenuTile("Words", String(rsvp::words()) + " per flash"));
  out.push_back(ui::MenuTile("Speed", String(rsvp::wpm()) + " wpm"));
  return out;
}

void openSpeedPicker() {
  // No speed read on this panel: the menu tile is hidden, so this is only a
  // guard against any other way in.
  if (!panel::speedReadSupported()) {
    openMenu();
    return;
  }
  if (library.empty()) {
    showEmptyLibrary("");
    return;
  }
  state = AppState::SPEED_PICK;
  refreshLibrarySaved();
  // Start on the open book: the likeliest pick.
  speedCursor = 0;
  for (size_t i = 0; i < library.size(); i++) {
    if (book.isOpen() && library[i].slug == book.slug()) speedCursor = (int)i;
  }
  ui::renderMenu("Speed read", speedTiles(), speedCursor);
}

void startSpeedRead(int bookIndex) {
  if (bookIndex < 0 || bookIndex >= (int)library.size()) return;
  String slug = library[bookIndex].slug;
  uint16_t word = 0;
  if (book.isOpen() && book.slug() == slug) {
    word = rsvp::resumeWord(slug, book.activeVariant(), currentPage);
  } else if (!openBook(slug)) {
    showEmptyLibrary("Could not open " + slug);
    return;
  }
  state = AppState::SPEED_READ;
  // Presses latched while reading or in the menu are not meant for this.
  input::clearLatched();
  rsvp::enter(book, currentPage, word);
  currentPage = rsvp::page();
}

void leaveSpeedRead() {
  rsvp::leave();
  currentPage = rsvp::page();
  input::clearLatched();
  state = AppState::READING;
  renderCurrentPage(true);
}

void handleSpeedPicker(Button b) {
  int books = (int)library.size();
  if (books == 0) {
    openMenu();
    return;
  }
  int count = books + 2;  // + Words, Speed
  if (speedCursor >= count) speedCursor = count - 1;
  if (moveGridCursor(b, speedCursor, count)) {
    ui::renderMenu("Speed read", speedTiles(), speedCursor);
  } else if (b == BTN_CENTER) {
    if (speedCursor < books) {
      startSpeedRead(speedCursor);
      return;
    }
    if (speedCursor == books) rsvp::cycleWords();
    else rsvp::cycleWpm();
    ui::renderMenu("Speed read", speedTiles(), speedCursor);
  }
}

// ---------------------------------------------------------------- screen
//
// Menu tile "Screen" -> list of supported panels, drawn on the current one.
// One tile per panel (short name, the one in use marked) and a Back tile,
// because the pad has no back button and LEFT is a grid move. Picking the
// panel in use or Back returns to the menu; picking another saves it and
// reboots straight into it. If that glass then stays blank, the boot picker
// (hold center through reset) gets the reader out.

std::vector<ui::MenuTile> screenTiles() {
  std::vector<ui::MenuTile> out;
  for (uint8_t i = 0; i < panel::count(); i++) {
    out.push_back(ui::MenuTile(panel::shortName(i),
                               i == panel::active() ? "in use" : "restarts"));
  }
  out.push_back(ui::MenuTile("Back", "keep this screen"));
  return out;
}

void openScreenPicker() {
  state = AppState::SCREEN_PICK;
  screenCursor = panel::active();
  ui::renderMenu("Screen", screenTiles(), screenCursor);
}

void switchPanel(uint8_t id) {
  std::vector<String> lines;
  lines.push_back("Switching screen, restarting.");
  lines.push_back("");
  lines.push_back(panel::name(id));
  lines.push_back("");
  if (!panel::set(id)) {
    logf("screen: could not save panel %u\n", (unsigned)id);
    lines[0] = "Could not save the screen choice.";
    lines.push_back("Nothing changed.");
    ui::renderMessage("screen", lines);
    waitForButtonOr(3000);
    input::flush();
    openMenu();
    return;
  }
  clearStoredRotation();
  logf("screen: panel %u (%s) saved, restarting\n", (unsigned)id,
       panel::name(id));
  lines.push_back("Connect that panel if it is not on");
  lines.push_back("yet. If the screen stays blank,");
  lines.push_back("hold center while pressing reset");
  lines.push_back("to pick the screen again.");
  ui::renderMessage("screen", lines);
  // Let the message settle on glass (and the log line drain) before reset.
  uint32_t start = millis();
  while (millis() - start < 1500) {
    esp_task_wdt_reset();
    delay(BUTTON_POLL_MS);
  }
  ESP.restart();
}

void handleScreenPicker(Button b) {
  const int count = (int)panel::count() + 1;  // + Back
  if (screenCursor >= count) screenCursor = count - 1;
  if (moveGridCursor(b, screenCursor, count)) {
    ui::renderMenu("Screen", screenTiles(), screenCursor);
    return;
  }
  if (b != BTN_CENTER) return;
  if (screenCursor < (int)panel::count() &&
      (uint8_t)screenCursor != panel::active()) {
    switchPanel((uint8_t)screenCursor);
    return;
  }
  openMenu();
}

void handleMenu(Button b) {
  std::vector<MenuRow> rows = menuRows();
  int count = (int)rows.size();
  if (menuCursor >= count) menuCursor = count - 1;

  // There is no back button: the menu is left via the Resume tile.
  if (moveGridCursor(b, menuCursor, count)) {
    ui::renderMenu(sensorHeader(), labelsOf(rows), menuCursor);
    return;
  }

  switch (b) {
    case BTN_CENTER: {
      const MenuRow &row = rows[menuCursor];
      switch (row.action) {
        case MenuAction::RESUME:
          if (book.isOpen()) {
            state = AppState::READING;
            renderCurrentPage(true);
          } else {
            showEmptyLibrary("");
          }
          break;
        case MenuAction::SPEED_READ:
          openSpeedPicker();
          break;
        case MenuAction::NUMBERS:
          openNumbers();
          break;
        case MenuAction::OPEN_BOOK:
          if (row.bookIndex >= 0 && row.bookIndex < (int)library.size()) {
            String slug = library[row.bookIndex].slug;
            if (openBook(slug)) {
              state = AppState::READING;
              renderCurrentPage(true);
            } else {
              showEmptyLibrary("Could not open " + slug);
            }
          }
          break;
        case MenuAction::TEXT_SIZE:
          toggleTextSize();
          break;
        case MenuAction::SYNC_NEWS:
          runNewsSync();
          menuCursor = 0;
          ui::renderMenu(sensorHeader(), labelsOf(menuRows()), menuCursor);
          break;
        case MenuAction::WIFI_SETUP:
          wifi_setup::run();
          ui::renderMenu(sensorHeader(), labelsOf(menuRows()), menuCursor);
          break;
        case MenuAction::WIFI_CHECK:
          wifi_check::run();
          ui::renderMenu(sensorHeader(), labelsOf(menuRows()), menuCursor);
          break;
        case MenuAction::GAMES:
          games::run();
          reloadLibraryAfterServer();
          ui::renderMenu(sensorHeader(), labelsOf(menuRows()), menuCursor);
          break;
        case MenuAction::FLIP_SCREEN:
          flipScreen();
          ui::renderMenu(sensorHeader(), labelsOf(menuRows()), menuCursor, true);
          break;
        case MenuAction::SCREEN:
          openScreenPicker();
          break;
        case MenuAction::AUTO_SYNC:
          news_sync::setAutoSync(!news_sync::autoSyncEnabled());
          logf("auto sync: %s\n", news_sync::autoSyncEnabled() ? "on" : "off");
          ui::renderMenu(sensorHeader(), labelsOf(menuRows()), menuCursor, true);
          break;
        case MenuAction::JUMP:
          enterJump();
          break;
        case MenuAction::BUTTON_TEST:
          runButtonTest();
          ui::renderMenu(sensorHeader(), labelsOf(menuRows()), menuCursor);
          break;
      }
      break;
    }
    default:
      break;
  }
}

// Moves the jump target by `delta` and wraps at both ends: one step back from
// page 1 lands on the last page, one step past the end lands on page 1, so
// the end of a book is one press away instead of a few hundred.
void moveJump(long delta) {
  long maxPage = (long)book.pageCount();
  if (maxPage < 1) return;
  long target = (long)jumpTarget + delta;
  if (target < 1) target = maxPage;
  else if (target > maxPage) target = 1;
  jumpTarget = (uint32_t)target;
  ui::renderJump(book.title(), jumpTarget, book.pageCount());
}

// Which way a pad moves the jump target. The d-pad sits on the back of the
// unit and its silkscreen maps to the firmware names as: silk UP = BTN_LEFT,
// silk LEFT = BTN_DOWN, silk RIGHT = BTN_UP, silk DOWN = BTN_RIGHT (dead on
// this unit). So: silk up +1, silk right +10, silk left -10, and the dead
// silk down would be -1. A hold repeats the pad's own step (pumpJumpHold).
long jumpDeltaFor(Button b) {
  switch (b) {
    case BTN_LEFT: return JUMP_STEP_SMALL;
    case BTN_DOWN: return -JUMP_STEP_LARGE;
    case BTN_UP: return JUMP_STEP_LARGE;
    case BTN_RIGHT: return -JUMP_STEP_SMALL;
    default: return 0;
  }
}

void handleJump(Button b) {
  if (!book.isOpen()) {
    showEmptyLibrary("");
    return;
  }
  if (b == BTN_CENTER) {
    state = AppState::READING;
    setPage((uint32_t)(jumpTarget - 1), true);
    return;
  }
  long delta = jumpDeltaFor(b);
  if (delta) moveJump(delta);
}

// Hold to repeat in the jump view: a pad held longer than JUMP_HOLD_MS keeps
// applying its own step every JUMP_REPEAT_MS until it is released. Reads the live pin levels because the debounced
// event stream only reports edges.
uint32_t jumpHeldSince = 0;
uint32_t jumpLastRepeat = 0;
Button jumpHeldButton = BTN_COUNT;

void pumpJumpHold() {
  uint8_t live = input::liveMask();
  Button held = BTN_COUNT;
  for (int i = 0; i < BTN_COUNT; i++) {
    Button b = (Button)i;
    if ((live & (1 << i)) && jumpDeltaFor(b) != 0) {
      held = b;
      break;
    }
  }
  uint32_t now = millis();
  if (held == BTN_COUNT || held != jumpHeldButton) {
    jumpHeldButton = held;
    jumpHeldSince = now;
    jumpLastRepeat = now;
    return;
  }
  if (now - jumpHeldSince < JUMP_HOLD_MS) return;
  if (now - jumpLastRepeat < JUMP_REPEAT_MS) return;
  jumpLastRepeat = now;
  moveJump(jumpDeltaFor(held));
}

// ------------------------------------------------------- boot panel picker
//
// The way out of a blank screen (wrong panel chosen, or a 7.5 inch build on
// its first boot): hold center while pressing reset or plugging in. The
// reader then brings up each supported panel in turn and draws "Press center
// if you can read this" on it; a center press while that text is readable
// saves the panel and boot carries on with it, no reboot. Nothing pressed
// for PICKER_ROUNDS passes over the list means the stored panel stays.
//
// This runs before input::begin(), which samples every pad's idle level and
// would take a held center for idle. So the picker reads the raw CENTER pin
// itself (no rotation, no pad turn) and judges "pressed" against the four
// direction pins, which sit on the same module and share its polarity.

const uint32_t PICKER_HOLD_MS = 300;     // center held this long at boot
const uint32_t PICKER_WINDOW_MS = 8000;  // per panel, after its frame lands
const uint32_t PICKER_PRESS_MS = 50;     // a press held this long counts
const int PICKER_ROUNDS = 3;

int pickerIdleLevel = HIGH;

// The pad's idle level by majority of the direction pins. Some d-pad modules
// short to GND (idle HIGH under our pullup), some to VCC (idle LOW); a tie
// goes to HIGH, the pullup's own level.
void pickerSamplePad() {
  const int dirs[4] = {PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT, PIN_BTN_RIGHT};
  pinMode(PIN_BTN_CENTER, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) pinMode(dirs[i], INPUT_PULLUP);
  delay(5);
  int high = 0;
  for (int i = 0; i < 4; i++) high += digitalRead(dirs[i]) == HIGH ? 1 : 0;
  pickerIdleLevel = (high >= 2) ? HIGH : LOW;
}

bool pickerCenterDown() { return digitalRead(PIN_BTN_CENTER) != pickerIdleLevel; }

// True when center reads down for the whole of `ms`, feeding the watchdog.
bool pickerCenterHeldFor(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    if (!pickerCenterDown()) return false;
    delay(BUTTON_POLL_MS);
  }
  return true;
}

void pickerWaitRelease() {
  while (pickerCenterDown()) {
    esp_task_wdt_reset();
    delay(BUTTON_POLL_MS);
  }
}

// One picker frame on the live panel. Big type for the question, since the
// point is to tell readable from garbled at a glance.
void pickerDraw(uint8_t id, int round) {
  const int x = TEXT_MARGIN_X;
  const int w = SCREEN_W - 2 * TEXT_MARGIN_X;
  ui::frameBegin(true);
  do {
    ui::gfx().fillScreen(ui::INK_WHITE);
    ui::setInk(false);
    ui::setFont(ui::Font::LABEL);
    ui::printAt(x, 110, ui::fitText("Press center", w));
    ui::printAt(x, 150, ui::fitText("if you can", w));
    ui::printAt(x, 190, ui::fitText("read this", w));
    ui::gfx().fillRect(x, 214, ui::TILE_ACCENT_W, 2, ui::INK_BLACK);
    ui::setFont(ui::Font::STATUS_BOLD);
    ui::printAt(x, 260, ui::fitText(panel::name(id), w));
    ui::setFont(ui::Font::STATUS);
    ui::printAt(x, 284,
                ui::fitText("Screen " + String((unsigned)id + 1) + " of " +
                                String((unsigned)panel::count()) + ", round " +
                                String(round + 1) + " of " + String(PICKER_ROUNDS),
                            w));
    ui::printAt(x, 324, ui::fitText("Release center first if still held.", w));
    ui::printAt(x, 344,
                ui::fitText("The next screen type is tried in " +
                                String((unsigned long)(PICKER_WINDOW_MS / 1000)) +
                                " s.",
                            w));
    ui::drawStatusStrip("screen picker", "");
  } while (ui::frameNext());
  ui::frameEnd();
}

// Waits PICKER_WINDOW_MS for a fresh center press. A press only counts after
// center has been seen up during this window, so the hold that started the
// picker (or one carried over from the previous panel) never selects.
bool pickerWaitForPress() {
  bool armed = !pickerCenterDown();
  uint32_t start = millis();
  while (millis() - start < PICKER_WINDOW_MS) {
    esp_task_wdt_reset();
    if (!pickerCenterDown()) {
      armed = true;
    } else if (armed && pickerCenterHeldFor(PICKER_PRESS_MS)) {
      return true;
    }
    delay(BUTTON_POLL_MS);
  }
  return false;
}

// Called from setup() right after ui::begin(), before input::begin().
// Returns with the chosen (or stored) panel live and its geometry applied.
void runBootPanelPicker() {
  pickerSamplePad();
  if (!pickerCenterDown() || !pickerCenterHeldFor(PICKER_HOLD_MS)) return;

  const uint8_t stored = panel::active();
  logf("screen picker: center held at boot, cycling %u panels\n",
       (unsigned)panel::count());
  for (int round = 0; round < PICKER_ROUNDS; round++) {
    for (uint8_t id = 0; id < panel::count(); id++) {
      ui::beginPanel(id);
      if (panel::active() != id) continue;  // driver did not fit; skip it
      logf("screen picker: trying panel %u (%s)\n", (unsigned)id,
           panel::name(id));
      pickerDraw(id, round);
      if (!pickerWaitForPress()) continue;
      if (panel::set(id)) {
        if (id != stored) clearStoredRotation();
        logf("screen picker: panel %u saved\n", (unsigned)id);
      } else {
        logf("screen picker: panel %u in use, NVS write failed\n", (unsigned)id);
      }
      // input::begin() samples idle levels next, so center must be up.
      pickerWaitRelease();
      return;
    }
  }
  logf("screen picker: nothing picked, back to panel %u\n", (unsigned)stored);
  ui::beginPanel(stored);
  pickerWaitRelease();
}

}  // namespace

void setup() {
  // A power glitch in the hand wiring can hard-fault the MCU mid-read; the
  // panel keeps its image, so a crash looks like dead buttons. The watchdog
  // turns that into a self-recovering reboot (boot redraws the saved page).
  // Read the reset reason before arming, so the count below reflects the boot
  // we just came out of.
  bool cameFromWdt = wdtCausedReboot();
  wdtBegin();

  Serial.begin(115200);

  ui::begin();
  // Hold center through reset to pick the panel (see runBootPanelPicker).
  // Must stay ahead of input::begin().
  runBootPanelPicker();

  if (!LittleFS.begin()) {
    logf("LittleFS mount failed\n");
  }

  {
    File f = LittleFS.open("/wdt.cnt", "r");
    if (f) {
      wdtRebootCount = (uint32_t)f.readString().toInt();
      f.close();
    }
    if (cameFromWdt) {
      wdtRebootCount++;
      File w = LittleFS.open("/wdt.cnt", "w");
      if (w) {
        w.print((unsigned long)wdtRebootCount);
        w.close();
      }
      logf("watchdog reboot #%lu\n", (unsigned long)wdtRebootCount);
    }
  }

  Wire.begin(PIN_BME_SDA, PIN_BME_SCL);
  bmeOk = bme.begin(BME280_ADDR_PRIMARY, &Wire) ||
          bme.begin(BME280_ADDR_SECONDARY, &Wire);
  if (!bmeOk) logf("BME280 not found, menu will show sensor n/a\n");

  input::begin();
  applyRotation(storedRotation());
  rsvp::begin();

  // The daily numbers feed used to be paginated into a book; since the grid
  // view it is a JSON file, and whatever the old version left in /books is
  // dead weight the library scan skips. Clear it and never resume into it.
  LittleFS.remove(BOOKS_DIR "/" METRICS_SLUG ".pgs");
  LittleFS.remove(BOOKS_DIR "/" METRICS_SLUG ".pos");

  library = books::list();
  String slug = books::loadCurrentSlug();
  if (slug == METRICS_SLUG) slug = "";
  bool opened = false;
  if (slug.length()) opened = openBook(slug);
  if (!opened && !library.empty()) opened = openBook(library[0].slug);

  if (opened) {
    state = AppState::READING;
    // First frame after a cold boot is always a full refresh.
    renderCurrentPage(true);
  } else {
    showEmptyLibrary(slug.length() ? ("Missing book: " + slug) : String(""));
  }

  // Auto sync is off by default and is a menu toggle, not a timer: the reader
  // has no clock between boots, so "older than 12 hours" is not a question it
  // can answer. Boot to the page first, then sync, so the reading page is on
  // glass as fast as it ever was and the sync is what waits.
  //
  // The boot sync is guarded three ways, because a sync that kills the device
  // is a reboot loop the reader cannot get out of: the menu is only reachable
  // once the sync has finished.
  //
  //  1. The NVS "bootsync" flag is armed just before the sync and cleared the
  //     moment it returns. Still set at boot means the last boot sync never
  //     returned, for any reason at all: watchdog, brownout, panic, or the
  //     battery being unplugged mid-transfer. Clear it and skip this boot.
  //  2. A watchdog reset skips the sync (the flag covers this too, but the
  //     reason is cheaper to trust than a flash write that may not have
  //     landed).
  //  3. A brownout reset skips it as well. WiFi TX peaks near 500 mA and this
  //     reader runs off a battery, so the sync is what browns the rail out.
  //
  // One quiet boot breaks the loop either way, and the menu header says
  // "auto sync skipped" so the reader knows why the brief is stale.
  bool bootSyncDied = bootSyncFlagSet();
  if (bootSyncDied) {
    setBootSyncFlag(false);
    logf("previous boot sync did not return, skipping this boot\n");
  }
  bool guardBlocks = bootSyncDied || cameFromWdt || brownoutCausedReboot();
  if (guardBlocks) autoSyncSkipped = true;

  if (FEEDS_CONFIGURED && news_sync::autoSyncEnabled() &&
      !wifi_store::list().empty() && !guardBlocks) {
    // Grace window: the page is already on glass, so give the reader a few
    // seconds to take the device before the radio comes up. A press means they
    // wanted the page, not a sync.
    if (waitForButtonOr(AUTO_SYNC_GRACE_MS)) {
      autoSyncSkipped = true;
      logf("auto sync cancelled by button\n");
      input::flush();
      return;
    }
    setBootSyncFlag(true);
    bool changed = runNewsSync();
    setBootSyncFlag(false);
    if (state == AppState::READING) {
      renderCurrentPage(true);
    } else if (changed) {
      // Two feeds now land in the library; open the brief by slug rather
      // than whatever sorts first.
      library = books::list();
      if (!library.empty() &&
          (openBook(NEWS_FEEDS[0].slug) || openBook(library[0].slug))) {
        state = AppState::READING;
        renderCurrentPage(true);
      } else {
        showEmptyLibrary("");
      }
    } else {
      showEmptyLibrary(slug.length() ? ("Missing book: " + slug) : String(""));
    }
  }

  input::flush();
}

void loop() {
  esp_task_wdt_reset();
  if (state == AppState::JUMP) pumpJumpHold();
  if (state == AppState::SPEED_READ && rsvp::tick()) {
    // A flash holds the loop for ~420 ms. Drop what the debouncer half saw
    // meanwhile; a real press in that window is still in the pin latch below.
    currentPage = rsvp::page();
    input::flush();
  }
  Button b;
  // Speed read spends most of its time inside a refresh, so it also takes
  // presses the pin interrupt latched while the loop was busy.
  if (input::poll(b) ||
      (state == AppState::SPEED_READ && input::takeLatched(b))) {
    switch (state) {
      case AppState::READING:
        handleReading(b);
        break;
      case AppState::MENU:
        handleMenu(b);
        break;
      case AppState::JUMP:
        handleJump(b);
        break;
      case AppState::NUMBERS:
        handleNumbers(b);
        break;
      case AppState::SPEED_PICK:
        handleSpeedPicker(b);
        break;
      case AppState::SCREEN_PICK:
        handleScreenPicker(b);
        break;
      case AppState::SPEED_READ:
        if (rsvp::handle(b)) {
          leaveSpeedRead();
        } else {
          currentPage = rsvp::page();
        }
        break;
      case AppState::EMPTY:
        if (b == BTN_CENTER) {
          library = books::list();
          if (!library.empty() && openBook(library[0].slug)) {
            state = AppState::READING;
            renderCurrentPage(true);
          } else {
            openMenu();
          }
        }
        break;
    }
    // Every branch above may have spent half a second or more inside a panel
    // refresh with nobody polling. Discard whatever piled up in that window so
    // the next page turn is one the reader actually saw.
    input::flush();
  }
  delay(BUTTON_POLL_MS);
}

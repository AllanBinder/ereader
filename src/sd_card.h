#pragma once

#include <Arduino.h>
#include <FS.h>

#include <functional>
#include <vector>

#include "books.h"

// microSD expansion module: plain text books on a FAT32 card.
//
// The card sits on its own SPI bus (SPI3, pins.h PIN_SD_*), never the panel's,
// so the two never wait on each other. A build without the module
// (PIN_SD_CS -1, the 5.76 inch reader) compiles this in and begin() simply
// returns false, as it does when the module is fitted but no card answers:
// the reader then runs exactly as without the feature, and says nothing.
//
// Books are .txt files (UTF-8) in /books at the card root. At boot every new
// or changed one is paginated on the device for the active panel with the
// WiFi upload's text book writer (news_sync::writeTextBookOn), card to card,
// and the result is kept in SD_CONVERTED_DIR for the next boot (layout and
// rules in include/config.h). Card books get slugs CARD_SLUG_PREFIX + the
// upload slug rule, and the rest of the firmware reads them through Book like
// any other book (books.cpp routes card slugs here).
//
// The card is only scanned at boot; a card put in later needs a restart.

namespace sd_card {

// True when this build has the module's pins (PIN_SD_CS >= 0).
bool fitted();

// Starts the card's SPI bus and mounts the card. False (quietly) when the
// build has no module, nothing answers, or the card is not FAT. Once only.
bool begin();

// True after a successful begin().
bool mounted();

// The card's filesystem (SD). Only meaningful while mounted().
fs::FS &fs();

// Progress for prepare(): the title being converted, its index from 0 and
// how many need converting. Only called when there is converting to do, so a
// boot with nothing new draws nothing.
using Progress = std::function<void(const String &title, int index, int total)>;

// Scans /books on the card, converts every .txt that has no up to date
// converted book, and remembers the card books that are ready. Blocking:
// seconds per book. Feeds the task watchdog as it goes. Returns the number of
// card books ready to read.
int prepare(Progress progress);

// The ready card books as library entries (onCard set), in card order. Empty
// with no card.
std::vector<BookEntry> list();

// Card books ready to read, and .txt files that were skipped (too big, or
// conversion failed).
int bookCount();
int skippedCount();

// Short card state for the menu header: "card: 3 books", "card: 3 books, 1
// skipped", or empty when no card is mounted.
String stateText();

// Where a card book's converted .pgs and its reading position live on the
// card. For books.cpp, by slug (CARD_SLUG_PREFIX + name).
String bookPath(const String &slug);
String posPath(const String &slug);

}  // namespace sd_card

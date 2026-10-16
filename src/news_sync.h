#pragma once

#include <Arduino.h>

#include <functional>

#include "config.h"

// Feed sync.
//
// Walks NEWS_FEEDS in include/config.h, which holds one row per endpoint: the
// news brief (slug "news", a plain text BOOK feed) and the daily numbers (slug
// "numbers", a JSON NUMBERS feed). All of them are fetched over a single WiFi
// association. A BOOK feed is paginated on the device with the same rules the
// host converter uses (src/text_paginate.cpp) and written to
// /books/<slug>.pgs, so it shows up in the library like any other book. A
// NUMBERS feed is stored verbatim at NUMBERS_FILE for src/numbers_view.* to
// draw. Every body is streamed to a LittleFS temp file, so nothing larger than
// one paragraph is ever in RAM: the 78 KB page buffer the panel needs leaves
// no room for a 200 KB download.
//
// NVS namespace "news", one key pair per feed (see config.h for the exact
// names, feed 0 carries the unsuffixed pair):
//   "etag"   String, the ETag of what is currently on the device. Sent back as
//            If-None-Match, so an unchanged feed costs one 304 and no download.
//   "stamp"  String, "YYYY-MM-DD HH:MM" (UTC) taken from the response's Date
//            header. Shown in the menu header and in the library entry.
//   "auto"   bool, sync once at boot. Default false. One flag for all feeds.

namespace news_sync {

enum class Result {
  OK,            // at least one feed downloaded and paginated
  NOT_MODIFIED,  // every feed said 304, what is on the device is current
  NO_NETWORKS,   // nothing in the credential store
  WIFI_FAILED,
  HTTP_FAILED,
  WRITE_FAILED,
};

struct Outcome {
  Result result = Result::HTTP_FAILED;
  String message;      // short reason, for "error: <reason>", slug prefixed
  uint32_t pages = 0;  // pages written, summed over the BOOK feeds that downloaded
  bool numbers = false;  // a NUMBERS feed downloaded, NUMBERS_FILE is new
  bool ok() const {
    return result == Result::OK || result == Result::NOT_MODIFIED;
  }
};

// Called with one short progress line at each step ("connecting", then
// "<feed title>: downloading", "<feed title>: 42 pages"). Never called after
// sync() returns.
using Progress = std::function<void(const String &line)>;

// Runs the whole sync: every feed, one WiFi session. OK if any feed
// downloaded, NOT_MODIFIED if all of them were unchanged, otherwise the first
// failure. A failing feed does not stop the ones after it.
//
// Blocking, tens of seconds worst case. It feeds the task watchdog wherever it
// owns the loop, and widens the watchdog window to NEWS_WDT_TIMEOUT_MS for the
// whole call to cover the blocking stretches inside HTTPClient (DNS, TLS
// handshake, first response byte) that nothing in this task can feed. The radio
// is off when this returns.
Outcome sync(Progress progress);

// "YYYY-MM-DD HH:MM" of the last successful news brief sync (feed 0), empty if
// it has never synced.
String lastStamp();

// Same for the daily numbers feed (NVS_KEY_METRICS_STAMP). Shown on the menu
// tile that opens the numbers view.
String numbersStamp();

bool autoSyncEnabled();
void setAutoSync(bool on);

// The paginator a BOOK feed runs through, for any plain text already on the
// filesystem (the games server uses it for a .txt uploaded from the browser).
// Reads UTF-8 `textFile` a chunk at a time, paginates it on every grid in
// NEWS_VARIANT_COUNT order (normal, then large) as the active panel has them
// at call time, and writes an MPG2 book with the grid extension (books.h) to
// BOOKS_DIR/<slug>.pgs, using `blobFile` as scratch. `pagesOut[v]` is the page
// count of variant v. Leaves `textFile` alone and the reading position alone.
// Needs roughly four times the text size free on the filesystem; checking
// that is the caller's job. Feeds the task watchdog as it goes, so the caller
// only has to make sure the watchdog window allows for the whole run.
bool writeTextBook(const char *textFile, const char *blobFile,
                   const String &slug, const String &title,
                   uint32_t pagesOut[NEWS_VARIANT_COUNT], String *err);

}  // namespace news_sync

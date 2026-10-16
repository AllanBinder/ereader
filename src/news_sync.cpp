#include "news_sync.h"

#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

#include <vector>

#include "books.h"
#include "ca_roots.h"
#include "config.h"
#include "net.h"
#include "text_paginate.h"
#include "wifi_store.h"


namespace {

// Anything beyond this is not a feed, it is a runaway response. The page
// table vector is the only per page RAM cost: 8 bytes per page per variant, so
// two variants at this cap is 48 KB of heap in the worst case. A 200 KB brief
// paginates to a couple of hundred pages on the larger grid, so this is two
// orders of magnitude of headroom.
const uint32_t MAX_PAGES = 3000;

// One page of one variant, as it is being built. The absolute file offset is
// only known once every page of every variant exists, because the MPG2 page
// tables sit ahead of the blob region.
struct PageMeta {
  uint32_t length;
  uint32_t anchor;
};

// A grid to paginate on, in the order the variants are written.
struct VariantSpec {
  uint8_t fontId;
  int cols;
  int rows;
};

// Font of each variant, in the order the variants are written.
const uint8_t kVariantFonts[NEWS_VARIANT_COUNT] = {BOOK_FONT_PROFONT22,
                                                   BOOK_FONT_PROFONT29};

// The grid of variant `v` on the active panel. Read at sync time, not copied
// at static init: TEXT_COLS and friends are filled by panel::applyGeometry()
// at boot, long after static initializers have run.
VariantSpec variantSpec(uint8_t v) {
  VariantSpec spec;
  spec.fontId = kVariantFonts[v];
  books::activeGrid(spec.fontId, spec.cols, spec.rows);
  return spec;
}

void feedWdt() { esp_task_wdt_reset(); }

// Sets the task watchdog timeout, keeping the panic-on-timeout behaviour
// wdtBegin() armed at boot.
void setWdtTimeout(uint32_t ms) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = ms;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(ms / 1000, true);
#endif
}

// Widens the watchdog window for as long as a sync is running, and puts the
// normal window back on every exit path. HTTPClient::GET() blocks inside DNS,
// the TLS handshake and the wait for the response with nothing this task can
// feed, so under the normal 8 s window a slow AP reboots the reader mid
// download instead of letting the request time out and report an error.
struct WdtWindow {
  WdtWindow() { setWdtTimeout(NEWS_WDT_TIMEOUT_MS); }
  ~WdtWindow() {
    esp_task_wdt_reset();
    setWdtTimeout(WDT_TIMEOUT_MS);
  }
};

String bookPath(const char *slug) {
  return String(BOOKS_DIR "/") + slug + ".pgs";
}
String posPath(const char *slug) {
  return String(BOOKS_DIR "/") + slug + ".pos";
}

// -------------------------------------------------------------- NVS helpers

String prefGetString(const char *key) {
  Preferences p;
  if (!p.begin(NVS_NS_NEWS, true)) return String();
  String v = p.getString(key, "");
  p.end();
  return v;
}

void prefPutString(const char *key, const String &value) {
  Preferences p;
  if (!p.begin(NVS_NS_NEWS, false)) return;
  p.putString(key, value);
  p.end();
}

// ------------------------------------------------------------ Date parsing

// "Wed, 21 Oct 2015 07:28:00 GMT" to "2015-10-21 07:28". Returns empty on any
// surprise: the stamp is cosmetic, so a parse failure is not a sync failure.
String stampFromHttpDate(const String &date) {
  int comma = date.indexOf(',');
  String rest = (comma >= 0) ? date.substring(comma + 1) : date;
  rest.trim();
  // day month year hh:mm:ss zone
  if (rest.length() < 20) return String();

  int day = rest.substring(0, 2).toInt();
  String mon = rest.substring(3, 6);
  int year = rest.substring(7, 11).toInt();
  int hh = rest.substring(12, 14).toInt();
  int mm = rest.substring(15, 17).toInt();

  static const char *kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = 0;
  for (int i = 0; i < 12; i++) {
    if (mon == kMonths[i]) {
      month = i + 1;
      break;
    }
  }
  if (month == 0 || day < 1 || day > 31 || year < 2000 || year > 2199) {
    return String();
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", year, month, day, hh,
           mm);
  return String(buf);
}

// ------------------------------------------------------------- body reader

// Reads a response body off the raw socket. HTTPClient parses the headers but
// leaves the chunked framing in the stream, and its own writeToStream() gives
// us no place to feed the watchdog, so do the framing here.
class BodyReader {
 public:
  BodyReader(WiFiClient *stream, int contentLength, bool chunked)
      : s_(stream), remaining_(contentLength), chunked_(chunked) {}

  // Returns bytes read, 0 at end of body, negative on error or timeout.
  int read(uint8_t *buf, size_t want) {
    if (done_) return 0;

    if (chunked_) {
      if (chunkLeft_ == 0) {
        if (!nextChunk()) return done_ ? 0 : -1;
        if (done_) return 0;
      }
      if (want > (size_t)chunkLeft_) want = (size_t)chunkLeft_;
    } else if (remaining_ >= 0) {
      if (remaining_ == 0) {
        done_ = true;
        return 0;
      }
      if (want > (size_t)remaining_) want = (size_t)remaining_;
    }

    int got = readSome(buf, want);
    if (got <= 0) {
      // An identity body with no Content-Length ends when the peer closes.
      if (!chunked_ && remaining_ < 0 && !s_->connected() && !s_->available()) {
        done_ = true;
        return 0;
      }
      return got == 0 ? -1 : got;
    }
    if (chunked_) {
      chunkLeft_ -= got;
      if (chunkLeft_ == 0) skipCrlf();
    } else if (remaining_ > 0) {
      remaining_ -= got;
    }
    return got;
  }

 private:
  int readSome(uint8_t *buf, size_t want) {
    uint32_t start = millis();
    while (millis() - start < NEWS_HTTP_TIMEOUT_MS) {
      feedWdt();
      int avail = s_->available();
      if (avail > 0) {
        size_t n = (size_t)avail < want ? (size_t)avail : want;
        return s_->read(buf, n);
      }
      if (!s_->connected()) return 0;
      delay(5);
    }
    return -1;
  }

  String readLine() {
    String line;
    uint32_t start = millis();
    while (millis() - start < NEWS_HTTP_TIMEOUT_MS) {
      feedWdt();
      if (s_->available()) {
        char c = (char)s_->read();
        if (c == '\n') return line;
        if (c != '\r') line += c;
        if (line.length() > 32) return line;  // not a chunk header
        continue;
      }
      if (!s_->connected()) break;
      delay(2);
    }
    return line;
  }

  bool nextChunk() {
    String line = readLine();
    line.trim();
    int semi = line.indexOf(';');
    if (semi >= 0) line = line.substring(0, semi);
    if (!line.length()) return false;
    long size = strtol(line.c_str(), nullptr, 16);
    if (size < 0) return false;
    if (size == 0) {
      done_ = true;
      return true;
    }
    chunkLeft_ = size;
    return true;
  }

  void skipCrlf() {
    readLine();
  }

  WiFiClient *s_;
  int remaining_;
  bool chunked_;
  long chunkLeft_ = 0;
  bool done_ = false;
};

// ------------------------------------------------------------- book writer

// Paginates the temp file on one grid, appending each finished page blob to
// the already open blob file and its length and anchor to `meta`.
bool paginateVariant(const VariantSpec &spec, const char *tmpFile,
                     fs::File &blobs, std::vector<PageMeta> &meta, String *err) {
  bool sinkFailed = false;
  textpage::Pipeline pipeline(
      spec.cols, spec.rows,
      [&](const std::vector<std::string> &lines) {
        if (sinkFailed || meta.size() >= MAX_PAGES) {
          sinkFailed = true;
          return;
        }
        std::string joined;
        for (size_t i = 0; i < lines.size(); i++) {
          if (i) joined.push_back('\n');
          joined += lines[i];
        }
        std::string utf8 = textpage::encodeUtf8(joined);
        size_t wrote = blobs.write((const uint8_t *)utf8.data(), utf8.size());
        if (wrote != utf8.size()) sinkFailed = true;
        PageMeta page;
        page.length = (uint32_t)utf8.size();
        page.anchor = pipeline.pageAnchor();
        meta.push_back(page);
        feedWdt();
      });

  fs::File src = LittleFS.open(tmpFile, "r");
  if (!src) {
    *err = "temp missing";
    return false;
  }
  uint8_t buf[512];
  while (true) {
    int got = src.read(buf, sizeof(buf));
    if (got <= 0) break;
    pipeline.feed(buf, (size_t)got);
    feedWdt();
    if (sinkFailed) break;
  }
  src.close();
  if (!sinkFailed) pipeline.finish();

  if (sinkFailed) {
    *err = "too many pages";
    return false;
  }
  // A feed with no text at all paginates to a single zero length page, and
  // the MPG2 parser rejects a zero length page table entry. Call it what it
  // is instead of writing a book that will not open.
  bool anyText = false;
  for (size_t i = 0; i < meta.size(); i++) {
    if (meta[i].length > 0) anyText = true;
  }
  if (!anyText) {
    *err = "no text";
    return false;
  }
  return true;
}

// Paginates the text file on every grid and writes BOOKS_DIR/<slug>.pgs as an
// MPG2 book, so the "Text size" menu item works on it like on a host built
// dual size book. Page blobs go to a second temp file first, because the MPG2
// page tables sit ahead of the blob region and their size is only known once
// every page of every variant exists. Public as news_sync::writeTextBook().
bool writeTextBookImpl(const char *textFile, const char *blobFile,
                       const String &slug, const String &title,
                       uint32_t *pagesOut, String *err) {
  // A filesystem that has never had books uploaded to it has no /books, and
  // LittleFS will not create a parent directory on open().
  LittleFS.mkdir(BOOKS_DIR);
  LittleFS.remove(blobFile);
  fs::File blobs = LittleFS.open(blobFile, "w");
  if (!blobs) {
    *err = "blob open";
    return false;
  }

  // Blobs are appended variant by variant in this order, and the offsets
  // written below walk the tables in the same order, so the two agree.
  std::vector<PageMeta> meta[NEWS_VARIANT_COUNT];
  VariantSpec specs[NEWS_VARIANT_COUNT];
  for (uint8_t v = 0; v < NEWS_VARIANT_COUNT; v++) specs[v] = variantSpec(v);
  for (uint8_t v = 0; v < NEWS_VARIANT_COUNT; v++) {
    if (!paginateVariant(specs[v], textFile, blobs, meta[v], err)) {
      blobs.close();
      LittleFS.remove(blobFile);
      return false;
    }
  }
  blobs.close();

  // MPG2 header: magic, u8 variant_count, u16 title_len, title, then per
  // variant u8 font_id, u32 page_count and page_count entries of
  // {u32 offset, u32 length, u32 anchor}, then the grid extension that
  // records which panel's grids these are (see books.h).
  std::string titleUtf8(title.c_str());
  uint32_t headerLen = 4 + 1 + 2 + (uint32_t)titleUtf8.size();
  for (uint8_t v = 0; v < NEWS_VARIANT_COUNT; v++) {
    headerLen += 1 + 4 + 12 * (uint32_t)meta[v].size();
  }
  headerLen += bookGridExtLen(NEWS_VARIANT_COUNT);

  fs::File out = LittleFS.open(bookPath(slug.c_str()).c_str(), "w");
  if (!out) {
    LittleFS.remove(blobFile);
    *err = "book open";
    return false;
  }

  auto putU32 = [&](uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                    (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)};
    out.write(b, 4);
  };

  out.write((const uint8_t *)"MPG2", 4);
  uint8_t vc = NEWS_VARIANT_COUNT;
  out.write(&vc, 1);
  uint16_t tl = (uint16_t)titleUtf8.size();
  uint8_t tb[2] = {(uint8_t)(tl & 0xFF), (uint8_t)((tl >> 8) & 0xFF)};
  out.write(tb, 2);
  out.write((const uint8_t *)titleUtf8.data(), titleUtf8.size());

  uint32_t cursor = headerLen;
  for (uint8_t v = 0; v < NEWS_VARIANT_COUNT; v++) {
    uint8_t fontId = specs[v].fontId;
    out.write(&fontId, 1);
    putU32((uint32_t)meta[v].size());
    for (size_t i = 0; i < meta[v].size(); i++) {
      putU32(cursor);
      putU32(meta[v][i].length);
      putU32(meta[v][i].anchor);
      cursor += meta[v][i].length;
    }
    feedWdt();
  }
  {
    uint8_t grid[2 * NEWS_VARIANT_COUNT];
    for (uint8_t v = 0; v < NEWS_VARIANT_COUNT; v++) {
      grid[2 * v] = (uint8_t)specs[v].cols;
      grid[2 * v + 1] = (uint8_t)specs[v].rows;
    }
    books::writeGridExt(out, NEWS_VARIANT_COUNT, grid);
  }

  uint8_t buf[512];
  fs::File blobIn = LittleFS.open(blobFile, "r");
  if (!blobIn) {
    out.close();
    *err = "blob reopen";
    return false;
  }
  while (true) {
    int got = blobIn.read(buf, sizeof(buf));
    if (got <= 0) break;
    if ((int)out.write(buf, (size_t)got) != got) {
      blobIn.close();
      out.close();
      LittleFS.remove(blobFile);
      *err = "book write";
      return false;
    }
    feedWdt();
  }
  blobIn.close();
  out.close();
  LittleFS.remove(blobFile);

  for (uint8_t v = 0; v < NEWS_VARIANT_COUNT; v++) {
    pagesOut[v] = (uint32_t)meta[v].size();
  }
  return true;
}

// The feed's temp file to /books/<slug>.pgs. `pagesOut` reports the page count
// of NEWS_DEFAULT_VARIANT, which is the variant a synced feed opens in.
bool paginateToBook(const NewsFeed &feed, const String &title,
                    uint32_t *pagesOut, String *err) {
  uint32_t pages[NEWS_VARIANT_COUNT] = {0};
  if (!writeTextBookImpl(feed.tmpFile, feed.blobFile, String(feed.slug), title,
                         pages, err)) {
    return false;
  }

  // A fresh feed opens at page 1: the old position points into text that no
  // longer exists. Write the position rather than deleting it, because that is
  // also what selects the large text variant the brief opens in. A missing
  // .pos would read back as variant 0.
  LittleFS.remove(posPath(feed.slug).c_str());
  books::savePosition(String(feed.slug), 0, NEWS_DEFAULT_VARIANT);

  *pagesOut = pages[NEWS_DEFAULT_VARIANT];
  return true;
}

// ----------------------------------------------------------------- request

// Runs the GET. `insecure` skips certificate validation, which is the
// documented fallback. Returns the HTTP status code, or a negative HTTPClient
// error.
int doRequest(HTTPClient &http, WiFiClientSecure &client, bool insecure,
              const char *url, const String &etag, const char *accept) {
  if (insecure) {
    // LOUD COMMENT, read before enabling NEWS_TLS_ALLOW_INSECURE_FALLBACK:
    // setInsecure() turns off certificate validation completely. The
    // connection is still encrypted, but anything on the path can impersonate
    // api.muonsortes.com and hand the reader whatever text it likes, and the
    // NEWS_TOKEN in the query string is handed to that impostor. The payload
    // here is a public news brief and the token guards nothing but rate, so
    // the trade is acceptable as a fallback; it is not acceptable as the
    // normal path. If this branch is what makes syncing work, the real fix is
    // to update src/ca_roots.h with the root the endpoint actually chains to.
    client.setInsecure();
  } else {
    client.setCACert(NEWS_CA_ROOTS);
  }
  client.setTimeout(NEWS_HTTP_TIMEOUT_MS / 1000);

  http.setTimeout(NEWS_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(NEWS_HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return -1000;

  const char *wanted[] = {"ETag", "Date", "Transfer-Encoding"};
  http.collectHeaders(wanted, 3);
  http.addHeader("User-Agent", "muon-ereader/1");
  http.addHeader("Accept", accept);
  if (etag.length()) http.addHeader("If-None-Match", etag);
  return http.GET();
}

}  // namespace

namespace news_sync {

bool writeTextBook(const char *textFile, const char *blobFile,
                   const String &slug, const String &title,
                   uint32_t pagesOut[NEWS_VARIANT_COUNT], String *err) {
  return writeTextBookImpl(textFile, blobFile, slug, title, pagesOut, err);
}

String lastStamp() { return prefGetString(NVS_KEY_NEWS_STAMP); }

String numbersStamp() { return prefGetString(NVS_KEY_METRICS_STAMP); }

bool autoSyncEnabled() {
  Preferences p;
  if (!p.begin(NVS_NS_NEWS, true)) return false;
  bool v = p.getBool(NVS_KEY_NEWS_AUTO, false);
  p.end();
  return v;
}

void setAutoSync(bool on) {
  Preferences p;
  if (!p.begin(NVS_NS_NEWS, false)) return;
  p.putBool(NVS_KEY_NEWS_AUTO, on);
  p.end();
}

namespace {

// Downloads one feed: a BOOK feed is paginated into its own book, a NUMBERS
// feed is stored as is at NUMBERS_FILE. The radio is already
// associated and stays that way: every feed in a session shares one
// association, so nothing here touches wifi_connect() or wifi_off().
Outcome syncFeed(const NewsFeed &feed, const Progress &progress) {
  Outcome outcome;
  auto say = [&](const String &line) {
    if (progress) progress(String(feed.title) + ": " + line);
  };

  say("downloading");
  String etag = prefGetString(feed.etagKey);

  // The TLS handshake is the heaviest moment of a sync in both stack and heap.
  // Print what is left going in, so a future failure here can be told apart
  // from a network one without a debugger.
  Serial.printf("sync(%s): heap %u free, %u largest, loop stack %u free\n",
                feed.slug, (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  const char *accept =
      feed.kind == NEWS_FEED_NUMBERS ? "application/json" : "text/plain";
  WiFiClientSecure client;
  HTTPClient http;
  int code = doRequest(http, client, false, feed.url, etag, accept);

#if NEWS_TLS_ALLOW_INSECURE_FALLBACK
  if (code < 0) {
    // Connection level failure, which a rejected certificate looks like.
    http.end();
    client.stop();
    say("retrying (tls)");
    code = doRequest(http, client, true, feed.url, etag, accept);
  }
#endif

  if (code < 0) {
    http.end();
    outcome.result = Result::HTTP_FAILED;
    outcome.message = "connect " + String(code);
    return outcome;
  }

  if (code == HTTP_CODE_NOT_MODIFIED) {
    http.end();
    outcome.result = Result::NOT_MODIFIED;
    outcome.message = "up to date";
    say("up to date");
    return outcome;
  }

  if (code != HTTP_CODE_OK) {
    http.end();
    outcome.result = Result::HTTP_FAILED;
    outcome.message = "http " + String(code);
    return outcome;
  }

  Serial.printf("sync(%s): http %d, heap %u free, loop stack %u free\n",
                feed.slug, code, (unsigned)ESP.getFreeHeap(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  String newEtag = http.header("ETag");
  String date = http.header("Date");
  String te = http.header("Transfer-Encoding");
  te.toLowerCase();
  bool chunked = te.indexOf("chunked") >= 0;
  int contentLength = http.getSize();

  LittleFS.remove(feed.tmpFile);
  fs::File tmp = LittleFS.open(feed.tmpFile, "w");
  if (!tmp) {
    http.end();
    outcome.result = Result::WRITE_FAILED;
    outcome.message = "temp open";
    return outcome;
  }

  BodyReader body(http.getStreamPtr(), contentLength, chunked);
  uint8_t buf[1024];
  uint32_t total = 0;
  bool overflow = false;
  bool readError = false;
  while (true) {
    feedWdt();
    int got = body.read(buf, sizeof(buf));
    if (got == 0) break;
    if (got < 0) {
      readError = true;
      break;
    }
    if (total + (uint32_t)got > NEWS_MAX_BYTES) {
      overflow = true;
      break;
    }
    if ((int)tmp.write(buf, (size_t)got) != got) {
      readError = true;
      outcome.message = "temp write";
      break;
    }
    total += (uint32_t)got;
  }
  tmp.close();
  http.end();

  if (overflow) {
    LittleFS.remove(feed.tmpFile);
    outcome.result = Result::HTTP_FAILED;
    outcome.message = "too large";
    return outcome;
  }
  if (readError || total == 0) {
    LittleFS.remove(feed.tmpFile);
    outcome.result = Result::HTTP_FAILED;
    if (!outcome.message.length()) outcome.message = "download";
    return outcome;
  }

  String stamp = stampFromHttpDate(date);

  if (feed.kind == NEWS_FEED_NUMBERS) {
    // JSON for the grid view: the body is the file. Replace the old one in a
    // single rename so a reader opening the view mid sync sees either version
    // whole, and clear out the book the text version of this feed used to
    // write, which the library scan no longer lists but which still costs
    // filesystem space.
    LittleFS.remove(NUMBERS_FILE);
    if (!LittleFS.rename(feed.tmpFile, NUMBERS_FILE)) {
      LittleFS.remove(feed.tmpFile);
      outcome.result = Result::WRITE_FAILED;
      outcome.message = "numbers write";
      return outcome;
    }
    LittleFS.remove(bookPath(feed.slug).c_str());
    LittleFS.remove(posPath(feed.slug).c_str());

    if (newEtag.length()) prefPutString(feed.etagKey, newEtag);
    if (stamp.length()) prefPutString(feed.stampKey, stamp);

    say("saved");
    outcome.result = Result::OK;
    outcome.numbers = true;
    outcome.message = "done";
    return outcome;
  }

  String title = feed.title;
  if (stamp.length()) title += "  " + stamp;

  say("paginating");
  uint32_t pages = 0;
  String err;
  bool ok = paginateToBook(feed, title, &pages, &err);
  LittleFS.remove(feed.tmpFile);
  if (!ok) {
    outcome.result = Result::WRITE_FAILED;
    outcome.message = err;
    return outcome;
  }

  if (newEtag.length()) prefPutString(feed.etagKey, newEtag);
  if (stamp.length()) prefPutString(feed.stampKey, stamp);

  say(String(pages) + " pages");
  outcome.result = Result::OK;
  outcome.pages = pages;
  outcome.message = "done";
  return outcome;
}

}  // namespace

// One association, every feed in NEWS_FEEDS, in order. The aggregate result is
// OK if anything was downloaded (with the page counts summed and `numbers` set
// if the numbers feed was among them), NOT_MODIFIED if
// every feed answered 304, and otherwise the first failure, slug prefixed so
// the panel says which feed it was. A feed that fails does not stop the ones
// after it: a stale metrics page is no reason to go without the news.
Outcome sync(Progress progress) {
  WdtWindow wdtWindow;
  Outcome outcome;
  auto say = [&](const String &line) {
    if (progress) progress(line);
  };

  if (wifi_store::list().empty()) {
    outcome.result = Result::NO_NETWORKS;
    outcome.message = "no saved network";
    return outcome;
  }

  say("connecting");
  String ssid;
  if (!wifi_connect(&ssid)) {
    outcome.result = Result::WIFI_FAILED;
    outcome.message = "wifi";
    return outcome;
  }

  // A full filesystem must be caught here: the bundled littlefs (2.9.x, via
  // esp_littlefs 1.14.1) divides by cfg->block_count inside its own "No more
  // free space" error path, and esp_littlefs mounts with that field at zero
  // (block count comes from the superblock), so running out of space during
  // a write panics the reader instead of returning an error. Seen 2026-09-16
  // with the partition 100% full of ROMs.
  {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    size_t freeBytes = total > used ? total - used : 0;
    if (freeBytes < NEWS_MIN_FREE_BYTES) {
      wifi_off();
      outcome.result = Result::WRITE_FAILED;
      outcome.message = "storage full, " + String((unsigned long)(freeBytes / 1024)) +
                        "k free. Delete a game.";
      return outcome;
    }
  }

  // TLS needs a plausible clock or every certificate reads as "not yet valid".
  net_time_sync();

  bool downloaded = false;
  uint32_t pages = 0;
  Outcome failure;
  bool failed = false;

  for (uint8_t i = 0; i < NEWS_FEED_COUNT; i++) {
    const NewsFeed &feed = NEWS_FEEDS[i];
    // A placeholder token would only earn a 401; leave the feed out.
    if (!feed.configured) continue;
    Outcome one = syncFeed(feed, progress);
    if (one.result == Result::OK) {
      downloaded = true;
      pages += one.pages;
      if (one.numbers) outcome.numbers = true;
    } else if (one.result != Result::NOT_MODIFIED && !failed) {
      failure = one;
      failure.message = String(feed.slug) + ": " + one.message;
      failed = true;
    }
  }

  wifi_off();

  if (downloaded) {
    outcome.result = Result::OK;
    outcome.pages = pages;
    // A feed that failed while another one worked is still worth naming, and
    // the panel only prints the message on the error paths, so fold it into
    // the "done" line instead of losing it.
    outcome.message = failed ? "done, " + failure.message : "done";
    return outcome;
  }
  if (failed) return failure;

  outcome.result = Result::NOT_MODIFIED;
  outcome.message = "up to date";
  return outcome;
}

}  // namespace news_sync

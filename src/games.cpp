#include "games.h"

#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <uri/UriBraces.h>

#include <vector>

#include "books.h"
#include "config.h"
#include "gbgfx.h"
#include "input.h"
#include "net.h"
#include "news_sync.h"
#include "pack.h"
#include "pokemon_state.h"
#include "pokemon_views.h"
#include "ui.h"
#include "wifi_store.h"

// The page and its scripts are gzipped by tools/build_www.sh into www/dist and
// linked into the app image (board_build.embed_files in platformio.ini), so
// updating the player never touches the filesystem: an uploadfs would wipe
// the ROMs and saves along with everything else on the LittleFS partition.
extern const uint8_t www_index_start[] asm("_binary_www_dist_index_html_gz_start");
extern const uint8_t www_index_end[] asm("_binary_www_dist_index_html_gz_end");
extern const uint8_t www_app_start[] asm("_binary_www_dist_app_js_gz_start");
extern const uint8_t www_app_end[] asm("_binary_www_dist_app_js_gz_end");
extern const uint8_t www_nes_start[] asm("_binary_www_dist_nes_js_gz_start");
extern const uint8_t www_nes_end[] asm("_binary_www_dist_nes_js_gz_end");
extern const uint8_t www_gb_start[] asm("_binary_www_dist_gb_js_gz_start");
extern const uint8_t www_gb_end[] asm("_binary_www_dist_gb_js_gz_end");

namespace {

WebServer *server = nullptr;
uint32_t requests = 0;
uint32_t lastRequestMs = 0;
uint32_t startMs = 0;
String stationSsid;
bool mdnsUp = false;

// Upload in flight (multipart ROM) or raw save body in flight. The WebServer
// is single threaded, so one of each at a time is all there is.
fs::File uploadFile;
String uploadName;
bool uploadFailed = false;
String uploadError;
fs::File saveFile;
String savePath;
bool saveFailed = false;
size_t saveBytes = 0;
bool uploadIsAux = false;

// Free bytes on the filesystem when the ROM upload in flight started. The
// upload stops before it would leave less than BOOK_UPLOAD_MIN_FREE_BYTES,
// because a full LittleFS panics rather than failing the write.
size_t uploadFreeAtStart = 0;

// Book upload in flight (POST /api/books/upload, multipart). A .txt lands in
// BOOK_UPLOAD_TMP_FILE and is paginated by the completion handler; a .pgs lands
// under BOOK_UPLOAD_STAGE_SLUG and is parsed before it replaces anything.
fs::File bookFile;
String bookSlug;
String bookStem;
bool bookIsText = false;
bool bookFailed = false;
String bookError;
uint32_t bookBytes = 0;
uint32_t bookBudget = 0;

// Raw work RAM body in flight (POST /api/wram). Accumulated into the decoder's
// own buffer and only committed once the whole 8 KB arrived.
size_t wramBytes = 0;
bool wramFailed = false;

// View state for the companion screen. The views agent owns what these mean;
// the plumbing only records the presses so a view can read them.
int auxPage = 0;
bool auxTerrain = false;
uint32_t shownSnapshot = 0;

void feedWdt() { esp_task_wdt_reset(); }

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

// Streaming a 130 KB script to a phone on a weak link can take longer than
// the normal 8 s window, and handleClient() cannot feed the watchdog while it
// streams. Same trick as the news sync.
struct WdtWindow {
  WdtWindow() { setWdtTimeout(NEWS_WDT_TIMEOUT_MS); }
  ~WdtWindow() {
    esp_task_wdt_reset();
    setWdtTimeout(WDT_TIMEOUT_MS);
  }
};

void touch() {
  requests++;
  lastRequestMs = millis();
}

// ------------------------------------------------------------ name rules

bool safeChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

bool romExtension(const String &name) {
  return name.endsWith(".nes") || name.endsWith(".gb") || name.endsWith(".gbc");
}

// An EPK1 asset pack (src/pack.*). Not a ROM: it goes to GAMES_AUX_DIR and the
// page never sees it.
bool packExtension(const String &name) { return name.endsWith(".pack"); }

// Takes the basename of whatever the browser sent, maps every character
// outside [A-Za-z0-9._-] to '_', lowercases, and cuts to GAMES_NAME_MAX while
// keeping the extension. Returns an empty string when the extension is neither
// one the page can play nor an asset pack.
String cleanRomName(const String &raw) {
  String base = raw;
  int slash = base.lastIndexOf('/');
  if (slash >= 0) base = base.substring(slash + 1);
  slash = base.lastIndexOf('\\');
  if (slash >= 0) base = base.substring(slash + 1);
  String out;
  out.reserve(base.length());
  for (size_t i = 0; i < base.length(); i++) {
    char c = base[i];
    out += safeChar(c) ? c : '_';
  }
  out.toLowerCase();
  if (!romExtension(out) && !packExtension(out)) return String();
  int dot = out.lastIndexOf('.');
  String ext = out.substring(dot);
  String stem = out.substring(0, dot);
  int room = GAMES_NAME_MAX - (int)ext.length();
  if ((int)stem.length() > room) stem = stem.substring(0, room);
  if (!stem.length()) stem = "rom";
  return stem + ext;
}

// A name as it arrives in a URL: must already be clean, no mapping.
bool validRomName(const String &name) {
  if (!name.length() || (int)name.length() > GAMES_NAME_MAX) return false;
  for (size_t i = 0; i < name.length(); i++) {
    if (!safeChar(name[i])) return false;
  }
  if (name[0] == '.') return false;
  return romExtension(name);
}

// "<rom>.sav" / "<rom>.state" / "<rom>.auto" with a valid rom in front.
bool validSaveName(const String &name) {
  static const char *kinds[] = {".sav", ".state", ".auto"};
  for (int k = 0; k < 3; k++) {
    if (name.endsWith(kinds[k])) {
      String rom = name.substring(0, name.length() - strlen(kinds[k]));
      return validRomName(rom);
    }
  }
  return false;
}

// Where an upload lands: an asset pack in the aux folder, everything else in
// the ROM folder.
String romPath(const String &name) {
  if (packExtension(name)) return String(GAMES_AUX_DIR "/") + name;
  return String(GAMES_ROM_DIR "/") + name;
}
String savePathFor(const String &file) {
  return String(GAMES_SAVE_DIR "/") + file;
}

size_t fsFreeBytes() {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  return total > used ? total - used : 0;
}

// ------------------------------------------------------------ book names

// Basename of whatever the browser sent, lowercased.
String baseName(const String &raw) {
  String base = raw;
  int slash = base.lastIndexOf('/');
  if (slash >= 0) base = base.substring(slash + 1);
  slash = base.lastIndexOf('\\');
  if (slash >= 0) base = base.substring(slash + 1);
  return base;
}

// The host converters' slug rule (tools/pdf2book.py slugify): lowercase, every
// run of characters outside [a-z0-9] becomes one '_', no '_' at either end,
// cut to BOOK_SLUG_MAX. The feed slugs are taken, so a book called "news"
// becomes "news_book" instead of being overwritten by the next sync.
String slugify(const String &stem) {
  String lower = stem;
  lower.toLowerCase();
  String out;
  bool pendingSep = false;
  for (size_t i = 0; i < lower.length(); i++) {
    char c = lower[i];
    bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!keep) {
      pendingSep = out.length() > 0;
      continue;
    }
    if (pendingSep) out += '_';
    pendingSep = false;
    out += c;
  }
  if ((int)out.length() > BOOK_SLUG_MAX) out = out.substring(0, BOOK_SLUG_MAX);
  while (out.endsWith("_")) out.remove(out.length() - 1);
  if (!out.length()) out = "book";
  if (out == NEWS_SLUG || out == METRICS_SLUG) out += "_book";
  return out;
}

// A slug as it arrives in a delete request. Books put on the filesystem with
// uploadfs follow the host slug rule, but be lenient and accept anything the
// ROM name rule accepts, minus the leading dot of a staged upload.
bool validBookSlug(const String &slug) {
  if (!slug.length() || slug.length() > 64 || slug[0] == '.') return false;
  for (size_t i = 0; i < slug.length(); i++) {
    if (!safeChar(slug[i])) return false;
  }
  return true;
}

String bookPathFor(const String &slug) { return String(BOOKS_DIR "/") + slug + ".pgs"; }
String bookPosFor(const String &slug) { return String(BOOKS_DIR "/") + slug + ".pos"; }

// A title for the library: control characters dropped, runs of spaces folded,
// cut to BOOK_TITLE_MAX characters (UTF-8 aware, never splits a character).
String cleanTitle(const String &raw) {
  String out;
  bool space = false;
  int chars = 0;
  for (size_t i = 0; i < raw.length(); i++) {
    uint8_t c = (uint8_t)raw[i];
    if (c < 0x20 || c == 0x7F || c == ' ') {
      space = out.length() > 0;
      continue;
    }
    bool lead = (c & 0xC0) != 0x80;
    if (lead) {
      if (chars >= BOOK_TITLE_MAX) break;
      if (space) {
        out += ' ';
        chars++;
      }
      space = false;
      chars++;
    }
    out += (char)c;
  }
  return out;
}

// Upload stem to a readable title: "alice_in_wonderland" -> "alice in wonderland".
String titleFromStem(const String &stem) {
  String t = stem;
  t.replace('_', ' ');
  t.replace('-', ' ');
  return cleanTitle(t);
}

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(uint8_t)c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

// ------------------------------------------------------------ responses

void noStore() {
  server->sendHeader("Cache-Control", "no-store");
}

void sendJson(int code, const String &body) {
  noStore();
  server->send(code, "application/json", body);
}

// Page assets revalidate on every load: the ETag is the asset length plus the
// firmware build time, so a reflash with new scripts is picked up at once and
// an unchanged one costs a 304.
void sendEmbedded(const uint8_t *start, const uint8_t *end, const char *type) {
  String etag = "\"" + String((unsigned long)(end - start)) + "-" +
                String(__DATE__ " " __TIME__).length() + "-" +
                String(__TIME__) + "\"";
  if (server->header("If-None-Match") == etag) {
    server->sendHeader("ETag", etag);
    server->sendHeader("Cache-Control", "no-cache");
    server->send(304, type, "");
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "no-cache");
  server->sendHeader("ETag", etag);
  server->send_P(200, type, (PGM_P)start, (size_t)(end - start));
}

void redirectHome() {
  server->sendHeader("Location", "/", true);
  noStore();
  server->send(303, "text/plain", "");
}

// ------------------------------------------------------------ handlers

// A zero length save file counts as absent: posting an empty body is how a
// bad state gets retired without deleting the ROM.
bool saveHasBytes(const String &file) {
  fs::File f = LittleFS.open(savePathFor(file), "r");
  if (!f) return false;
  bool has = f.size() > 0;
  f.close();
  return has;
}

void handleRoms() {
  touch();
  String out = "[";
  fs::File dir = LittleFS.open(GAMES_ROM_DIR, "r");
  bool first = true;
  if (dir && dir.isDirectory()) {
    for (fs::File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      String name = f.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!validRomName(name)) continue;
      if (!first) out += ",";
      first = false;
      out += "{\"name\":\"" + name + "\",\"size\":" + String((unsigned long)f.size());
      out += ",\"sav\":" + String(saveHasBytes(name + ".sav") ? "true" : "false");
      out += ",\"state\":" + String(saveHasBytes(name + ".state") ? "true" : "false");
      out += ",\"auto\":" + String(saveHasBytes(name + ".auto") ? "true" : "false");
      out += "}";
    }
  }
  out += "]";
  sendJson(200, out);
}

void handleRom() {
  touch();
  String name = server->pathArg(0);
  if (!validRomName(name)) {
    server->send(400, "text/plain", "bad name");
    return;
  }
  fs::File f = LittleFS.open(romPath(name), "r");
  if (!f) {
    server->send(404, "text/plain", "no such rom");
    return;
  }
  server->sendHeader("Cache-Control", "max-age=86400");
  server->streamFile(f, "application/octet-stream");
  f.close();
}

// Multipart ROM upload. Bytes go straight to the file as they arrive; the
// completion handler below answers once the body is consumed.
void handleUploadData() {
  HTTPUpload &up = server->upload();
  if (up.status == UPLOAD_FILE_START) {
    touch();
    uploadFailed = false;
    uploadError = "";
    uploadName = cleanRomName(up.filename);
    if (!uploadName.length()) {
      uploadFailed = true;
      uploadError = "only .nes, .gb, .gbc and .pack files";
      return;
    }
    uploadIsAux = packExtension(uploadName);
    uploadFreeAtStart = fsFreeBytes();
    // A pack is read while the screen draws, so it cannot be rewritten under
    // the open File.
    if (uploadIsAux) {
      gbgfx::reset();
      pokemon_views::reset();
      pack::close();
    }
    LittleFS.mkdir(GAMES_DIR);
    LittleFS.mkdir(uploadIsAux ? GAMES_AUX_DIR : GAMES_ROM_DIR);
    uploadFile = LittleFS.open(romPath(uploadName) + ".tmp", "w");
    if (!uploadFile) {
      uploadFailed = true;
      uploadError = "could not open file";
    }
    Serial.printf("games: upload %s -> %s\n", up.filename.c_str(),
                  uploadName.c_str());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFailed || !uploadFile) return;
    uint32_t cap = uploadIsAux ? GAMES_MAX_AUX_BYTES : GAMES_MAX_ROM_BYTES;
    if (up.totalSize + up.currentSize > cap) {
      uploadFailed = true;
      uploadError = uploadIsAux ? "pack larger than 3 MB" : "rom larger than 2 MB";
      uploadFile.close();
      LittleFS.remove(romPath(uploadName) + ".tmp");
      return;
    }
    // A full filesystem panics inside littlefs instead of failing the write,
    // so stop while there is still room to spare.
    if (up.totalSize + up.currentSize + BOOK_UPLOAD_MIN_FREE_BYTES >
        uploadFreeAtStart) {
      uploadFailed = true;
      uploadError = "not enough free space on the reader";
      uploadFile.close();
      LittleFS.remove(romPath(uploadName) + ".tmp");
      return;
    }
    if (uploadFile.write(up.buf, up.currentSize) != up.currentSize) {
      uploadFailed = true;
      uploadError = "out of space";
      uploadFile.close();
      LittleFS.remove(romPath(uploadName) + ".tmp");
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFailed) return;
    uploadFile.close();
    LittleFS.remove(romPath(uploadName));
    if (!LittleFS.rename(romPath(uploadName) + ".tmp", romPath(uploadName))) {
      uploadFailed = true;
      uploadError = "rename failed";
      LittleFS.remove(romPath(uploadName) + ".tmp");
    }
    Serial.printf("games: stored %s (%u bytes)\n", uploadName.c_str(),
                  (unsigned)up.totalSize);
    if (uploadIsAux && !uploadFailed) {
      pack::open();
      pokemon_views::reset();
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    LittleFS.remove(romPath(uploadName) + ".tmp");
    uploadFailed = true;
    uploadError = "upload aborted";
  }
}

void handleUploadDone() {
  if (uploadFailed) {
    noStore();
    server->send(400, "text/plain", uploadError);
    return;
  }
  redirectHome();
}

void handleDelete() {
  touch();
  String name = server->arg("name");
  if (!validRomName(name)) {
    server->send(400, "text/plain", "bad name");
    return;
  }
  LittleFS.remove(romPath(name));
  LittleFS.remove(savePathFor(name + ".sav"));
  LittleFS.remove(savePathFor(name + ".state"));
  LittleFS.remove(savePathFor(name + ".auto"));
  Serial.printf("games: deleted %s\n", name.c_str());
  redirectHome();
}

void handleSaveGet() {
  touch();
  String file = server->pathArg(0);
  if (!validSaveName(file)) {
    server->send(400, "text/plain", "bad name");
    return;
  }
  fs::File f = LittleFS.open(savePathFor(file), "r");
  if (!f) {
    noStore();
    server->send(404, "text/plain", "no save");
    return;
  }
  noStore();
  server->streamFile(f, "application/octet-stream");
  f.close();
}

// Raw POST body of a save. Written to a temp file and renamed at the end, so
// a dropped connection never leaves a half written save behind.
// Raw bodies arrive one byte per callback (see HTTP_RAW_BUFLEN in
// platformio.ini), so saves are staged here and written in 1 KB runs.
uint8_t saveStage[1024];
size_t saveStaged = 0;

bool flushSaveStage() {
  if (!saveStaged) return true;
  size_t n = saveStaged;
  saveStaged = 0;
  return saveFile.write(saveStage, n) == n;
}

void handleSaveData() {
  HTTPRaw &raw = server->raw();
  if (raw.status == RAW_START) {
    touch();
    saveFailed = false;
    saveBytes = 0;
    saveStaged = 0;
    String file = server->pathArg(0);
    if (!validSaveName(file)) {
      saveFailed = true;
      return;
    }
    LittleFS.mkdir(GAMES_DIR);
    LittleFS.mkdir(GAMES_SAVE_DIR);
    savePath = savePathFor(file);
    saveFile = LittleFS.open(savePath + ".tmp", "w");
    if (!saveFile) saveFailed = true;
  } else if (raw.status == RAW_WRITE) {
    if (saveFailed || !saveFile) return;
    bool ok = saveBytes + raw.currentSize <= GAMES_MAX_SAVE_BYTES;
    size_t done = 0;
    while (ok && done < raw.currentSize) {
      size_t n = raw.currentSize - done;
      if (n > sizeof(saveStage) - saveStaged) n = sizeof(saveStage) - saveStaged;
      memcpy(saveStage + saveStaged, raw.buf + done, n);
      saveStaged += n;
      done += n;
      if (saveStaged == sizeof(saveStage)) ok = flushSaveStage();
    }
    if (!ok) {
      saveFailed = true;
      saveFile.close();
      LittleFS.remove(savePath + ".tmp");
      return;
    }
    saveBytes += raw.currentSize;
  } else if (raw.status == RAW_END) {
    if (saveFailed) return;
    if (!flushSaveStage()) saveFailed = true;
    saveFile.close();
    if (saveFailed) {
      LittleFS.remove(savePath + ".tmp");
      return;
    }
    LittleFS.remove(savePath);
    if (!LittleFS.rename(savePath + ".tmp", savePath)) {
      saveFailed = true;
      LittleFS.remove(savePath + ".tmp");
    }
  } else if (raw.status == RAW_ABORTED) {
    if (saveFile) saveFile.close();
    LittleFS.remove(savePath + ".tmp");
    saveFailed = true;
  }
}

void handleSaveDone() {
  if (saveFailed) {
    sendJson(500, "{\"ok\":false}");
    return;
  }
  Serial.printf("games: save %s (%u bytes)\n", savePath.c_str(),
                (unsigned)saveBytes);
  sendJson(200, "{\"ok\":true,\"size\":" + String((unsigned long)saveBytes) + "}");
}

// Raw POST body of the Game Boy work RAM, exactly GAMES_WRAM_BYTES long. The
// page sends it every few seconds while a GB game runs, and only when the
// bytes changed, so this route is quiet on a paused game. Nothing is written
// to the filesystem: the snapshot lives in RAM for the companion screen.
uint32_t wramStartMs = 0;
uint32_t wramPrevMs = 0;

void handleWramData() {
  HTTPRaw &raw = server->raw();
  if (raw.status == RAW_START) {
    touch();
    wramBytes = 0;
    wramFailed = false;
    wramStartMs = millis();
  } else if (raw.status == RAW_WRITE) {
    if (wramFailed) return;
    if (wramBytes + raw.currentSize > GAMES_WRAM_BYTES) {
      wramFailed = true;
      return;
    }
    memcpy(pokemon::wramBuffer() + wramBytes, raw.buf, raw.currentSize);
    wramBytes += raw.currentSize;
  } else if (raw.status == RAW_END) {
    if (wramFailed || wramBytes != GAMES_WRAM_BYTES) {
      wramFailed = true;
      return;
    }
    pokemon::wramCommit(wramBytes);
    uint32_t now = millis();
    Serial.printf("games: wram n=%lu gap=%lu ms body=%lu ms\n",
                  (unsigned long)pokemon::wramCounter(),
                  (unsigned long)(now - wramPrevMs),
                  (unsigned long)(now - wramStartMs));
    wramPrevMs = now;
  } else if (raw.status == RAW_ABORTED) {
    wramFailed = true;
  }
}

void handleWramDone() {
  if (wramFailed || wramBytes != GAMES_WRAM_BYTES) {
    sendJson(400, "{\"ok\":false,\"want\":" +
                      String((unsigned long)GAMES_WRAM_BYTES) + "}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"size\":" + String((unsigned long)wramBytes) +
                    ",\"n\":" + String((unsigned long)pokemon::wramCounter()) +
                    "}");
}

// The last snapshot back out again, for debugging the decoder against a host.
void handleWramGet() {
  touch();
  if (!pokemon::wramValid()) {
    noStore();
    server->send(404, "text/plain", "no snapshot");
    return;
  }
  noStore();
  server->setContentLength(GAMES_WRAM_BYTES);
  server->send(200, "application/octet-stream", "");
  server->sendContent((const char *)pokemon::wram(), GAMES_WRAM_BYTES);
}

// ------------------------------------------------------------ books

void handleBooks() {
  touch();
  std::vector<BookEntry> list = books::list();
  String out = "[";
  for (size_t i = 0; i < list.size(); i++) {
    if (i) out += ",";
    out += "{\"slug\":\"" + jsonEscape(list[i].slug) + "\"";
    out += ",\"title\":\"" + jsonEscape(list[i].title) + "\"";
    out += ",\"pages\":" + String((unsigned long)list[i].pageCount);
    out += ",\"page\":" + String((unsigned long)list[i].savedPage);
    out += ",\"sizes\":" + String((unsigned)list[i].variantCount);
    out += ",\"size\":" + String((unsigned long)list[i].fileBytes) + "}";
  }
  out += "]";
  sendJson(200, out);
}

void failBook(const String &why) {
  bookFailed = true;
  bookError = why;
  if (bookFile) bookFile.close();
  LittleFS.remove(BOOK_UPLOAD_TMP_FILE);
  LittleFS.remove(bookPathFor(BOOK_UPLOAD_STAGE_SLUG));
}

String kb(uint32_t bytes) { return String((unsigned long)(bytes / 1024)) + " KB"; }

// Multipart book upload. The page passes ?size=<bytes> so a book that cannot
// fit is refused before a single byte is written; the byte count is checked
// again as the body arrives, since the query is only a claim.
void handleBookUploadData() {
  HTTPUpload &up = server->upload();
  if (up.status == UPLOAD_FILE_START) {
    touch();
    bookFailed = false;
    bookError = "";
    bookBytes = 0;
    String base = baseName(up.filename);
    String lower = base;
    lower.toLowerCase();
    bookIsText = lower.endsWith(".txt");
    if (!bookIsText && !lower.endsWith(".pgs")) {
      failBook("only .txt and .pgs books");
      return;
    }
    bookStem = base.substring(0, base.length() - 4);
    bookSlug = slugify(bookStem);

    uint32_t freeBytes = (uint32_t)fsFreeBytes();
    uint32_t room = freeBytes > BOOK_UPLOAD_MIN_FREE_BYTES
                        ? freeBytes - BOOK_UPLOAD_MIN_FREE_BYTES
                        : 0;
    uint32_t cap = bookIsText ? BOOK_MAX_TXT_BYTES : BOOK_MAX_PGS_BYTES;
    bookBudget = bookIsText ? room / BOOK_TXT_SPACE_FACTOR : room;
    if (bookBudget > cap) bookBudget = cap;
    uint32_t claimed = (uint32_t)server->arg("size").toInt();
    Serial.printf("books: upload %s -> %s (%s, %lu bytes claimed, budget %lu, "
                  "free %lu)\n",
                  up.filename.c_str(), bookSlug.c_str(), bookIsText ? "txt" : "pgs",
                  (unsigned long)claimed, (unsigned long)bookBudget,
                  (unsigned long)freeBytes);
    if (claimed > cap) {
      failBook(bookIsText ? "a .txt book can be at most " + kb(cap)
                          : "a .pgs book can be at most " + kb(cap));
      return;
    }
    if (claimed > bookBudget || bookBudget == 0) {
      failBook("not enough free space: " + kb(freeBytes) + " free, this book needs about " +
               kb((bookIsText ? claimed * BOOK_TXT_SPACE_FACTOR : claimed) +
                  BOOK_UPLOAD_MIN_FREE_BYTES));
      return;
    }
    LittleFS.mkdir(BOOKS_DIR);
    LittleFS.remove(BOOK_UPLOAD_TMP_FILE);
    LittleFS.remove(bookPathFor(BOOK_UPLOAD_STAGE_SLUG));
    bookFile = LittleFS.open(bookIsText ? String(BOOK_UPLOAD_TMP_FILE)
                                        : bookPathFor(BOOK_UPLOAD_STAGE_SLUG),
                             "w");
    if (!bookFile) failBook("could not open file");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (bookFailed || !bookFile) return;
    if (bookBytes + up.currentSize > bookBudget) {
      failBook("not enough free space for this book");
      return;
    }
    if (bookFile.write(up.buf, up.currentSize) != up.currentSize) {
      failBook("write failed");
      return;
    }
    bookBytes += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (bookFailed) return;
    bookFile.close();
    if (!bookBytes) failBook("empty file");
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    failBook("upload aborted");
  }
}

void sendBookError(const String &why) {
  Serial.printf("books: upload failed: %s\n", why.c_str());
  sendJson(400, "{\"ok\":false,\"error\":\"" + jsonEscape(why) + "\"}");
}

// Moves the staged book over /books/<slug>.pgs. The old reading position
// points into a book that is gone, so it goes too, and the new book opens at
// page 1 in the normal text size.
bool commitStagedBook(const String &slug) {
  LittleFS.remove(bookPathFor(slug));
  LittleFS.remove(bookPosFor(slug));
  return LittleFS.rename(bookPathFor(BOOK_UPLOAD_STAGE_SLUG), bookPathFor(slug));
}

void handleBookUploadDone() {
  if (bookFailed) {
    sendBookError(bookError);
    return;
  }
  if (!bookBytes) {
    sendBookError("no file in the request");
    return;
  }

  String title;
  uint32_t pages = 0;
  if (bookIsText) {
    title = cleanTitle(server->arg("title"));
    if (!title.length()) title = titleFromStem(bookStem);
    {
      std::vector<String> lines;
      lines.push_back("Adding a book");
      lines.push_back("");
      lines.push_back(title);
      lines.push_back("");
      lines.push_back("Paginating " + kb(bookBytes) + " of text ...");
      ui::renderStatusScreen("books", lines);
    }
    uint32_t t0 = millis();
    uint32_t variantPages[NEWS_VARIANT_COUNT] = {0};
    String err;
    bool ok = news_sync::writeTextBook(BOOK_UPLOAD_TMP_FILE, BOOK_UPLOAD_BLOB_FILE,
                                       String(BOOK_UPLOAD_STAGE_SLUG), title,
                                       variantPages, &err);
    LittleFS.remove(BOOK_UPLOAD_TMP_FILE);
    if (!ok) {
      LittleFS.remove(bookPathFor(BOOK_UPLOAD_STAGE_SLUG));
      sendBookError("could not paginate: " + err);
      return;
    }
    pages = variantPages[0];
    Serial.printf("books: paginated %s in %lu ms, %lu + %lu pages\n",
                  bookSlug.c_str(), (unsigned long)(millis() - t0),
                  (unsigned long)variantPages[0],
                  (unsigned long)(NEWS_VARIANT_COUNT > 1 ? variantPages[1] : 0));
  }

  // Whatever was written, it has to open before it replaces anything.
  {
    Book check;
    if (!check.open(BOOK_UPLOAD_STAGE_SLUG)) {
      String why = check.errorText();
      LittleFS.remove(bookPathFor(BOOK_UPLOAD_STAGE_SLUG));
      sendBookError(bookIsText ? "book did not verify: " + why
                               : "not a valid .pgs book: " + why);
      return;
    }
    if (!bookIsText) {
      title = check.title();
      pages = check.pageCount();
    }
  }
  if (!commitStagedBook(bookSlug)) {
    LittleFS.remove(bookPathFor(BOOK_UPLOAD_STAGE_SLUG));
    sendBookError("rename failed");
    return;
  }
  Serial.printf("books: stored %s \"%s\", %lu pages\n", bookSlug.c_str(),
                title.c_str(), (unsigned long)pages);
  touch();
  sendJson(200, "{\"ok\":true,\"slug\":\"" + jsonEscape(bookSlug) +
                    "\",\"title\":\"" + jsonEscape(title) +
                    "\",\"pages\":" + String((unsigned long)pages) + "}");
}

void handleBookDelete() {
  touch();
  String slug = server->arg("slug");
  if (!validBookSlug(slug)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad name\"}");
    return;
  }
  bool had = LittleFS.exists(bookPathFor(slug));
  LittleFS.remove(bookPathFor(slug));
  LittleFS.remove(bookPosFor(slug));
  Serial.printf("books: deleted %s\n", slug.c_str());
  sendJson(had ? 200 : 404, had ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"no such book\"}");
}

void handlePing() {
  touch();
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  size_t freeBytes = total > used ? total - used : 0;
  sendJson(200, "{\"ok\":true,\"free\":" + String((unsigned long)freeBytes) +
                    ",\"total\":" + String((unsigned long)total) +
                    ",\"reserve\":" + String((unsigned long)BOOK_UPLOAD_MIN_FREE_BYTES) +
                    ",\"txtFactor\":" + String((unsigned long)BOOK_TXT_SPACE_FACTOR) +
                    ",\"txtMax\":" + String((unsigned long)BOOK_MAX_TXT_BYTES) +
                    ",\"books\":" + String((unsigned long)games::bookCount()) +
                    ",\"uptime\":" + String((unsigned long)((millis() - startMs) / 1000)) +
                    ",\"roms\":" + String((unsigned long)games::romCount()) +
                    ",\"pack\":" + String(pack::isOpen() ? "true" : "false") +
                    ",\"wram\":" + String(pokemon::wramValid() ? "true" : "false") +
                    "}");
}

void handleNotFound() {
  noStore();
  server->send(404, "text/plain", "not found");
}

// ------------------------------------------------------------ screen

String idleText() {
  uint32_t idle = (millis() - lastRequestMs) / 1000;
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(idle / 60),
           (unsigned long)(idle % 60));
  return String(buf);
}

void drawScreen() {
  std::vector<String> lines;
  lines.push_back("Books and games over WiFi");
  lines.push_back("");
  if (stationSsid.length()) {
    lines.push_back("On " + stationSsid + " open");
    lines.push_back("  http://" + WiFi.localIP().toString() + "/");
    if (mdnsUp) lines.push_back("  http://" GAMES_MDNS_HOST ".local/");
  } else {
    lines.push_back("No stored network joined.");
  }
  lines.push_back("");
  lines.push_back("Or join WiFi " GAMES_AP_SSID);
  lines.push_back("  password  " GAMES_AP_PASSWORD);
  lines.push_back("  http://" GAMES_AP_IP_TEXT "/");
  lines.push_back("");
  // 40 columns at most on panel 0 (54 there); a narrower grid (37 on the 7.5
  // inch panel) gets the same words on three lines instead of clipping.
  if (TEXT_COLS >= 40) {
    lines.push_back("On that page: add books (.txt or .pgs),");
    lines.push_back("delete books, and upload and play games.");
  } else {
    lines.push_back("On that page: add books (.txt or");
    lines.push_back(".pgs), delete books, and upload and");
    lines.push_back("play games.");
  }
  lines.push_back("");
  lines.push_back(String((unsigned long)games::bookCount()) + " books, " +
                  String((unsigned long)games::romCount()) + " roms, " +
                  String((unsigned long)(fsFreeBytes() / 1024)) + " KB free");
  lines.push_back(String((unsigned long)requests) + " requests, idle " +
                  idleText());
  lines.push_back("");
  lines.push_back("Press center to exit.");
  lines.push_back("Stops by itself after 15 min idle.");
  ui::renderStatusScreen("games", lines);
}

// ------------------------------------------------------- companion screen
//
// The four views live in src/pokemon_views.cpp and are drawn to the pixel
// layout in docs/pokemon-aux-display-layout.md. This only hands them the pad
// state: which page is current and whether the terrain overlay is up.
void drawCompanion() { pokemon_views::draw(auxPage, auxTerrain); }

}  // namespace

namespace games {

size_t romCount() {
  size_t n = 0;
  fs::File dir = LittleFS.open(GAMES_ROM_DIR, "r");
  if (!dir || !dir.isDirectory()) return 0;
  for (fs::File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (validRomName(name)) n++;
  }
  return n;
}

size_t bookCount() {
  size_t n = 0;
  fs::File dir = LittleFS.open(BOOKS_DIR, "r");
  if (!dir || !dir.isDirectory()) return 0;
  for (fs::File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.endsWith(".pgs") && !name.startsWith(".") &&
        name != METRICS_SLUG ".pgs") {
      n++;
    }
  }
  return n;
}

void run() {
  WdtWindow wdt;
  requests = 0;
  startMs = millis();
  lastRequestMs = startMs;
  stationSsid = "";
  mdnsUp = false;

  {
    std::vector<String> lines;
    lines.push_back("Books and games over WiFi");
    lines.push_back("");
    lines.push_back(wifi_store::list().empty() ? "No stored network, AP only ..."
                                               : "Joining WiFi ...");
    ui::renderStatusScreen("games", lines, true);
  }

  LittleFS.mkdir(GAMES_DIR);
  LittleFS.mkdir(GAMES_ROM_DIR);
  LittleFS.mkdir(GAMES_SAVE_DIR);
  LittleFS.mkdir(GAMES_AUX_DIR);
  LittleFS.mkdir(BOOKS_DIR);
  // Leftovers of a book upload that died with the reader (power pulled mid
  // pagination): nothing refers to them, and they can be megabytes.
  LittleFS.remove(BOOK_UPLOAD_TMP_FILE);
  LittleFS.remove(BOOK_UPLOAD_BLOB_FILE);
  LittleFS.remove(bookPathFor(BOOK_UPLOAD_STAGE_SLUG));

  auxPage = 0;
  auxTerrain = false;
  shownSnapshot = 0;
  gbgfx::reset();
  pokemon_views::reset();
  pokemon::wramInvalidate();
  pack::open();   // stays open for the life of the server

  WiFi.persistent(false);
  if (!wifi_store::list().empty()) {
    String ssid;
    if (wifi_connect(&ssid)) stationSsid = ssid;
  }

  // The AP rides alongside the station link; both reach the same server.
  WiFi.mode(WIFI_AP_STA);
  IPAddress apIp(192, 168, 4, 1);
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(GAMES_AP_SSID, GAMES_AP_PASSWORD);

  if (stationSsid.length()) {
    mdnsUp = MDNS.begin(GAMES_MDNS_HOST);
    if (mdnsUp) MDNS.addService("http", "tcp", 80);
  }

  server = new WebServer(80);
  // Only collected headers are readable from a handler.
  static const char *kHeaders[] = {"If-None-Match"};
  server->collectHeaders(kHeaders, 1);
  server->on("/", HTTP_GET, []() {
    touch();
    sendEmbedded(www_index_start, www_index_end, "text/html");
  });
  server->on("/app.js", HTTP_GET, []() {
    touch();
    sendEmbedded(www_app_start, www_app_end, "application/javascript");
  });
  server->on("/nes.js", HTTP_GET, []() {
    touch();
    sendEmbedded(www_nes_start, www_nes_end, "application/javascript");
  });
  server->on("/gb.js", HTTP_GET, []() {
    touch();
    sendEmbedded(www_gb_start, www_gb_end, "application/javascript");
  });
  server->on("/api/roms", HTTP_GET, handleRoms);
  server->on("/api/wram", HTTP_GET, handleWramGet);
  server->on("/api/wram", HTTP_POST, handleWramDone, handleWramData);
  server->on("/api/ping", HTTP_GET, handlePing);
  server->on("/api/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server->on("/api/delete", HTTP_POST, handleDelete);
  server->on("/api/books", HTTP_GET, handleBooks);
  server->on("/api/books/upload", HTTP_POST, handleBookUploadDone,
             handleBookUploadData);
  server->on("/api/books/delete", HTTP_POST, handleBookDelete);
  server->on(UriBraces("/roms/{}"), HTTP_GET, handleRom);
  server->on(UriBraces("/saves/{}"), HTTP_GET, handleSaveGet);
  server->on(UriBraces("/saves/{}"), HTTP_POST, handleSaveDone, handleSaveData);
  server->onNotFound(handleNotFound);
  server->begin();

  Serial.printf("games: up, station %s ip %s, ap %s\n",
                stationSsid.length() ? stationSsid.c_str() : "(none)",
                WiFi.localIP().toString().c_str(), GAMES_AP_SSID);

  uint32_t shownRequests = (uint32_t)-1;
  uint32_t lastDraw = 0;
  bool redrawAux = false;
  bool exitRequested = false;
  bool pendingDraw = false;
  uint32_t shownSignature = 0;
  auto handlePad = [&](Button b) {
    if (b == BTN_CENTER) { exitRequested = true; return; }
    if (b == BTN_UP) {
      auxPage = (auxPage + 1) % pokemon_views::PAGE_COUNT;
      redrawAux = true;
    } else if (b == BTN_DOWN) {
      auxPage = (auxPage + pokemon_views::PAGE_COUNT - 1) %
                pokemon_views::PAGE_COUNT;
      redrawAux = true;
    } else if (b == BTN_LEFT) {
      auxTerrain = !auxTerrain;
      redrawAux = true;
    }
  };
  input::clearLatched();
  while (true) {
    feedWdt();
    server->handleClient();

    // Redraw when something happened, at most every GAMES_SCREEN_REDRAW_MS:
    // a burst of asset requests must not turn into a burst of refreshes.
    // Once a work RAM snapshot has arrived the companion screen takes over
    // and follows the snapshot counter instead of the request counter.
    uint32_t now = millis();
    if (pokemon::wramValid()) {
      uint32_t snap = pokemon::wramCounter();
      // A pad press redraws at once. A new snapshot redraws after the short
      // companion throttle, and only when the bytes the views draw from
      // changed (work RAM itself changes every frame).
      if (snap != shownSnapshot) {
        shownSnapshot = snap;
        uint32_t sig = pokemon::viewSignature();
        if (sig != shownSignature) {
          shownSignature = sig;
          pendingDraw = true;
        }
      }
      if (redrawAux || (pendingDraw && now - lastDraw >= GAMES_AUX_REDRAW_MS)) {
        pendingDraw = false;
        redrawAux = false;
        shownRequests = requests;
        lastDraw = now;
        uint32_t t0 = millis();
        drawCompanion();
        Serial.printf("games: companion draw %lu ms\n",
                      (unsigned long)(millis() - t0));
        // The draw blocks the loop for over a second and the reader redraws
        // almost continuously, so presses made during it are taken from the
        // interrupt latch instead of being flushed away.
        input::flush();
        Button lb;
        while (input::takeLatched(lb)) handlePad(lb);
        if (exitRequested) break;
      }
    } else if (shownRequests != requests &&
               now - lastDraw >= GAMES_SCREEN_REDRAW_MS) {
      shownRequests = requests;
      lastDraw = now;
      drawScreen();
      input::flush();
    }

    if (now - lastRequestMs >= GAMES_IDLE_TIMEOUT_MS) {
      Serial.printf("games: idle timeout\n");
      break;
    }

    // Pads. The silk on the case does not match the logical names: silk right
    // is BTN_UP (next page), silk left is BTN_DOWN (back), silk up is BTN_LEFT
    // (terrain toggle). The presses are only recorded here; the views pass
    // decides what they mean.
    Button b;
    if (input::poll(b) || input::takeLatched(b)) handlePad(b);
    if (exitRequested) break;
    delay(BUTTON_POLL_MS);
  }

  server->stop();
  delete server;
  server = nullptr;
  if (mdnsUp) MDNS.end();
  gbgfx::reset();
  pokemon_views::reset();
  pack::close();
  WiFi.softAPdisconnect(true);
  wifi_off();
  Serial.printf("games: down after %lu requests\n", (unsigned long)requests);
}

}  // namespace games

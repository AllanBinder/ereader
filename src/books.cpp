#include "books.h"

#include <LittleFS.h>
#include <esp_task_wdt.h>

#include <memory>
#include <string>

#include "config.h"
#include "text_paginate.h"

namespace {

const uint32_t MAX_PAGE_COUNT = 100000;
const uint16_t MAX_TITLE_LEN = 512;

// The grids a file without a grid extension was paginated on: panel 0's,
// which were the only grids before multi panel support. Fixed numbers on
// purpose, not TEXT_COLS and friends, which follow the active panel.
const uint8_t LEGACY_COLS = 54;
const uint8_t LEGACY_ROWS = 37;
const uint8_t LEGACY_LARGE_COLS = 40;
const uint8_t LEGACY_LARGE_ROWS = 27;

void legacyGrid(uint8_t fontId, uint8_t &cols, uint8_t &rows) {
  if (fontId == BOOK_FONT_PROFONT29) {
    cols = LEGACY_LARGE_COLS;
    rows = LEGACY_LARGE_ROWS;
  } else {
    cols = LEGACY_COLS;
    rows = LEGACY_ROWS;
  }
}

bool readU8(fs::File &f, uint8_t &out) {
  return f.read(&out, 1) == 1;
}

bool readU32(fs::File &f, uint32_t &out) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) return false;
  out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
        ((uint32_t)b[3] << 24);
  return true;
}

bool readU16(fs::File &f, uint16_t &out) {
  uint8_t b[2];
  if (f.read(b, 2) != 2) return false;
  out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
  return true;
}

uint32_t decodeU32(const uint8_t *b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

String bookPath(const String &slug) {
  return String(BOOKS_DIR "/") + slug + ".pgs";
}

String posPath(const String &slug) {
  return String(BOOKS_DIR "/") + slug + ".pos";
}

// Fallback title for a book whose header carries none. The synced news brief
// writes its own title (see src/news_sync.cpp), so this only matters for a
// news.pgs left behind by an interrupted sync, but the mapping belongs here
// with the rest of the filename to title derivation.
String defaultTitleForSlug(const String &slug) {
  if (slug == NEWS_SLUG) return String(NEWS_TITLE);
  return slug;
}

}  // namespace

bool Book::open(const String &slug) {
  close();
  slug_ = slug;

  fs::File f = LittleFS.open(bookPath(slug).c_str(), "r");
  if (!f) {
    error_ = "Cannot open book file";
    return false;
  }
  fileSize_ = (uint32_t)f.size();

  char magic[5] = {0};
  if (f.read((uint8_t *)magic, 4) != 4) {
    error_ = "Truncated header";
    f.close();
    return false;
  }

  bool ok = false;
  if (strcmp(magic, "MPG1") == 0) {
    ok = parseMpg1(f);
  } else if (strcmp(magic, "MPG2") == 0) {
    ok = parseMpg2(f);
  } else {
    error_ = "Bad magic (not an MPG1 or MPG2 file)";
  }
  if (ok) readGridExt(f);
  f.close();

  if (!ok) {
    variants_.clear();
    return false;
  }
  keepMatchingVariants();

  if (!title_.length()) title_ = defaultTitleForSlug(slug_);
  activeVariant_ = 0;
  open_ = true;
  error_ = "";
  return true;
}

// MPG1: u32 page_count, u16 title_len, title, u32 offsets[page_count]. Page p
// runs from offsets[p] to offsets[p+1], or to EOF for the last page.
bool Book::parseMpg1(fs::File &f) {
  uint32_t pages = 0;
  if (!readU32(f, pages) || pages == 0 || pages > MAX_PAGE_COUNT) {
    error_ = "Bad page count";
    return false;
  }

  uint16_t titleLen = 0;
  if (!readU16(f, titleLen) || titleLen > MAX_TITLE_LEN) {
    error_ = "Bad title length";
    return false;
  }
  if (titleLen > 0) {
    std::vector<char> buf(titleLen + 1, 0);
    if (f.read((uint8_t *)buf.data(), titleLen) != (int)titleLen) {
      error_ = "Truncated title";
      return false;
    }
    title_ = String(buf.data());
  }

  BookVariant variant;
  variant.fontId = BOOK_FONT_PROFONT22;
  legacyGrid(variant.fontId, variant.cols, variant.rows);
  variant.pages.resize(pages);
  for (uint32_t i = 0; i < pages; i++) {
    uint32_t off = 0;
    if (!readU32(f, off) || off > fileSize_) {
      error_ = "Truncated or invalid offset table";
      return false;
    }
    variant.pages[i].offset = off;
    if (i > 0) {
      uint32_t prev = variant.pages[i - 1].offset;
      variant.pages[i - 1].length = (off > prev) ? (off - prev) : 0;
    }
  }
  uint32_t last = variant.pages[pages - 1].offset;
  variant.pages[pages - 1].length = (fileSize_ > last) ? (fileSize_ - last) : 0;

  variants_.push_back(variant);
  return true;
}

// MPG2: u8 variant_count, u16 title_len, title, then per variant u8 font_id,
// u32 page_count, and page_count entries of {u32 offset, u32 length,
// u32 anchor}.
bool Book::parseMpg2(fs::File &f) {
  uint8_t variantCount = 0;
  if (!readU8(f, variantCount) || variantCount == 0 ||
      variantCount > MPG2_MAX_VARIANTS) {
    error_ = "Bad variant count";
    return false;
  }

  uint16_t titleLen = 0;
  if (!readU16(f, titleLen) || titleLen > MAX_TITLE_LEN) {
    error_ = "Bad title length";
    return false;
  }
  if (titleLen > 0) {
    std::vector<char> buf(titleLen + 1, 0);
    if (f.read((uint8_t *)buf.data(), titleLen) != (int)titleLen) {
      error_ = "Truncated title";
      return false;
    }
    title_ = String(buf.data());
  }

  variants_.reserve(variantCount);
  for (uint8_t v = 0; v < variantCount; v++) {
    BookVariant variant;
    if (!readU8(f, variant.fontId)) {
      error_ = "Truncated variant header";
      return false;
    }
    legacyGrid(variant.fontId, variant.cols, variant.rows);
    uint32_t pages = 0;
    if (!readU32(f, pages) || pages == 0 || pages > MAX_PAGE_COUNT) {
      error_ = "Bad page count";
      return false;
    }
    variant.pages.resize(pages);
    // Read the table 12 bytes at a time: one entry per read keeps the code
    // simple and the LittleFS read cache does the buffering.
    uint8_t entry[12];
    uint32_t prevAnchor = 0;
    for (uint32_t i = 0; i < pages; i++) {
      if (f.read(entry, 12) != 12) {
        error_ = "Truncated page table";
        return false;
      }
      PageRef &ref = variant.pages[i];
      ref.offset = decodeU32(entry);
      ref.length = decodeU32(entry + 4);
      ref.anchor = decodeU32(entry + 8);
      if (ref.length == 0 || ref.offset > fileSize_ ||
          ref.offset + ref.length > fileSize_) {
        error_ = "Page table entry out of range";
        return false;
      }
      if (i > 0 && ref.anchor < prevAnchor) {
        error_ = "Page anchors are not in order";
        return false;
      }
      prevAnchor = ref.anchor;
    }
    variants_.push_back(variant);
  }
  return true;
}

// The optional grid extension right after the page tables (see books.h). The
// file is positioned at the end of the tables. It only counts when every page
// blob starts after it, so a text page that happens to begin with "GRID" in a
// file without the extension can never be mistaken for one. Anything that does
// not parse leaves the legacy grids in place.
void Book::readGridExt(fs::File &f) {
  const uint8_t n = (uint8_t)variants_.size();
  const uint32_t pos = (uint32_t)f.position();
  const uint32_t need = bookGridExtLen(n);
  uint32_t firstBlob = fileSize_;
  for (size_t v = 0; v < variants_.size(); v++) {
    const std::vector<PageRef> &pages = variants_[v].pages;
    for (size_t i = 0; i < pages.size(); i++) {
      if (pages[i].offset < firstBlob) firstBlob = pages[i].offset;
    }
  }
  if (pos + need > firstBlob) return;

  uint8_t buf[4 + 1 + 2 * MPG2_MAX_VARIANTS];
  if (f.read(buf, need) != (int)need) return;
  if (memcmp(buf, BOOK_GRID_EXT_MAGIC, 4) != 0 || buf[4] != n) return;
  for (uint8_t v = 0; v < n; v++) {
    if (buf[5 + 2 * v] == 0 || buf[6 + 2 * v] == 0) return;
  }
  for (uint8_t v = 0; v < n; v++) {
    variants_[v].cols = buf[5 + 2 * v];
    variants_[v].rows = buf[6 + 2 * v];
  }
}

// Keeps the variants made for this panel's grids. With none, every variant
// stays (the pages still draw, clipped or short) and the book is flagged for
// books::reformat().
void Book::keepMatchingVariants() {
  needsReformat_ = false;
  std::vector<BookVariant> keep;
  for (size_t v = 0; v < variants_.size(); v++) {
    int cols = 0;
    int rows = 0;
    books::activeGrid(variants_[v].fontId, cols, rows);
    if (variants_[v].cols == cols && variants_[v].rows == rows) {
      keep.push_back(variants_[v]);
    }
  }
  if (keep.empty()) {
    needsReformat_ = true;
    return;
  }
  if (keep.size() != variants_.size()) variants_.swap(keep);
}

void Book::close() {
  open_ = false;
  slug_ = "";
  title_ = "";
  error_ = "";
  fileSize_ = 0;
  activeVariant_ = 0;
  needsReformat_ = false;
  variants_.clear();
}

uint32_t Book::pageCount() const {
  if (!open_ || activeVariant_ >= variants_.size()) return 0;
  return (uint32_t)variants_[activeVariant_].pages.size();
}

uint32_t Book::anchorOf(uint32_t page) const {
  if (!open_ || activeVariant_ >= variants_.size()) return 0;
  const std::vector<PageRef> &pages = variants_[activeVariant_].pages;
  return page < pages.size() ? pages[page].anchor : 0;
}

uint8_t Book::fontId() const {
  if (!open_ || activeVariant_ >= variants_.size()) return BOOK_FONT_PROFONT22;
  return variants_[activeVariant_].fontId;
}

uint8_t Book::cols() const {
  if (!open_ || activeVariant_ >= variants_.size()) return 0;
  return variants_[activeVariant_].cols;
}

uint8_t Book::rows() const {
  if (!open_ || activeVariant_ >= variants_.size()) return 0;
  return variants_[activeVariant_].rows;
}

bool Book::pageSpan(uint32_t index, uint32_t &offset, uint32_t &length) const {
  if (!open_ || activeVariant_ >= variants_.size()) return false;
  const std::vector<PageRef> &pages = variants_[activeVariant_].pages;
  if (index >= pages.size()) return false;
  offset = pages[index].offset;
  length = pages[index].length;
  return true;
}

bool Book::setVariant(uint8_t variant) {
  if (variant >= variants_.size()) return false;
  activeVariant_ = variant;
  return true;
}

uint32_t Book::mapPage(uint32_t page, uint8_t variant) const {
  if (!open_ || variant >= variants_.size()) return 0;
  if (activeVariant_ >= variants_.size()) return 0;
  const std::vector<PageRef> &from = variants_[activeVariant_].pages;
  const std::vector<PageRef> &to = variants_[variant].pages;
  if (from.empty() || to.empty()) return 0;
  if (page >= from.size()) page = (uint32_t)from.size() - 1;
  uint32_t anchor = from[page].anchor;

  // Anchors are non-decreasing, so binary search for the last page at or
  // before this anchor.
  uint32_t lo = 0;
  uint32_t hi = (uint32_t)to.size();  // first page known to be past `anchor`
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (to[mid].anchor <= anchor) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo > 0 ? lo - 1 : 0;
}

void PageData::clear() {
  isImage = false;
  text = "";
  width = 0;
  height = 0;
  flags = 0;
  raw.clear();
}

bool Book::loadPage(uint32_t index, PageData &out) {
  out.clear();
  if (!open_ || activeVariant_ >= variants_.size()) return false;
  const std::vector<PageRef> &pages = variants_[activeVariant_].pages;
  if (index >= pages.size()) return false;

  uint32_t start = pages[index].offset;
  uint32_t len = pages[index].length;
  if (len == 0 || start + len > fileSize_) return false;

  fs::File f = LittleFS.open(bookPath(slug_).c_str(), "r");
  if (!f) return false;
  if (!f.seek(start, SeekSet)) {
    f.close();
    return false;
  }

  if (len >= MPG1_IMAGE_HEADER_LEN) {
    uint8_t hdr[MPG1_IMAGE_HEADER_LEN];
    if (f.read(hdr, MPG1_IMAGE_HEADER_LEN) != (int)MPG1_IMAGE_HEADER_LEN) {
      f.close();
      return false;
    }
    if (hdr[0] == 0x01 && hdr[1] == 'I' && hdr[2] == 'M' && hdr[3] == 'G') {
      uint16_t w = (uint16_t)((uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8));
      uint16_t h = (uint16_t)((uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8));
      uint32_t stride = ((uint32_t)w + 7) / 8;
      uint32_t need = stride * (uint32_t)h;
      if (w == 0 || h == 0 || need > MPG1_MAX_IMAGE_BYTES ||
          need != len - MPG1_IMAGE_HEADER_LEN) {
        f.close();
        return false;
      }
      out.raw.assign(need, 0);
      int got = f.read(out.raw.data(), need);
      f.close();
      if (got != (int)need) {
        out.clear();
        return false;
      }
      out.isImage = true;
      out.width = w;
      out.height = h;
      out.flags = hdr[8];
      return true;
    }
    // Not an image page after all, so rewind and read it as text.
    if (!f.seek(start, SeekSet)) {
      f.close();
      return false;
    }
  }

  std::vector<char> buf(len + 1, 0);
  int got = f.read((uint8_t *)buf.data(), len);
  f.close();
  if (got <= 0) return false;
  buf[got] = 0;
  out.text = String(buf.data());
  return true;
}

String Book::page(uint32_t index) {
  PageData data;
  if (!loadPage(index, data) || data.isImage) return String();
  return data.text;
}

namespace books {

// Every entry is opened and parsed from scratch here, so a file that was
// replaced since the last scan (news.pgs after a sync, most obviously) is read
// as it is now, and one that fails to parse is quietly left out of the list
// rather than taking the menu down with it.
std::vector<BookEntry> list() {
  std::vector<BookEntry> out;

  fs::File dir = LittleFS.open(BOOKS_DIR, "r");
  if (!dir || !dir.isDirectory()) return out;

  fs::File entry = dir.openNextFile();
  while (entry) {
    String name = entry.name();
    // Some FS implementations return a full path; keep only the basename.
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    uint32_t fileBytes = (uint32_t)entry.size();
    entry.close();

    if (name.endsWith(".pgs")) {
      String slug = name.substring(0, name.length() - 4);
      // The daily numbers feed is a JSON file drawn by its own view since the
      // grid version; a numbers.pgs is a leftover of the text version and is
      // not a book any more. A slug with a leading dot is a book upload being
      // assembled (BOOK_UPLOAD_STAGE_SLUG), or the leftover of one that died.
      if (slug == METRICS_SLUG || slug.startsWith(".")) {
        entry = dir.openNextFile();
        continue;
      }
      Book b;
      if (b.open(slug)) {
        BookEntry e;
        e.slug = slug;
        e.title = b.title();
        e.fileBytes = fileBytes;
        e.variantCount = b.variantCount();
        e.needsReformat = b.needsReformat();
        uint8_t variant = 0;
        e.savedPage = loadPosition(slug, &variant);
        if (variant >= e.variantCount) variant = 0;
        b.setVariant(variant);
        e.savedVariant = variant;
        e.pageCount = b.pageCount();
        if (e.savedPage >= e.pageCount) e.savedPage = 0;
        out.push_back(e);
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return out;
}

uint32_t loadPosition(const String &slug, uint8_t *variant) {
  if (variant) *variant = 0;
  fs::File f = LittleFS.open(posPath(slug).c_str(), "r");
  if (!f) return 0;
  uint8_t b[8];
  uint32_t page = 0;
  int got = f.read(b, 8);
  if (got >= 4) {
    page = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
  }
  // A 4 byte file predates text size switching and means variant 0.
  if (got == 8 && variant) {
    uint32_t v = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                 ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    if (v < MPG2_MAX_VARIANTS) *variant = (uint8_t)v;
  }
  f.close();
  return page;
}

void savePosition(const String &slug, uint32_t page, uint8_t variant) {
  fs::File f = LittleFS.open(posPath(slug).c_str(), "w");
  if (!f) return;
  uint8_t b[8] = {(uint8_t)(page & 0xFF), (uint8_t)((page >> 8) & 0xFF),
                  (uint8_t)((page >> 16) & 0xFF), (uint8_t)((page >> 24) & 0xFF),
                  variant, 0, 0, 0};
  f.write(b, 8);
  f.close();
}

String loadCurrentSlug() {
  fs::File f = LittleFS.open(CURRENT_FILE, "r");
  if (!f) return String();
  String s = f.readStringUntil('\n');
  f.close();
  s.trim();
  return s;
}

void saveCurrentSlug(const String &slug) {
  fs::File f = LittleFS.open(CURRENT_FILE, "w");
  if (!f) return;
  f.print(slug);
  f.close();
}

void activeGrid(uint8_t fontId, int &cols, int &rows) {
  if (fontId == BOOK_FONT_PROFONT29) {
    cols = TEXT_LARGE_COLS;
    rows = TEXT_LARGE_ROWS;
  } else {
    cols = TEXT_COLS;
    rows = TEXT_ROWS;
  }
}

bool writeGridExt(fs::File &out, uint8_t count, const uint8_t *colsRows) {
  uint8_t head[5];
  memcpy(head, BOOK_GRID_EXT_MAGIC, 4);
  head[4] = count;
  if (out.write(head, 5) != 5) return false;
  size_t n = 2 * (size_t)count;
  return out.write(colsRows, n) == n;
}

bool needsReformat(const String &slug) {
  Book b;
  if (!b.open(slug)) return false;
  return b.needsReformat();
}

}  // namespace books

// ------------------------------------------------------------- reformat
//
// One pass over the source variant's pages. Text pages go through a Rejoiner
// back into paragraphs, and every paragraph goes to one Pipeline per output
// variant at once, so the book is read once whatever the variant count. Each
// pipeline's pages are appended to one shared blob scratch file as they come,
// and their page table entries to a per variant scratch file, so RAM stays at
// one paragraph plus a page, whatever the book's length. Image pages are not
// copied until the end: they are listed by their offset in the source file and
// stored once in the new book, shared by every variant.
//
// New book layout (MPG2): header and page tables, grid extension, the image
// blobs, then the text blobs in the order they were paginated.

namespace {

#define REFORMAT_STAGE_SLUG ".reformat"     // leading dot: books::list() skips it
#define REFORMAT_BLOB_FILE "/reformat.blob"  // text page blobs, all variants
#define REFORMAT_META_PREFIX "/reformat.m"   // + variant index: page entries

// Space that must still be free once the new book and its scratch files are
// all on flash, on top of the estimate. The same reserve a book upload keeps.
const uint32_t REFORMAT_MIN_FREE_BYTES = BOOK_UPLOAD_MIN_FREE_BYTES;
// While writing, stop well before the filesystem fills: a full littlefs panics
// (see news_sync.cpp) instead of failing the write.
const uint32_t REFORMAT_FLOOR_BYTES = 96UL * 1024UL;
// How often free space is re-checked while writing.
const uint32_t REFORMAT_CHECK_EVERY = 16UL * 1024UL;
const uint32_t REFORMAT_PROGRESS_MS = 1500;
// A runaway re-pagination (a grid far smaller than the source's) is a failure,
// not a book. Lives in a scratch file, not RAM, so this only bounds flash.
const uint32_t REFORMAT_MAX_PAGES = 50000;
// Page table entry flag in the scratch files: the offset field is an index
// into the image list, not an offset into the blob scratch file.
const uint32_t IMAGE_ENTRY = 0x80000000UL;

void feedWdt() { esp_task_wdt_reset(); }

uint32_t fsFree() {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  return total > used ? (uint32_t)(total - used) : 0;
}

String metaPath(uint8_t v) { return String(REFORMAT_META_PREFIX) + String(v); }

void putU32(uint8_t *b, uint32_t v) {
  b[0] = (uint8_t)(v & 0xFF);
  b[1] = (uint8_t)((v >> 8) & 0xFF);
  b[2] = (uint8_t)((v >> 16) & 0xFF);
  b[3] = (uint8_t)((v >> 24) & 0xFF);
}

void removeScratch(uint8_t variants) {
  LittleFS.remove(REFORMAT_BLOB_FILE);
  for (uint8_t v = 0; v < variants; v++) LittleFS.remove(metaPath(v).c_str());
  LittleFS.remove(bookPath(REFORMAT_STAGE_SLUG).c_str());
}

bool isImageBlob(const uint8_t *head, uint32_t len) {
  return len >= MPG1_IMAGE_HEADER_LEN && head[0] == 0x01 && head[1] == 'I' &&
         head[2] == 'M' && head[3] == 'G';
}

struct ImageRef {
  uint32_t srcOffset;
  uint32_t length;
};

// Appends `len` bytes, re-checking free space every REFORMAT_CHECK_EVERY bytes
// written through it. False on a short write or when space runs low.
class GuardedWriter {
 public:
  explicit GuardedWriter(uint32_t *sinceCheck) : since_(sinceCheck) {}
  bool write(fs::File &f, const uint8_t *data, size_t len) {
    *since_ += (uint32_t)len;
    if (*since_ >= REFORMAT_CHECK_EVERY) {
      *since_ = 0;
      if (fsFree() < REFORMAT_FLOOR_BYTES) {
        lowSpace = true;
        return false;
      }
    }
    return f.write(data, len) == len;
  }
  bool lowSpace = false;

 private:
  uint32_t *since_;
};

// Copies [offset, offset + len) of `src` to `out`.
bool copyRange(fs::File &src, uint32_t offset, uint32_t len, fs::File &out,
               GuardedWriter &w) {
  if (!src.seek(offset, SeekSet)) return false;
  uint8_t buf[512];
  while (len > 0) {
    size_t want = len < sizeof(buf) ? len : sizeof(buf);
    int got = src.read(buf, want);
    if (got != (int)want) return false;
    if (!w.write(out, buf, want)) return false;
    len -= (uint32_t)want;
    feedWdt();
  }
  return true;
}

}  // namespace

namespace books {

bool reformat(const String &slug, ReformatProgress progress, String *err) {
  String dummy;
  if (!err) err = &dummy;

  Book src;
  if (!src.open(slug)) {
    *err = "cannot open: " + src.errorText();
    return false;
  }
  if (!src.needsReformat()) return true;  // already fits this panel

  // Read the text from the variant the reader was in, so the saved page can
  // be located in the text stream. Every variant holds the same text.
  uint8_t savedVariant = 0;
  uint32_t savedPage = loadPosition(slug, &savedVariant);
  if (savedVariant >= src.variantCount()) savedVariant = 0;
  src.setVariant(savedVariant);
  const uint32_t srcPages = src.pageCount();
  if (savedPage >= srcPages) savedPage = 0;
  const int srcCols = src.cols() > 0 ? src.cols() : LEGACY_COLS;

  // One output variant per distinct font the book had, in file order, each on
  // this panel's grid for that font.
  uint8_t fonts[MPG2_MAX_VARIANTS];
  uint8_t outCount = 0;
  for (uint8_t v = 0; v < src.variantCount(); v++) {
    src.setVariant(v);
    uint8_t font = src.fontId();
    bool seen = false;
    for (uint8_t k = 0; k < outCount; k++) seen = seen || fonts[k] == font;
    if (!seen && outCount < MPG2_MAX_VARIANTS) fonts[outCount++] = font;
  }
  src.setVariant(savedVariant);
  const uint8_t savedFont = src.fontId();
  uint8_t newVariant = 0;
  for (uint8_t k = 0; k < outCount; k++) {
    if (fonts[k] == savedFont) newVariant = k;
  }

  // Size the job: text and distinct image bytes of the source variant.
  uint32_t textBytes = 0;
  uint32_t imageBytes = 0;
  {
    fs::File f = LittleFS.open(bookPath(slug).c_str(), "r");
    if (!f) {
      *err = "cannot read book";
      return false;
    }
    uint32_t lastImage = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < srcPages; i++) {
      uint32_t off = 0;
      uint32_t len = 0;
      if (!src.pageSpan(i, off, len)) continue;
      uint8_t head[4] = {0, 0, 0, 0};
      if (len >= MPG1_IMAGE_HEADER_LEN && f.seek(off, SeekSet)) f.read(head, 4);
      if (isImageBlob(head, len)) {
        // Shared blobs repeat their offset; count each once. Pages of one
        // variant never repeat an image out of order in practice, so
        // comparing with the last one is enough for an estimate.
        if (off != lastImage) imageBytes += len;
        lastImage = off;
      } else {
        textBytes += len;
      }
      if ((i & 63) == 0) feedWdt();
    }
    f.close();
  }

  // New text: about the old text per variant (a line break becomes a space or
  // the other way round), plus slack for the extra blank lines a narrower grid
  // can add. Page tables: generously one page per 256 bytes of text. The blob
  // scratch file holds the text again until the end, and littlefs rounds every
  // file up to whole blocks.
  const uint32_t newText = (textBytes + textBytes / 8 + 4096) * outCount;
  const uint32_t tables = (textBytes / 256 + srcPages + 64) * 12 * outCount;
  const uint32_t needed =
      newText * 2 + imageBytes + tables * 2 + 64UL * 1024UL;
  const uint32_t freeNow = fsFree();
  if (freeNow < needed + REFORMAT_MIN_FREE_BYTES) {
    *err = "storage full, need " +
           String((unsigned long)((needed + REFORMAT_MIN_FREE_BYTES) / 1024)) +
           "k, " + String((unsigned long)(freeNow / 1024)) + "k free";
    return false;
  }

  LittleFS.mkdir(BOOKS_DIR);
  removeScratch(MPG2_MAX_VARIANTS);

  fs::File in = LittleFS.open(bookPath(slug).c_str(), "r");
  fs::File blobs = LittleFS.open(REFORMAT_BLOB_FILE, "w");
  fs::File metas[MPG2_MAX_VARIANTS];
  bool filesOk = in && blobs;
  for (uint8_t k = 0; k < outCount && filesOk; k++) {
    metas[k] = LittleFS.open(metaPath(k).c_str(), "w");
    filesOk = (bool)metas[k];
  }
  auto closeAll = [&]() {
    if (in) in.close();
    if (blobs) blobs.close();
    for (uint8_t k = 0; k < outCount; k++) {
      if (metas[k]) metas[k].close();
    }
  };
  if (!filesOk) {
    closeAll();
    removeScratch(outCount);
    *err = "scratch open";
    return false;
  }

  uint32_t sinceCheck = 0;
  GuardedWriter writer(&sinceCheck);
  bool failed = false;
  String why;
  uint32_t blobPos = 0;
  uint32_t counts[MPG2_MAX_VARIANTS] = {0};
  std::vector<ImageRef> images;

  auto addEntry = [&](uint8_t k, uint32_t off, uint32_t len, uint32_t anchor) {
    if (counts[k] >= REFORMAT_MAX_PAGES) {
      failed = true;
      why = "too many pages";
      return;
    }
    uint8_t e[12];
    putU32(e, off);
    putU32(e + 4, len);
    putU32(e + 8, anchor);
    if (!writer.write(metas[k], e, 12)) {
      failed = true;
      why = writer.lowSpace ? "storage full" : "table write";
      return;
    }
    counts[k]++;
  };

  // One pipeline per output variant; each finished page goes straight to the
  // blob scratch file and its entry to that variant's table.
  std::vector<std::unique_ptr<textpage::Pipeline>> pipes;
  for (uint8_t k = 0; k < outCount; k++) {
    int cols = 0;
    int rows = 0;
    activeGrid(fonts[k], cols, rows);
    pipes.emplace_back(new textpage::Pipeline(
        cols, rows, [&, k](const std::vector<std::string> &lines) {
          if (failed) return;
          std::string joined;
          for (size_t i = 0; i < lines.size(); i++) {
            if (i) joined.push_back('\n');
            joined += lines[i];
          }
          std::string utf8 = textpage::encodeUtf8(joined);
          // The MPG2 parser rejects an empty entry. Only a page holding no
          // text at all could be one, and the paragraph path never emits it.
          if (utf8.empty()) return;
          if (!writer.write(blobs, (const uint8_t *)utf8.data(), utf8.size())) {
            failed = true;
            why = writer.lowSpace ? "storage full" : "page write";
            return;
          }
          addEntry(k, blobPos, (uint32_t)utf8.size(), pipes[k]->pageAnchor());
          blobPos += (uint32_t)utf8.size();
          feedWdt();
        }));
  }

  textpage::Rejoiner rejoin(srcCols, [&](const std::string &para) {
    for (uint8_t k = 0; k < outCount && !failed; k++) pipes[k]->paragraph(para);
  });

  uint32_t savedAnchor = 0;
  uint32_t lastReport = millis();
  if (progress) progress(0, srcPages);
  std::vector<uint8_t> buf;

  for (uint32_t i = 0; i < srcPages && !failed; i++) {
    uint32_t off = 0;
    uint32_t len = 0;
    if (!src.pageSpan(i, off, len) || !in.seek(off, SeekSet)) {
      failed = true;
      why = "page table";
      break;
    }
    uint8_t head[4] = {0, 0, 0, 0};
    if (len >= MPG1_IMAGE_HEADER_LEN && in.read(head, 4) != 4) {
      failed = true;
      why = "page read";
      break;
    }
    if (isImageBlob(head, len)) {
      // An image ends the run of text before it, as in txt2book.
      rejoin.breakRun();
      for (uint8_t k = 0; k < outCount && !failed; k++) pipes[k]->endRun();
      if (failed) break;
      uint32_t index = 0;
      while (index < images.size() && images[index].srcOffset != off) index++;
      if (index == images.size()) images.push_back(ImageRef{off, len});
      uint32_t anchor = rejoin.streamPos();
      for (uint8_t k = 0; k < outCount && !failed; k++) {
        addEntry(k, IMAGE_ENTRY | index, len, anchor);
      }
      if (i == savedPage) savedAnchor = anchor;
    } else {
      if (!in.seek(off, SeekSet)) {
        failed = true;
        why = "page read";
        break;
      }
      buf.resize(len);
      if (len > 0 && in.read(buf.data(), len) != (int)len) {
        failed = true;
        why = "page read";
        break;
      }
      rejoin.page(textpage::decodeLatin1(buf.data(), len));
      if (i == savedPage) savedAnchor = rejoin.pageAnchor();
    }
    feedWdt();
    if (progress && millis() - lastReport >= REFORMAT_PROGRESS_MS) {
      lastReport = millis();
      progress(i + 1, srcPages);
    }
  }
  if (!failed) {
    rejoin.finish();
    for (uint8_t k = 0; k < outCount && !failed; k++) pipes[k]->endRun();
  }
  pipes.clear();
  buf.clear();
  buf.shrink_to_fit();
  blobs.close();
  for (uint8_t k = 0; k < outCount; k++) metas[k].close();

  if (!failed) {
    for (uint8_t k = 0; k < outCount; k++) {
      if (counts[k] == 0) {
        failed = true;
        why = "no pages";
      }
    }
  }
  if (failed) {
    closeAll();
    removeScratch(outCount);
    *err = why;
    return false;
  }

  // ---- assemble the new book under the staged name
  std::string title(src.title().c_str());
  if (title.size() > MAX_TITLE_LEN) title.resize(MAX_TITLE_LEN);
  uint32_t headerLen = 4 + 1 + 2 + (uint32_t)title.size();
  for (uint8_t k = 0; k < outCount; k++) headerLen += 1 + 4 + 12 * counts[k];
  headerLen += bookGridExtLen(outCount);
  std::vector<uint32_t> imageAt(images.size());
  uint32_t imagesTotal = 0;
  for (size_t j = 0; j < images.size(); j++) {
    imageAt[j] = headerLen + imagesTotal;
    imagesTotal += images[j].length;
  }
  const uint32_t textBase = headerLen + imagesTotal;

  fs::File out = LittleFS.open(bookPath(REFORMAT_STAGE_SLUG).c_str(), "w");
  if (!out) {
    closeAll();
    removeScratch(outCount);
    *err = "book open";
    return false;
  }
  bool ok = true;
  {
    uint8_t hdr[7];
    memcpy(hdr, "MPG2", 4);
    hdr[4] = outCount;
    hdr[5] = (uint8_t)(title.size() & 0xFF);
    hdr[6] = (uint8_t)((title.size() >> 8) & 0xFF);
    ok = writer.write(out, hdr, 7) &&
         writer.write(out, (const uint8_t *)title.data(), title.size());
  }
  for (uint8_t k = 0; k < outCount && ok; k++) {
    uint8_t vh[5];
    vh[0] = fonts[k];
    putU32(vh + 1, counts[k]);
    ok = writer.write(out, vh, 5);
    fs::File m = LittleFS.open(metaPath(k).c_str(), "r");
    if (!m) ok = false;
    uint8_t chunk[12 * 32];
    while (ok) {
      int got = m.read(chunk, sizeof(chunk));
      if (got <= 0) break;
      got -= got % 12;
      for (int e = 0; e < got; e += 12) {
        uint32_t field = decodeU32(chunk + e);
        uint32_t at = (field & IMAGE_ENTRY)
                          ? imageAt[field & ~IMAGE_ENTRY]
                          : textBase + field;
        putU32(chunk + e, at);
      }
      ok = writer.write(out, chunk, (size_t)got);
      feedWdt();
    }
    if (m) m.close();
  }
  if (ok) {
    uint8_t grid[2 * MPG2_MAX_VARIANTS];
    for (uint8_t k = 0; k < outCount; k++) {
      int cols = 0;
      int rows = 0;
      activeGrid(fonts[k], cols, rows);
      grid[2 * k] = (uint8_t)cols;
      grid[2 * k + 1] = (uint8_t)rows;
    }
    ok = writeGridExt(out, outCount, grid);
  }
  for (size_t j = 0; j < images.size() && ok; j++) {
    ok = copyRange(in, images[j].srcOffset, images[j].length, out, writer);
  }
  if (ok) {
    fs::File b = LittleFS.open(REFORMAT_BLOB_FILE, "r");
    ok = (bool)b && copyRange(b, 0, blobPos, out, writer);
    if (b) b.close();
  }
  out.close();
  closeAll();
  LittleFS.remove(REFORMAT_BLOB_FILE);
  for (uint8_t k = 0; k < outCount; k++) LittleFS.remove(metaPath(k).c_str());
  if (!ok) {
    LittleFS.remove(bookPath(REFORMAT_STAGE_SLUG).c_str());
    *err = writer.lowSpace ? "storage full" : "book write";
    return false;
  }

  // The new book has to open, and fit, before it replaces the old one.
  uint32_t newPage = 0;
  {
    Book check;
    if (!check.open(REFORMAT_STAGE_SLUG) || check.needsReformat() ||
        check.variantCount() != outCount) {
      LittleFS.remove(bookPath(REFORMAT_STAGE_SLUG).c_str());
      *err = "did not verify";
      return false;
    }
    // Reading position: the last page at or before the old page's anchor, in
    // the same text size. Anchors never go backwards, so stop at the first
    // page past it.
    check.setVariant(newVariant);
    uint32_t lo = 0;
    uint32_t hi = check.pageCount();
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (check.anchorOf(mid) <= savedAnchor) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    newPage = lo > 0 ? lo - 1 : 0;
  }

  // Swap it in. There is no atomic replace on LittleFS here, so the old file
  // goes first, as the upload commit does.
  LittleFS.remove(bookPath(slug).c_str());
  if (!LittleFS.rename(bookPath(REFORMAT_STAGE_SLUG).c_str(),
                       bookPath(slug).c_str())) {
    *err = "rename failed";
    return false;
  }
  savePosition(slug, newPage, newVariant);
  if (progress) progress(srcPages, srcPages);
  return true;
}

}  // namespace books

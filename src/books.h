#pragma once

#include <Arduino.h>
#include <FS.h>

#include <functional>
#include <vector>

// MPG1 and MPG2 (.pgs) book reader over LittleFS.
//
// MPG1 holds one pagination of a book. MPG2 holds the same book paginated for
// more than one text size ("variants"), sharing a single copy of every image
// blob, and gives every page an anchor into the book's character stream so a
// reading position survives a size change. The two formats are told apart by
// the 4 byte magic.
//
// Grid identity. A variant was paginated for one text grid: (font id, cols,
// rows). The grid depends on the panel (see panel::applyGeometry), so each
// file records the grid of each variant in an optional grid extension that
// sits in the gap between the page table(s) and the first page blob:
//     4 bytes 'GRID', u8 variant_count (equal to the header's, 1 for MPG1),
//     then per variant, in header order, u8 cols and u8 rows.
// Every reader addresses pages by absolute offset, so firmware and tools that
// predate the extension never look at that gap and still read the file. It
// only counts when every page starts after it. A file without it (every book
// made before multi panel support) is read as the panel 0 grids: font 0 is
// 54x37, font 1 is 40x27. tools/pdf2book.py writes the same extension
// (GRID_EXT_MAGIC there).
//
// When a book has no variant on the active panel's grids it still opens (its
// pages draw, clipped), needsReformat() says so, and books::reformat() rebuilds
// it for this panel from its own text.

#define BOOK_GRID_EXT_MAGIC "GRID"
static inline uint32_t bookGridExtLen(uint8_t variantCount) {
  return 4 + 1 + 2 * (uint32_t)variantCount;
}

// A loaded page: either UTF-8 text or a 1 bit bitmap. The bitmap payload
// lives in `raw`, so keep the PageData alive for as long as bits() is used.
struct PageData {
  bool isImage = false;

  String text;  // valid when isImage is false

  uint16_t width = 0;   // stored bitmap size, valid when isImage is true
  uint16_t height = 0;
  uint8_t flags = 0;  // bit0 (MPG1_IMAGE_FLAG_DOUBLE): render each pixel 2x2

  std::vector<uint8_t> raw;  // raster, stride = (width + 7) / 8

  const uint8_t *bits() const { return raw.empty() ? nullptr : raw.data(); }
  void clear();
};

// One page table entry. Lengths are explicit because image blobs are shared
// between variants, so a page does not always end where the next one begins.
struct PageRef {
  uint32_t offset = 0;
  uint32_t length = 0;
  uint32_t anchor = 0;
};

// One pagination of a book: a font id (see BOOK_FONT_* in config.h), the grid
// it was paginated on, and its page table.
struct BookVariant {
  uint8_t fontId = 0;
  uint8_t cols = 0;
  uint8_t rows = 0;
  std::vector<PageRef> pages;
};

struct BookEntry {
  String slug;
  String title;
  uint32_t pageCount = 0;
  uint32_t savedPage = 0;     // zero based, in savedVariant
  uint8_t savedVariant = 0;
  uint8_t variantCount = 1;
  uint32_t fileBytes = 0;     // size of the .pgs on the filesystem
  bool needsReformat = false;  // made for another panel, see books::reformat
};

class Book {
 public:
  // Opens /books/<slug>.pgs and parses the header. Returns false and sets
  // errorText() on any failure; the object is then closed and unusable.
  // Only the variants whose grid matches the active panel are kept, so the
  // variant indices below count those. A book with none keeps every variant
  // and reports needsReformat().
  bool open(const String &slug);
  void close();

  bool isOpen() const { return open_; }
  const String &slug() const { return slug_; }
  const String &title() const { return title_; }
  const String &errorText() const { return error_; }

  // Page count, font and grid of the active variant.
  uint32_t pageCount() const;
  uint8_t fontId() const;
  uint8_t cols() const;
  uint8_t rows() const;

  // True when no variant was paginated on the active panel's grids: the book
  // was made for another screen. books::reformat() fixes that.
  bool needsReformat() const { return needsReformat_; }

  // Where page `index` of the active variant sits in the file. False when out
  // of range.
  bool pageSpan(uint32_t index, uint32_t &offset, uint32_t &length) const;

  // Character-stream position of `page`'s first character in the active
  // variant (MPG2). 0 for MPG1 books, which carry no anchors, and past the end.
  uint32_t anchorOf(uint32_t page) const;

  uint8_t variantCount() const { return (uint8_t)variants_.size(); }
  uint8_t activeVariant() const { return activeVariant_; }

  // Switches the active variant without touching any page number. Returns
  // false if the index is out of range.
  bool setVariant(uint8_t variant);

  // Position mapping for a size change: the largest page index in `variant`
  // whose anchor is at or before the anchor of `page` in the active variant.
  uint32_t mapPage(uint32_t page, uint8_t variant) const;

  // Returns page text (lines separated by \n), empty on failure. Image pages
  // come back empty here; use loadPage() to handle both kinds.
  String page(uint32_t index);

  // Loads a page of either kind into `out`. Returns false on any failure.
  // `out` is reused across calls, which keeps the raster vector's capacity.
  bool loadPage(uint32_t index, PageData &out);

 private:
  bool parseMpg1(fs::File &f);
  bool parseMpg2(fs::File &f);
  void readGridExt(fs::File &f);
  void keepMatchingVariants();

  bool open_ = false;
  String slug_;
  String title_;
  String error_;
  uint32_t fileSize_ = 0;
  uint8_t activeVariant_ = 0;
  bool needsReformat_ = false;
  std::vector<BookVariant> variants_;
};

namespace books {

// Lists every /books/*.pgs, reading title, page count, and saved position.
std::vector<BookEntry> list();

// Reading position in /books/<slug>.pos: u32 LE page, then u32 LE variant.
// A legacy 4 byte file is read as page with variant 0.
uint32_t loadPosition(const String &slug, uint8_t *variant = nullptr);
void savePosition(const String &slug, uint32_t page, uint8_t variant = 0);

// Current book slug in /current.txt.
String loadCurrentSlug();
void saveCurrentSlug(const String &slug);

// The active panel's text grid for a book font id (TEXT_COLS/TEXT_ROWS for
// font 0, TEXT_LARGE_COLS/TEXT_LARGE_ROWS for font 1; an unknown id gets font
// 0's). Read at call time: these are runtime globals since multi panel.
void activeGrid(uint8_t fontId, int &cols, int &rows);

// Writes the grid extension for `count` variants; `colsRows` holds cols, rows
// per variant in header order. For the book writers (news_sync, reformat).
bool writeGridExt(fs::File &out, uint8_t count, const uint8_t *colsRows);

// True when /books/<slug>.pgs opens but has no variant on the active panel's
// grids. False for a book that matches, and for one that does not open.
bool needsReformat(const String &slug);

// Progress for reformat(): source pages done out of the total. Called at the
// start, then at most every REFORMAT_PROGRESS_MS or so, and once at the end.
using ReformatProgress = std::function<void(uint32_t done, uint32_t total)>;

// Re-paginates /books/<slug>.pgs for the active panel from the book's own
// text, replacing the file. The text of one variant is joined back into
// paragraphs (textpage::Rejoiner) and paginated with the on-device paginator
// on the current grid of every font the book had, so an MPG1 book stays one
// size and a dual size book stays dual size; the result is always MPG2 with
// the grid extension. Image pages stay image pages, in order, stored once (the
// renderer scales an oversize one down). The reading position in .pos is
// carried over by stream anchor, in the same text size.
//
// Built next to the original under a staged name and only swapped in once it
// opens and matches, so a failure leaves the book as it was. Checks free space
// up front (the new book and its scratch files on top of the old book, plus
// BOOK_UPLOAD_MIN_FREE_BYTES) and keeps checking while it writes, because a
// full LittleFS panics instead of failing a write. Feeds the task watchdog as
// it goes. Blocking: seconds for a short book, longer for a novel.
//
// Any Book object open on this slug holds a stale page table afterwards and
// must be reopened. Returns false and sets *err (short, for the screen) on
// failure.
bool reformat(const String &slug, ReformatProgress progress, String *err);

}  // namespace books

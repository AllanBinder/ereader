#pragma once

// On-device text paginator: the C++ twin of the host pipeline in
// tools/pdf2book.py (normalize_text, pages_to_paragraphs, wrap_paragraph_spans,
// paginate_spans) as tools/txt2book.py drives it for a plain text input with no
// form feeds and no figures.
//
// The two implementations have to agree exactly or a book paginated on the
// device would render differently from one paginated on the host. See
// tools/paginate_test.cpp and tools/check_paginator.py: that test compiles this
// file natively and diffs its pages against the Python pipeline character for
// character.
//
// Deliberately free of Arduino, FreeRTOS and LittleFS so it can be built with a
// plain g++ on the host. Everything is streaming: bytes go in a chunk at a
// time, finished pages come out through a callback, and nothing larger than one
// paragraph is ever held in memory.
//
// Character model. Input is UTF-8. Normalization transliterates the codepoints
// the host converter knows about, keeps ASCII plus the Latin-1 supplement, and
// drops everything else, exactly like pdf2book.normalize_text. The result is
// held as one byte per character (Latin-1), which is what makes column counting
// match Python's, where a column is a codepoint and not a byte. Page blobs are
// encoded back to UTF-8 on the way out, so the bytes in the .pgs file are the
// same bytes the host writer would have produced.

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace textpage {

// A finished page: at most `rows` lines, each at most `cols` characters, in the
// Latin-1 character model described above. Use encodeUtf8() before writing.
using PageSink = std::function<void(const std::vector<std::string> &lines)>;

// A paragraph longer than this is flushed early rather than buffered without
// limit. Real prose does not come close; this only fires on pathological input
// (a whole file with no blank line in it), and when it does the on-device
// pagination stops matching the host for that book. Reported by truncated().
static const size_t PARAGRAPH_MAX_CHARS = 32768;

// Latin-1 characters to UTF-8 bytes, the encoding the page blobs use on disk.
std::string encodeUtf8(const std::string &latin1);

class Pipeline {
 public:
  Pipeline(int cols, int rows, PageSink sink);

  // Feed raw UTF-8 input. Safe to call with a partial multi-byte sequence at
  // the end of a chunk; the decoder carries state across calls.
  void feed(const uint8_t *data, size_t len);

  // Flushes the last paragraph and the last page. Emits one empty page if the
  // input held no text at all, which is what the host writer does. Calling it
  // twice is harmless.
  void finish();

  uint32_t pageCount() const { return pages_; }
  bool truncated() const { return truncated_; }

  // Re-pagination entry points (books::reformat). paragraph() adds one
  // already clean paragraph in the Latin-1 model, skipping the line cleaning
  // that feed() does (strip, page number filter, dehyphenation), so text that
  // was paginated once comes out exactly as the host would paginate the same
  // paragraphs. Do not mix it with feed() in one pipeline.
  void paragraph(const std::string &latin1);
  // Ends a run of text the way an image page ends one in
  // txt2book.pages_for_grid: the partial page is emitted, and the next
  // paragraph starts a fresh page with no blank line ahead of it. The stream
  // position carries on, so anchors stay comparable across grids.
  void endRun();
  // Position in the logical text stream after the last paragraph added. An
  // image page inserted here carries this as its anchor.
  uint32_t streamPos() const { return paraBase_; }

  // Anchor of the page currently being handed to the sink: the cumulative
  // character count into the book's logical text stream at the start of that
  // page, which is what an MPG2 page table stores. Only meaningful from inside
  // the sink callback. The stream is the cleaned paragraphs concatenated with
  // one separator character each, so the same word carries the same anchor on
  // every grid. Mirrors pdf2book.paginate_spans with base 0.
  uint32_t pageAnchor() const { return emitAnchor_; }

 private:
  void emitCodepoint(uint32_t cp);  // one decoded codepoint, normalized
  void pushChar(char c);        // a normalized character or '\n'
  void endLine();               // current line is complete
  void flushParagraph();        // paragraph buffer is complete
  void addParagraph(const std::string &para);
  void emitPage();

  int cols_;
  int rows_;
  PageSink sink_;

  // UTF-8 decoder state.
  uint32_t cp_ = 0;
  uint32_t min_ = 0;  // smallest codepoint this length may encode (overlong check)
  int need_ = 0;

  std::string line_;
  std::string para_;
  std::vector<std::string> current_;
  bool firstParagraph_ = true;
  bool finished_ = false;
  bool truncated_ = false;
  uint32_t pages_ = 0;

  // Anchor bookkeeping, mirroring paginate_spans' para_base / current_anchor.
  uint32_t paraBase_ = 0;
  uint32_t currentAnchor_ = 0;
  uint32_t emitAnchor_ = 0;
};

// UTF-8 page blob bytes to the Latin-1 character model. Page blobs only ever
// hold codepoints up to U+00FF, so anything above that (or malformed) is
// dropped rather than guessed at.
std::string decodeLatin1(const uint8_t *data, size_t len);

// Turns the pages of a book paginated on one grid back into the paragraphs
// they were made from, for re-pagination on another grid. Pages go in one at a
// time in reading order (lines separated by \n, Latin-1 model) and finished
// paragraphs come out through the sink, ready for Pipeline::paragraph().
//
// What the page format keeps, and so what this can recover:
//   - a blank line inside a page is a paragraph break, always;
//   - consecutive lines of one page belong to one paragraph, joined with one
//     space, except after a hard broken piece of an over long word (a line of
//     exactly `cols` characters with no space), which joins with nothing;
//   - across a page boundary the format is ambiguous: paginate_spans puts no
//     blank line at the top of a page, so a paragraph that ended on the last
//     row looks like one that runs on. If the next page's first word would
//     have fitted on the previous line, the wrap would have put it there, so
//     that is a paragraph break; otherwise it is read as a continuation, which
//     is right far more often than not (a paragraph end needs the last line
//     to be nearly full as well as on the last row).
// A word of exactly `cols` characters reads as a hard broken piece and joins
// the next word without a space. Both misreadings are rare and cosmetic.
class Rejoiner {
 public:
  using ParagraphSink = std::function<void(const std::string &para)>;

  // `cols` is the column count of the grid the pages were made on.
  Rejoiner(int cols, ParagraphSink sink);

  // One text page. pageAnchor() is valid once this returns.
  void page(const std::string &latin1);
  // An image page (or anything else that ends a run of text): the paragraph
  // in progress is complete.
  void breakRun();
  // End of the book.
  void finish() { breakRun(); }

  // Stream position of the first character of the last page fed, in the same
  // stream Pipeline anchors count in, provided the paragraphs went into the
  // pipeline unchanged.
  uint32_t pageAnchor() const { return pageAnchor_; }
  // Stream position after every paragraph flushed so far.
  uint32_t streamPos() const { return paraBase_; }

 private:
  void flush();
  bool hardBroken(const std::string &line) const;
  bool continues(const std::string &prev, const std::string &next) const;

  int cols_;
  ParagraphSink sink_;
  std::string para_;
  std::string last_;  // last line added to para_
  uint32_t paraBase_ = 0;
  uint32_t pageAnchor_ = 0;
};

// Exposed for the host parity test.
namespace detail {

// One wrapped line plus the offset of its first character inside the
// paragraph. Two grids wrapping the same paragraph produce different lines but
// the same offsets for the same words, which is what makes anchors comparable
// across variants.
// Aggregate on purpose (no default member initializers), so it can be built
// with brace initialization under the C++11 the Arduino core compiles with.
struct LineSpan {
  std::string text;
  size_t offset;
};

bool isPageNumber(const std::string &stripped);
std::string strip(const std::string &s);
std::string dehyphenate(const std::string &s);
std::vector<LineSpan> wrapParagraphSpans(const std::string &para, int cols);
std::vector<std::string> wrapParagraph(const std::string &para, int cols);
}  // namespace detail

}  // namespace textpage

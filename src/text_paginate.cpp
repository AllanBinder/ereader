#include "text_paginate.h"

#include <string.h>

namespace textpage {
namespace {

// pdf2book.TRANSLIT, codepoint for codepoint. Keep this list and the Python
// dict in step: tools/check_paginator.py diffs the two implementations on a
// sample that exercises every entry.
struct Translit {
  uint32_t cp;
  const char *repl;
};

const Translit kTranslit[] = {
    {0x2018, "'"}, {0x2019, "'"},  {0x201A, "'"},   {0x201B, "'"},
    {0x201C, "\""}, {0x201D, "\""}, {0x201E, "\""}, {0x201F, "\""},
    {0x2032, "'"}, {0x2033, "\""},
    {0x2010, "-"}, {0x2011, "-"},  {0x2012, "-"},   {0x2013, "-"},
    {0x2014, "-"}, {0x2015, "-"},  {0x2212, "-"},
    {0x2026, "..."},
    {0x00A0, " "}, {0x2007, " "},  {0x202F, " "},   {0x2009, " "},
    {0x2002, " "}, {0x2003, " "},  {0x2004, " "},   {0x2005, " "},
    {0x2006, " "}, {0x200A, " "},  {0x3000, " "},
    {0x200B, ""},  {0x200C, ""},   {0x200D, ""},    {0xFEFF, ""},
    {0x00AD, ""},
    {0x2022, "*"}, {0x00B7, "*"},  {0x2043, "-"},
    {0xFB00, "ff"}, {0xFB01, "fi"}, {0xFB02, "fl"},
    {0xFB03, "ffi"}, {0xFB04, "ffl"}, {0xFB05, "st"}, {0xFB06, "st"},
    {0x0152, "OE"}, {0x0153, "oe"}, {0x00C6, "AE"}, {0x00E6, "ae"},
    {0x2039, "<"}, {0x203A, ">"},  {0x00AB, "\""},  {0x00BB, "\""},
    {0x2044, "/"}, {0x2215, "/"},
    {0x0009, " "},
};

const char *translitFor(uint32_t cp) {
  for (size_t i = 0; i < sizeof(kTranslit) / sizeof(kTranslit[0]); i++) {
    if (kTranslit[i].cp == cp) return kTranslit[i].repl;
  }
  return nullptr;
}

// pdf2book.renderable: ASCII printable plus the Latin-1 supplement.
bool renderable(uint32_t cp) {
  return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA1 && cp <= 0xFF);
}

bool isSpace(char c) {
  // The characters Python's str.strip() would remove from a normalized line.
  // Tabs are transliterated to spaces upstream and the other ASCII whitespace
  // is either a line break or unrenderable, so this only has to cover a few.
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

// pdf2book DEHYPH character classes. The Python source spells them as
// [A-Za-z<A-grave>-<y-diaeresis>] and [a-z<a-grave>-<y-diaeresis>], which are
// the literal ranges A-Z, a-z, U+00C0 to U+00FF and a-z, U+00E0 to U+00FF. Both
// of those Latin-1 ranges also swallow the multiplication and division signs;
// that is what the regex does, so match it rather than being clever.
bool dehyphLetter(char c) {
  unsigned char u = (unsigned char)c;
  return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u >= 0xC0;
}

bool dehyphLower(char c) {
  unsigned char u = (unsigned char)c;
  return (u >= 'a' && u <= 'z') || u >= 0xE0;
}

}  // namespace

std::string encodeUtf8(const std::string &latin1) {
  std::string out;
  out.reserve(latin1.size() + latin1.size() / 8);
  for (size_t i = 0; i < latin1.size(); i++) {
    unsigned char u = (unsigned char)latin1[i];
    if (u < 0x80) {
      out.push_back((char)u);
    } else {
      out.push_back((char)(0xC0 | (u >> 6)));
      out.push_back((char)(0x80 | (u & 0x3F)));
    }
  }
  return out;
}

namespace detail {

std::string strip(const std::string &s) {
  size_t a = 0;
  size_t b = s.size();
  while (a < b && isSpace(s[a])) a++;
  while (b > a && isSpace(s[b - 1])) b--;
  return s.substr(a, b - a);
}

// pdf2book.is_page_number on an already stripped line: a bare decimal, or the
// narrow BARE_NUMBER form (optional bracket, roman numeral or digits, optional
// bracket) capped at 8 characters and required to hold at least one digit.
bool isPageNumber(const std::string &s) {
  if (s.empty()) return false;

  bool allDigits = true;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] < '0' || s[i] > '9') {
      allDigits = false;
      break;
    }
  }
  if (allDigits) return true;

  size_t i = 0;
  const size_t n = s.size();
  if (s[i] == '[' || s[i] == '(') i++;
  while (i < n && isSpace(s[i])) i++;
  size_t start = i;
  bool anyDigit = false;
  while (i < n) {
    char c = s[i];
    bool digit = (c >= '0' && c <= '9');
    bool roman = strchr("ivxlcdmIVXLCDM", c) != nullptr && c != '\0';
    if (!digit && !roman) break;
    if (digit) anyDigit = true;
    i++;
  }
  if (i == start) return false;
  while (i < n && isSpace(s[i])) i++;
  if (i < n && (s[i] == ']' || s[i] == ')')) i++;
  if (i != n) return false;
  return anyDigit && s.size() <= 8;
}

// pdf2book DEHYPH: join a word broken across a line by a trailing hyphen.
std::string dehyphenate(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    if (s[i] == '-' && i + 1 < n && s[i + 1] == '\n' && i >= 2 &&
        dehyphLetter(s[i - 1]) && dehyphLetter(s[i - 2]) && i + 3 < n &&
        dehyphLower(s[i + 2]) && dehyphLower(s[i + 3])) {
      i += 2;  // drop the hyphen and the newline, keep the letters
      continue;
    }
    out.push_back(s[i]);
    i++;
  }
  return out;
}

// pdf2book.wrap_paragraph_spans. No hyphenation; a word longer than the column
// count is hard broken. Each line carries the offset of its first character
// inside the paragraph, which is what the anchors are built from.
std::vector<LineSpan> wrapParagraphSpans(const std::string &para, int cols) {
  std::vector<LineSpan> lines;
  std::string current;
  size_t currentStart = 0;
  const size_t ucols = (size_t)(cols > 0 ? cols : 1);

  size_t pos = 0;
  while (pos <= para.size()) {
    size_t sp = para.find(' ', pos);
    size_t wordStart = pos;
    std::string word = (sp == std::string::npos) ? para.substr(pos)
                                                 : para.substr(pos, sp - pos);
    pos = (sp == std::string::npos) ? para.size() + 1 : sp + 1;
    if (word.empty()) continue;

    size_t consumed = 0;
    while (word.size() > ucols) {
      if (!current.empty()) {
        lines.push_back(LineSpan{current, currentStart});
        current.clear();
      }
      lines.push_back(LineSpan{word.substr(0, ucols), wordStart + consumed});
      word = word.substr(ucols);
      consumed += ucols;
    }
    if (current.empty()) {
      current = word;
      currentStart = wordStart + consumed;
    } else if (current.size() + 1 + word.size() <= ucols) {
      current += " ";
      current += word;
    } else {
      lines.push_back(LineSpan{current, currentStart});
      current = word;
      currentStart = wordStart + consumed;
    }
  }
  if (!current.empty()) lines.push_back(LineSpan{current, currentStart});
  if (lines.empty()) lines.push_back(LineSpan{std::string(), 0});
  return lines;
}

// pdf2book.wrap_paragraph: the same wrap, lines only.
std::vector<std::string> wrapParagraph(const std::string &para, int cols) {
  std::vector<LineSpan> spans = wrapParagraphSpans(para, cols);
  std::vector<std::string> lines;
  lines.reserve(spans.size());
  for (size_t i = 0; i < spans.size(); i++) lines.push_back(spans[i].text);
  return lines;
}

}  // namespace detail

Pipeline::Pipeline(int cols, int rows, PageSink sink)
    : cols_(cols), rows_(rows > 0 ? rows : 1), sink_(sink) {}

void Pipeline::emitCodepoint(uint32_t cp) {
  if (cp == '\n') {
    pushChar('\n');
    return;
  }
  const char *repl = translitFor(cp);
  if (repl != nullptr) {
    for (const char *p = repl; *p; p++) pushChar(*p);
    return;
  }
  if (renderable(cp)) pushChar((char)cp);
  // Everything else is dropped, matching pdf2book.normalize_text.
}

void Pipeline::feed(const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i < len) {
    uint8_t b = data[i];

    if (need_ > 0) {
      if ((b & 0xC0) != 0x80) {
        // Malformed sequence. Python reads the file with errors="replace", so
        // the broken run becomes U+FFFD, which is not renderable and is
        // dropped. Drop it here too and reconsider this byte as a fresh start
        // without consuming it.
        need_ = 0;
        cp_ = 0;
        min_ = 0;
        continue;
      }
      cp_ = (cp_ << 6) | (uint32_t)(b & 0x3F);
      i++;
      if (--need_ == 0) {
        uint32_t cp = cp_;
        bool overlong = cp < min_;
        bool surrogate = (cp >= 0xD800 && cp <= 0xDFFF);
        cp_ = 0;
        min_ = 0;
        if (!overlong && !surrogate) emitCodepoint(cp);
      }
      continue;
    }

    i++;
    if (b < 0x80) {
      emitCodepoint(b);
    } else if ((b & 0xE0) == 0xC0) {
      cp_ = b & 0x1F;
      need_ = 1;
      min_ = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
      cp_ = b & 0x0F;
      need_ = 2;
      min_ = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
      cp_ = b & 0x07;
      need_ = 3;
      min_ = 0x10000;
    }
    // Anything else is a stray continuation or an invalid lead byte: dropped.
  }
}

void Pipeline::pushChar(char c) {
  if (c == '\n') {
    endLine();
    return;
  }
  line_.push_back(c);
}

// One input line, after pdf2book's per line cleaning: strip it, drop bare page
// numbers, and treat a blank line as a paragraph break. The running header and
// footer pass is skipped on purpose: find_running_lines returns an empty set
// for fewer than four form feed separated pages, and a news brief is one page.
void Pipeline::endLine() {
  std::string s = detail::strip(line_);
  line_.clear();

  if (detail::isPageNumber(s)) return;
  if (s.empty()) {
    flushParagraph();
    return;
  }
  if (para_.size() + s.size() + 1 > PARAGRAPH_MAX_CHARS) {
    truncated_ = true;
    flushParagraph();
  }
  if (!para_.empty()) para_.push_back('\n');
  para_ += s;
}

void Pipeline::flushParagraph() {
  if (para_.empty()) return;
  std::string joined = detail::dehyphenate(para_);
  para_.clear();

  // pages_to_paragraphs joins the surviving lines with a single space and
  // collapses runs of two or more spaces.
  std::string out;
  out.reserve(joined.size());
  for (size_t i = 0; i < joined.size(); i++) {
    char c = (joined[i] == '\n') ? ' ' : joined[i];
    if (c == ' ' && !out.empty() && out[out.size() - 1] == ' ') continue;
    out.push_back(c);
  }
  out = detail::strip(out);
  if (out.empty()) return;
  addParagraph(out);
}

// paginate_spans: one blank line between paragraphs, never at the top of a
// page, and a page break whenever the grid's row count is full.
void Pipeline::addParagraph(const std::string &para) {
  if (!firstParagraph_ && !current_.empty()) {
    if ((int)current_.size() == rows_) {
      emitPage();
    } else {
      current_.push_back(std::string());
    }
  }
  firstParagraph_ = false;

  std::vector<detail::LineSpan> lines = detail::wrapParagraphSpans(para, cols_);
  for (size_t i = 0; i < lines.size(); i++) {
    if ((int)current_.size() == rows_) emitPage();
    if (current_.empty()) {
      currentAnchor_ = paraBase_ + (uint32_t)lines[i].offset;
    }
    current_.push_back(lines[i].text);
  }
  // One separator character between paragraphs in the logical text stream.
  paraBase_ += (uint32_t)para.size() + 1;
}

void Pipeline::paragraph(const std::string &latin1) {
  if (latin1.empty()) return;
  addParagraph(latin1);
}

void Pipeline::endRun() {
  if (!current_.empty()) emitPage();
  firstParagraph_ = true;
}

void Pipeline::emitPage() {
  emitAnchor_ = currentAnchor_;
  if (sink_) sink_(current_);
  pages_++;
  current_.clear();
}

void Pipeline::finish() {
  if (finished_) return;
  finished_ = true;
  // A file that does not end in a newline still has that last partial line as
  // a line, which is how "\n".join() sees it on the host.
  if (!line_.empty()) endLine();
  flushParagraph();
  if (!current_.empty()) emitPage();
  if (pages_ == 0) {
    current_.push_back(std::string());
    emitPage();
  }
}

// ------------------------------------------------------------- rejoiner

std::string decodeLatin1(const uint8_t *data, size_t len) {
  std::string out;
  out.reserve(len);
  size_t i = 0;
  while (i < len) {
    uint8_t b = data[i];
    if (b < 0x80) {
      out.push_back((char)b);
      i++;
    } else if ((b & 0xE0) == 0xC0 && i + 1 < len &&
               (data[i + 1] & 0xC0) == 0x80) {
      uint32_t cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(data[i + 1] & 0x3F);
      if (cp >= 0x80 && cp <= 0xFF) out.push_back((char)cp);
      i += 2;
    } else {
      // Not something encodeUtf8() writes: skip the lead byte and any
      // continuation bytes after it.
      i++;
      while (i < len && (data[i] & 0xC0) == 0x80) i++;
    }
  }
  return out;
}

Rejoiner::Rejoiner(int cols, ParagraphSink sink)
    : cols_(cols > 0 ? cols : 1), sink_(sink) {}

void Rejoiner::flush() {
  if (para_.empty()) return;
  if (sink_) sink_(para_);
  // One separator character between paragraphs, as in paginate_spans.
  paraBase_ += (uint32_t)para_.size() + 1;
  para_.clear();
  last_.clear();
}

void Rejoiner::breakRun() { flush(); }

// wrapParagraphSpans emits a hard broken piece as a line of exactly `cols`
// characters on its own, so it can never hold a space.
bool Rejoiner::hardBroken(const std::string &line) const {
  return (int)line.size() == cols_ && line.find(' ') == std::string::npos;
}

// Across a page boundary: does `next` carry on the paragraph `prev` ended?
bool Rejoiner::continues(const std::string &prev,
                         const std::string &next) const {
  if (hardBroken(prev)) return true;
  size_t sp = next.find(' ');
  size_t word = (sp == std::string::npos) ? next.size() : sp;
  // The wrap would have put a word that fits on the previous line.
  if (prev.size() + 1 + word <= (size_t)cols_) return false;
  return true;
}

void Rejoiner::page(const std::string &latin1) {
  pageAnchor_ = paraBase_ + (uint32_t)para_.size();
  bool first = true;
  size_t pos = 0;
  while (pos <= latin1.size()) {
    size_t nl = latin1.find('\n', pos);
    std::string line = (nl == std::string::npos) ? latin1.substr(pos)
                                                 : latin1.substr(pos, nl - pos);
    pos = (nl == std::string::npos) ? latin1.size() + 1 : nl + 1;

    if (line.empty()) {
      // A blank line is a paragraph break. Pages never start with one, but a
      // page can end with one (the separator landed on the last row).
      flush();
      if (first) pageAnchor_ = paraBase_;
      first = false;
      continue;
    }
    if (!para_.empty() && first && !continues(last_, line)) flush();
    if (para_.empty()) {
      if (first) pageAnchor_ = paraBase_;
      para_ = line;
    } else {
      // Inside a page, consecutive lines are always one paragraph.
      if (!hardBroken(last_)) para_.push_back(' ');
      if (first) pageAnchor_ = paraBase_ + (uint32_t)para_.size();
      para_ += line;
    }
    last_ = line;
    first = false;
    if (para_.size() > PARAGRAPH_MAX_CHARS) flush();
  }
}

}  // namespace textpage

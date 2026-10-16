#!/usr/bin/env python3
"""Convert text-based PDFs into MPG1 .pgs book files for the Muon Bench e-reader.

Usage:
    python3 tools/pdf2book.py                 convert every PDF in books_src/
    python3 tools/pdf2book.py a.pdf b.pdf     convert the named PDFs

Flags:
    --title "..."   override the book title (only valid with a single input)
    --out DIR       output directory (default data/books)
    --panel {576,750}
                    the screen the book is paginated for (default 576, the
                    5.76 inch Good Display panel; 750 is the Waveshare 7.5
                    inch V2). A book made for another panel still opens: the
                    reader re-paginates it on the device the first time.

The pagination contract lives in SPEC.md: on the default panel 54 columns by
37 rows, monospace, on the portrait 680x920 canvas (37x32 on the 480x800
canvas of the 7.5 inch panel). Every line this script emits fits the grid, so
the firmware can render lines verbatim with no wrapping. The grid is recorded
in the file (see GRID_EXT_MAGIC), so the reader knows which screen it is for.

Only text is extracted. Images are skipped inherently. A scanned PDF with no
text layer produces an empty book; run OCR on it first.
"""

import argparse
import os
import re
import struct
import sys

# ------------------------------------------------------------- page geometry
#
# Single source of truth for the host side. Everything below is derived from
# the panel size, and every one of these numbers has a twin in
# include/config.h; the two must agree or host pagination and on-glass
# rendering disagree. tools/txt2book.py imports these, and
# tools/extract_figures.swift mirrors IMAGE_* at the top of its own file.
#
# Panel: Good Display GDEH0576T81, 5.76 inch, 920x680 native, 1 bit. The UI is
# PORTRAIT: the firmware rotates the panel 90 degrees (config.h
# DISPLAY_ROTATION), so the logical page is 680 wide by 920 tall.
PANEL_W = 680
PANEL_H = 920
MARGIN_X = 16
# Status strip: rule 24 px above the bottom edge, status baseline 8 px above.
STATUS_RULE_Y = PANEL_H - 24          # 896
STATUS_BASELINE_Y = PANEL_H - 8       # 912
# Everything above the rule is content.
CONTENT_W = PANEL_W                   # 680
CONTENT_H = STATUS_RULE_Y             # 896
TEXT_W = PANEL_W - 2 * MARGIN_X       # 648

# Text grids, keyed by the font_id stored in an MPG2 variant header.
#   font 0: u8g2_font_profont22_mf, 12 px advance, so 648/12 = 54 columns
#           exactly. First baseline 26, pitch 24, so baseline n is
#           26 + (n-1)*24: the 37th is 26 + 36*24 = 890 and its 4 px descender
#           ends at 894, clear of the status rule at 896. A 38th would be 914.
#   font 1: u8g2_font_profont29_mf, 16 px advance, so 648/16 = 40.5, floored
#           to 40 columns. First baseline 32 (same ~8 px of ink margin at the
#           top), pitch 32, so baseline n is 32*n: the 27th is 864 and its
#           5 px descender ends at 869. A 28th would land on the rule at 896.
# Advance widths come from the max_char_width byte of the u8g2 font headers in
# U8g2_for_Adafruit_GFX/src/u8g2_fonts.c (12 for profont22_mf, 16 for
# profont29_mf); both are monospace (_mf) faces.
GRIDS = {
    0: (TEXT_W // 12, 37),   # 54 x 37
    1: (TEXT_W // 16, 27),   # 40 x 27
}

COLS, ROWS = GRIDS[0]

# Every panel the firmware supports, keyed by the --panel value, as the
# portrait canvas (width, height). Mirrors the table in src/panel.cpp (native
# frame turned on its side). GRIDS above is panel 576, which is also the grid
# a book with no recorded grid is assumed to have.
PANELS = {
    576: (680, 920),   # panel 0, Good Display GDEH0576T81
    750: (480, 800),   # panel 1, Waveshare 7.5 inch V2
}
DEFAULT_PANEL = 576

# Font metrics behind the grids, by font id: (advance, first baseline, pitch,
# descent). Twins of TEXT_* and TEXT_LARGE_* in include/config.h.
FONT_METRICS = {
    0: (12, 26, 24, 4),   # u8g2_font_profont22_mf
    1: (16, 32, 32, 5),   # u8g2_font_profont29_mf
}
STATUS_STRIP_H = 24


def panel_grids(panel):
    """Text grids {font_id: (cols, rows)} for a panel, computed exactly the way
    panel::applyGeometry() computes them on the device:
        cols = (W - 2*MARGIN_X) // advance
        rows = (ruleY - 1 - descent - firstBaseline) // pitch + 1
    with ruleY = H - STATUS_STRIP_H. Panel 576 gives 54x37 and 40x27, panel
    750 gives 37x32 and 28x24.
    """
    width, height = PANELS[panel]
    rule_y = height - STATUS_STRIP_H
    grids = {}
    for font_id, (advance, first, pitch, descent) in FONT_METRICS.items():
        cols = (width - 2 * MARGIN_X) // advance
        rows = (rule_y - 1 - descent - first) // pitch + 1
        grids[font_id] = (cols, rows)
    return grids


def panel_content(panel):
    """Content area (width, height) above the status rule for a panel."""
    width, height = PANELS[panel]
    return width, height - STATUS_STRIP_H


assert panel_grids(DEFAULT_PANEL) == GRIDS

# Figure page targets. A full res figure fills the content area outright; the
# default reduced pass stores far fewer pixels and sets MPG1_IMAGE_FLAG_DOUBLE
# so the firmware draws each stored pixel as 2x2.
#
# The reduced box is 2/5 of the content area, not 1/2. Storing at 1/2 (340x448
# here) is what the landscape build did, and it makes a doubled plate fill the
# page exactly, but 52 plates of the Marathon Monks book at that size cost
# 892 KB, which overran the 1.94 MB filesystem the 4 MB partition map gave us
# once a ~600 KB news sync transient was accounted for. The map is 8 MB now, so
# this is headroom rather than a hard limit. 2/5 costs
# (0.4/0.5)^2 = 64% of that, about 570 KB, and a doubled plate still covers
# 544x716 of the 680x896 page (80% linear), centred with a margin. Raise this
# back to // 2 for a book with few figures.
IMAGE_FULL_W = CONTENT_W              # 680
IMAGE_FULL_H = CONTENT_H              # 896
IMAGE_HALF_W = CONTENT_W * 2 // 5     # 272
IMAGE_HALF_H = CONTENT_H * 2 // 5     # 358

SLUG_MAX = 24
# LittleFS partition is 0x5F0000 = 6.09 MB (the spiffs row in partitions.csv),
# the whole of an 8 MB module above the app slot. It was 0x1F0000 while the
# table assumed a 4 MB module. Warn well short of the top: LittleFS metadata
# and block rounding cost real space, and a news sync needs about 600 KB of
# transient room.
FS_BUDGET = 0x5F0000
FS_WARN = 5400 * 1024

INSTALL_HINT = (
    "No PDF text extraction library found.\n"
    "Install one of these for the Python you run this script with:\n"
    "    python3 -m pip install pypdf          (preferred)\n"
    "    python3 -m pip install pdfminer.six   (fallback)\n"
    "If pip is broken for your default python3 (common with Homebrew builds),\n"
    "use a virtual environment:\n"
    "    /usr/bin/python3 -m venv .venv\n"
    "    .venv/bin/pip install -r tools/requirements.txt\n"
    "    .venv/bin/python tools/pdf2book.py"
)


# ---------------------------------------------------------------- extraction

def extraction_backend():
    """Return (name, callable) for the best available extractor, or (None, None)."""
    try:
        import pypdf  # noqa: F401
        return ("pypdf", _extract_pypdf)
    except ImportError:
        pass
    try:
        import pdfminer  # noqa: F401
        return ("pdfminer.six", _extract_pdfminer)
    except ImportError:
        pass
    return (None, None)


def _extract_pypdf(path):
    """Return a list of raw text strings, one per PDF page, plus the metadata title."""
    from pypdf import PdfReader
    reader = PdfReader(path)
    title = None
    try:
        meta = reader.metadata
        if meta and meta.title:
            title = str(meta.title).strip() or None
    except Exception:
        title = None
    pages = []
    for page in reader.pages:
        text = ""
        # Layout mode keeps the vertical whitespace, which is what tells us where
        # one paragraph ends and the next begins. Fall back to plain mode if the
        # installed pypdf is too old or the page defeats it.
        try:
            text = page.extract_text(extraction_mode="layout") or ""
        except Exception:
            text = ""
        if not text.strip():
            try:
                text = page.extract_text() or ""
            except Exception as exc:
                sys.stderr.write("warning: page extraction failed: %s\n" % exc)
                text = ""
        pages.append(text)
    return pages, title


def _extract_pdfminer(path):
    from pdfminer.high_level import extract_text
    from pdfminer.pdfparser import PDFParser
    from pdfminer.pdfdocument import PDFDocument

    title = None
    try:
        with open(path, "rb") as fh:
            doc = PDFDocument(PDFParser(fh))
            for info in doc.info or []:
                raw = info.get("Title")
                if raw:
                    if isinstance(raw, bytes):
                        raw = raw.decode("utf-8", "replace")
                    title = str(raw).strip() or None
                    break
    except Exception:
        title = None

    # pdfminer separates pages with a form feed.
    text = extract_text(path) or ""
    pages = text.split("\f")
    if pages and not pages[-1].strip():
        pages.pop()
    return pages, title


# ----------------------------------------------------------------- cleaning

TRANSLIT = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"',
    "′": "'", "″": '"',
    "‐": "-", "‑": "-", "‒": "-", "–": "-",
    "—": "-", "―": "-", "−": "-",
    "…": "...",
    " ": " ", " ": " ", " ": " ", " ": " ",
    " ": " ", " ": " ", " ": " ", " ": " ",
    " ": " ", " ": " ", "　": " ",
    "​": "", "‌": "", "‍": "", "﻿": "",
    "­": "",  # soft hyphen, invisible in profont22 but not a real character
    "•": "*", "·": "*", "⁃": "-",
    "ﬀ": "ff", "ﬁ": "fi", "ﬂ": "fl",
    "ﬃ": "ffi", "ﬄ": "ffl", "ﬅ": "st", "ﬆ": "st",
    "Œ": "OE", "œ": "oe", "Æ": "AE", "æ": "ae",
    "‹": "<", "›": ">", "«": '"', "»": '"',
    "⁄": "/", "∕": "/",
    "\t": " ",
}


def renderable(ch):
    """True if profont22 can draw this character (ASCII plus Latin-1 supplement)."""
    code = ord(ch)
    if 0x20 <= code <= 0x7E:
        return True
    if 0xA1 <= code <= 0xFF:
        return True
    return False


def normalize_text(text):
    """Transliterate to the device charset and drop anything unrenderable."""
    out = []
    for ch in text:
        if ch == "\n":
            out.append(ch)
            continue
        repl = TRANSLIT.get(ch)
        if repl is not None:
            out.append(repl)
        elif renderable(ch):
            out.append(ch)
        # anything else is dropped
    return "".join(out)


BARE_NUMBER = re.compile(r"^[\[\(]?\s*[ivxlcdm0-9]+\s*[\]\)]?$", re.IGNORECASE)


def is_page_number(line):
    s = line.strip()
    if not s:
        return False
    if s.isdigit():
        return True
    # Roman numerals and bracketed forms, kept deliberately narrow.
    return bool(BARE_NUMBER.match(s)) and len(s) <= 8 and any(c.isdigit() for c in s)


def find_running_lines(pages, edge=2, min_share=0.5):
    """Collect header/footer lines that repeat on most pages."""
    if len(pages) < 4:
        return set()
    counts = {}
    for raw in pages:
        lines = [l.strip() for l in raw.split("\n") if l.strip()]
        # Bare page numbers are dropped separately, so look past them.
        lines = [l for l in lines if not is_page_number(l)]
        if not lines:
            continue
        candidates = set(lines[:edge]) | set(lines[-edge:])
        for cand in candidates:
            # Running furniture is short and is not a sentence. Requiring both
            # keeps repeated body prose from being mistaken for a header.
            if len(cand) > 60 or SENTENCE_END.search(cand):
                continue
            counts[cand] = counts.get(cand, 0) + 1
    threshold = max(3, int(len(pages) * min_share))
    return set(k for k, v in counts.items() if v >= threshold)


DEHYPH = re.compile(r"([A-Za-zÀ-ÿ]{2,})-\n([a-zà-ÿ]{2,})")
SENTENCE_END = re.compile(r"[.!?][\"')\]]?$")


def pages_to_paragraphs(raw_pages, running=None):
    """Strip furniture, join hard-wrapped lines, and return a list of paragraphs.

    `running` lets a caller pass a header/footer set computed over the whole
    book. That matters when the text is processed in short segments (figure
    insertion), where per-segment detection would see too few pages to work.
    """
    if running is None:
        running = find_running_lines(raw_pages)
    chunks = []
    for raw in raw_pages:
        kept = []
        for line in normalize_text(raw).split("\n"):
            stripped = line.strip()
            if stripped in running:
                continue
            if is_page_number(stripped):
                continue
            kept.append(stripped)
        body = "\n".join(kept).strip("\n")
        if body.strip():
            chunks.append(body)

    if not chunks:
        return []

    # Stitch pages together. A page whose last line ends a sentence starts a new
    # paragraph, otherwise the paragraph runs on across the page break.
    text = chunks[0]
    for chunk in chunks[1:]:
        last = text.rstrip().split("\n")[-1].strip()
        joiner = "\n\n" if (not last or SENTENCE_END.search(last)) else "\n"
        text += joiner + chunk

    text = DEHYPH.sub(r"\1\2", text)

    paragraphs = []
    for block in re.split(r"\n\s*\n+", text):
        joined = " ".join(part.strip() for part in block.split("\n"))
        joined = re.sub(r" {2,}", " ", joined).strip()
        if joined:
            paragraphs.append(joined)
    return paragraphs


# --------------------------------------------------------------- pagination

def wrap_paragraph_spans(para, cols=COLS):
    """Word wrap to `cols`, returning (line, offset) pairs.

    `offset` is the index into `para` of the line's first character. Two grids
    wrapping the same paragraph produce different lines but the same offsets
    for the same words, which is what makes anchors comparable across variants.
    No hyphenation. Words longer than cols are hard broken.
    """
    lines = []
    current = ""
    current_start = 0
    scan = 0
    for word in para.split(" "):
        word_start = scan
        scan += len(word) + 1
        if not word:
            continue
        consumed = 0
        while len(word) > cols:
            if current:
                lines.append((current, current_start))
                current = ""
            lines.append((word[:cols], word_start + consumed))
            word = word[cols:]
            consumed += cols
        if not current:
            current = word
            current_start = word_start + consumed
        elif len(current) + 1 + len(word) <= cols:
            current += " " + word
        else:
            lines.append((current, current_start))
            current = word
            current_start = word_start + consumed
    if current:
        lines.append((current, current_start))
    return lines or [("", 0)]


def wrap_paragraph(para, cols=COLS):
    """Word wrap to `cols`. No hyphenation. Words longer than cols are hard broken."""
    return [line for line, _ in wrap_paragraph_spans(para, cols)]


def paginate_spans(paragraphs, cols=COLS, rows=ROWS, base=0):
    """Paginate and anchor. Returns (pages, consumed) where each page is a
    (lines, anchor) pair and `consumed` is the length of this run of paragraphs
    in the logical text stream.

    The stream is the paragraphs concatenated with one separator character
    each, so a given word sits at the same stream position no matter which grid
    paginated it. A page's anchor is the stream position of its first
    character.
    """
    pages = []
    current = []
    current_anchor = base
    para_base = base
    for index, para in enumerate(paragraphs):
        if index > 0:
            # One blank line between paragraphs, but never at the top of a page.
            if current:
                if len(current) == rows:
                    pages.append((current, current_anchor))
                    current = []
                else:
                    current.append("")
        for line, offset in wrap_paragraph_spans(para, cols):
            if len(current) == rows:
                pages.append((current, current_anchor))
                current = []
            if not current:
                current_anchor = para_base + offset
            current.append(line)
        para_base += len(para) + 1
    if current:
        pages.append((current, current_anchor))
    if not pages:
        pages = [([""], base)]
    return pages, para_base - base


def paginate(paragraphs, cols=COLS, rows=ROWS):
    """Turn paragraphs into pages, each a list of at most `rows` lines."""
    pages, _ = paginate_spans(paragraphs, cols, rows)
    return [lines for lines, _ in pages]


# ---------------------------------------------------------------- image pages

# A page blob starting with these four bytes is an image page, not UTF-8 text.
# Text pages never start with 0x01, so old books keep working unchanged.
IMAGE_MAGIC = b"\x01IMG"
IMAGE_HEADER_LEN = 10  # magic(4) + w(2) + h(2) + flags(1) + reserved(1)
IMAGE_FLAG_DOUBLE = 0x01


def read_pbm_p4(path):
    """Read a binary PBM (P4). Returns (width, height, raster bytes).

    P4 packing is exactly the payload layout of an image page: rows top to
    bottom, MSB first inside each byte, bit 1 = black.
    """
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"P4":
        raise ValueError("%s is not a binary PBM (P4)" % path)

    pos = 2
    fields = []
    while len(fields) < 2:
        if pos >= len(data):
            raise ValueError("%s has a truncated P4 header" % path)
        ch = data[pos:pos + 1]
        if ch.isspace():
            pos += 1
            continue
        if ch == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        token_start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        token = data[token_start:pos]
        if not token.isdigit():
            raise ValueError("%s has a malformed P4 header" % path)
        fields.append(int(token))
    width, height = fields
    # Exactly one whitespace character separates the header from the raster.
    pos += 1

    stride = (width + 7) // 8
    need = stride * height
    raster = data[pos:pos + need]
    if len(raster) != need:
        raise ValueError("%s raster is %d bytes, expected %d"
                         % (path, len(raster), need))
    return width, height, raster


def build_image_blob(width, height, flags, raster):
    """Pack an image page blob: header plus the raster verbatim."""
    stride = (width + 7) // 8
    drawn_w = width * (2 if flags & IMAGE_FLAG_DOUBLE else 1)
    drawn_h = height * (2 if flags & IMAGE_FLAG_DOUBLE else 1)
    if drawn_w > CONTENT_W or drawn_h > CONTENT_H:
        sys.stderr.write("warning: figure draws at %dx%d, larger than the "
                         "%dx%d content area; it will be scaled down on "
                         "device\n"
                         % (drawn_w, drawn_h, CONTENT_W, CONTENT_H))
    if len(raster) != stride * height:
        raise ValueError("raster size %d does not match %dx%d" % (len(raster), width, height))
    if width <= 0 or height <= 0 or width > 0xFFFF or height > 0xFFFF:
        raise ValueError("image dimensions out of range: %dx%d" % (width, height))
    out = bytearray(IMAGE_MAGIC)
    out += struct.pack("<HH", width, height)
    out += struct.pack("<BB", flags & 0xFF, 0)
    out += raster
    return bytes(out)


def load_figures(manifest_path):
    """Read a figure manifest and return {source_page: [blob, ...]}.

    Review workflow: the human deletes the review PNG of any figure they
    reject, so an entry only counts if its PNG is still on disk. Entries whose
    PBM is missing are warned about and skipped.
    """
    import json

    base = os.path.dirname(os.path.abspath(manifest_path))
    with open(manifest_path, "r", encoding="utf-8") as fh:
        entries = json.load(fh)
    if isinstance(entries, dict):
        entries = entries.get("figures", [])

    by_page = {}
    included = 0
    rejected = 0
    missing = 0
    for entry in entries:
        fig_id = entry.get("id", "?")
        pbm_rel = entry.get("pbm")
        png_rel = entry.get("png")
        pbm_path = os.path.join(base, pbm_rel) if pbm_rel else None
        png_path = os.path.join(base, png_rel) if png_rel else None

        if pbm_path is None or not os.path.isfile(pbm_path):
            sys.stderr.write("warning: figure %s has no PBM at %s, skipped\n"
                             % (fig_id, pbm_rel))
            missing += 1
            continue
        if png_path is None or not os.path.isfile(png_path):
            # Reviewer deleted the preview, which means "do not include".
            rejected += 1
            continue

        width, height, raster = read_pbm_p4(pbm_path)
        man_w = entry.get("w")
        man_h = entry.get("h")
        if man_w is not None and man_h is not None and (man_w, man_h) != (width, height):
            raise ValueError("figure %s: manifest says %sx%s, PBM is %dx%d"
                             % (fig_id, man_w, man_h, width, height))
        blob = build_image_blob(width, height, int(entry.get("flags", 0)), raster)
        page = int(entry["page"])
        by_page.setdefault(page, []).append(blob)
        included += 1

    return by_page, included, rejected, missing


# ------------------------------------------------------------------- output

def slugify(name):
    slug = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    slug = re.sub(r"_{2,}", "_", slug)
    if not slug:
        slug = "book"
    return slug[:SLUG_MAX].strip("_") or "book"


# Grid extension. Optional, and placed in the gap between the page table(s)
# and the first page blob, which no reader looks at: both MPG1 and MPG2 address
# pages by absolute offset, so firmware that predates the extension skips it.
#     4 bytes 'GRID', u8 variant_count (equal to the header's: 1 for MPG1),
#     then per variant, in header order, u8 cols and u8 rows.
# A book without it is read as the panel 576 grids (GRIDS). The firmware twin
# is in src/books.h (GRID_EXT_MAGIC).
GRID_EXT_MAGIC = b"GRID"


def grid_ext_len(variant_count):
    return 4 + 1 + 2 * variant_count


def grid_ext_bytes(grids):
    """Pack the grid extension for a list of (cols, rows), one per variant."""
    out = bytearray(GRID_EXT_MAGIC)
    out += struct.pack("<B", len(grids))
    for cols, rows in grids:
        if not (0 < cols < 256 and 0 < rows < 256):
            raise ValueError("grid %dx%d does not fit the extension" % (cols, rows))
        out += struct.pack("<BB", cols, rows)
    return bytes(out)


def write_mpg1(path, title, pages, grid=None):
    """Write the MPG1 container described in SPEC.md. Returns the byte count.

    Each entry of `pages` is either a list of text lines (a text page, joined
    with newlines) or a bytes object (a prebuilt page blob, written verbatim).
    Image pages are built by build_image_blob() and passed as bytes. `grid` is
    the (cols, rows) the text was paginated on, recorded in the grid extension
    (default: the panel 576 normal grid).
    """
    blobs = [bytes(page) if isinstance(page, (bytes, bytearray))
             else "\n".join(page).encode("utf-8") for page in pages]
    title_bytes = title.encode("utf-8")
    ext = grid_ext_bytes([grid or GRIDS[0]])
    table_end = 4 + 4 + 2 + len(title_bytes) + 4 * len(blobs)
    header_len = table_end + len(ext)

    offsets = []
    cursor = header_len
    for blob in blobs:
        offsets.append(cursor)
        cursor += len(blob)

    out = bytearray()
    out += b"MPG1"
    out += struct.pack("<I", len(blobs))
    out += struct.pack("<H", len(title_bytes))
    out += title_bytes
    for off in offsets:
        out += struct.pack("<I", off)
    assert len(out) == table_end
    out += ext
    assert len(out) == header_len
    for blob in blobs:
        out += blob

    with open(path, "wb") as fh:
        fh.write(out)
    return len(out)


def blob_bytes(payload):
    """A page payload is either a prebuilt blob (bytes) or a list of text lines."""
    if isinstance(payload, (bytes, bytearray)):
        return bytes(payload)
    return "\n".join(payload).encode("utf-8")


def write_mpg2(path, title, variants, grids=None):
    """Write the MPG2 multi variant container. Returns (size, header_len).

    `variants` is a list of (font_id, pages) where pages is a list of
    (payload, anchor) pairs. `grids` is the (cols, rows) of each variant, in
    the same order, for the grid extension (default: the panel 576 grid of
    each variant's font). Identical blobs are stored once and every table
    entry that needs them points at the same offset, which is how both variants
    share a single copy of each figure. Because blobs are shared, a page's
    length cannot be derived from the next offset, so the table carries it.

    Layout: magic 'MPG2', u8 variant_count, u16 title_len, title, then per
    variant u8 font_id, u32 page_count, page_count entries of
    {u32 offset, u32 length, u32 anchor}, then the grid extension, then the
    blob region.
    """
    if grids is None:
        grids = [GRIDS[font_id] for font_id, _pages in variants]
    if len(grids) != len(variants):
        raise ValueError("one grid per variant")
    ext = grid_ext_bytes(grids)
    title_bytes = title.encode("utf-8")
    table_end = 4 + 1 + 2 + len(title_bytes)
    for _font_id, pages in variants:
        table_end += 1 + 4 + 12 * len(pages)
    header_len = table_end + len(ext)

    blobs = []
    index_of = {}
    tables = []
    for _font_id, pages in variants:
        table = []
        for payload, anchor in pages:
            blob = blob_bytes(payload)
            index = index_of.get(blob)
            if index is None:
                index = len(blobs)
                index_of[blob] = index
                blobs.append(blob)
            table.append((index, len(blob), anchor))
        tables.append(table)

    offsets = []
    cursor = header_len
    for blob in blobs:
        offsets.append(cursor)
        cursor += len(blob)

    out = bytearray()
    out += b"MPG2"
    out += struct.pack("<B", len(variants))
    out += struct.pack("<H", len(title_bytes))
    out += title_bytes
    for (font_id, _pages), table in zip(variants, tables):
        out += struct.pack("<B", font_id)
        out += struct.pack("<I", len(table))
        for index, length, anchor in table:
            out += struct.pack("<III", offsets[index], length, anchor)
    assert len(out) == table_end
    out += ext
    assert len(out) == header_len
    for blob in blobs:
        out += blob

    with open(path, "wb") as fh:
        fh.write(out)
    return len(out), header_len


def human(n):
    if n >= 1024 * 1024:
        return "%.2f MB" % (n / (1024.0 * 1024.0))
    if n >= 1024:
        return "%.1f KB" % (n / 1024.0)
    return "%d B" % n


# --------------------------------------------------------------------- main

def convert(pdf_path, out_dir, extract, title_override=None,
            panel=DEFAULT_PANEL):
    raw_pages, meta_title = extract(pdf_path)
    stem = os.path.splitext(os.path.basename(pdf_path))[0]
    title = title_override or meta_title or stem
    title = normalize_text(title).strip() or stem

    paragraphs = pages_to_paragraphs(raw_pages)
    if sum(len(p) for p in paragraphs) < 200:
        # Do not write a stub book: an empty .pgs on the device becomes the
        # default book and reads as a firmware failure. Scanned PDFs land here.
        sys.stderr.write(
            "error: no usable text in %s, nothing written. If it is a scanned "
            "PDF, OCR it first: swift tools/ocr_pdf.swift book.pdf > book.txt, "
            "then python3 tools/txt2book.py book.txt\n" % pdf_path
        )
        return None
    cols, rows = panel_grids(panel)[0]
    pages = paginate(paragraphs, cols, rows)

    slug = slugify(stem)
    out_path = os.path.join(out_dir, slug + ".pgs")
    size = write_mpg1(out_path, title, pages, (cols, rows))
    return {
        "title": title,
        "slug": slug,
        "path": out_path,
        "pages": len(pages),
        "bytes": size,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Convert text-based PDFs into MPG1 .pgs books."
    )
    parser.add_argument("pdfs", nargs="*", help="PDF paths (default: every PDF in books_src/)")
    parser.add_argument("--title", default=None, help="override the book title")
    parser.add_argument("--out", default=None, help="output directory (default data/books)")
    parser.add_argument(
        "--panel", type=int, choices=sorted(PANELS), default=DEFAULT_PANEL,
        help="screen to paginate for: 576 (5.76 inch Good Display, default) "
             "or 750 (Waveshare 7.5 inch V2)")
    args = parser.parse_args(argv)

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = args.out or os.path.join(root, "data", "books")

    if args.pdfs:
        pdfs = list(args.pdfs)
    else:
        src = os.path.join(root, "books_src")
        if not os.path.isdir(src):
            sys.stderr.write("no books_src/ directory at %s\n" % src)
            return 1
        pdfs = sorted(
            os.path.join(src, n) for n in os.listdir(src) if n.lower().endswith(".pdf")
        )
        if not pdfs:
            sys.stderr.write("no PDFs found in %s\n" % src)
            return 1

    if args.title and len(pdfs) > 1:
        sys.stderr.write("--title only makes sense with a single input PDF\n")
        return 1

    name, extract = extraction_backend()
    if extract is None:
        sys.stderr.write(INSTALL_HINT + "\n")
        return 1

    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    print("extraction backend: %s" % name)
    print("output directory:   %s" % out_dir)
    print("panel:              %d (grid %dx%d)"
          % ((args.panel,) + panel_grids(args.panel)[0]))
    print("")

    failures = 0
    for pdf in pdfs:
        if not os.path.isfile(pdf):
            sys.stderr.write("missing file: %s\n" % pdf)
            failures += 1
            continue
        try:
            info = convert(pdf, out_dir, extract, args.title, args.panel)
        except Exception as exc:
            sys.stderr.write("failed on %s: %s\n" % (pdf, exc))
            failures += 1
            continue
        if info is None:
            failures += 1
            continue
        print("%-28s -> %s" % (os.path.basename(pdf), os.path.basename(info["path"])))
        print("    title: %s" % info["title"])
        print("    pages: %d   size: %s" % (info["pages"], human(info["bytes"])))

    total = 0
    for name_ in sorted(os.listdir(out_dir)):
        full = os.path.join(out_dir, name_)
        if os.path.isfile(full):
            total += os.path.getsize(full)

    print("")
    print("books directory total: %s of %s budget" % (human(total), human(FS_BUDGET)))
    if total > FS_WARN:
        print("WARNING: total exceeds %s. The LittleFS image may not fit."
              % human(FS_WARN))

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

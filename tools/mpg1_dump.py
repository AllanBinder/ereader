#!/usr/bin/env python3
"""Inspect an MPG1 or MPG2 .pgs book without hardware.

Usage:
    python3 tools/mpg1_dump.py book.pgs               header info and a check summary
    python3 tools/mpg1_dump.py book.pgs 3             print page 3 (1 based) as text
    python3 tools/mpg1_dump.py book.pgs 3 --variant 1 same, from variant 1
    python3 tools/mpg1_dump.py book.pgs --map 150     show how page 150 of variant 0
                                                      maps into every other variant

The check summary verifies the pagination contract from SPEC.md: page spans are
in range, no line exceeds the variant's column count, no page exceeds its row
count. For MPG2 it also verifies that anchors are non-decreasing within each
variant and that every image blob is stored once and shared by all variants.

The text grid of each variant comes from the file's grid extension when it
has one (written by the converters since multi panel support), otherwise it is
the default panel's grid for the variant's font, which is what the firmware
assumes too. The summary says which, and which panel the grid belongs to.

Image pages (blobs starting with 0x01 'I' 'M' 'G') are reported by dimensions,
flags, and payload size instead of being decoded as text. Dumping a single
image page prints a coarse ASCII preview.
"""

import os
import struct
import sys

# Text grids keyed by variant font_id. Imported rather than restated:
# tools/pdf2book.py is the single source of truth for page geometry on the
# host side, and include/config.h is its twin on the device.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pdf2book import (GRIDS, COLS, ROWS, GRID_EXT_MAGIC, PANELS,  # noqa: E402
                      grid_ext_len, panel_grids)

IMAGE_MAGIC = b"\x01IMG"
IMAGE_HEADER_LEN = 10


class Variant(object):
    """One paginated view of a book: a font id plus a page table."""

    def __init__(self, font_id, spans):
        self.font_id = font_id
        self.spans = spans  # list of (offset, length, anchor)
        self.recorded = None  # (cols, rows) from the grid extension, if any

    @property
    def page_count(self):
        return len(self.spans)

    def grid(self):
        if self.recorded is not None:
            return self.recorded
        return GRIDS.get(self.font_id, (COLS, ROWS))

    def panels(self):
        """Panels whose grid for this variant's font is this variant's grid."""
        return [panel for panel in sorted(PANELS)
                if panel_grids(panel).get(self.font_id) == self.grid()]

    def blob(self, data, index):
        offset, length, _anchor = self.spans[index]
        return data[offset:offset + length]


class BookFile(object):
    def __init__(self, magic, title, header_len, data, variants):
        self.magic = magic
        self.title = title
        self.header_len = header_len
        self.data = data
        self.variants = variants
        self.grid_ext = read_grid_ext(data, header_len, variants)
        if self.grid_ext:
            self.header_len += grid_ext_len(len(variants))


def read_grid_ext(data, pos, variants):
    """Read the optional grid extension that sits between the page tables (ending
    at `pos`) and the first blob. Sets each variant's `recorded` grid. Returns
    True if the extension is present. Same rules as Book::readGridExt in
    src/books.cpp: it only counts if every page starts after it."""
    need = grid_ext_len(len(variants))
    first_blob = min((span[0] for v in variants for span in v.spans),
                     default=len(data))
    if first_blob < pos + need or pos + need > len(data):
        return False
    if data[pos:pos + 4] != GRID_EXT_MAGIC or data[pos + 4] != len(variants):
        return False
    for index, variant in enumerate(variants):
        cols = data[pos + 5 + 2 * index]
        rows = data[pos + 6 + 2 * index]
        if cols == 0 or rows == 0:
            return False
    for index, variant in enumerate(variants):
        variant.recorded = (data[pos + 5 + 2 * index], data[pos + 6 + 2 * index])
    return True


def load(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 8:
        raise ValueError("file is too short to be a book")
    if data[:4] == b"MPG1":
        return load_mpg1(data)
    if data[:4] == b"MPG2":
        return load_mpg2(data)
    raise ValueError("not an MPG1 or MPG2 file (bad magic)")


def load_mpg1(data):
    pos = 4
    page_count = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    title_len = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    title = data[pos:pos + title_len].decode("utf-8", "replace")
    pos += title_len
    offsets = list(struct.unpack_from("<%dI" % page_count, data, pos))
    pos += 4 * page_count
    header_len = pos

    spans = []
    for i, off in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < page_count else len(data)
        spans.append((off, max(0, end - off), 0))
    return BookFile("MPG1", title, header_len, data, [Variant(0, spans)])


def load_mpg2(data):
    pos = 4
    variant_count = data[pos]
    pos += 1
    title_len = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    title = data[pos:pos + title_len].decode("utf-8", "replace")
    pos += title_len

    variants = []
    for _ in range(variant_count):
        font_id = data[pos]
        pos += 1
        page_count = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        spans = []
        for _p in range(page_count):
            spans.append(struct.unpack_from("<III", data, pos))
            pos += 12
        variants.append(Variant(font_id, spans))
    return BookFile("MPG2", title, pos, data, variants)


def page_image(blob):
    """Return (w, h, flags, payload_len) if this blob is an image page, else None."""
    if len(blob) < IMAGE_HEADER_LEN or blob[:4] != IMAGE_MAGIC:
        return None
    w = struct.unpack_from("<H", blob, 4)[0]
    h = struct.unpack_from("<H", blob, 6)[0]
    flags = blob[8]
    return (w, h, flags, len(blob) - IMAGE_HEADER_LEN)


def image_summary(info):
    w, h, flags, payload = info
    stride = (w + 7) // 8
    expected = stride * h
    note = "" if payload == expected else "  BAD PAYLOAD (expected %d)" % expected
    scale = " (pixel doubled to %dx%d)" % (w * 2, h * 2) if flags & 1 else ""
    return "image %dx%d  flags 0x%02X%s  payload %d bytes%s" % (
        w, h, flags, scale, payload, note)


def print_image_preview(blob, info):
    w, h, _flags, _payload = info
    stride = (w + 7) // 8
    raster = blob[IMAGE_HEADER_LEN:]
    # Coarse ASCII preview, one character per 8x16 block of stored pixels.
    for y in range(0, h, 16):
        row = []
        for x in range(0, w, 8):
            ink = 0
            for yy in range(y, min(y + 16, h)):
                byte_index = yy * stride + (x // 8)
                if byte_index < len(raster):
                    ink += bin(raster[byte_index]).count("1")
            row.append(" .:-=+*#%@"[min(9, ink // 13)])
        print("   " + "".join(row))


def page_head(book, variant, index, words=12):
    """First few words of a page, for anchor spot checks."""
    blob = variant.blob(book.data, index)
    if page_image(blob) is not None:
        return "(image page)"
    text = blob.decode("utf-8", "replace").replace("\n", " ")
    return " ".join(text.split()[:words])


def page_text_at(book, variant, index, offset, words=12):
    """Words of a page starting `offset` characters in, for anchor spot checks."""
    blob = variant.blob(book.data, index)
    if page_image(blob) is not None:
        return "(image page)"
    text = blob.decode("utf-8", "replace").replace("\n", " ")
    if offset < 0:
        offset = 0
    if offset > len(text):
        return "(past the end of this page)"
    return " ".join(text[offset:].split()[:words])


def map_page(book, from_variant, index, to_variant):
    """Largest page in `to_variant` whose anchor is at or before this page's."""
    anchor = from_variant.spans[index][2]
    best = 0
    for i, span in enumerate(to_variant.spans):
        if span[2] <= anchor:
            best = i
        else:
            break
    return best


def dump_page(book, variant, number):
    blob = variant.blob(book.data, number - 1)
    info = page_image(blob)
    header = "--- %s   page %d/%d   variant font %d, grid %dx%d ---" % (
        (book.title, number, variant.page_count, variant.font_id)
        + variant.grid())
    if info is not None:
        print(header)
        print(image_summary(info))
        print_image_preview(blob, info)
        return 0
    cols, _rows = variant.grid()
    text = blob.decode("utf-8", "replace")
    lines = text.split("\n")
    print(header)
    for i, line in enumerate(lines, 1):
        flag = "  <-- %d chars" % len(line) if len(line) > cols else ""
        print("%2d| %s%s" % (i, line, flag))
    print("--- %d lines, %d bytes ---" % (len(lines), len(text.encode("utf-8"))))
    return 0


def check_variant(book, variant, label, problems):
    """Per variant contract checks. Returns (image_offsets, stats)."""
    data = book.data
    cols, rows = variant.grid()
    image_pages = 0
    image_bytes = 0
    text_bytes = 0
    max_line = 0
    max_lines = 0
    image_offsets = set()
    prev_anchor = None

    for i, (offset, length, anchor) in enumerate(variant.spans):
        if offset < book.header_len or offset + length > len(data) or length == 0:
            problems.append("%s page %d span %d+%d is out of bounds"
                            % (label, i + 1, offset, length))
            continue
        if prev_anchor is not None and anchor < prev_anchor:
            problems.append("%s page %d anchor %d goes backwards (previous %d)"
                            % (label, i + 1, anchor, prev_anchor))
        prev_anchor = anchor

        blob = data[offset:offset + length]
        info = page_image(blob)
        if info is not None:
            image_pages += 1
            image_bytes += length
            image_offsets.add(offset)
            w, h, _flags, payload = info
            if payload != ((w + 7) // 8) * h:
                problems.append("%s page %d image payload is %d bytes, expected %d"
                                % (label, i + 1, payload, ((w + 7) // 8) * h))
            continue
        text_bytes += length
        lines = blob.decode("utf-8", "replace").split("\n")
        if len(lines) > rows:
            problems.append("%s page %d has %d lines (max %d)"
                            % (label, i + 1, len(lines), rows))
        max_lines = max(max_lines, len(lines))
        for j, line in enumerate(lines, 1):
            if len(line) > cols:
                problems.append("%s page %d line %d is %d chars (max %d)"
                                % (label, i + 1, j, len(line), cols))
            max_line = max(max_line, len(line))
        if lines and lines[0] == "":
            problems.append("%s page %d starts with a blank line" % (label, i + 1))

    panels = variant.panels()
    print("%s: font %d, grid %dx%d (%s, panel %s), %d pages (%d text, %d image)"
          % (label, variant.font_id, cols, rows,
             "recorded" if variant.recorded is not None else "assumed",
             "/".join(str(x) for x in panels) if panels else "none",
             variant.page_count, variant.page_count - image_pages, image_pages))
    print("    text bytes %d   image bytes %d   longest line %d/%d   tallest page %d/%d"
          % (text_bytes, image_bytes, max_line, cols, max_lines, rows))
    return image_offsets


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 1
    path = argv[1]
    rest = argv[2:]

    variant_index = 0
    map_page_number = None
    page_number = None
    i = 0
    while i < len(rest):
        arg = rest[i]
        if arg == "--variant":
            i += 1
            variant_index = int(rest[i])
        elif arg == "--map":
            i += 1
            map_page_number = int(rest[i])
        elif not arg.startswith("-"):
            page_number = int(arg)
        else:
            sys.stderr.write("unknown option: %s\n" % arg)
            return 1
        i += 1

    book = load(path)
    if variant_index < 0 or variant_index >= len(book.variants):
        sys.stderr.write("variant out of range (0..%d)\n" % (len(book.variants) - 1))
        return 1
    variant = book.variants[variant_index]

    if map_page_number is not None:
        if map_page_number < 1 or map_page_number > variant.page_count:
            sys.stderr.write("page out of range (1..%d)\n" % variant.page_count)
            return 1
        index = map_page_number - 1
        anchor = variant.spans[index][2]
        print("variant %d page %d  anchor %d" % (variant_index, map_page_number, anchor))
        print("    %s" % page_head(book, variant, index))
        for other_index, other in enumerate(book.variants):
            if other_index == variant_index:
                continue
            mapped = map_page(book, variant, index, other)
            mapped_anchor = other.spans[mapped][2]
            next_anchor = (other.spans[mapped + 1][2]
                           if mapped + 1 < other.page_count else None)
            print("  -> variant %d page %d  anchor %d  (anchor lands %d chars in%s)"
                  % (other_index, mapped + 1, mapped_anchor, anchor - mapped_anchor,
                     "" if next_anchor is None
                     else ", next page anchor %d" % next_anchor))
            print("    page starts: %s" % page_head(book, other, mapped))
            print("    at anchor:   %s"
                  % page_text_at(book, other, mapped, anchor - mapped_anchor))
        return 0

    if page_number is not None:
        if page_number < 1 or page_number > variant.page_count:
            sys.stderr.write("page out of range (1..%d)\n" % variant.page_count)
            return 1
        return dump_page(book, variant, page_number)

    print("file:        %s" % os.path.abspath(path))
    print("magic:       %s" % book.magic)
    print("title:       %s" % book.title)
    print("variants:    %d" % len(book.variants))
    print("header_len:  %d bytes" % book.header_len)
    print("grid ext:    %s" % ("yes" if book.grid_ext
                               else "no (default panel grids assumed)"))
    print("file size:   %d bytes" % len(book.data))
    print("")

    problems = []
    image_offset_sets = []
    for index, v in enumerate(book.variants):
        image_offset_sets.append(check_variant(book, v, "variant %d" % index, problems))

    if len(book.variants) > 1:
        first = image_offset_sets[0]
        shared = True
        for index in range(1, len(image_offset_sets)):
            if image_offset_sets[index] != first:
                shared = False
                problems.append("variant %d image offsets differ from variant 0, "
                                "so figures are not shared" % index)
        print("")
        print("shared image blobs: %d distinct offset(s), shared by all variants: %s"
              % (len(first), "yes" if shared else "NO"))

    if problems:
        print("")
        print("PROBLEMS (%d):" % len(problems))
        for problem in problems[:40]:
            print("  " + problem)
        return 1
    print("")
    print("checks: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

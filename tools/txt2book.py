#!/usr/bin/env python3
"""Build an MPG1 book from plain text instead of a PDF.

Intended for OCR output from tools/ocr_pdf.swift: pages separated by form
feeds (\\f). A file with no form feeds is treated as one big page, which is
fine (running-header detection just finds nothing).

Usage:
    python3 tools/txt2book.py input.txt --title "Book Title" [--out DIR]
    python3 tools/txt2book.py input.txt --title "..." --figures figs_out/manifest.json

With --figures, extracted figures are inserted as image pages right after the
text of the source PDF page they came from. The reviewer's veto is the review
PNG: delete review/<id>.png and that figure is left out.

With --dual the output is an MPG2 container holding the same book paginated
twice: variant 0 on the profont22 grid (54x37 on the default panel), variant 1
on the profont29 grid (40x27). Both grids come from pdf2book.panel_grids, the
single source of truth for page geometry on the host side.

--panel {576,750} picks the screen the grids are computed for: 576 (default)
is the 5.76 inch Good Display panel, 750 the Waveshare 7.5 inch V2 (37x32 and
28x24). The grids are recorded in the file, and a reader on another panel
re-paginates the book itself the first time it is opened. Figures keep the
size the extractor gave them; the firmware scales an oversize one down. Figure blobs are stored once and shared by both variants, and
every page carries an anchor into the text stream so the firmware can keep the
reading position when the reader switches size.

Reuses the cleaning, pagination, and MPG1 writer from pdf2book.py so the
output is byte-compatible with the firmware parser.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pdf2book as p


def build_items(raw_pages, figures_by_page):
    """Split the book into a reading-order list of text runs and image blobs.

    raw_pages[i] is 1-based source page i+1. A figure from source page k must
    land right after the text of source page k, so the text is cleaned in
    segments (prev_boundary, k] and the image blobs sit between segments. A
    paragraph that spans a segment boundary simply splits there.

    Returns a list of ("text", paragraphs) and ("image", blob) items. The list
    is grid independent, so every variant paginates the same material.
    """
    if not figures_by_page:
        paragraphs = p.pages_to_paragraphs(raw_pages)
        return [("text", paragraphs)] if paragraphs else []

    # Header and footer detection needs the whole book in view; short segments
    # would see too few pages to spot repeats.
    running = p.find_running_lines(raw_pages)

    items = []
    prev = 0  # number of source pages already consumed
    for k in sorted(figures_by_page):
        if k > len(raw_pages):
            sys.stderr.write("warning: figure page %d is past the end of the text "
                             "(%d pages), appending at the end\n" % (k, len(raw_pages)))
        segment = raw_pages[prev:k]
        if segment:
            paragraphs = p.pages_to_paragraphs(segment, running=running)
            if paragraphs:
                items.append(("text", paragraphs))
        for blob in figures_by_page[k]:
            items.append(("image", blob))
        prev = max(prev, k)

    tail = raw_pages[prev:]
    if tail:
        paragraphs = p.pages_to_paragraphs(tail, running=running)
        if paragraphs:
            items.append(("text", paragraphs))
    return items


def pages_for_grid(items, cols, rows):
    """Paginate the items on one grid. Returns a list of (payload, anchor).

    A payload is either a list of text lines or an image blob (bytes). An
    image takes the anchor of the text position it was inserted at, so the
    same figure carries the same anchor in every variant.
    """
    pages = []
    anchor = 0
    for kind, value in items:
        if kind == "image":
            pages.append((value, anchor))
            continue
        run, consumed = p.paginate_spans(value, cols, rows, anchor)
        pages.extend(run)
        anchor += consumed
    return pages


def payload_bytes(pages):
    """Split a variant's pages into (text bytes, image bytes, image page count)."""
    text_bytes = 0
    image_bytes = 0
    image_pages = 0
    for payload, _anchor in pages:
        if isinstance(payload, (bytes, bytearray)):
            image_bytes += len(payload)
            image_pages += 1
        else:
            text_bytes += len("\n".join(payload).encode("utf-8"))
    return text_bytes, image_bytes, image_pages


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("txt", help="input text file, pages separated by form feeds")
    parser.add_argument("--title", default=None, help="book title (default: filename stem)")
    parser.add_argument("--out", default=None, help="output directory (default data/books)")
    parser.add_argument(
        "--strip-regex", action="append", default=[],
        help="drop any input line fully matching this regex (repeatable), "
             "for page furniture the automatic cleaner cannot catch")
    parser.add_argument(
        "--figures", default=None, metavar="MANIFEST_JSON",
        help="figure manifest written by the figure extractor "
             "(<outdir>/manifest.json); figures whose review PNG was deleted "
             "are skipped")
    parser.add_argument(
        "--dual", action="store_true",
        help="write an MPG2 book with two text sizes (profont22 54x37 and "
             "profont29 40x27 on the default panel) instead of a single size "
             "MPG1 book")
    parser.add_argument(
        "--panel", type=int, choices=sorted(p.PANELS), default=p.DEFAULT_PANEL,
        help="screen to paginate for: 576 (5.76 inch Good Display, default) "
             "or 750 (Waveshare 7.5 inch V2)")
    args = parser.parse_args(argv)
    grids = p.panel_grids(args.panel)
    strip_res = [re.compile(pat) for pat in args.strip_regex]

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = args.out or os.path.join(root, "data", "books")
    os.makedirs(out_dir, exist_ok=True)

    with open(args.txt, "r", encoding="utf-8", errors="replace") as fh:
        raw = fh.read()

    stem = os.path.splitext(os.path.basename(args.txt))[0]
    title = p.normalize_text(args.title or stem).strip() or stem

    raw_pages = raw.split("\f")
    if strip_res:
        dropped = 0
        cleaned_pages = []
        for page in raw_pages:
            kept = []
            for line in page.split("\n"):
                if any(r.fullmatch(line.strip()) for r in strip_res):
                    dropped += 1
                else:
                    kept.append(line)
            cleaned_pages.append("\n".join(kept))
        raw_pages = cleaned_pages
        print("stripped %d furniture line(s)" % dropped)

    figures_by_page = {}
    if args.figures:
        figures_by_page, included, rejected, missing = p.load_figures(args.figures)
        print("figures: %d included, %d rejected (review PNG deleted), %d missing PBM"
              % (included, rejected, missing))

    items = build_items(raw_pages, figures_by_page)
    if not items:
        print("no text found in input, nothing written")
        return 1

    slug = p.slugify(title)
    out_path = os.path.join(out_dir, slug + ".pgs")

    if args.dual:
        variants = []
        for font_id in (0, 1):
            cols, rows = grids[font_id]
            variants.append((font_id, pages_for_grid(items, cols, rows)))
        size, header_len = p.write_mpg2(
            out_path, title, variants,
            [grids[font_id] for font_id, _pages in variants])

        shared_image_bytes = 0
        seen = set()
        for _font_id, pages in variants:
            for payload, _anchor in pages:
                if isinstance(payload, (bytes, bytearray)):
                    blob = bytes(payload)
                    if blob not in seen:
                        seen.add(blob)
                        shared_image_bytes += len(blob)

        print("%s -> %s" % (args.txt, out_path))
        print("    title: %s   format: MPG2, %d variants   panel: %d"
              % (title, len(variants), args.panel))
        for font_id, pages in variants:
            cols, rows = grids[font_id]
            text_bytes, _image_bytes, image_pages = payload_bytes(pages)
            print("    variant %d (font %d, %dx%d): %d pages "
                  "(%d text, %d image), text %s"
                  % (font_id, font_id, cols, rows, len(pages),
                     len(pages) - image_pages, image_pages, p.human(text_bytes)))
        print("    shared image blobs: %s in %d figure(s)"
              % (p.human(shared_image_bytes), len(seen)))
        print("    header (title plus %d page table(s)): %s"
              % (len(variants), p.human(header_len)))
        print("    file total: %s" % p.human(size))
        return 0

    cols, rows = grids[0]
    pages = [payload for payload, _anchor in
             pages_for_grid(items, cols, rows)]
    size = p.write_mpg1(out_path, title, pages, (cols, rows))

    text_bytes = 0
    image_bytes = 0
    image_pages = 0
    for page in pages:
        if isinstance(page, (bytes, bytearray)):
            image_bytes += len(page)
            image_pages += 1
        else:
            text_bytes += len("\n".join(page).encode("utf-8"))

    print("%s -> %s" % (args.txt, out_path))
    print("    title: %s" % title)
    print("    panel: %d (grid %dx%d)" % (args.panel, cols, rows))
    print("    pages: %d   size: %s" % (len(pages), p.human(size)))
    print("    breakdown: text %s in %d page(s), images %s in %d page(s), "
          "file total %s"
          % (p.human(text_bytes), len(pages) - image_pages,
             p.human(image_bytes), image_pages, p.human(size)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

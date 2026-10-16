#!/usr/bin/env python3
"""Diff the on-device paginator against the host pipeline.

The firmware paginates a downloaded news brief itself (src/text_paginate.cpp).
That C++ has to produce exactly what tools/txt2book.py would have produced for
the same plain text, or a book made on the device would not match one made on
the host. This script is how the two are kept in sync: it compiles the C++ with
the system compiler, runs both over the same input, and diffs the pages line by
line.

It also checks re-pagination (books::reformat, for a book made for another
panel): the C++ paginates the text on a source grid, rejoins those pages into
paragraphs with textpage::Rejoiner and paginates them again on the target
grid. That has to match the host paginating the original text on the target
grid directly, anchors included.

Usage:
    python3 tools/check_paginator.py                 built-in sample, every
                                                     grid of every panel plus
                                                     reflow between panels
    python3 tools/check_paginator.py some_brief.txt  a file of your own
    python3 tools/check_paginator.py --cols 37 --rows 32
    python3 tools/check_paginator.py --cols 37 --rows 32 --src-cols 54 --src-rows 37

Exit status is 0 when every page of every grid matches.
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pdf2book as p


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
TOOLS = os.path.join(ROOT, "tools")

# Exercises the transliteration table, the page number filter, dehyphenation,
# paragraph joining, space collapsing, hard word breaks and blank line runs.
SAMPLE = (
    "The Muon Bench News Brief\n"
    "\n"
    "12\n"
    "\n"
    "Curly \u201cquotes\u201d and \u2018singles\u2019 and a long dash \u2014 plus an\n"
    "ellipsis… and a non breaking space and a soft­hyphen.\n"
    "\n"
    "A paragraph that is hard wrapped by the sender with a hyphen-\n"
    "ated word split across the line break, which the cleaner has to\n"
    "put back together again before it wraps to the reading grid.\n"
    "\n"
    "iv\n"
    "\n"
    "Ligatures: oﬀice, ﬁnd, ﬂow, and a fraction slash ⁄ too.\n"
    "Bullets • become stars and ‹guillemets› become angle brackets.\n"
    "\n"
    "Supercalifragilisticexpialidociousandthensomemoreletterspastthecolumnlimitto"
    "forceahardbreakintheword right here.\n"
    "\n"
    "  Leading and trailing spaces   with   internal   runs   collapse.  \n"
    "\n"
    "\n"
    "\n"
    "A final short paragraph.\n"
) + ("Filler sentence number %d to push the text past a single page boundary. "
     % 0) + " ".join(
    "Filler sentence number %d to push the text past a single page boundary."
    % i for i in range(1, 60)) + "\n"


def host_pages(text, cols, rows):
    """Run the host pipeline exactly as txt2book.py does for a plain text.

    Returns (pages, anchors)."""
    raw_pages = text.split("\f")
    paragraphs = p.pages_to_paragraphs(raw_pages)
    if not paragraphs:
        return [[""]], [0]
    pages, _consumed = p.paginate_spans(paragraphs, cols, rows, 0)
    return ([lines for lines, _anchor in pages],
            [anchor for _lines, anchor in pages])


def device_pages(binary, data, cols, rows, src=None):
    """Run the C++ harness. With `src` (cols, rows) it paginates on that grid
    first and re-paginates the result on (cols, rows). Returns (pages, anchors).
    """
    cmd = [binary, str(cols), str(rows)]
    if src is not None:
        cmd += [str(src[0]), str(src[1])]
    proc = subprocess.run(cmd, input=data,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
        raise SystemExit("paginate_test failed")
    pages = []
    anchors = []
    current = None
    for line in proc.stdout.decode("utf-8").split("\n")[:-1]:
        if line.startswith("=== page "):
            if current is not None:
                pages.append(current)
            current = []
            fields = line.split()
            anchors.append(int(fields[-1]) if "anchor" in fields else None)
        else:
            current.append(line)
    if current is not None:
        pages.append(current)
    return pages, anchors


def build(out_path):
    cmd = ["c++", "-std=c++11", "-O1", "-Wall", "-I", SRC,
           os.path.join(TOOLS, "paginate_test.cpp"),
           os.path.join(SRC, "text_paginate.cpp"),
           "-o", out_path]
    print("building: %s" % " ".join(cmd))
    subprocess.run(cmd, check=True)


def compare(host, device, cols, rows, host_anchors=None, device_anchors=None):
    ok = True
    if host_anchors is not None and device_anchors is not None:
        for i in range(min(len(host_anchors), len(device_anchors))):
            if host_anchors[i] != device_anchors[i]:
                print("  page %d anchor differs: host %s, device %s"
                      % (i, host_anchors[i], device_anchors[i]))
                ok = False
                break
    if len(host) != len(device):
        print("  page count differs: host %d, device %d"
              % (len(host), len(device)))
        ok = False
    for i in range(min(len(host), len(device))):
        if host[i] != device[i]:
            ok = False
            print("  page %d differs" % i)
            for j in range(max(len(host[i]), len(device[i]))):
                h = host[i][j] if j < len(host[i]) else "<missing>"
                d = device[i][j] if j < len(device[i]) else "<missing>"
                if h != d:
                    print("    line %d host   : %r" % (j, h))
                    print("    line %d device : %r" % (j, d))
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("txt", nargs="?", default=None,
                    help="input text file (default: a built-in sample)")
    ap.add_argument("--cols", type=int, default=None)
    ap.add_argument("--rows", type=int, default=None)
    ap.add_argument("--src-cols", type=int, default=None,
                    help="with --src-rows: check re-pagination from this grid")
    ap.add_argument("--src-rows", type=int, default=None)
    args = ap.parse_args(argv)

    if args.txt:
        with open(args.txt, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    else:
        text = SAMPLE
    data = text.encode("utf-8")

    # (target grid, source grid or None). Default: every text grid of every
    # panel, then each font's grid reflowed from every other panel's grid.
    checks = []
    if args.cols and args.rows:
        src = None
        if args.src_cols and args.src_rows:
            src = (args.src_cols, args.src_rows)
        checks.append(((args.cols, args.rows), src))
    else:
        by_panel = [(panel, p.panel_grids(panel)) for panel in sorted(p.PANELS)]
        for _panel, grids in by_panel:
            for font_id in sorted(grids):
                checks.append((grids[font_id], None))
        for _panel, grids in by_panel:
            for _other, other_grids in by_panel:
                for font_id in sorted(grids):
                    if grids[font_id] != other_grids[font_id]:
                        checks.append((grids[font_id], other_grids[font_id]))

    with tempfile.TemporaryDirectory() as tmp:
        binary = os.path.join(tmp, "paginate_test")
        build(binary)
        all_ok = True
        for (cols, rows), src in checks:
            host, host_anchors = host_pages(text, cols, rows)
            device, device_anchors = device_pages(binary, data, cols, rows, src)
            label = "grid %dx%d" % (cols, rows)
            if src is not None:
                label += " reflowed from %dx%d" % src
            print("%s: host %d pages, device %d pages"
                  % (label, len(host), len(device)))
            if compare(host, device, cols, rows, host_anchors, device_anchors):
                print("  match")
            else:
                all_ok = False

    if not all_ok:
        print("MISMATCH: the paginators have drifted apart")
        return 1
    print("all grids match")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Convert a Digital Vector File (.vec) into a Markdown document.
How to use:
    python3 vec_to_md.py INPUT_FILENAME.vec -o OUTPUT_FILENAME.md
"""

import argparse
import re
import sys
from pathlib import Path

SIGNAL_RE = re.compile(r"^\s*\d")


def parse_vec(lines):
    """Return (signal_names, blocks).

    Each block is ('comment', [str, ...]) or ('table', [[cell, ...], ...]).
    """
    names = None
    blocks = []

    def push(kind, item):
        if blocks and blocks[-1][0] == kind:
            blocks[-1][1].append(item)
        else:
            blocks.append((kind, [item]))

    for raw in lines:
        line = raw.rstrip("\n")
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith(";"):
            push("comment", stripped[1:].strip())
        elif SIGNAL_RE.match(line):
            push("table", stripped.split())
        elif names is None and stripped.split()[0] == "vname":
            names = stripped.split()[1:]

    if names is None:
        raise ValueError("no `vname` line found")
    return names, blocks


def md_escape(text):
    return text.replace("|", r"\|")


def render(names, blocks, title=None):
    header = ["time"] + names
    out = []
    if title:
        out.append(f"# {title}\n")

    for kind, items in blocks:
        if kind == "comment":
            for text in items:
                # two trailing spaces = hard line break, so comment lines
                # keep the layout they had in the .vec file
                out.append(md_escape(text) + "  " if text else "")
            out.append("")
        else:
            width = len(header)
            out.append("| " + " | ".join(f"`{md_escape(h)}`" for h in header) + " |")
            out.append("|" + "|".join(["---"] * width) + "|")
            for row in items:
                cells = [md_escape(c) for c in row[:width]]
                cells += [""] * (width - len(cells))
                out.append("| " + " | ".join(cells) + " |")
            out.append("")

    return "\n".join(out).rstrip() + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Convert a .vec vector file to Markdown.")
    ap.add_argument("input", type=Path, help="input .vec file")
    ap.add_argument("-o", "--output", type=Path, help="output .md file (default: alongside input)")
    ap.add_argument("--no-title", action="store_true", help="omit the file-name heading")
    args = ap.parse_args(argv)

    text = args.input.read_text(encoding="utf-8", errors="replace")
    names, blocks = parse_vec(text.splitlines())
    md = render(names, blocks, None if args.no_title else args.input.name)

    out = args.output or args.input.with_suffix(".md")
    out.write_text(md, encoding="utf-8")
    print(f"wrote {out} ({len(names)} signals)", file=sys.stderr)


if __name__ == "__main__":
    main()

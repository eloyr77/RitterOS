#!/usr/bin/env python3
"""doccheck.py [--index]: the manual's command pages (50j) against the programs.

Every program (user/bin/*.c, user/apps/*.c) must have docs/commands/NAME.md
whose title is `# NAME: DESCRIPTION` and whose first code block's first line
is the usage, both exactly as the program's PROGRAM_HELP gives them (what
`help` shows), so the pages and the programs cannot drift apart. Every page
must be listed in docs/commands/README.md, the index. Exit 1 with a line per
problem; --index writes the index first (from the pages' titles).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGES = os.path.join(ROOT, "docs", "commands")
INDEX = os.path.join(PAGES, "README.md")
STRING = r'"((?:[^"\\]|\\.)*)"'


def program_help(path):
    """(usage, description) from a source file's PROGRAM_HELP, or None."""
    text = open(path, encoding="utf-8").read()
    m = re.search(r'PROGRAM_HELP\(\s*((?:' + STRING + r'\s*)+),\s*((?:' + STRING + r'\s*)+)\)', text)
    if not m:
        return None
    join = lambda s: "".join(re.findall(STRING, s)).encode().decode("unicode_escape")
    return join(m.group(1)), join(m.group(3))


def programs():
    found = {}
    for folder in ("user/bin", "user/apps"):
        for name in sorted(os.listdir(os.path.join(ROOT, folder))):
            if name.endswith(".c"):
                found[name[:-2]] = os.path.join(ROOT, folder, name)
    return found


def page_parts(path):
    """(title, the first code block's first line) of a page."""
    title = synopsis = None
    in_code = False
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if title is None and line.startswith("# "):
            title = line[2:]
        elif line.startswith("```"):
            if in_code:
                break
            in_code = True
        elif in_code and synopsis is None:
            synopsis = line
    return title, synopsis


def write_index(pages):
    lines = ["# The commands", "",
             "A page for every program of RitterOS (on the machine: `man NAME`; `man`",
             "lists them, `man -k WORD` searches). The install guide is",
             "[install.md](../install.md), the user's manual [manual.md](../manual.md).",
             "This list is made by `python3 scripts/doccheck.py --index`.", "",
             "| Program | What it does |", "|---|---|"]
    for name in sorted(pages):
        title, _ = page_parts(os.path.join(PAGES, name + ".md"))
        what = title.split(": ", 1)[1] if title and ": " in title else ""
        lines.append(f"| [`{name}`]({name}.md) | {what} |")
    with open(INDEX, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    pages = {f[:-3] for f in os.listdir(PAGES) if f.endswith(".md") and f != "README.md"}
    if "--index" in sys.argv[1:]:
        write_index(pages)
    problems = []
    for name, source in programs().items():
        help_text = program_help(source)
        if help_text is None:
            problems.append(f"{name}: no PROGRAM_HELP in {os.path.relpath(source, ROOT)}")
            continue
        usage, description = help_text
        page = os.path.join(PAGES, name + ".md")
        if name not in pages:
            problems.append(f"{name}: no page docs/commands/{name}.md")
            continue
        title, synopsis = page_parts(page)
        if title != f"{name}: {description}":
            problems.append(f"{name}: the title is '{title}', the program says '{name}: {description}'")
        if synopsis != usage:
            problems.append(f"{name}: the usage is '{synopsis}', the program says '{usage}'")
    for name in sorted(pages - set(programs())):
        problems.append(f"{name}: a page for no program")
    index = open(INDEX, encoding="utf-8").read() if os.path.exists(INDEX) else ""
    for name in sorted(pages):
        if f"]({name}.md)" not in index:
            problems.append(f"{name}: not in docs/commands/README.md (doccheck.py --index)")
    for p in problems:
        print("doccheck: " + p)
    print(f"doccheck: {len(pages)} page(s), {len(programs())} program(s), {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

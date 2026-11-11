#!/usr/bin/env python3
"""(37e-1b) A settings file's default text from common/settings_defaults.h,
for the tests: settingstext.py NAME prints SETTINGS_NAME_TEXT as the file
holds it (NAME: CPU, POWER, ...); settingstext.py --list prints every
NAME and its path, one per line."""
import os
import re
import sys

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common",
                      "settings_defaults.h")


def texts():
    """NAME -> (path, text) for every file of the header."""
    source = open(HEADER).read()
    paths = dict(re.findall(r'^#define SETTINGS_([A-Z_]+) "([^"]*)"$', source, re.M))
    out = {}
    for name, body in re.findall(r'^#define SETTINGS_([A-Z_]+)_TEXT \\\n((?:    ".*"(?: \\)?\n)+)',
                                 source, re.M):
        text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', body))
        text = re.sub(r'\\(.)', lambda m: {"n": "\n", "\\": "\\", '"': '"'}[m.group(1)], text)
        out[name] = (paths[name], text)
    return out


def main():
    t = texts()
    if sys.argv[1:] == ["--list"]:
        for name, (path, _) in t.items():
            print(name, path)
    elif len(sys.argv) == 2 and sys.argv[1] in t:
        sys.stdout.write(t[sys.argv[1]][1])
    else:
        sys.exit("usage: settingstext.py NAME | --list (NAME: %s)" % ", ".join(t))


if __name__ == "__main__":
    main()

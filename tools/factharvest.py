#!/usr/bin/env python3
"""factharvest: build a fact table for this machine's own processes.

Reads the distinct process names from the ActivityScope history (read-only)
and asks the machine itself what each one is: man page one-liners first
(best per-binary sentences), then desktop entries, then systemd unit
descriptions, then the owning package's description, then path rules.
Writes ~/.local/share/activityscope/facts/local.tsv, which layers between
the shipped base.tsv and the user's custom.tsv. Run it whenever; it never
touches the shipped or custom files.

  factharvest            write local.tsv
  factharvest --dry-run  print what would be written
"""

import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile

HOME = os.path.expanduser("~")
DATA = os.environ.get("XDG_DATA_HOME", HOME + "/.local/share") + "/activityscope"
DB = os.environ.get("ASK_DB", DATA + "/history.db")
OUT = DATA + "/facts/local.tsv"


def run(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def clean(text):
    text = " ".join(text.split())
    return text.replace("\t", " ").strip()


def first_sentence(text):
    text = clean(text)
    m = re.split(r"(?<=[.!?]) ", text, maxsplit=1)
    s = m[0].rstrip(".")
    return s[:160]


def names_from_db():
    con = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    rows = con.execute(
        "SELECT name, COUNT(*) c, MAX(IFNULL(exe,'')), MAX(IFNULL(cmdline,'')), MAX(IFNULL(unit,'')) "
        "FROM procs GROUP BY name ORDER BY c DESC").fetchall()
    con.close()
    return rows


def whatis(name):
    out = run(["whatis", "-l", name])
    for line in out.splitlines():
        # "sleep (1) - delay for a specified amount of time"
        m = re.match(r"\S+\s*\((\d+)\w*\)\s*-\s*(.+)", line)
        if m and m.group(1) in ("1", "8", "6"):
            return first_sentence(m.group(2))
    return ""


def desktop_index():
    idx = {}
    dirs = [HOME + "/.local/share/applications", "/usr/share/applications"]
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fn in os.listdir(d):
            if not fn.endswith(".desktop"):
                continue
            try:
                txt = open(os.path.join(d, fn), encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            exe = comment = generic = name = ""
            for line in txt.splitlines():
                if line.startswith("Exec=") and not exe:
                    exe = os.path.basename(line[5:].split()[0]) if line[5:].split() else ""
                elif line.startswith("Comment=") and not comment:
                    comment = line[8:]
                elif line.startswith("GenericName=") and not generic:
                    generic = line[12:]
                elif line.startswith("Name=") and not name:
                    name = line[5:]
            best = comment or generic or name
            if exe and best and exe not in idx:
                idx[exe.lower()] = first_sentence(best)
    return idx


def unit_description(unit):
    if not unit or unit.endswith(".scope"):
        return ""
    for scope in (["systemctl"], ["systemctl", "--user"]):
        out = run(scope + ["show", "-p", "Description", "--value", unit])
        d = clean(out)
        if d and d != unit:
            return first_sentence(d)
    return ""


def pacman_owner(exe):
    if not exe or not exe.startswith("/"):
        return ""
    out = run(["pacman", "-Qoq", exe])
    return out.strip().splitlines()[0] if out.strip() else ""


def pacman_description(pkg):
    out = run(["pacman", "-Qi", pkg])
    for line in out.splitlines():
        if line.startswith("Description"):
            return first_sentence(line.split(":", 1)[1])
    return ""


PATH_RULES = [
    ("/tmp/", "test", "a throwaway binary run from the temporary folder, usually a test fixture"),
    ("/usr/local/", "user-built", "a manually installed program, not from any package"),
    ("/build/", "user-built", "a binary built from source in one of your own project folders"),
    ("ms-playwright", "test", "a browser bundled by Playwright for automated testing"),
]

INTERPRETERS = ("python", "python3", "node")


def script_of(cmdline):
    parts = cmdline.split()
    for p in parts[1:]:
        if not p.startswith("-"):
            return os.path.basename(p)
    return ""


def harvest():
    rows = names_from_db()
    desktops = desktop_index()
    out = []
    for name, _count, exe, cmdline, unit in rows:
        base = os.path.basename(exe) if exe else ""
        lookup = name
        # A truncated 15-char comm matches its binary by prefix.
        if base and base != name and base.startswith(name):
            lookup = base
        # Version-suffixed names fold onto their tool: ghc-pkg-9.6.6.
        stripped = re.sub(r"-[0-9][0-9.]*$", "", lookup)
        explanation = category = source = ""
        w = whatis(lookup) or (whatis(stripped) if stripped != lookup else "")
        if w:
            explanation, category, source = w, "cli-tool", "man"
        if not explanation:
            d = desktops.get(lookup.lower()) or desktops.get(name.lower())
            if d:
                explanation, category, source = d, "app", "desktop"
        if not explanation and unit:
            u = unit_description(unit)
            if u:
                explanation, category, source = u + " (ran inside " + unit + ")", "system-service", "unit:" + unit
        if not explanation and exe:
            pkg = pacman_owner(exe)
            if pkg:
                p = pacman_description(pkg)
                if p:
                    explanation, category, source = p + " (from the " + pkg + " package)", "app", "pacman:" + pkg
        if not explanation and exe:
            for frag, cat, text in PATH_RULES:
                if frag in exe:
                    explanation, category, source = text, cat, "path"
                    break
        if explanation and os.path.basename(exe or "") in INTERPRETERS:
            s = script_of(cmdline or "")
            if s:
                explanation += "; here it ran " + s
        if explanation:
            # Fold a leading capital unless it looks like an acronym (DBus).
            if explanation[0].isupper() and not explanation[1:2].isupper():
                explanation = explanation[0].lower() + explanation[1:]
            out.append((name, category, explanation, source))
    return out, len(rows)


def main():
    dry = "--dry-run" in sys.argv
    facts, total = harvest()
    lines = ["# Generated by factharvest from this machine; regenerate any time.",
             "# Hand edits belong in custom.tsv, which outranks this file."]
    for name, cat, expl, src in facts:
        lines.append(f"{name}\t{cat}\t{expl}\t{src}")
    body = "\n".join(lines) + "\n"
    if dry:
        sys.stdout.write(body)
        print(f"# {len(facts)} of {total} names explained", file=sys.stderr)
        return
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(OUT))
    with os.fdopen(fd, "w") as f:
        f.write(body)
    shutil.move(tmp, OUT)
    print(f"factharvest: {len(facts)} of {total} names explained -> {OUT}")


if __name__ == "__main__":
    main()

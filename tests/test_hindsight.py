"""Tests run the whole tool against a sandbox; nothing real is touched."""

import contextlib
import importlib.machinery
import importlib.util
import io
import os
import sqlite3
import subprocess
import tempfile
import time
import unittest

BASE = tempfile.mkdtemp(prefix="hindsight-test-")
os.environ["HINDSIGHT_HOME"] = os.path.join(BASE, "home")
os.environ["HINDSIGHT_ETC"] = os.path.join(BASE, "etc")
os.environ["HINDSIGHT_DATA"] = os.path.join(BASE, "data")
os.environ["HINDSIGHT_AS_DB"] = os.path.join(BASE, "history.db")
os.environ["HINDSIGHT_PACMAN_LOG"] = os.path.join(BASE, "pacman.log")

_loader = importlib.machinery.SourceFileLoader(
    "hindsight", os.path.join(os.path.dirname(__file__), "..", "tools", "hindsight"))
_spec = importlib.util.spec_from_loader("hindsight", _loader)
hs = importlib.util.module_from_spec(_spec)
_loader.exec_module(hs)


def write(path, text="x\n"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def commits():
    r = subprocess.run(["git", "-C", hs.REPO, "rev-list", "--count", "HEAD"],
                       capture_output=True, text=True)
    return int(r.stdout.strip() or 0)


def quiet(fn, *a):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        fn(*a)
    return buf.getvalue()


class TestSnap(unittest.TestCase):
    def test_snapshot_flow(self):
        home, etc = hs.HOME, hs.ETC
        write(etc + "/normal.conf", "key=1\n")
        write(etc + "/ssl/private.conf", "never\n")
        write(etc + "/apitoken.conf", "never\n")
        write(etc + "/huge.conf", "x" * 600 * 1024)
        write(home + "/.config/kwinrc", "[a]\nb=1\n")
        write(home + "/.config/kwalletrc", "never\n")
        write(home + "/.config/gh/hosts.yml", "never\n")
        write(home + "/.config/firefox/prefs.js", "never\n")
        write(home + "/.bashrc", "alias ll='ls -l'\n")
        unreadable = etc + "/locked.conf"
        write(unreadable, "secret root stuff\n")
        os.chmod(unreadable, 0)

        out = quiet(hs.snap, "test baseline")
        self.assertIn("Snapshot taken", out)
        self.assertEqual(commits(), 1)
        self.assertEqual(os.stat(hs.DATA).st_mode & 0o777, 0o700)

        kept = os.path.join(hs.REPO, "etc/normal.conf")
        self.assertTrue(os.path.exists(kept))
        self.assertTrue(os.path.exists(os.path.join(hs.REPO, "home/.bashrc")))
        self.assertTrue(os.path.exists(os.path.join(hs.REPO, "home/config/kwinrc")))
        for never in ["etc/ssl/private.conf", "etc/apitoken.conf", "etc/huge.conf",
                      "etc/locked.conf", "home/config/kwalletrc",
                      "home/config/gh/hosts.yml", "home/config/firefox/prefs.js"]:
            self.assertFalse(os.path.exists(os.path.join(hs.REPO, never)), never)

        out = quiet(hs.snap, "no change")
        self.assertIn("No changes", out)
        self.assertEqual(commits(), 1)

        write(etc + "/normal.conf", "key=2\n")
        # the edit happens within the same second as the baseline; give the
        # file a later clock so rsync's quick check sees it, as real edits do
        later = time.time() + 5
        os.utime(etc + "/normal.conf", (later, later))
        quiet(hs.snap, "edited")
        self.assertEqual(commits(), 2)

        os.remove(home + "/.config/kwinrc")
        quiet(hs.snap, "removed")
        self.assertEqual(commits(), 3)
        self.assertFalse(os.path.exists(os.path.join(hs.REPO, "home/config/kwinrc")))

        os.chmod(unreadable, 0o644)  # so the sandbox can be deleted

        rows = hs.config_entries(0, False)
        self.assertTrue(any("config file" in text for _, _, text in rows))
        self.assertTrue([text for _, _, text in rows if "[edited]" in text])


class TestStorySources(unittest.TestCase):
    def test_pacman_batches(self):
        t0 = "2026-10-01T10:00:%02d-0400"
        lines = ["[%s] [ALPM] upgraded pkg%d (1-1 -> 1-2)" % (t0 % i, i) for i in range(12)]
        lines.append("[2026-10-01T11:00:00-0400] [ALPM] installed bar (1-1)")
        lines.append("[2020-01-01T00:00:00-0400] [ALPM] removed old (1-1)")
        write(hs.PACMAN_LOG, "\n".join(lines) + "\n")
        since = time.mktime(time.strptime("2026-09-01", "%Y-%m-%d"))
        rows = hs.pacman_entries(since)
        self.assertEqual(len(rows), 2)  # one batch per gap over 10 minutes
        self.assertIn("upgraded 12 packages", rows[0][2])
        self.assertIn("installed bar", rows[1][2])
        self.assertFalse(any("removed old" in r[2] for r in rows))  # before since

    def test_activityscope_entries(self):
        db = sqlite3.connect(hs.AS_DB)
        db.execute("CREATE TABLE log(t REAL, category TEXT, title TEXT)")
        db.execute("CREATE TABLE events(start REAL, notable INTEGER, summary TEXT)")
        db.execute("INSERT INTO log VALUES (100, 'software', 'Upgraded foo')")
        db.execute("INSERT INTO log VALUES (200, 'network', 'claude connected somewhere')")
        db.execute("INSERT INTO events VALUES (150, 0, '10:00:00 to 10:01:00 (1 min). CPU peaked at 83%, mostly blender.')")
        db.execute("INSERT INTO events VALUES (300, 1, '11:00:00 to 11:02:00 (2 min). It came close to shutting down.')")
        db.commit()
        db.close()
        rows = sorted(hs.activityscope_entries(0, False))
        tags = [tag for _, tag, _ in rows]
        self.assertNotIn("log software", tags)  # pacman's log tells that story
        self.assertEqual(tags, ["routine", "log network", "event"])  # time order
        self.assertTrue(rows[0][2].startswith("CPU peaked"))  # time range stripped

    def test_story_prints_in_order(self):
        out = quiet(hs.story, "@0", False, False)
        times = [line[2:7] for line in out.splitlines() if line.startswith("  ")]
        self.assertTrue(len(times) >= 4)
        days = [line for line in out.splitlines() if line and not line.startswith(" ")]
        self.assertTrue(days)


class TestTimeDiff(unittest.TestCase):
    def test_diff_since_phrase(self):
        if commits() == 0:  # running alone: build the same snapshots
            write(hs.ETC + "/normal.conf", "key=1\n")
            write(hs.HOME + "/.config/kwinrc", "[a]\nb=1\n")
            quiet(hs.snap, "test baseline")
            write(hs.ETC + "/normal.conf", "key=2\n")
            later = time.time() + 5
            os.utime(hs.ETC + "/normal.conf", (later, later))
            os.remove(hs.HOME + "/.config/kwinrc")
            quiet(hs.snap, "edited")
        import sys
        old_argv = sys.argv
        try:
            sys.argv = ["hindsight", "diff", "@0"]  # before every snapshot
            out = quiet(hs.main)
            self.assertIn("comparing against the oldest", out)
            self.assertIn("key=2", out)           # the edit is in the diff
            self.assertIn("kwinrc", out)          # the deletion is in the diff
            sys.argv = ["hindsight", "diff", "now"]
            out = quiet(hs.main)
            self.assertIn("Nothing has changed", out)
            sys.argv = ["hindsight", "diff", "HEAD"]  # a git id still works
            out = quiet(hs.main)
            self.assertIn("snapshot", out)
        finally:
            sys.argv = old_argv


class TestHelpers(unittest.TestCase):
    def test_trim(self):
        self.assertEqual(hs.trim("short"), "short")
        long = "word " * 100
        cut = hs.trim(long)
        self.assertTrue(len(cut) <= 175 and cut.endswith("..."))
        self.assertNotIn("  ", cut)

    def test_parse_when(self):
        self.assertEqual(hs.parse_when("@1700000000"), 1700000000)
        with self.assertRaises(SystemExit):
            hs.parse_when("not a time at all zzz")


if __name__ == "__main__":
    unittest.main(verbosity=2)

#!/usr/bin/env python3
"""Progress for any long task, shown in the KDE system tray by
progress_tray.py and in the terminal by `unllm_ingest.py progress --watch`.

From Python:

    from taskprogress import Task
    t = Task("layer-sweep", "Layer sweep", total=5700)
    t.skip(300)                        # done in an earlier run: counted, not this run's pace
    t.step(detail="setting 4 of 19")   # one unit done
    t.finish()

From a shell script (the task belongs to the calling shell unless --pid
names another process):

    taskprogress.py start backup "Backup" 120
    taskprogress.py skip backup 40                     # done in an earlier run
    taskprogress.py step backup --detail "photos"      # one more done
    taskprogress.py set backup 57                      # or say how many
    taskprogress.py finish backup

Each task is ~/.local/share/unllm/progress/task-<name>.json (pid, title,
done, total, started, detail, moved: when done last changed, and recent:
samples of this run's work about 5 s apart, for the last minutes). A task
whose process is gone shows as stopped.

The time left is the work still to do at the pace of the last five minutes
(this run's average until there are enough samples), counted from the last
finished step rather than from now. So it counts down while a step runs,
and is not shown once that step has run past the estimate (a growing guess
helps nobody). A slowdown still raises it, as it should, but it settles
within a few steps instead of climbing for the rest of the job. Work done
in an earlier run goes through skip(), so it never counts as this run's pace.
"""

import argparse
import glob
import json
import os
import sys
import threading
import time

PROGRESS = os.path.expanduser("~/.local/share/unllm/progress")
WINDOW = 300  # seconds before the last step that the pace is read from
SPACING = 5  # seconds between kept samples
KEEP = 60  # samples kept per task


def path_of(name):
    return os.path.join(PROGRESS, f"task-{name}.json")


def save(path, state):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, path)


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def alive(pid):
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, ValueError, TypeError):
        return False


def record(state, now):
    """Keep [time, work done this run] in state["recent"]: the newest sample always, older ones SPACING apart."""
    did = state["done"] - state.get("start_done", 0)
    recent = state.setdefault("recent", [])
    if len(recent) >= 2 and recent[-1][0] - recent[-2][0] < SPACING:
        recent[-1] = [now, did]
    else:
        recent.append([now, did])
    del recent[:-KEEP]


def pace(state):
    """Units per second over the samples of the WINDOW before the last step (at least the last three), else
    this run's average. None when nothing has been done in this run."""
    moved = state.get("moved")
    if not moved:
        return None
    recent = state.get("recent") or []
    window = [s for s in recent if s[0] >= moved - WINDOW]
    if len(window) < 3:
        window = recent[-3:]
    if len(window) >= 2 and window[-1][0] - window[0][0] > 10 and window[-1][1] > window[0][1]:
        return (window[-1][1] - window[0][1]) / (window[-1][0] - window[0][0])
    did = state["done"] - state.get("start_done", 0)
    took = moved - state.get("started", moved)
    return did / took if did > 0 and took > 10 else None


def time_left(state, now):
    """Seconds left at the current pace, counted from the last step; None when unknown, when the task is done,
    or once the current step has run past the estimate."""
    moved, rate = state.get("moved"), pace(state)
    if not moved or not rate or state["done"] >= state["total"]:
        return None
    left = moved + (state["total"] - state["done"]) / rate - now
    return left if left > 0 else None


class Task:
    def __init__(self, name, title, total, done=0, pid=None):
        self.path = path_of(name)
        self.state = {"pid": pid or os.getpid(), "title": title, "done": done, "total": total,
                      "started": time.time(), "start_done": done, "detail": "", "recent": []}
        self.lock = threading.Lock()
        self.last = 0.0
        os.makedirs(PROGRESS, exist_ok=True)
        save(self.path, self.state)

    def step(self, n=1, detail=None):
        with self.lock:
            now = time.time()
            self.state["done"] += n
            if n:
                self.state["moved"] = now
            if detail is not None:
                self.state["detail"] = detail
            if now - self.last > 1 or self.state["done"] >= self.state["total"]:
                self.last = now
                record(self.state, now)
                save(self.path, self.state)

    def skip(self, n, detail=None):
        """n units finished in an earlier run (or needing no work): counted as done, not as this run's pace."""
        with self.lock:
            self.state["done"] += n
            self.state["start_done"] += n
            if detail is not None:
                self.state["detail"] = detail
            save(self.path, self.state)

    def note(self, detail):
        with self.lock:
            self.state["detail"] = detail
            save(self.path, self.state)

    def finish(self):
        try:
            os.remove(self.path)
        except OSError:
            pass


class Pace:
    """Time left for a number a watcher polls rather than a task writes (a pack build's fraction, an index's
    passage count): the same samples and estimate as a task file, kept per key while the watcher runs. The
    first poll of a build is the baseline, so a single poll shows no time."""

    def __init__(self):
        self.seen = {}

    def left(self, key, done, total, started, now):
        t = self.seen.get(key)
        if t is None or t["total"] != total or done < t["done"]:  # a new build under the same key
            t = self.seen[key] = {"done": done, "total": total, "started": started or now, "start_done": 0,
                                  "recent": []}
            record(t, now)
        elif done > t["done"]:
            t["done"], t["moved"] = done, now
            record(t, now)
        return time_left(t, now)


def task_states():
    """Every task file: a dict each with name, title, fraction, done, total,
    left (seconds, when known), detail, and alive (False when the file is
    there but its process has died)."""
    now = time.time()
    out = []
    for path in sorted(glob.glob(os.path.join(PROGRESS, "task-*.json"))):
        t = load(path)
        if not t:
            continue
        total = max(1, int(t.get("total", 1)))
        done = t.get("done", 0)
        out.append({"name": os.path.basename(path)[5:-5], "title": t.get("title", "?"),
                    "fraction": min(1.0, done / total), "done": done, "total": total, "detail": t.get("detail", ""),
                    "alive": alive(t.get("pid")), "left": time_left(dict(t, done=done, total=total), now),
                    "pid": t.get("pid")})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("start")
    s.add_argument("name")
    s.add_argument("title")
    s.add_argument("total", type=int)
    s.add_argument("--pid", type=int, help="the process the task belongs to (default: the calling shell)")
    st = sub.add_parser("step")
    st.add_argument("name")
    st.add_argument("n", nargs="?", type=int, default=1)
    st.add_argument("--detail")
    se = sub.add_parser("set")
    se.add_argument("name")
    se.add_argument("done", type=int)
    se.add_argument("--detail")
    sk = sub.add_parser("skip", help="units finished in an earlier run: counted, not as this run's pace")
    sk.add_argument("name")
    sk.add_argument("n", type=int)
    sk.add_argument("--detail")
    f = sub.add_parser("finish")
    f.add_argument("name")
    args = ap.parse_args()
    path = path_of(args.name)
    if args.cmd == "start":
        Task(args.name, args.title, args.total, pid=args.pid or os.getppid())
        return 0
    if args.cmd == "finish":
        try:
            os.remove(path)
        except OSError:
            pass
        return 0
    state = load(path)
    if not state:
        print(f"taskprogress: no task {args.name}", file=sys.stderr)
        return 1
    now = time.time()
    if args.cmd == "skip":
        state["done"] += args.n
        state["start_done"] = state.get("start_done", 0) + args.n
    else:
        done = state["done"] + args.n if args.cmd == "step" else args.done
        if done != state["done"]:
            state["moved"] = now
        state["done"] = done
        record(state, now)
    if args.detail is not None:
        state["detail"] = args.detail
    save(path, state)
    return 0


if __name__ == "__main__":
    sys.exit(main())

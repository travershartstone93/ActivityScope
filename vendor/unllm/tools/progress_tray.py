#!/usr/bin/env python3
"""Shows unllm's long jobs as KDE progress items in the system tray (the
same kind Dolphin shows for a file copy), so nobody has to keep a terminal
open: pack builds, search index builds, and any task writing
~/.local/share/unllm/progress/task-*.json (tools/taskprogress.py).

Runs in the background (tools/systemd/unllm-progress-tray.service) and looks
every 2 s. A job's item appears when it starts and closes when it ends;
one whose process died closes as "stopped". The terminal view
(`unllm_ingest.py progress --watch`) shows the same numbers.

Plasma draws any job below 1 % as a bar sliding back and forth (it has no
separate "unknown" state), so a live job is sent as at least 1 %. There is
no item for a command with nothing to count: Plasma could only draw it
sliding, which reads as broken. Anything long gets a counted task instead.

Needs a KDE Plasma session (org.kde.JobViewServer) and python-gobject.
Without unllm_ingest.py next to it, only task files are shown.
"""

import math
import os
import signal
import sys
import time

from gi.repository import Gio, GLib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from taskprogress import task_states  # noqa: E402

try:  # pack and search index builds, when the pack builder is here
    import unllm_ingest as ui
except ImportError:
    ui = None

SERVICE, SERVER = "org.kde.JobViewServer", "/JobViewServer"
DESKTOP = "unllm-progress"  # ~/.local/share/applications/unllm-progress.desktop: the name and icon shown


def human_time(seconds):
    s = int(max(0, seconds))
    if s < 60:
        return f"{s} s"
    if s < 3600:
        return f"{s // 60} min"
    return f"{s // 3600} h {s % 3600 // 60} min"


def percent(fraction):
    """What Plasma is told: whole percent, rounded up, never below 1 (below 1 it draws the bar sliding)."""
    return min(100, max(1, math.ceil(100 * max(0.0, fraction))))


def jobs():
    """{key: {title, fraction, detail, detail2, alive}} for everything running now."""
    out = {}
    for t in task_states():
        left = f", about {human_time(t['left'])} left" if t["left"] else ""
        out["task:" + t["name"]] = {"title": t["title"], "fraction": t["fraction"], "alive": t["alive"],
                                   "detail": f"{t['done']} of {t['total']}{left}", "detail2": t["detail"]}
    if ui is None:
        return out
    running = ui.indexers_running()
    if running:
        items, states = ui.index_states(running)
        for it, st in zip(items, states):
            if st["state"] == "building":
                out["index:" + it["name"]] = {"title": f"Search index: {it['name']}", "fraction": st["fraction"],
                                             "alive": True, "detail": st["detail"], "detail2": ""}
    q = ui.load_json(ui.QUEUE)
    building = ui.packs_being_built()
    if (q and ui.alive(q.get("pid"))) or building:
        items, since = (q["items"], q.get("started")) if q and ui.alive(q.get("pid")) else (ui.discover(), None)
        states, overall, left = ui.collect(items, since)
        now = [f"{it['name']}: {st['detail']}" for it, st in zip(items, states) if st["state"] == "building"]
        finished = sum(1 for st in states if st["state"] == "done")
        tail = f", about {human_time(left)} left" if left else ""
        out["packs"] = {"title": "Pack builds", "fraction": overall, "alive": True,
                        "detail": f"{finished} of {len(items)} done{tail}", "detail2": "; ".join(now)}
    return out


class Tray:
    def __init__(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        self.views = {}  # key -> object path

    def call(self, path, method, sig, args):
        self.bus.call_sync(SERVICE, path, "org.kde.JobViewV3", method, GLib.Variant(sig, args), None,
                           Gio.DBusCallFlags.NONE, 5000, None)

    def show(self, key, job):
        if key not in self.views:
            r = self.bus.call_sync(SERVICE, SERVER, "org.kde.JobViewServerV2", "requestView",
                                   GLib.Variant("(sia{sv})", (DESKTOP, 0, {})), GLib.VariantType("(o)"),
                                   Gio.DBusCallFlags.NONE, 5000, None)
            self.views[key] = r.unpack()[0]
        fields = {"title": GLib.Variant("s", job["title"]),
                  "infoMessage": GLib.Variant("s", job["detail"]),
                  "descriptionLabel1": GLib.Variant("s", "Now"),
                  "descriptionValue1": GLib.Variant("s", job["detail2"] or job["detail"]),
                  "percent": GLib.Variant("u", percent(job["fraction"] or 0.0))}
        self.call(self.views[key], "update", "(a{sv})", (fields,))

    def close(self, key, error=""):
        path = self.views.pop(key, None)
        if path:
            try:
                self.call(path, "terminate", "(usa{sv})", (1 if error else 0, error, {}))
            except GLib.Error:
                pass

    def tick(self):
        now = jobs()
        for key in list(self.views):
            if key not in now:
                self.close(key)
        for key, job in now.items():
            if not job["alive"]:
                if key in self.views:
                    self.close(key, "stopped before finishing")
                continue
            try:
                self.show(key, job)
            except GLib.Error:  # the panel restarted: start this item again next time
                self.views.pop(key, None)


def main():
    tray = None

    # Stopped (a restart, a log-out): close every item first, or the panel keeps them frozen at their last value.
    def stop(signum, frame):
        if tray is not None:
            for key in list(tray.views):
                tray.close(key, "the progress view restarted")
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    while True:
        try:
            if tray is None:
                tray = Tray()
            tray.tick()
        except GLib.Error as e:
            print(f"progress_tray: {e.message}", file=sys.stderr, flush=True)
            tray = None
        except Exception as e:  # a half-written progress file and the like: try again next time
            print(f"progress_tray: {e!r}", file=sys.stderr, flush=True)
        time.sleep(2)


if __name__ == "__main__":
    sys.exit(main())

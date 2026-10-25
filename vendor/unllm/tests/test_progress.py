#!/usr/bin/env python3
"""Tests for the progress helpers (tools/taskprogress.py, tools/progress_tray.py):
the time left counts down and hides on overrun, settles after a slowdown instead
of climbing for the rest of the job, never counts work from an earlier run as
this run's pace, and the tray never tells Plasma 0 % (it would draw the bar
sliding). Standard library only: python3 tests/test_progress.py
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import taskprogress as tp  # noqa: E402


def simulate(step_times, total, skipped=0):
    """A task whose steps finish at the given seconds from start; the state a task file would hold."""
    state = {"done": skipped, "total": total, "started": 0.0, "start_done": skipped, "recent": []}
    for t in step_times:
        state["done"] += 1
        state["moved"] = t
        tp.record(state, t)
    return state


class TimeLeft(unittest.TestCase):
    def test_steady_job_counts_down_through_a_step_boundary(self):
        s = simulate([10 * i for i in range(1, 21)], total=100)  # 20 steps of 10 s
        self.assertAlmostEqual(tp.time_left(s, 200), 800, delta=1)
        self.assertAlmostEqual(tp.time_left(s, 205), 795, delta=1)  # counts down while the step runs
        s = simulate([10 * i for i in range(1, 22)], total=100)
        self.assertAlmostEqual(tp.time_left(s, 210), 790, delta=1)  # and keeps falling at the boundary

    def test_hidden_once_the_step_has_run_past_the_estimate(self):
        s = simulate([10 * i for i in range(1, 21)], total=100)
        self.assertIsNone(tp.time_left(s, 200 + 800 + 1))

    def test_slowdown_settles_within_ten_steps(self):
        fast = [10 * i for i in range(1, 101)]  # 100 steps of 10 s
        slow = [1000 + 30 * i for i in range(1, 51)]  # then 50 steps of 30 s
        s = simulate(fast + slow[:11], total=300)
        truth = (300 - 111) * 30
        self.assertAlmostEqual(tp.time_left(s, s["moved"]), truth, delta=truth * 0.05)
        s = simulate(fast + slow, total=300)
        truth = 150 * 30
        self.assertAlmostEqual(tp.time_left(s, s["moved"]), truth, delta=truth * 0.05)
        # the whole-run average would have said about 2500 s here: the old climbing estimate
        self.assertGreater(tp.time_left(s, s["moved"]), 4000)

    def test_estimate_falls_across_boundaries_once_the_pace_is_steady(self):
        s = simulate([10 * i for i in range(1, 101)] + [1000 + 30 * i for i in range(1, 31)], total=300)
        seen = []
        for k in range(31, 41):
            s["done"] += 1
            s["moved"] = 1000 + 30 * k
            tp.record(s, s["moved"])
            seen.append(tp.time_left(s, s["moved"]))
        self.assertEqual(seen, sorted(seen, reverse=True))

    def test_skipped_work_is_not_this_runs_pace(self):
        s = simulate([10 * i for i in range(1, 3)], total=1000, skipped=500)
        self.assertAlmostEqual(tp.time_left(s, 20), 4980, delta=1)
        s = simulate([10 * i for i in range(1, 51)], total=1000, skipped=500)
        self.assertAlmostEqual(tp.time_left(s, 500), 4500, delta=1)

    def test_pace_recovers_within_three_steps_after_a_long_pause(self):
        times = [10 * i for i in range(1, 31)] + [900 + 10 * i for i in range(1, 4)]
        s = simulate(times, total=100)
        self.assertAlmostEqual(tp.time_left(s, s["moved"]), (100 - 33) * 10, delta=1)

    def test_nothing_known_without_a_step_or_with_an_old_file(self):
        self.assertIsNone(tp.time_left({"done": 0, "total": 10, "started": 0.0}, 5))
        old = {"done": 3, "total": 10, "started": 0.0, "moved": 30.0}  # a file from before samples were kept
        self.assertAlmostEqual(tp.time_left(old, 30), 70, delta=1)
        self.assertIsNone(tp.time_left({"done": 10, "total": 10, "started": 0.0, "moved": 100.0}, 100))

    def test_samples_stay_spaced_and_bounded(self):
        s = {"done": 0, "total": 1000, "started": 0.0, "start_done": 0, "recent": []}
        for t in range(1, 500):
            s["done"] += 1
            tp.record(s, float(t))
        r = s["recent"]
        self.assertLessEqual(len(r), tp.KEEP)
        self.assertEqual(r[-1], [499.0, 499])
        gaps = [b[0] - a[0] for a, b in zip(r, r[1:-1])]
        self.assertTrue(all(g >= tp.SPACING for g in gaps), gaps)


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.saved, tp.PROGRESS = tp.PROGRESS, self.tmp.name

    def tearDown(self):
        tp.PROGRESS = self.saved
        self.tmp.cleanup()

    def test_task_skip_step_finish(self):
        t = tp.Task("t1", "Test", total=10)
        t.skip(5)
        s = tp.load(tp.path_of("t1"))
        self.assertEqual((s["done"], s["start_done"]), (5, 5))
        self.assertNotIn("moved", s)
        t.step(detail="one")
        s = tp.load(tp.path_of("t1"))
        self.assertEqual((s["done"], s["start_done"], s["detail"]), (6, 5, "one"))
        self.assertIn("moved", s)
        self.assertEqual(s["recent"][-1][1], 1)  # one unit of this run's work
        st = tp.task_states()[0]
        self.assertEqual((st["name"], st["done"], st["total"], st["alive"]), ("t1", 6, 10, True))
        t.finish()
        self.assertEqual(tp.task_states(), [])

    def test_shell_commands(self):
        def run(*argv):
            sys.argv = ["taskprogress.py", *argv]
            return tp.main()

        self.assertEqual(run("start", "sh", "Shell test", "8"), 0)
        self.assertEqual(run("skip", "sh", "3"), 0)
        self.assertEqual(run("step", "sh", "--detail", "a"), 0)
        self.assertEqual(run("set", "sh", "6"), 0)
        s = tp.load(tp.path_of("sh"))
        self.assertEqual((s["done"], s["start_done"], s["detail"], s["pid"]), (6, 3, "a", os.getppid()))
        self.assertEqual(s["recent"][-1][1], 3)
        self.assertEqual(run("finish", "sh"), 0)
        self.assertFalse(os.path.exists(tp.path_of("sh")))
        self.assertEqual(run("step", "sh"), 1)


class PolledPace(unittest.TestCase):
    def test_single_poll_knows_nothing_then_counts_down(self):
        p = tp.Pace()
        self.assertIsNone(p.left("k", 0.10, 1.0, 0.0, 100.0))
        left = None
        for i in range(1, 16):  # a build gaining 1 % every 2 s
            left = p.left("k", 0.10 + 0.01 * i, 1.0, 0.0, 100.0 + 2 * i)
        self.assertAlmostEqual(left, 0.75 / 0.005, delta=1)  # 75 % to go at 0.5 % a second
        self.assertAlmostEqual(p.left("k", 0.25, 1.0, 0.0, 131.0), 149, delta=1)  # a stalled poll counts down
        self.assertIsNone(p.left("k", 0.25, 1.0, 0.0, 400.0))  # and hides once overrun

    def test_a_new_build_under_the_same_key_starts_over(self):
        p = tp.Pace()
        for i in range(10):
            p.left("k", 0.5 + 0.01 * i, 1.0, 0.0, 10.0 * i)
        self.assertIsNone(p.left("k", 0.02, 1.0, 500.0, 500.0))


class TrayPercent(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import progress_tray
        except ImportError as e:  # no python-gobject here
            raise unittest.SkipTest(str(e))
        cls.pt = progress_tray

    def test_never_zero_never_over_100(self):
        p = self.pt.percent
        self.assertEqual([p(0.0), p(0.004), p(0.011), p(0.5), p(1.0), p(1.3), p(-1)], [1, 1, 2, 50, 100, 100, 1])

    def test_update_carries_the_floor(self):
        from gi.repository import GLib  # noqa: F401
        tray = self.pt.Tray.__new__(self.pt.Tray)
        tray.views, sent = {"k": "/view/1"}, []
        tray.call = lambda path, method, sig, args: sent.append((method, args))
        tray.show("k", {"title": "T", "fraction": 0.0, "detail": "0 of 3", "detail2": ""})
        method, (fields,) = sent[-1]
        self.assertEqual(method, "update")
        self.assertEqual(fields["percent"].get_uint32(), 1)

    def test_jobs_show_tasks_and_nothing_uncounted(self):
        with tempfile.TemporaryDirectory() as d:
            saved, tp.PROGRESS = tp.PROGRESS, d
            saved_ui, self.pt.ui = self.pt.ui, None
            try:
                tp.Task("x", "X job", total=4).step()
                out = self.pt.jobs()
            finally:
                tp.PROGRESS, self.pt.ui = saved, saved_ui
        self.assertEqual(list(out), ["task:x"])
        self.assertEqual((out["task:x"]["fraction"], out["task:x"]["alive"]), (0.25, True))
        self.assertFalse(hasattr(self.pt, "claude_command"))


if __name__ == "__main__":
    unittest.main()

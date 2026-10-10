# ActivityScope

A task manager for Linux that remembers. It shows what is using your computer
right now, lets you sort, inspect and end processes, and keeps a record of
every stretch of heavy CPU, memory, disk, GPU or heat load together with the
programs that caused it. When the fan spins up and then settles before you can
look, the History page still knows what it was.

Written in C++20 with Qt 6. It never asks for root.

![Summary](docs/summary.png)

## What it does

- **Summary, Performance, Processes.** Live meters and graphs for CPU (per
  core), memory and swap, each GPU, each disk, network, battery and thermals.
  The process view groups your apps, background services and system
  processes, nests children under the program that started them, and can
  pause, resume or end a process.
- **History.** A timeline of the last 24 hours with every recorded event
  marked. Each event gets a plain-language summary, the conditions that
  tripped it, a graph that includes the minute before it started, and a ranked
  list of culprits. Clicking any moment on the timeline shows which programs
  were busiest then, even if nothing crossed a threshold.
- **Shutdown watch.** Warns before the system turns itself off for heat (see
  below), and records every past shutdown it finds in the system log.
- **Log.** One timeline of everything worth knowing in an investigation:
  USB devices and boards plugged in and out, drives connected, mounted,
  formatted or partitioned (and by which program), boards flashed (tool,
  port, board and firmware file), packages installed or removed, sudo and
  pkexec commands, sleep, wake, lid and power changes, network changes,
  failed services, crashes and out-of-memory kills, and programs run for the
  first time from outside the system folders. Searchable, filterable by kind,
  and exportable as a text file.
- **System pages.** System info, startup apps (with on and off switches),
  users and their sessions, services (start, stop and restart your own),
  network connections by program, installed apps with their package, version
  and size, disk space with a folder-size scan, and power and clock speeds.
- **Popout.** A small always-on-top readout of CPU, memory, GPU, heat and fan.
- **Always recording.** The app lives in the system tray. Closing the window
  keeps it recording; quitting from the tray menu stops it.

## How it catches programs that are already gone

Sampling once a second misses a compiler that runs for 200 ms. ActivityScope
still accounts for that work in three ways, none of which need root:

1. **Exit tracking.** A process seen last second and gone now is kept with its
   final totals and marked as exited.
2. **Reaped children.** When a process exits, the kernel adds its CPU time to
   its parent. ActivityScope compares that with what it saw by name, so a
   burst of tiny programs shows up as "programs too brief to catch by name,
   started by make".
3. **Systemd units.** Every app and service runs in its own cgroup, whose CPU
   counter keeps counting processes that have exited.

Short-lived processes with the same name and parent are grouped, for example
"512 short-lived cc1plus processes started by make".

## Events

An event opens when one of these holds for a few seconds (all adjustable in
Settings):

| Resource | Default trigger |
| --- | --- |
| CPU | over 50% for 2 s, one core at 95% for 3 s, one program over 25% of the machine, or 50 new processes in 5 s |
| Memory | programs waiting on memory over 2% of the time, swap-in over 1 MB/s, free memory falling 500 MB in 10 s, or one program growing 300 MB in 10 s |
| Disk | busy over 60% or moving over 50 MB/s for 3 s |
| GPU | load over 50% for 3 s, or video memory jumping by 500 MB |
| Heat | CPU at 85 °C, a rise of 15 °C in 30 s, fan over 85%, or the fan stepping up 20 points |

It closes after 30 calm seconds. Triggers that arrive while it is open are
merged into the same event.

## Before the machine shuts itself down for heat

Linux turns the machine off when its critical thermal zone reaches its limit
(on the laptop this was built on, the `acpitz` zone at 100 °C). ActivityScope
reads that zone and its limit from the system rather than assuming a number:

- It works out how fast the sensor is climbing and how many seconds are left
  at that rate. The sensor can jump 20 °C for a single second, so decisions use
  the middle of the last three readings, and a countdown is only given within
  20 °C of the limit.
- Within 5 °C of the limit, or about 20 seconds away, it shows a critical
  desktop notification naming the program using the most CPU, with a button to
  pause it. A paused program keeps its work and carries on when you resume it.
  The warning is withdrawn once things calm down.
- Heat events say how close the machine came ("within 3 °C of the 100 °C
  point, about 6 seconds away at its fastest climb").
- A fan that stays at 0% while the other fan works hard and the CPU is hot is
  called out as not spinning.
- Near the limit, history is written to disk every second, so a sudden
  power-off still leaves a record. When the app starts it reads how earlier
  boots ended and records each heat shutdown or unexplained power-off.

## Where the log comes from

Everything is read as your own user: device events from udev, the system
journal (followed from where the last run stopped, so nothing is lost while
the app is closed), `/var/log/pacman.log`, the mount table, and the process
list for flashing and disk tools. Repeats are folded ("zsh crashed 8 times
between 16:23:01 and 16:23:12"). Without root it cannot see which files were
copied to or from a drive, only that the drive came and went.

## Answering "what just happened?" from a terminal

Everything is in one SQLite file, `~/.local/share/activityscope/history.db`:

```sh
sqlite3 ~/.local/share/activityscope/history.db \
  "select datetime(start,'unixepoch','localtime'), summary from events order by start desc limit 5"
```

Tables: `samples` (one row per second, kept 24 hours), `minute_top` (the
busiest programs of each minute, kept 24 hours), `events`, `event_culprits`
and `event_samples` (kept 30 days), and `log` (kept 90 days):

```sh
sqlite3 ~/.local/share/activityscope/history.db \
  "select datetime(t,'unixepoch','localtime'), category, title from log order by t desc limit 20"
```

## Popout skins

Right-click the popout to pick a skin. Besides the built-in ones, any JSON
file in `~/.config/activityscope/skins/` becomes a skin. A skin is colours
only, nothing in it can run, and anything left out is taken from the app
theme:

```json
{ "name": "Midnight", "window": "#0b1020", "text": "#e8ecff", "cpu": "#7cf0a0", "heat": "#ff8a5c" }
```

Keys: `window`, `panel`, `text`, `dim`, `cpu`, `memory`, `gpu`, `heat`, `fan`.

## Cost

Measured on an i7-10750H laptop while hidden in the tray: about 0.65 s of CPU
per minute (roughly 1% of one core) and 100 to 160 MB of memory. All processes
are scanned every second while the machine is busy and every 3 s while it is
calm, which loses nothing because CPU time is averaged over the gap. Slow
sensors are read less often: the battery every 10 s (each read takes about
100 ms of firmware time), drive and Wi-Fi temperatures every 5 s, NVIDIA's
load every 3 s and its power and clock every 15 s. A sleeping laptop GPU is
never woken to be measured, and when the GPU is idle the app lets go of it so
it can sleep.

## Sharing with conky

ActivityScope writes `$XDG_RUNTIME_DIR/activityscope/conky.txt` every second
(it lives in RAM). Each line is `key value`: CPU and per-core load, clock,
temperatures, fans, NVIDIA load and memory, disk and network rates, and the top
8 processes as `topN cpu% mem% name`. A conky config can read it instead of
measuring the same things again.

## Build

Needs CMake 3.24 or newer, a C++20 compiler, Qt 6 (Widgets, Svg, Network) and
SQLite 3.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/ascore_tests
cmake --install build --prefix ~/.local
```

To start it hidden at login, tick "Start recording when I log in" in Settings,
or install `systemd/activityscope.service` as a user unit.

## Layout

- `src/core` measures and records: readers for `/proc` and `/sys`, the event
  engine and the SQLite store. It has no Qt dependency and is tested against
  fake `/proc` and `/sys` trees in `tests/`.
- `src/app` is the Qt interface and the recorder thread.

## Credits

The look is inspired by Dave Plummer's Task Manager TMOG. Headings use the
Oxanium font and icons come from Lucide; see `THIRD_PARTY_NOTICES.md`.

## License

MIT

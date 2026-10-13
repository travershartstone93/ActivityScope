#!/usr/bin/env bash
# Builds ActivityScope and sets it up for this user. Nothing needs root.
# What it does: compile, install under ~/.local, start the tray recorder at
# login, and make the bundled weird scanner available on your PATH.
set -euo pipefail
cd "$(dirname "$0")"
PREFIX="${PREFIX:-$HOME/.local}"

for tool in cmake g++ python3 rsync; do
    command -v "$tool" >/dev/null || { echo "install.sh: $tool is required (pacman -S $tool)"; exit 1; }
done

echo "Building (this takes a minute)..."
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" >/dev/null
echo "Installing to $PREFIX..."
cmake --install build --prefix "$PREFIX" >/dev/null

# Start recording at login, and now.
mkdir -p "$HOME/.config/systemd/user"
cp systemd/activityscope.service "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable activityscope.service
# A restart rather than a plain start, so an upgrade takes effect at once.
systemctl --user restart activityscope.service

# The bundled weird scanner, as a command too, unless one is already installed.
if ! command -v weird >/dev/null && [ ! -e "$PREFIX/bin/weird" ]; then
    ln -s "$PREFIX/share/activityscope/thatsweird/bin/weird" "$PREFIX/bin/weird"
    echo "The weird command now points at the bundled copy."
fi

# Make sure $PREFIX/bin is on PATH, for weird and for activityscope itself.
case ":$PATH:" in
*":$PREFIX/bin:"*) ;;
*)
    marker="# added by the ActivityScope installer"
    if [ -w "$HOME/.bashrc" ] && ! grep -qF "$marker" "$HOME/.bashrc"; then
        printf '\n%s\nexport PATH="%s"\n' "$marker" "$PREFIX/bin:\$PATH" >> "$HOME/.bashrc"
        echo "Added $PREFIX/bin to PATH in ~/.bashrc (takes effect in new shells)."
    fi
    if command -v fish >/dev/null; then
        fish -c "contains \"$PREFIX/bin\" \$fish_user_paths; or set -U fish_user_paths \"$PREFIX/bin\" \$fish_user_paths" 2>/dev/null || true
        echo "Added $PREFIX/bin to fish's user paths."
    fi
    ;;
esac

echo
echo "Done. ActivityScope is recording in the system tray."
echo "Open it from your application launcher, the tray icon, or: $PREFIX/bin/activityscope"

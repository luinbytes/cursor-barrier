# cursor-barrier

Hard cursor confinement for XWayland games on Hyprland.

XWayland windows don't participate in Wayland pointer constraints, so fullscreen games can't lock the cursor. `cursor-barrier` solves this at the kernel input layer: it grabs the physical mouse and enforces a hard wall at the monitor boundary when your game is focused.

## How it works

Three states:

| State | Condition | CPU |
|-------|-----------|-----|
| `IDLE` | Game not focused | Zero (event-driven) |
| `WATCHING` | Game focused, cursor far from edge | ~30ms poll |
| `GUARDING` | Game focused, cursor near edge | Tight evdev loop |

The mouse is only grabbed in `GUARDING` state — a configurable zone near the boundary (default 300px). Outside that zone, the mouse is completely untouched, so acceleration and feel are 100% native. The grab zone is wide enough that even a fast throw can't escape before the grab activates.

Button presses delay the grab transition. If you're holding a mouse button when you approach the boundary, the grab waits until you release it. This prevents games from missing button-release events when the virtual device takes over (which causes stuck freecam, stuck drag, etc.).

A virtual uinput device is created that forwards all events from the real device — minus any leftward movement that would cross the wall.

## Requirements

- Linux
- Hyprland compositor
- `libevdev` (and `libevdev-uinput`)
- Run as a user with read access to `/dev/input/event*` (typically the `input` group)

```
# Arch / Omarchy
sudo pacman -S libevdev

# Debian / Ubuntu
sudo apt install libevdev-dev
```

Check your group membership:
```
groups | grep input
# If missing:
sudo usermod -aG input $USER
```

## Build & Install

```
make
sudo make install
```

Or install to a local prefix:
```
make install PREFIX=~/.local
```

## Usage

```
cursor-barrier [OPTIONS] PATTERN
```

`PATTERN` is a case-insensitive substring matched against the active window's class and title (as reported by Hyprland's `activewindow` event).

### Options

| Flag | Description |
|------|-------------|
| `-m NAME`, `--monitor NAME` | Confine to this monitor (e.g. `DP-1`). Boundary auto-detected from monitor position. |
| `-x X`, `--boundary X` | Hard-code the left boundary X pixel coordinate. Overrides `--monitor`. |
| `-b PX`, `--buffer PX` | Width of the grab activation zone in pixels (default: 300). |
| `-h`, `--help` | Show usage. |

If neither `--monitor` nor `--boundary` is given, the boundary is auto-detected from the matched window's current monitor at startup.

### Examples

```bash
# Auto-detect monitor from where War Thunder is running
cursor-barrier "war thunder"

# Explicitly confine to DP-1 (boundary at right edge of DP-1's X position + width)
cursor-barrier --monitor DP-1 "war thunder"

# Confine to HDMI monitor with a wider grab zone
cursor-barrier --monitor HDMI-A-1 --buffer 400 "minecraft"

# Hard-code the boundary (for a 1920px-wide left monitor in a dual-monitor setup)
cursor-barrier --boundary 1920 "war thunder"
```

### Autostart (Hyprland)

Add to `~/.config/hypr/autostart.conf`:

```
exec-once = cursor-barrier --monitor DP-1 "war thunder"
```

Or, with Hyprland's `uwsm-app` wrapper if you're using UWSM:

```
exec-once = uwsm-app -- cursor-barrier --monitor DP-1 "war thunder"
```

## Why not Wayland pointer constraints?

XWayland games run inside a nested X server. The Wayland compositor (Hyprland) doesn't expose pointer constraints to XWayland windows the same way it does to native Wayland clients. Even with the game set to fullscreen, the cursor can escape to other monitors.

This tool works at the evdev layer — below the compositor — so it doesn't care whether the window is XWayland or native Wayland.

## Limitations

- Only confines the **left** boundary (right edge of the left monitor in a side-by-side setup). Designed for two-monitor setups where the game is on the left monitor.
- Requires access to `/dev/input/event*`. If you see "no mice found", check your `input` group membership.
- The virtual uinput device may have slightly different properties than the physical device depending on your mouse. The grab zone is intentionally used to minimize time spent on the virtual device.

## License

MIT

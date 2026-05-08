# cursor-barrier

**Hard cursor confinement for XWayland games on Hyprland.**

Multi-monitor gaming on Hyprland: you're in a tense moment, you whip the mouse left — and your cursor escapes to your second monitor, losing game focus. cursor-barrier fixes this permanently by intercepting mouse input at the `/dev/input` level, below the compositor and XWayland. When your game is focused, the cursor cannot leave your monitor.

---

## Why not just use...

| Workaround | Problem |
|---|---|
| `xconf` / X11 barriers | Hyprland manages the cursor at the Wayland level, not X11 |
| `cursor:no_warps` | Not designed for this use case |
| Gamescope | Adds latency, breaks taskbar, compatibility issues |
| Wayland pointer constraints | XWayland games can't use them |

---

## How it works

The daemon watches Hyprland's event socket and operates in three states:

| State | When | CPU |
|---|---|---|
| **IDLE** | Game not focused | Zero — event-driven |
| **WATCHING** | Game focused, cursor away from edge | Minimal — polls every 30ms |
| **GUARDING** | Game focused, cursor near boundary | Active — reads raw input |

The mouse is only grabbed in **GUARDING** state — a configurable zone near the monitor edge (default: 300px). Outside that zone, your physical mouse is completely untouched; your DPI, acceleration, and feel are 100% native.

If you're holding a mouse button when approaching the boundary (e.g. right-click for freecam), the grab is deferred until you release it — preventing stuck buttons from mismatched press/release events across devices.

---

## Requirements

- Linux, Hyprland
- `libevdev`
- User in the `input` group

```bash
# Arch
sudo pacman -S libevdev

# Debian / Ubuntu
sudo apt install libevdev-dev

# Check group membership
groups | grep input

# Add yourself if missing (then log out and back in)
sudo usermod -aG input $USER
```

---

## Installation

```bash
git clone https://github.com/luinbytes/cursor-barrier
cd cursor-barrier
make
sudo make install        # system-wide
make install PREFIX=~/.local  # or just for your user
```

Installs the `cursor-barrier` daemon, `cbgames` CLI, `cursor-barrier-gui` GTK app, and a `.desktop` file for your app launcher.

---

## Usage

### Daemon

```
cursor-barrier [OPTIONS] PATTERN [PATTERN...]
```

Patterns are matched case-insensitively against the focused window's class. Confinement activates on match, deactivates instantly on alt-tab.

**Options**

| Flag | Description |
|---|---|
| `-m`, `--monitor NAME` | Confine to this monitor (e.g. `DP-1`) |
| `-x`, `--boundary X` | Manually set left boundary X coordinate |
| `-b`, `--buffer PX` | Grab zone width in pixels (default: `300`) |

If no monitor/boundary is given, it's auto-detected from whichever monitor the matched window is on.

**Examples**

```bash
cursor-barrier "war thunder"                              # auto-detect monitor
cursor-barrier "war thunder" "call of duty"              # multiple games
cursor-barrier --monitor DP-1 "war thunder"              # explicit monitor
cursor-barrier --monitor HDMI-A-1 --buffer 500 "minecraft"
cursor-barrier --boundary 1920 "war thunder"             # hard-coded boundary
```

**Find your monitor name**

```bash
hyprctl monitors | grep "Monitor "
```

**Autostart**

```bash
# ~/.config/hypr/autostart.conf
exec-once = cursor-barrier "war thunder" "call of duty"

# With UWSM
exec-once = uwsm-app -- cursor-barrier "war thunder" "call of duty"
```

---

### cbgames

CLI for managing the game list — edits `autostart.conf` and restarts the daemon automatically.

```bash
cbgames list               # show configured games
cbgames add "elden ring"   # add and restart
cbgames remove "halfsword" # remove and restart
cbgames restart            # restart without changes
```

---

### cursor-barrier-gui

GTK4 + libadwaita GUI. Launch from your app menu or run `cursor-barrier-gui`.

- Game list with per-entry remove buttons
- Add dialog with a **window picker** — click the eyedropper, click any window, class name is filled in automatically
- Every change restarts the daemon immediately

**Requirements:** Python 3, `python-gobject`

**Float the window (Hyprland):**
```
windowrule = float = true, match:class lu.cursorbarrier
```

---

> **Raw input note:** Games that properly implement raw input / pointer lock don't need cursor-barrier. If confinement is causing issues (e.g. blocking in-game mouse movement at the screen edge), remove that game from the list.

---

## Troubleshooting

**"no mice found"**
Not in the `input` group, or mouse doesn't expose `REL_X`. Check with `sudo evtest`.

**Cursor still escapes on fast throws**
Try `--buffer 500` or higher. High-DPI mice can travel further between poll intervals.

**Mouse feels different at the edge**
While GUARDING, a virtual uinput device mirrors your mouse. Ensure `force_no_accel = true` in your Hyprland input config so libinput doesn't apply different acceleration to it.

**Buttons stuck in-game**
Fixed in a recent version — update and try again.

---

## Limitations

- Only confines the **left** boundary (designed for a second monitor to the left of your game monitor). Full multi-edge support is a potential future addition.
- Requires `/dev/input` access — won't work in sandboxed environments.

---

## License

MIT

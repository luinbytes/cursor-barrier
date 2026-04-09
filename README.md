# cursor-barrier

**Hard cursor confinement for XWayland games on Hyprland.**

If you game on a multi-monitor Linux setup with Hyprland, you've probably felt this: you're in a tense moment, you whip your mouse left, and suddenly your cursor has escaped to your second monitor and your game has lost focus. cursor-barrier fixes this permanently.

It works at the kernel input layer — below the compositor, below XWayland — so it doesn't matter how the game handles pointer locking. When your game is focused, the cursor cannot leave your monitor.

---

## Why this exists

Wayland has a proper pointer-constraint protocol for locking the cursor. Native Wayland games can use it. **XWayland games can't** — they run inside a nested X server, and the compositor doesn't expose pointer constraints to them the same way. This means games like War Thunder, older Source titles, and most Windows games running through Proton have no reliable way to lock the cursor.

Common workarounds have problems:
- **`xconf` / X11 barriers** — don't work because Hyprland manages the cursor at the Wayland level, not X11
- **Hyprland's `cursor:no_warps`** — not designed for this use case
- **Gamescope** — works for some games but adds latency, hides the window from your taskbar, and has compatibility issues

cursor-barrier takes a different approach: it intercepts the mouse at the `/dev/input` level, grabs the physical device, and filters out any movement that would cross the boundary. The cursor physically cannot escape.

---

## How it works

cursor-barrier runs as a background daemon and watches Hyprland's event socket. It has three states:

| State | When | CPU usage |
|---|---|---|
| **IDLE** | Your game isn't focused | Zero — fully event-driven |
| **WATCHING** | Game is focused, cursor is away from the edge | Minimal — polls position every 30ms |
| **GUARDING** | Game is focused, cursor is near the boundary | Active — reads raw input events |

**The mouse is only grabbed in GUARDING state** — a configurable zone near the monitor edge (default: 300px). Outside that zone, your physical mouse is completely untouched. Your acceleration curve, DPI, and feel are 100% native during normal gameplay. The grab zone is wide enough that even a fast throw activates the lock before the cursor can escape.

**Button-aware transitions** — if you're holding a mouse button when you approach the boundary (e.g. holding right-click for freecam), the grab is deferred until you release it. This prevents the game from seeing a button press on one device and a button release on a different device, which would leave buttons "stuck" down.

---

## Requirements

- Linux
- Hyprland compositor
- `libevdev`
- User must be in the `input` group (to read `/dev/input/event*`)

**Install libevdev:**
```bash
# Arch / Omarchy
sudo pacman -S libevdev

# Debian / Ubuntu
sudo apt install libevdev-dev
```

**Check your input group membership:**
```bash
groups | grep input
```
If it's missing:
```bash
sudo usermod -aG input $USER
# Log out and back in for the group to take effect
```

---

## Installation

```bash
git clone https://github.com/luinbytes/cursor-barrier
cd cursor-barrier
make
sudo make install
```

Or install to your home directory (no sudo required):
```bash
make install PREFIX=~/.local
```

This installs the `cursor-barrier` daemon, the `cbgames` CLI, and the `cursor-barrier-gui` GTK app. The `.desktop` file is also installed so the GUI appears in your app launcher.

---

## Usage

### Daemon

```
cursor-barrier [OPTIONS] PATTERN [PATTERN...]
```

`PATTERN` is a case-insensitive substring matched against the focused window's class and title, as reported by Hyprland. When a window matching any pattern is focused, confinement activates. When you alt-tab or focus something else, it deactivates instantly.

#### Options

| Flag | Description |
|---|---|
| `-m NAME`, `--monitor NAME` | Confine cursor to this monitor (e.g. `DP-1`). The boundary is auto-detected from the monitor's position. |
| `-x X`, `--boundary X` | Manually set the left boundary X coordinate in pixels. Overrides `--monitor`. |
| `-b PX`, `--buffer PX` | Width of the grab activation zone in pixels. Default: `300`. |
| `-h`, `--help` | Show usage. |

If neither `--monitor` nor `--boundary` is given, the boundary is auto-detected from whichever monitor the matched window is currently on.

#### Examples

```bash
# Auto-detect monitor from wherever War Thunder is running
cursor-barrier "war thunder"

# Multiple games in one daemon instance
cursor-barrier "war thunder" "call of duty"

# Explicitly confine to DP-1
cursor-barrier --monitor DP-1 "war thunder"

# Confine Minecraft to HDMI-A-1 with a larger buffer zone
cursor-barrier --monitor HDMI-A-1 --buffer 500 "minecraft"

# Hard-code the boundary at X=1920 (right edge of a 1920px-wide left monitor)
cursor-barrier --boundary 1920 "war thunder"
```

#### Finding your monitor name

```bash
hyprctl monitors | grep "Monitor "
```

#### Autostart with Hyprland

Add to `~/.config/hypr/autostart.conf`:

```
exec-once = cursor-barrier "war thunder" "call of duty"
```

If you're using UWSM session management:
```
exec-once = uwsm-app -- cursor-barrier "war thunder" "call of duty"
```

---

### cbgames — CLI game list manager

`cbgames` manages the game list in your Hyprland autostart config and restarts the daemon automatically.

```bash
cbgames list               # show all games currently in the list
cbgames add "elden ring"   # add a game and restart the daemon
cbgames remove "halfsword" # remove a game and restart the daemon
cbgames restart            # restart the daemon without changing the list
```

`add` and `remove` are case-insensitive and update `~/.config/hypr/autostart.conf` in place.

---

### cursor-barrier-gui — GTK4 GUI

A graphical interface for managing the game list, built with GTK4 and libadwaita.

Launch it from your app menu ("Cursor Barrier") or run `cursor-barrier-gui`.

**Features:**
- Lists all configured games with a remove button per entry
- Add dialog with a **window picker** — click the eyedropper, then click any window to detect its class name automatically (uses the Hyprland event socket)
- Changes take effect immediately — the daemon is restarted on every add/remove

**Requirements:** Python 3, GTK4, libadwaita (`python-gobject` on Arch).

**Floating window rule (Hyprland):** Add to your `hyprland.conf` to keep the GUI floating:
```
windowrule = float = true, match:class lu.cursorbarrier
```

---

> **Note on games that use raw input:** Some games handle mouse input correctly regardless of cursor position (raw input / pointer lock), meaning cursor-barrier isn't needed for them. If you notice that confinement is *causing* issues (e.g. blocking in-game mouse movement at the screen edge), remove that game from the list — it doesn't need it.

---

## Troubleshooting

**"no mice found"**
You're not in the `input` group, or your mouse doesn't expose `REL_X` events. Check with `sudo evtest`.

**Cursor still escapes on very fast throws**
Increase the buffer zone: `--buffer 500` or higher. The default 300px is sufficient for most mice, but very high-DPI mice can cover more distance between poll intervals.

**Mouse feels different during intense edge activity**
While GUARDING, a virtual uinput device mirrors your physical mouse. If your system applies different acceleration to it (e.g. via libinput profiles), you may feel a slight difference right at the edge. This only happens within the buffer zone. Make sure `force_no_accel = true` is set in your Hyprland input config.

**Buttons getting stuck in-game**
Make sure you're running the latest version — this was a known issue that has been fixed.

---

## Limitations

- Currently only confines the **left** boundary of the cursor's range (the left edge of the rightmost active zone). Designed for two-monitor setups where the game monitor sits to the right of a second display, or where you want to prevent the cursor from crossing into a monitor on the left. Full multi-edge support is a potential future addition.
- Requires `/dev/input` access. Sandboxed or containerized environments may not work.

---

## License

MIT

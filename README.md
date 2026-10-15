# sway-master-stack

![C23](https://img.shields.io/badge/standard-ISO%20C23-blue)
![Platform](https://img.shields.io/badge/platform-Sway%20%7C%20SwayFX-orange)
![RAM](https://img.shields.io/badge/RAM-~2MB-brightgreen)
![License](https://img.shields.io/badge/license-MIT-green)

A lightweight DWM/DWL-style master/stack tiling daemon for [Sway](https://github.com/swaywm/sway) and SwayFX.

Written in pure **ISO C23** using the native Sway UNIX IPC socket (`$SWAYSOCK`) and `json-c`.

---

## How It Works

The daemon maintains a two-column layout on each workspace:
- **Master area (left)**: Exactly 1 window occupying **55%** of the workspace width.
- **Stack area (right)**: All other windows stacked vertically occupying the remaining **45%**.

```text
+------------------------+---------------------+
|                        |       Stack 1       |
|                        +---------------------+
|        Master          |       Stack 2       |
|         55%            +---------------------+
|                        |       Stack 3       |
|                        |         45%         |
+------------------------+---------------------+
```

### Behavior:
1. **Window creation**:
   - The first window takes 100% width.
   - The second window opens to the right; the master window resizes to 55%.
   - Subsequent windows automatically join the vertical stack on the right.
2. **Master promotion & swap**:
   - Moving a stack window left (`move left`) promotes it to Master.
   - Moving the Master window right (`move right`) sends it into the stack, promoting the top stack window.
3. **Floating & close recovery**:
   - Toggling Master to floating (`$mod+Space`) or closing it automatically promotes the top stack window to Master without collapsing the layout or creating nested container leaks.
   - Floating windows and scratchpad containers are centered and ignored by the tiling logic.
4. **Multi-monitor**:
   - Workspaces and containers are tracked by `con_id` within their local tree, completely isolated across outputs.

---

## Requirements

- `clang` (with C23 support)
- `cmake` (>= 3.25)
- `lld`
- `json-c`
- `pkgconf`

Install dependencies on Arch Linux:
```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

---

## Building & Installation

```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack

# Build release binary
CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Install binary to /usr/local/bin and systemd unit
sudo cmake --install build
```

---

## Configuration

Add this to your `~/.config/sway/config`:

```sway
exec sway-master-stack
```

Or run it as a systemd user service:

```sh
systemctl --user daemon-reload
systemctl --user enable --now sway-master-stack.service
```

---

## Technical Details

- **Footprint**: ~2 MB RSS, 0.0% CPU when idle.
- **Safety**: Clean under AddressSanitizer and UndefinedBehaviorSanitizer ==> zero memory leaks.
- **Atomic execution**: Generates single-transaction batch commands joined by `;` to avoid visual tearing and race conditions.

---

## Credits

Written and developed with the assistance of **GLM 5.3**.

---

## License

[MIT](LICENSE) © 2026 [pama1232](https://github.com/pama1232)

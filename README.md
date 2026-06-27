# whot

A small screenshot tool for **Wayland** compositors — a full
port of [shot](https://github.com/HimadriChakra12/shot) from X11/Xlib to native
Wayland protocols.

All features are identical to the original.  Only the display backend changed.

## Requirements

**Runtime:**
- A wlroots-based compositor
- `wl-copy` from [wl-clipboard](https://github.com/bugaevc/wl-clipboard) (for copy action)

**Build:**
- `wayland-client`, `wayland-cursor`, `wayland-scanner`
- `cairo`
- `xkbcommon`
- `libwebp`

---

## Build

```sh
make
sudo make install
```

To regenerate protocol glue from XML (only needed if you update the XMLs):
```sh
make protocols
make
```

---

## Usage

Identical to the original `shot`:

```sh
whot
whot any-arg
```

### Key bindings (phase 1 — before selecting)

| Key | Action |
|---|---|
| `f` | fullscreen capture |
| click & drag | region drag |
| `Escape` / right-click | cancel |

### Key bindings (phase 3 — after selecting)

| Key | Action |
|---|---|
| `w` | save to `OPTDIR` |
| `y` | copy to clipboard (`wl-copy`) |
| `a` | open in annotation tool (`OPTANNOTATE`) |
| `1`–`9` | run script from `OPTSCRIPTDIR` |
| `Escape` / right-click | cancel |

---

## Compositor compatibility

Requires two wlroots unstable protocols:

- `zwlr_layer_shell_v1` — for the fullscreen input-capturing overlay
- `zwlr_screencopy_manager_v1` — for screen capture

---

## License
Same as the original — see upstream.

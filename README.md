# Myna OSK

Myna is a small Phosh/Droidian on-screen keyboard focused on Chinese Pinyin
input through Rime.

The project is intentionally split in two layers:

- Rust owns keyboard state, layouts, candidates, and mode switching.
- C owns platform edges: GTK, Wayland protocols, D-Bus, and a thin librime shim.

## Features in this initial implementation

- `sm.puri.OSK0.desktop` entry for Phosh OSK discovery.
- `mobi.phosh.OSK.service` user unit for Phosh 0.50 and newer.
- `sm.puri.OSK0` D-Bus interface with `SetVisible`, `GetVisible`, and
  `Visible`.
- gnome-session client registration with app-id `sm.puri.OSK0`.
- Bottom anchored layer-shell panel with adaptive height.
- Latin and Pinyin modes on a responsive QWERTY layout.
- Pinyin mode backed by librime candidates and commit text.
- Text submission through `zwp_input_method_v2.commit_string`.
- UTF-8 aware deletion when surrounding text is available.

## Layout and screen adaptation

The first layout keeps the same key geometry for English and Pinyin. The mode
button switches between direct Latin commits and Rime-backed Pinyin composition.
The panel is anchored to the bottom via `wlr-layer-shell`, claims an exclusive
zone, and sizes itself from the primary monitor height so small Droidian screens
keep a usable candidate strip and four key rows.

## Build on Droidian

Install build dependencies equivalent to:

```sh
sudo apt install meson ninja-build rustc cargo pkg-config \
  libgtk-3-dev libglib2.0-dev libwayland-dev libxkbcommon-dev librime-dev
```

Then build on the Droidian device:

```sh
meson setup build
ninja -C build
```

This repository was prepared without local device compilation or runtime tests.

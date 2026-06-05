# Myna OSK

Myna is a small Phosh/Droidian on-screen keyboard focused on Chinese Pinyin
input through Rime.

The project is intentionally split in two layers:

- Rust owns keyboard state, layouts, candidates, and mode switching.
- C owns platform edges: GTK, Wayland protocols, D-Bus, and a thin librime shim.

## Features in this initial implementation

- `mobi.phosh.MynaOsk.desktop` entry for Phosh OSK discovery through the
  `Phosh-OSK` alternatives link.
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
zone, and sizes itself from the logical monitor width so small Droidian screens
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

Install and reload the Phosh OSK service metadata:

```sh
sudo ninja -C build install
sudo update-alternatives --install \
  /usr/share/applications/sm.puri.OSK0.desktop \
  Phosh-OSK \
  /usr/share/applications/mobi.phosh.MynaOsk.desktop \
  60
systemctl --user daemon-reload
systemctl --user restart mobi.phosh.OSK.service
```

On systems without `systemd.pc`, Meson installs the user unit to
`/usr/lib/systemd/user` by default. Override it when needed:

```sh
meson setup build --reconfigure -Dsystemd_user_unit_dir=/usr/lib/systemd/user
```

Run a full Rime maintenance/deployment pass after changing schemas or
dictionaries:

```sh
myna-osk --rime-deploy
```

Rime may report missing schemas if the user's schema list references data files
that are not installed on the device.

This repository was prepared without local device compilation or runtime tests.

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
- A settings app for the default input mode and keyboard height, with a field
  for trying the keyboard.

## Settings

After installation, open **Myna Settings** from the application list, or run
`myna-settings`. The default input mode takes effect when the OSK starts.
Keyboard height changes take effect the next time the keyboard opens. The
settings app and OSK share per-user settings and are installed together.

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

To build a Debian package on Debian, Ubuntu, or Droidian, install the same
development dependencies plus `debhelper-compat`, then run:

```sh
dpkg-buildpackage -us -uc
```

The resulting `myna-osk_*.deb` is written to the parent directory. Install it
with `sudo apt install ./myna-osk_*.deb`.

Every push to `main` and every pull request runs the Rust tests, a Meson build,
and Debian package builds for `amd64` and `arm64` through GitHub Actions.
Successful workflow runs publish the generated packages as downloadable
artifacts. Pushing a tag such as `v0.1.0` automatically creates a GitHub
Release with both packages attached and generated release notes.

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

## License

Myna’s own code is licensed under the [GNU General Public License version 3
or later](LICENSE). Third-party protocol files in `protocols/` retain their
original copyright and license notices.

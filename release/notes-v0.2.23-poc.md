**Proof of concept**: an experimental build for enthusiasts. It installs kernel modules on your player; use it at your own risk.

New in 0.2.23:

- **libVLC with VLC patches 0060–0067** (`3.0.24-101-gd9645d814c`):
  - **HDR10+ on every picture** (0060): files whose encoder sends the HDR10+ metadata only now and then (x265 `--dhdr10-opt`) played as plain HDR10 on some TVs; the last metadata is now repeated on every picture.
  - **DTS-HD MA / DTS:X in MP4** goes through the DTS packetizer, so it can be passed through as DTS-HD (0061).
  - **Blu-ray menus:** a menu highlight whose picture changes is redrawn (0062); libbluray 1.5.0 with SamuriHL's libbluray fixes for menus, BD-J and seeks (0063).
  - **Fast seeks in TS / m2ts files** land on their time with *Always fast seek* off too (0065).
  - 0064 (`--mediacodec-flush-restart`), 0066 (`--ts-seek-index`) and 0067 (subtitles in both views of a frame packed output) are in the build but not used by this app (0067 switches itself on only for a player that asks the module for the GUI in both views).
- **mvc3d `2026.11.24.2`** (S928X) and **libmvc3dplay `2026.11.24.2`:** the module can put the player's GUI into both eyes' views of a frame packed 3D picture when the player asks for it (`gui2` on `/dev/mvc3dhw`). VLC FEL itself does not ask for it yet, so its controls and subtitles still show in the left eye only. The first 3D film installs the new module (root question), unless the new Dune HD plugin `mvc3d 2026.11.24.2-0.2.23` already loads it at boot.

libdvfelplay (`2026.11.17.1`), the dvfel kernel modules (S928X `2026.10.29.2`, S905X4 `2026.10.31.1`), the S905X4 mvc3d (`2026.11.06.8-sc2`) and the app patches are unchanged.

**Tested** on a Dune HD Pro 8K Plus (S928X, firmware `261101_0347_r25`) with this APK: a profile 7 FEL film and a 3D MVC film played; the 3D module was loaded at boot by the new mvc3d plugin, with root access off. The libVLC changes were tested beforehand with the same libVLC in a test player. Not tested: this build on the S905X4.

### Install / update
Install `VLC-FEL-0.2.23-poc-armeabi-v7a.apk` over an older version, or as a new install. `README.txt` (Hungarian / English) has the details, notes and uninstall steps.

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause):
- kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d` (`kernel/mvc3d`, `kernel/mvc3d/sc2`);
- libdvfelplay and libmvc3dplay;
- VLC patches 0001–0058 and 0060–0067 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`); 0059 was left out;
- the libvlcjni patches `vlc/jni-patches/0001` and `0002`;
- the app patches `vlc/android-patches/0001`–`0004` against vlc-android `fc5fa1f`.

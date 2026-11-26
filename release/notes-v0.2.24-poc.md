**Proof of concept**: an experimental build for enthusiasts. It installs kernel modules on your player; use it at your own risk.

New in 0.2.24, for 3D MVC on the **S905X4** (Dune HD Pro Vision 4K and the like):

- **No HDMI re-handshake at seeks and chapter changes.** Frame packing goes out as 8 bit 4:4:4, and the Dune's Auto HDR took that for a wrong colour format: at every seek it set the mode again (seconds without a picture on a projector). The module now reads back the format the firmware expects while frame packing is on.
- **3D when the player was started before the TV or projector.** The module loaded at boot before the HDMI output had a mode, and left frame packing off until the next reboot; it now sets it up when a player first asks for it.
- **The GUI in both eyes**, as on the S928X since 0.2.23: the module puts the player's GUI into both views when the player asks for it (`gui2` on `/dev/mvc3dhw`). VLC FEL itself does not ask for it yet, so its controls and subtitles still show in the left eye only.

These come with the S905X4 module **mvc3d `2026.11.26.4-sc2`**, bundled in libmvc3dplay. The first 3D film installs it (root question), unless the new Dune HD plugin `mvc3d 2026.11.26.1-0.2.24` already loads it at boot.

libVLC (`3.0.24-101-gd9645d814c`), libdvfelplay (`2026.11.17.1`), the dvfel kernel modules, the S928X mvc3d (`2026.11.24.2`) and the app patches are unchanged: apart from libmvc3dplay, this APK is 0.2.23.

**Tested:** the S905X4 module `2026.11.26.4-sc2` with this libmvc3dplay in a test player, on a Dune HD Pro Vision 4K (S905X4, firmware `R3.20261021`) with an Optoma projector: 3D MVC in frame packing, chapter changes without an HDMI mode set, menus and subtitles in both eyes, and the module installed over the older one. **Not tested:** this APK on a box (apart from libmvc3dplay it is the 0.2.23 APK), and the new plugin.

### Install / update
Install `VLC-FEL-0.2.24-poc-armeabi-v7a.apk` over an older version, or as a new install. `README.txt` (Hungarian / English) has the details, notes and uninstall steps.

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause):
- kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d` (`kernel/mvc3d`, `kernel/mvc3d/sc2`);
- libdvfelplay and libmvc3dplay;
- VLC patches 0001–0058 and 0060–0067 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`); 0059 was left out;
- the libvlcjni patches `vlc/jni-patches/0001` and `0002`;
- the app patches `vlc/android-patches/0001`–`0004` against vlc-android `fc5fa1f`.

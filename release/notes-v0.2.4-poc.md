**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.4:

- **No subtitle stutter with a 2160p output either.** 0.2.3 fixed the dropped frames at subtitle changes with a 1080p output; with a 2160p output (a 2160p desktop, or the player's resolution matching) a frame was still dropped at many subtitle changes. Rendering a subtitle line takes the player's CPU 20–60 ms, and the picture after it is handed to the display that much later; with a 2160p output the display needs it earlier than with 1080p. Up to 30 fps Dolby Vision pictures now go to the display 140 ms ahead of their time (was 80 ms); at 50 / 60 fps it stays 80 ms, and 3D MVC is unchanged.
- For players built on this libVLC: the live level 1 (min / average / max) and level 5 (active area) of the RPUs, written to a file in the app's files directory while the app asks for it (an information panel; VLC FEL itself does not use it).

VLC patches 0014 and 0015 are new; kernel modules unchanged (`dvfel 2026.10.07.1`, `mvc3d 2026.10.12.3`), libdvfelplay `2026.10.13.1` unchanged. Everything from 0.2.3 as before.

**Tested:** Dune HD Pro One 8K Plus (R24), Dolby Vision TV, 2160p24 output, 4K 24p FEL with subtitles: a film with embedded subtitles (38 subtitle changes, 2 dropped frames, none at a subtitle change; with 80 ms ahead 14 at 34 changes) and an external .srt changing every 4 s (60 s: 0 dropped frames); without subtitles 0. Measure on an idle player: right after installing VLC FEL its media library scans and makes thumbnails for a while.

### Install / update
Install `VLC-FEL-0.2.4-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0015 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the app patch `vlc/android-patches/0001` against vlc-android `fc5fa1f`.

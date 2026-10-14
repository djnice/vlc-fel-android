**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.3:

- **No more stutter with subtitles on.** With a subtitle shown, Profile 7 FEL dropped a frame at most subtitle changes (measured ~40 dropped frames per minute on a 4K film); now under 1 per minute.
- The subtitles are drawn at the display's size instead of the video's: on a 1080p desktop 1920x1080 instead of 3840x2160 (a quarter of the pixels to render, blend and compose). They look and sit the same.
- A picture is now handed to the display at its time before the subtitle is drawn. Rendering a subtitle line takes 20–60 ms on the player's CPU; a picture prepared after it was shown at once, and a Dolby Vision picture, which is released ahead of its time for the FEL composition, then reached the display early and took the vsync of the frame before it.

VLC patches 0012 and 0013 are new; kernel modules unchanged (`dvfel 2026.10.07.1`, `mvc3d 2026.10.12.3`), libdvfelplay `2026.10.13.1` unchanged. Everything from 0.2.2 (FEL demo mode, dual track / dual PID FEL, 3D MVC) as before.

**Tested:** Dune HD Pro One 8K Plus (R24), Dolby Vision TV, 4K 24p FEL: a film with embedded subtitles (8 minutes, 128 subtitle changes: 6 dropped frames; before ~40 per minute) and an external .srt changing every 1.5 s (45 s: 1 dropped frame); without subtitles 0.

### Install / update
Install `VLC-FEL-0.2.3-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0013 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the app patch `vlc/android-patches/0001` against vlc-android `fc5fa1f`.

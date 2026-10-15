**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.5:

- **No stutter for the rest of the film after a seek.** Once in a while the decoder of the FEL's enhancement layer got stuck at a seek: it kept the pictures given to it and gave none out. From then on every frame waited for its enhancement layer in vain, went to the display late and as base layer only (a steady stutter), and the next seek did not help either – it was never handled. Now a stuck enhancement layer decoder is emptied after 1.5 s and starts again at its next key picture (the frames until then are shown without delay from the base layer), and a seek is handled at once in every case. The enhancement layer's parameter sets are also kept and given to the decoder again after a seek when the stream does not repeat them.
- libVLC: a track marked disabled (Matroska `FlagEnabled 0`, MP4 `tkhd` without `track_enabled`, written so by some muxing tools) is selected when no track of its kind is enabled, as other players do. VLC FEL's player picked such a video track itself already; other players built on this libVLC played these files without a picture or without sound.
- For players built on this libVLC: the live level 1 / level 5 file (0.2.4) is now in time with the picture on the screen.

libdvfelplay `2026.10.15.1` and VLC patches 0016, 0017 are new; kernel modules unchanged (`dvfel 2026.10.07.1`, `mvc3d 2026.10.12.3`). Everything from 0.2.4 as before.

**Tested:** Dune HD Pro One 8K Plus (R24): the 4K 24p P7 FEL MKV that stayed stuck after a seek, with the same libdvfelplay in another player on this libVLC: 7 seeks forward / back / a long held scrub, each followed by 10 s with every frame composed with the enhancement layer and none late (the stuck state itself could not be brought about on demand). VLC FEL: libdvfelplay 2026.10.15.1 loads, FEL composition as before; an MKV whose only video track has FlagEnabled 0 plays with a picture.

### Install / update
Install `VLC-FEL-0.2.5-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0017 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the app patch `vlc/android-patches/0001` against vlc-android `fc5fa1f`.

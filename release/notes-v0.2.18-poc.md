**Proof of concept**: an experimental build for enthusiasts. It installs kernel modules on your player; use it at your own risk.

New in 0.2.18:

- **External subtitles on TS and Blu-ray** ([#3](https://github.com/djnice/vlc-fel-android/issues/3)). An `.srt` (or an external audio file) added to a TS file or a Blu-ray folder, at the start or during playback, got a program 0 of its own. That program took the selection from the disc's program: the picture went black and the position ran ahead. Now it joins the disc's program (VLC patch 0021).
- **The subtitles in time there.** The subtitle file counts from 0, while TS and Blu-ray streams carry their own timestamps. So the subtitles never showed where those do not start near 0 (a Blu-ray clip from 4200 s), and came early on other TS files. Now they are put on the program's timestamps (VLC patch 0022): the first picture after the start is 0, as in an MKV. Seeks and the next clip of a Blu-ray playlist keep it.
- MKV, MP4 and the other single-program files are not changed.

The VLC patches 0021 and 0022 are new. libdvfelplay (`2026.10.31.1`), the kernel modules (S928X `dvfel 2026.10.29.2`, S905X4 `dvfel 2026.10.31.1`), the app patches, libmvc3dplay and `mvc3d` are unchanged. The Dune HD plugin (`2026.10.31.1-0.2.17`) stays current.

**Tested:** on a Dune HD Pro 8K Plus (S928X, r25), against 0.2.17 with the same files:
- a TS with an `.srt` of the same name (timestamps from 4200 s and from 1.4 s);
- a TS and a Blu-ray folder with an `.srt` added during playback;
- seeks forward on both;
- an MKV with the same `.srt` for comparison.

With 0.2.17 the disc's program was dropped in every case. With 0.2.18 playback went on and the subtitles came at the same point as in the MKV, also after the seeks. Not tested: a Blu-ray playlist of more than one clip (the clip change), and `--program`.

### Install / update
Install `VLC-FEL-0.2.18-poc-armeabi-v7a.apk` over an older version, or as a new install. `README.txt` (Hungarian / English) has the details, notes and uninstall steps.

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause):
- kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`;
- libdvfelplay and libmvc3dplay;
- VLC patches 0001–0022 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`);
- the libvlcjni patch `vlc/jni-patches/0001`;
- the app patches `vlc/android-patches/0001` and `0002` against vlc-android `fc5fa1f`.

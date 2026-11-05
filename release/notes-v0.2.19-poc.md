**Proof of concept**: an experimental build for enthusiasts. It installs kernel modules on your player; use it at your own risk.

New in 0.2.19:

- **VC-1 Blu-ray remuxes without noise.** A VC-1 film in Matroska (a Blu-ray remux) keeps its start codes in the frames. VLC added another one in front of each frame, and Amlogic's decoder showed noise and green blocks. A frame that already starts with a start code is now passed as it is (VLC patch 0031).
- **The frame rate from the start.** Every video decoder now gets the stream's frame rate (VLC patches 0030 and 0032). Without it Amlogic's decoder guessed one when the stream carries no timing, and the automatic frame rate switched the display twice at the start: a 23.976 fps film to 24 Hz first, and an iPhone 60 fps Dolby Vision video to 24 Hz on S905X4.
- **The Dolby Vision profile for Codec2 decoders.** The Dolby Vision decoder also gets the stream's profile (VLC patch 0029). Amlogic's Codec2 decoder (Android 12+) otherwise kept profile 5 for a profile 8.1 stream; the OMX decoders of Android 11 are not changed.
- **TrueHD to the audio HAL as it is**, as an option: `--audiotrack-truehd-raw` (off by default) passes TrueHD to the audio HAL, which wraps it in MAT for the HDMI sink itself (VLC patch 0023). The default pass-through is unchanged.
- **Five fixes from VLC 3.0.x** (VLC patches 0024–0028): DynamicsProcessing failures are no longer fatal, Matroska files with infinite elements in a finite parent open again, a converter payload size, an Ogg overflow at 128 packets, and libsmb2 6.3.1.

The VLC patches 0023–0032 are new. libdvfelplay (`2026.10.31.1`), the kernel modules (S928X `dvfel 2026.10.29.2`, S905X4 `dvfel 2026.10.31.1`), the app patches, libmvc3dplay and `mvc3d` are unchanged. The Dune HD plugin (`2026.10.31.1-0.2.17`) stays current.

**Tested:** the patches come from CineFuze, where they have played on Dune HD players since 3 November (S928X, S905X4, and Homatics' firmware). This VLC FEL build itself was released without a box test.

### Install / update
Install `VLC-FEL-0.2.19-poc-armeabi-v7a.apk` over an older version, or as a new install. `README.txt` (Hungarian / English) has the details, notes and uninstall steps.

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause):
- kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`;
- libdvfelplay and libmvc3dplay;
- VLC patches 0001–0032 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`);
- the libvlcjni patch `vlc/jni-patches/0001`;
- the app patches `vlc/android-patches/0001` and `0002` against vlc-android `fc5fa1f`.

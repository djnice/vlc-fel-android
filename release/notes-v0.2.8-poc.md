**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.8:

- **Audio as an HDMI bitstream (passthrough).** Settings → Audio → *Digital audio output (passthrough)* (off by default) sends the audio undecoded to the AV receiver: **Dolby TrueHD (Atmos)** as MAT and **DTS-HD MA / DTS:X**, both on the 8 channel 192 kHz HBR carrier, **E-AC-3 (Atmos)**, **AC-3** and **DTS** – for the formats both the HDMI output and the connected device (EDID) accept; the rest (e.g. AAC) stays PCM. No root needed.
  - TrueHD was never offered before: Android lists no TrueHD encoding for the HDMI output of these players, although the receiver's EDID has it. It is now detected from the Amlogic HDMI driver's EDID data (`MAT`), and its MAT framing places every TrueHD frame by its timing (the old fixed slots dropped 20 ms of audio whenever a frame did not fit).
  - DTS-HD MA did not fit on the 2 channel carrier stock VLC 3 used, so only the DTS core went through; now it goes out at full rate.
  - Pass-through audio is in sync with the picture (stock VLC 3 played it ahead by the amount it buffered).
  - No fallback to PCM after a track change: Android keeps an output it just released for ~1 s; it is now retried.

VLC patches `0018`, `0019` and the libvlcjni patch `vlc/jni-patches/0001` are new; kernel modules (`dvfel 2026.10.15.1`, `mvc3d`), libdvfelplay `2026.10.16.1` and the app patch unchanged. Everything from 0.2.7 as before.

**Tested:** Dune HD Pro 8K Plus (R22): TrueHD, DTS-HD MA, E-AC-3, AC-3, DTS go out as bitstreams (format, channels and rate checked in the box's HDMI driver, e.g. TrueHD: MAT, 8 ch, 192 kHz), track changes and seeks fine. The packers' output is byte for byte identical to FFmpeg's `-f spdif`. Feedback on what your AV receiver shows is welcome.

### Install / update
Install `VLC-FEL-0.2.8-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0019 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patch `vlc/android-patches/0001` against vlc-android `fc5fa1f`.

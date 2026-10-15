# dvfelplay

Dolby Vision profile 7 FEL playback on the Android firmware of Amlogic S928X players (Dune HD, R_volution)
for any MediaCodec based player: a small C API (used by VLC FEL).

The player keeps decoding the base layer with its own MediaCodec instance (Dolby Vision decoder, SurfaceView)
and passes every access unit through `dvfp_filter()`: the RPU goes to the decoder as profile 8.1 with a no-op
mapping, the EL NAL units are taken out. In FEL mode the library decodes the EL (second MediaCodec, imported
by the `dvfel` kernel module) and composes BL + EL + original RPU mapping on the GPU for every displayed frame.
See `include/dvfelplay.h` for the API and its call sequence.

Requirements: the `dvfel` kernel module `2026.10.06.2` or newer loaded, `/dev/dvfel` accessible. The library
bundles the module (`DVFEL_KO` at build time) and installs it on request: `dvfp_module_install()` loads it with
root (`su`; asks through a player callback to switch the firmware's root access on for the installation) and
installs it with a boot script into the Dune HD firmware's boot hook. The Dune HD / R_volution userspace is
32-bit: `armeabi-v7a`.

Lead time (module `2026.10.07.1`): on Android the video composer hands each frame out only at its display
vsync, leaving the GPU about one frame period. The library asks the module for a lead time
(`DVFEL_IOC_SET_LEAD`, `out_lead_ms`, 125 ms): the player presents every frame `dvfp_present_lead_ns()` early
and the module holds the composed frame back to its vsync.

## Layout

| Path | Content |
|---|---|
| `include/dvfelplay.h` | public C API |
| `src/dvfelplay.cpp` | filter (P7 → P8.1, EL extraction), EL decoder, GPU thread, BL/EL pairing by RPU CRC |
| `src/felgpu.*` | GLES 3.2 composer |
| `src/dvfel_uapi.h` | kernel interface (from the kernel module) |
| `src/module_install.cpp`, `src/module_blob.S.in` | the bundled kernel module and its installer |
| `test/` | `dvfp_test`: NativeActivity test player (no audio) for an `.au` file |
| `tools/mkau.py` | MKV/MP4 → `.au` (Annex-B access units with pts, as a player hands them to MediaCodec) |
| `build.sh` | library + test APK (WSL/Linux, NDK r28c, libdovi built with cargo-c, `LIBDOVI_DIR`) |

## Test

```
./build.sh                                  # ~/dvfelplay-work/build/dvfptest.apk
python3 tools/mkau.py in.mkv test.au
adb install -r dvfptest.apk
adb push test.au /sdcard/Android/data/org.dvfel.dvfptest/files/test.au
adb shell am start -n org.dvfel.dvfptest/android.app.NativeActivity
adb logcat -s dvfp_test dvfelplay
```

Switches (`adb shell setprop`): `debug.dvfp.test.fel` (0: conversion only), `debug.dvfp.test.loops`,
`debug.dvfp.test.seek` (seconds jumped every 10 s), `debug.dvfp.test.codec`; library: `debug.dvfp.dither`
(0/1/2), `debug.dvfp.pattern`, `debug.dvfp.demo`, `debug.dvfp.nogpu`, `debug.dvfp.profile` (1: GPU pass
times, 2: frame times and the player's release lead, every 5 s), `debug.dvfp.lead_ms` (lead time override, 0:
off), `debug.dvfp.dump` (frames for `tools/felbench`).

`test/au_check.sh dvfptest.apk test.au [seek s]` plays the file to its end and fails on frames shown without
their RPU, a stalled EL decoder or late frames (e.g. Dune HD's `MP4_P7_FEL_STDL.mp4`, whose access units from
frame 105 on are about half without EL and RPU).


## License

GPL-2.0-or-later.

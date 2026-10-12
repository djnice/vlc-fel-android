# libmvc3dplay

3D MVC (Blu-ray 3D) playback with HDMI 1.4 frame packing for any MediaCodec based player on the Android
firmware of Amlogic S928X players (Dune HD / R_volution), with the `mvc3d` kernel module
(`../kernel/mvc3d`). API: `include/mvc3dplay.h`.

```
player ─ MediaCodec H.264 (base view) → HWC → video_composer.0 → mvc3d (kernel) → video_render.0
   │ every buffer queued to MediaCodec (numbered as the Amlogic decoder numbers its input)
   └─ libmvc3dplay (32-bit, in the player) ─ socket ─ mvc3dhelper (static aarch64 process)
        edge264-mvc: both views in software → 1920x2205 UYVY (L, 45 lines, R) into mvc3d's buffers
        mvc3d job per frame the composer releases (its decoder picture's number) → the matching picture
```

- `src/mvc3dplay.c`: starts `mvc3dhelper` (shipped as `libmvc3dhelper.so` next to the library: the
  player's userspace is 32-bit, the kernel arm64), streams the access units, lead time, flush/restart.
- `helper/mvc3dhelper.c`: decoding (edge264-mvc, multithreaded), frame packing composition ahead of time,
  answering the module's jobs; frames the player drops and the preroll after a seek are skipped.
- `src/module_install.c` + `src/module_blob.S`: the bundled `mvc3d.ko` and its installer (root question,
  boot script `config/boot/mvc3d.sh`), as libdvfelplay does it for `dvfel.ko`.
- `edge264/`: edge264-mvc (BSD) at `5757e71` plus `0001-*.patch` (picture tags: `edge264_set_tag()`,
  `Edge264Frame.tag`), `edge264/build.sh` builds it as a static aarch64 library.

Build (WSL/Linux): `edge264/build.sh`, the module (`tools/kbuild.sh kernel/mvc3d`), then `./build.sh`
(NDK r28c for the library, a static capable aarch64 gcc for the helper; see the variables in the script).
`tools/swaplibs.sh` (maintainer test): replaces both files in an installed VLC FEL over adb (root).

GPL-2.0-or-later (edge264-mvc: BSD-2-Clause).

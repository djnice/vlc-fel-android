**Proof of concept**: an experimental build for enthusiasts. It installs kernel modules on your player; use it at your own risk.

New in 0.2.21:

- **3D MVC decoder setting on the S928X.** Settings → Video → *3D MVC: hardware decoder*. Off by default: the software decoder, as before. On: the SoC's MVC decoder.
- **3D MVC on the S905X4**, with the hardware decoder (no setting there, no software decoder). Its decoder's DMA bug, which overwrites system memory, is worked around in the `mvc3d` module (`2026.11.06.8-sc2`).
- A new Dune HD plugin, `mvc3d`, loads the module at boot with no root question: see the release `dune-plugin-mvc3d-2026.11.06.1`.

`mvc3d` is new for both SoCs (S928X `2026.11.06.1`), and so is libmvc3dplay (`2026.11.06.5`). libdvfelplay and the `dvfel` modules are unchanged.

**Tested** on a Dune HD Pro 8K Plus (S928X, r25) with a 3D projector: the disc's `.ssif` with both decoders, and a P7 FEL m2ts.

**Not tested:**
- this app on the S905X4. The same library and module were tested on a Dune HD Pro Vision 4K with CineFuze (`.ssif`, MKV, mk3d, m2ts, ISO), not with this app;
- a whole film with the hardware decoder. On the S928X one start of a Matroska film jerked before present times were snapped to the vsyncs; after that 0 of 18 starts did, a small sample. The software decoder stays the default for that reason.

With the hardware decoder, the switch to frame packing comes about 8 s after the picture.

**Proof of concept**: an experimental build for enthusiasts. Use it at your own risk.

A new version of the Dune HD firmware plugin for 3D MVC (r25, plugin `mvc3d`, shown as *3D MVC (S928X, S905X4)*). It updates `2026.11.24.2-0.2.23`; it is separate from the `s928x_dvfel` plugin, and you can have both.

What it does at boot, as before:

- loads the box's `mvc3d` kernel module, so VLC FEL needs no root for it;
- keeps a newer module that a player has installed;
- copies libmvc3dplay to `config/`, for a player of the firmware that may use it.

What changed since `2026.11.24.2`, all on the **S905X4** (module `2026.11.26.4-sc2`):

- **No HDMI re-handshake at seeks and chapter changes.** Frame packing goes out as 8 bit 4:4:4, the 3D clock table's format. When a seek or chapter change opens the MVC decoder again, the firmware's Auto HDR (SystemControl) read `444,8bit` from `amhdmitx0/attr`, set its own format for the mode (`ycc444,10bit`) and set the mode again: about eight HDMI mode sets per chapter change, seconds without a picture on a projector. While frame packing forces 8 bit, `attr` now reads back the format user space wrote last, so SystemControl finds nothing to change.
- **Frame packing set up late if needed.** Loaded at boot before the TV or projector gave the HDMI output a mode, the module found no current video parameters, took the hdmitx layout for unknown and left frame packing off until the next boot. It now checks again when a player first asks for frame packing.
- **The GUI in both views**, as on the S928X: `gui2` on `/dev/mvc3dhw` (see the `2026.11.24.2` notes).
- libmvc3dplay bundles the new module; the library's own code is unchanged (it still reports `2026.11.24.2`).

The S928X module (`2026.11.24.2`) is the same file as in `2026.11.24.2`.

**The Dune player itself does not play 3D MVC with this plugin:** the firmware's player has no code for it today.

**Tested:** the S905X4 module `2026.11.26.4-sc2` with a test player on a Dune HD Pro Vision 4K (S905X4, firmware `R3.20261021`) with an Optoma projector: 3D MVC in frame packing, chapter changes without an HDMI mode set, menus and subtitles in both eyes. **Not tested:** the plugin itself (installation from the file browser and loading at boot), and the late frame packing set-up after a boot with the TV off.

### Install
Copy `dune_plugin_mvc3d-2026.11.26.1-0.2.24.zip` to a drive or network share the player reaches, open it in the player's file browser and confirm.

Source: this repository, `kernel/mvc3d`, `kernel/mvc3d/sc2`, `kernel/mvc3d/dune-plugin` and `libmvc3dplay/` (GPL-2.0-or-later). The plugin is built on Dune HD's `s928x_dvfel` plugin.

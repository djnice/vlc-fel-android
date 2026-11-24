**Proof of concept**: an experimental build for enthusiasts. Use it at your own risk.

A new version of the Dune HD firmware plugin for 3D MVC (r25, plugin `mvc3d`, shown as *3D MVC (S928X, S905X4)*). It updates the `2026.11.06.1-0.2.21` plugin; it is separate from the `s928x_dvfel` plugin, and you can have both.

What it does at boot, as before:

- loads the box's `mvc3d` kernel module, so VLC FEL needs no root for it;
- keeps a newer module that a player has installed;
- copies libmvc3dplay to `config/`, for a player of the firmware that may use it.

What changed since `2026.11.06.1`:

- **S928X module `2026.11.24.2`: the GUI in both views of frame packing.** A player can ask for it by writing `gui2` on `/dev/mvc3dhw` (`gui1`, or closing the file, ends it). While the output is frame packed, the module then stretches the OSD planes over the whole frame packed picture (1920x2205: left view, 45 lines of active space, right view), and the player draws its GUI twice: squeezed into the upper 1080/2205 of the screen for the left eye, and from 1125/2205 down for the right eye. The status read from `/dev/mvc3dhw` has a new `gui=0|1` field. Without such a request nothing changes.
- **libmvc3dplay `2026.11.24.2`** bundles that module. Its version is the one VLC FEL 0.2.23 brings, so with this plugin it does not ask for root access to update the module.
- The S905X4 module (`2026.11.06.8-sc2`) is unchanged and has no `gui` request yet.

**The Dune player itself does not play 3D MVC with this plugin:** the firmware's player has no code for it today.

**Tested** on a Dune HD Pro 8K Plus (S928X, r25): installed from the file browser over `2026.11.06.1`; after a reboot the module `2026.11.24.2` was loaded with `/dev/mvc3d` and `/dev/mvc3dhw` open to the players, root access off; VLC FEL 0.2.23 played 3D MVC. A test player using the GUI request showed its menus, info panel and subtitles readable in both eyes on a projector.

**Not tested:** the plugin on the S905X4.

### Install
Copy `dune_plugin_mvc3d-2026.11.24.2-0.2.23.zip` to a drive or network share the player reaches, open it in the player's file browser and confirm.

Source: this repository, `kernel/mvc3d`, `kernel/mvc3d/sc2`, `kernel/mvc3d/dune-plugin` and `libmvc3dplay/` (GPL-2.0-or-later). The plugin is built on Dune HD's `s928x_dvfel` plugin.

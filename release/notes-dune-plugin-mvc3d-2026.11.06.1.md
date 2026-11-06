**Proof of concept**: an experimental build for enthusiasts. Use it at your own risk.

A new Dune HD firmware plugin for 3D MVC (r25, plugin `mvc3d`, shown as *3D MVC (S928X, S905X4)*). It is separate from the `s928x_dvfel` plugin; you can have both.

What it does at boot:

- loads the box's `mvc3d` kernel module (S928X `2026.11.06.1`, S905X4 `2026.11.06.8-sc2`), so VLC FEL and CineFuze need no root for it;
- keeps a newer module that a player has installed;
- copies libmvc3dplay (`2026.11.06.5`) to `config/`, for a player of the firmware that may use it.

**The Dune player itself does not play 3D MVC with this plugin:** the firmware's player has no code for it today.

**Tested** on a Dune HD Pro 8K Plus (S928X, r25): installed from the file browser, after a reboot the module loaded and CineFuze 1.1.2 played a Blu-ray 3D `.ssif` in frame packing with root access off.

**Not tested:** the plugin on the S905X4 (the module itself was tested there by hand).

### Install
Copy `dune_plugin_mvc3d-2026.11.06.1-0.2.21.zip` to a drive or network share the player reaches, open it in the player's file browser and confirm.

Source: this repository, `kernel/mvc3d`, `kernel/mvc3d/sc2`, `kernel/mvc3d/dune-plugin` and `libmvc3dplay/` (GPL-2.0-or-later). The plugin is built on Dune HD's `s928x_dvfel` plugin.

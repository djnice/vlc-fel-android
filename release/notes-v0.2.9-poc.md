**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.9:

- **The files of USB / internal drives show.** Users saw the folders of their player's drives in VLC FEL but no files in them. The player's media index leaves its drives out, and on Android 11 an app without *All files access* sees only indexed files there – the storage permission shows the folders, none of the files – while the firmware has no settings page to allow that access. VLC FEL now grants it to itself with root when it starts: with `su` if the root access is on; if it is off, it is switched on for a moment (~0.7 s measured) and set back to its previous value right after. Should the app die in that moment, the next start restores it. Nothing to do on your side; after a failure (no root at all) it tries again a day later, or when you ask for the storage permission.

The app patch `vlc/android-patches/0002` is new; kernel modules (`dvfel 2026.10.15.1`, `mvc3d`), libdvfelplay `2026.10.16.1` and the VLC patches unchanged (the native libraries are byte for byte those of 0.2.8). Everything from 0.2.8 as before.

**Tested:** Dune HD Pro One 8K Plus (R24) in the state users reported (storage permission given, root access off): at the start the root access was on for 0.64–0.74 s and back off, *All files access* granted, the browser lists the drive's videos with thumbnails and the folders' file counts; a crash while root was on is undone at the next start.

### Install / update
Install `VLC-FEL-0.2.9-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0019 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patches `vlc/android-patches/0001`, `0002` against vlc-android `fc5fa1f`.

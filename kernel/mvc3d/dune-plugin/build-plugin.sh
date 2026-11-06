#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# WSL: the Dune HD plugin zip. MVC3D_KO, MVC3D_KO_SC2: the modules; MVC3D_DIR: libmvc3dplay.so + libmvc3dhelper.so
set -e
S=$(dirname "$(readlink -f "$0")")
V=$(sed -n 's#.*<version>\(.*\)</version>.*#\1#p' "$S/dune_plugin.xml")
OUT=${OUT:-$S}
T=$(mktemp -d)
mkdir "$T/bin"
tr -d '\r' < "$S/bin/ctl.sh" > "$T/bin/ctl.sh"; chmod 755 "$T/bin/ctl.sh"
tr -d '\r' < "$S/dune_plugin.xml" > "$T/dune_plugin.xml"
cp "$MVC3D_KO" "$T/mvc3d.ko"
cp "$MVC3D_KO_SC2" "$T/mvc3d_sc2.ko"
cp "$MVC3D_DIR/libmvc3dplay.so" "$MVC3D_DIR/libmvc3dhelper.so" "$T/"
(cd "$T" && rm -f "$OUT/dune_plugin_mvc3d-$V.zip" && zip -q -r -X "$OUT/dune_plugin_mvc3d-$V.zip" bin dune_plugin.xml mvc3d.ko mvc3d_sc2.ko libmvc3dplay.so libmvc3dhelper.so)
rm -rf "$T"
(cd "$OUT" && sha256sum "dune_plugin_mvc3d-$V.zip" | tee "dune_plugin_mvc3d-$V.zip.sha256")
unzip -l "$OUT/dune_plugin_mvc3d-$V.zip"

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 djnice <djnice@freemail.hu>
"""
felframes.py - test frames for tools/felbench without a player: <name>_bl.bin, <name>_el.bin
and <name>_meta.bin in the dvfel buffer layout (one word a pixel: Cr | Cb << 10 | Y << 20).
The reference output comes from felbench itself (-r, with the composer to compare against).

  # the layers and the RPU of a profile 7 FEL stream (dovi_tool, ffmpeg)
  dovi_tool demux in.hevc                              # BL.hevc, EL.hevc
  dovi_tool extract-rpu EL.hevc -o rpu.bin && dovi_tool export -i rpu.bin -d all=rpu.json
  ffmpeg -i BL.hevc -vf "select='eq(n,240)'" -vsync 0 -pix_fmt yuv420p10le -f rawvideo bl.yuv
  ffmpeg -i EL.hevc -vf "select='eq(n,240)'" -vsync 0 -pix_fmt yuv420p10le -f rawvideo el.yuv

  felframes.py picture f240 --bl bl.yuv --el el.yuv --rpu rpu.json --frame 240 --out frames
  felframes.py noise n240 --rpu rpu.json --frame 240 --out frames

picture: a decoded base layer picture and its EL picture (frame --index of the .yuv files) with
the mapping of RPU --frame. noise: random samples with that mapping - every code, the clamps and
the picture edges; the chroma at odd x or y, which the kernel interpolates and the composer must
not read, is random too. --no-el: the mapping without the residual (a frame whose EL is missing).
"""
import argparse
import json
import os
import struct

import numpy as np


def fixed(i, f, denom):
    return float(i) + float(f) / float(1 << denom)


def meta_bytes(rpu, use_el=True):
    """struct felgpu_meta (felgpu.h) of an RPU as dovi_tool exports it; 5240 bytes on armeabi-v7a"""
    hdr, m = rpu["header"], rpu["rpu_data_mapping"]
    denom = hdr["coefficient_log2_denom"]
    npiv = [0] * 3
    pivots = [[0] * 9 for _ in range(3)]
    mapping = [[0] * 8 for _ in range(3)]
    poly = [[[0.0] * 3 for _ in range(8)] for _ in range(3)]
    mmr_order = [[0] * 8 for _ in range(3)]
    mmr_const = [[0.0] * 8 for _ in range(3)]
    mmr = [[[[0.0] * 7 for _ in range(3)] for _ in range(8)] for _ in range(3)]
    nlq_offset = [0] * 3
    nlq_slope, nlq_thresh, nlq_max = [0.0] * 3, [0.0] * 3, [0.0] * 3
    el = use_el and not hdr["disable_residual_flag"] and m.get("nlq") is not None
    for c in range(3):
        cv = m["curves"][c]
        n = cv["num_pivots_minus2"] + 2
        npiv[c] = n
        acc = 0
        for i in range(n):  # coded as deltas
            acc += cv["pivots"][i]
            pivots[c][i] = acc
        for k in range(n - 1):
            if cv["mapping_idc"] == "Polynomial":
                order = cv["poly_order_minus1"][k] + 1
                for j in range(min(order, 2) + 1):
                    poly[c][k][j] = fixed(cv["poly_coef_int"][k][j], cv["poly_coef"][k][j], denom)
            else:
                mapping[c][k] = 1
                order = cv["mmr_order_minus1"][k] + 1
                mmr_order[c][k] = order
                mmr_const[c][k] = fixed(cv["mmr_constant_int"][k], cv["mmr_constant"][k], denom)
                for o in range(min(order, 3)):
                    for t in range(7):
                        mmr[c][k][o][t] = fixed(cv["mmr_coef_int"][k][o][t], cv["mmr_coef"][k][o][t], denom)
        if el:
            q = m["nlq"]
            nlq_offset[c] = q["nlq_offset"][c]
            nlq_slope[c] = fixed(q["linear_deadzone_slope_int"][c], q["linear_deadzone_slope"][c], denom)
            nlq_thresh[c] = fixed(q["linear_deadzone_threshold_int"][c], q["linear_deadzone_threshold"][c], denom)
            nlq_max[c] = fixed(q["vdr_in_max_int"][c], q["vdr_in_max"][c], denom)
    b = struct.pack("<3i", hdr["bl_bit_depth_minus8"] + 8, hdr["el_bit_depth_minus8"] + 8, 1 if el else 0)
    b += struct.pack("<3i", *npiv)
    for c in range(3):
        b += struct.pack("<9i", *pivots[c])
    for c in range(3):
        b += struct.pack("<8i", *mapping[c])
    b += b"\0" * 4  # the doubles are 8 byte aligned
    for c in range(3):
        for k in range(8):
            b += struct.pack("<3d", *poly[c][k])
    for c in range(3):
        b += struct.pack("<8i", *mmr_order[c])
    for c in range(3):
        b += struct.pack("<8d", *mmr_const[c])
    for c in range(3):
        for k in range(8):
            for o in range(3):
                b += struct.pack("<7d", *mmr[c][k][o])
    b += struct.pack("<3i", *nlq_offset) + b"\0" * 4
    b += struct.pack("<3d", *nlq_slope) + struct.pack("<3d", *nlq_thresh) + struct.pack("<3d", *nlq_max)
    b += struct.pack("<2i", -1, -1) + struct.pack("<4i", -1, -1, -1, -1)  # no level 1, no level 5
    assert len(b) == 5240, len(b)
    return b


def read_yuv(path, w, h, index):
    size = w * h * 3  # yuv420p10le
    with open(path, "rb") as f:
        f.seek(index * size)
        d = np.frombuffer(f.read(size), dtype="<u2")
    if d.size != w * h * 3 // 2:
        raise SystemExit(f"{path}: no {w}x{h} frame {index}")
    c = (w // 2) * (h // 2)
    return d[:w * h].reshape(h, w), d[w * h:w * h + c].reshape(h // 2, w // 2), d[w * h + c:].reshape(h // 2, w // 2)


def pack(y, cb, cr, junk=None):
    full = [np.repeat(np.repeat(p, 2, axis=0), 2, axis=1).astype(np.uint32) for p in (cb, cr)]
    if junk is not None:
        for a in full:
            keep = a[::2, ::2].copy()
            a[:, :] = junk.integers(0, 1024, size=a.shape, dtype=np.uint32)
            a[::2, ::2] = keep
    return (full[1] | (full[0] << 10) | (y.astype(np.uint32) << 20)).astype("<u4")


def noise(w, h, rng, lo, hi):
    return (rng.integers(lo, hi + 1, size=(h, w), dtype=np.uint16),
            rng.integers(lo, hi + 1, size=(h // 2, w // 2), dtype=np.uint16),
            rng.integers(lo, hi + 1, size=(h // 2, w // 2), dtype=np.uint16))


def main():
    ap = argparse.ArgumentParser(description="test frames for tools/felbench")
    ap.add_argument("kind", choices=("picture", "noise"))
    ap.add_argument("name")
    ap.add_argument("--rpu", required=True, help="dovi_tool export -d all=rpu.json")
    ap.add_argument("--frame", type=int, required=True, help="the RPU's frame number")
    ap.add_argument("--out", default=".")
    ap.add_argument("--size", default="3840x2160", help="base layer size (the EL is half of it)")
    ap.add_argument("--no-el", action="store_true", help="the mapping without the residual")
    ap.add_argument("--bl", help="picture: base layer, yuv420p10le")
    ap.add_argument("--el", help="picture: enhancement layer, yuv420p10le")
    ap.add_argument("--index", type=int, default=0, help="picture: frame of the .yuv files")
    ap.add_argument("--bl-range", type=int, nargs=2, default=(0, 1023), help="noise: base layer codes")
    ap.add_argument("--el-range", type=int, nargs=2, default=(0, 1023), help="noise: EL codes")
    ap.add_argument("--seed", type=int, default=20261114)
    a = ap.parse_args()
    w, h = (int(v) for v in a.size.split("x"))
    meta = meta_bytes(json.load(open(a.rpu))[a.frame], use_el=not a.no_el)
    rng = np.random.default_rng(a.seed)
    if a.kind == "picture":
        if not a.bl or not a.el:
            ap.error("picture needs --bl and --el")
        bl, el, junk = read_yuv(a.bl, w, h, a.index), read_yuv(a.el, w // 2, h // 2, a.index), None
    else:
        bl, el, junk = noise(w, h, rng, *a.bl_range), noise(w // 2, h // 2, rng, *a.el_range), rng
    os.makedirs(a.out, exist_ok=True)
    pack(*bl, junk=junk).tofile(f"{a.out}/{a.name}_bl.bin")
    pack(*el, junk=junk).tofile(f"{a.out}/{a.name}_el.bin")
    with open(f"{a.out}/{a.name}_meta.bin", "wb") as f:
        f.write(meta)


if __name__ == "__main__":
    main()

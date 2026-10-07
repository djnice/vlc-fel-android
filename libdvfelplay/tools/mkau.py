#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
mkau.py - the video access units of a file (MKV/MP4, HEVC) for dvfp_test:
Annex-B with their pts, in decode order, as a player hands them to MediaCodec.

  mkau.py in.mkv out.au [max_seconds]

Format (little endian):
  header: "DVAU", u32 version (1), u32 width, u32 height, u32 fps_num, u32 fps_den
  per access unit: u32 size, u32 flags (1: key frame), i64 pts (us), size bytes

Key frames get the VPS/SPS/PPS in front (the stream's codec config).
Needs ffmpeg and ffprobe.
"""
import json
import struct
import subprocess
import sys


def run(args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE).stdout


def annexb_nals(data):
    """NAL units of an Annex-B buffer"""
    out = []
    i = data.find(b"\x00\x00\x01")
    while i >= 0:
        start = i + 3
        j = data.find(b"\x00\x00\x01", start)
        end = j if j >= 0 else len(data)
        nal = data[start:end].rstrip(b"\x00")
        if nal:
            out.append(nal)
        i = j
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    max_s = float(sys.argv[3]) if len(sys.argv) > 3 else None

    info = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                           "stream=width,height,r_frame_rate,codec_name", "-of", "json", src]))
    st = info["streams"][0]
    if st["codec_name"] != "hevc":
        sys.exit("not HEVC")
    num, den = (int(x) for x in st["r_frame_rate"].split("/"))

    pk = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                         "packet=pts_time,dts_time,size,flags", "-of", "json", src]))["packets"]

    # the codec config: the parameter sets mp4toannexb puts in front of the first frame
    first = run(["ffmpeg", "-v", "error", "-i", src, "-map", "0:v:0", "-c", "copy", "-bsf:v",
                 "hevc_mp4toannexb", "-frames:v", "1", "-f", "hevc", "-"])
    params = b"".join(b"\x00\x00\x00\x01" + n for n in annexb_nals(first)
                      if ((n[0] >> 1) & 0x3F) in (32, 33, 34))
    if not params:
        sys.exit("no VPS/SPS/PPS found")

    # the packets as stored (length prefixed, 4 bytes)
    raw = run(["ffmpeg", "-v", "error", "-i", src, "-map", "0:v:0", "-c", "copy", "-f", "data", "-"])

    out = open(dst, "wb")
    out.write(b"DVAU" + struct.pack("<5I", 1, st["width"], st["height"], num, den))
    pos = 0
    n = 0
    for p in pk:
        size = int(p["size"])
        data = raw[pos:pos + size]
        pos += size
        if len(data) != size:
            sys.exit("packet data short")
        t = p.get("pts_time") or p.get("dts_time")
        pts = int(round(float(t) * 1e6))
        if max_s is not None and pts > max_s * 1e6 and "K" in p["flags"]:
            break
        au = bytearray()
        i = 0
        while i + 4 <= size:
            ln = struct.unpack(">I", data[i:i + 4])[0]
            i += 4
            if ln == 0 or i + ln > size:
                sys.exit("packet %d: bad NAL length %d (length size not 4?)" % (n, ln))
            au += b"\x00\x00\x00\x01" + data[i:i + ln]
            i += ln
        key = "K" in p["flags"]
        if key:
            au = bytearray(params) + au
        out.write(struct.pack("<IIq", len(au), 1 if key else 0, pts))
        out.write(au)
        n += 1
    out.close()
    print("%d access units, %dx%d %d/%d fps -> %s" % (n, st["width"], st["height"], num, den, dst))


if __name__ == "__main__":
    main()

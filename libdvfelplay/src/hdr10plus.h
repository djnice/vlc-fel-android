/*
 *  Copyright (C) 2026 djnice <djnice@freemail.hu>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * HDR10+ (SMPTE ST 2094-40 SEI) -> Dolby Vision profile 8.1 RPU, as the
 * SamuriHL CoreELEC Kodi build does it ("Convert HDR10+ video to DV profile
 * 8.1 during playback", HDR10PlusConvert.cpp, peak brightness source
 * "histogram plus"): every access unit's HDR10+ dynamic metadata becomes a
 * level 1 (min / max / avg PQ) of a generated profile 8.1 RPU with CM v4.0,
 * the stream's mastering display and content light levels its level 6 and
 * source PQ range. The RPU is libdovi's generator output (dovi_tool generate,
 * long play mode) for these values: a template with the values patched in.
 *
 * No Android dependency (tested on the build host: test/hdr10plus_test.cpp).
 */
#ifndef DVFP_HDR10PLUS_H
#define DVFP_HDR10PLUS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hdr10plus
{

// the stream's static HDR metadata, from its SEIs as they come
struct Static
{
  bool hasMdcv{false};
  uint32_t maxLum{0}; // nits
  uint32_t minLum{0}; // 0.0001 nits
  bool hasCll{false};
  uint16_t maxCll{0};
  uint16_t maxFall{0};
};

// the ST 2094-40 values the conversion uses (window 0)
struct Meta
{
  int numWindows{0};
  uint32_t maxscl[3]{};
  uint32_t averageMaxrgb{0};
  int numPercentiles{0};
  uint8_t percentage[16]{};
  uint32_t percentile[16]{};
};

// the RPU's values (after libdovi's level 1 clamping)
struct Values
{
  uint16_t sourceMinPq, sourceMaxPq;
  uint16_t minPq, maxPq, avgPq;
  uint16_t mdlMax, mdlMin, maxCll, maxFall;
  bool operator==(const Values& o) const;
  bool operator!=(const Values& o) const { return !(*this == o); }
};

/*
 * a prefix SEI NAL unit (type 39, with its 2-byte header, escaped): updates
 * st from mastering display colour volume / content light level messages;
 * true if it carries HDR10+ dynamic metadata (then in meta)
 */
bool ParseSei(const uint8_t* nal, size_t size, Static& st, Meta& meta);

// the RPU's values for one picture's HDR10+ metadata (Kodi's
// create_rpu_nalu_for_hdr10plus, then libdovi's clamping)
Values Convert(const Meta& meta, const Static& st);

// the RPU as an HEVC NAL unit type 62 (2-byte header, escaped, no start code)
std::vector<uint8_t> WriteRpu(const Values& v);

} // namespace hdr10plus

#endif

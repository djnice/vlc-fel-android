/*
 *  Copyright (C) 2026 djnice <djnice@freemail.hu>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hdr10plus.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hdr10plus
{
namespace
{

// dovi_tool 2.3.4 generate (libdovi 3.x generator): profile 8.1, CM v4.0, long
// play mode, one shot; source_min_pq 7, source_max_pq 3079, L1 0 / 2500 /
// 1229, L6 1000 / 1 / 0 / 0. RBSP from the 0x19 prefix to the byte before the
// CRC32 (tools: the template's fields found by generating with each value
// changed; test/hdr10plus_test.cpp checks the output against dovi_tool's).
const uint8_t kRpuTemplate[146] = {
  0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0x50, 0x6f, 0x00, 0x3f, 0xf8, 0x01, 0xff, 0xc0, 0x0f,
  0xff, 0xd0, 0x00, 0x00, 0x08, 0x00, 0x00, 0x06, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x34, 0x00,
  0x00, 0x02, 0x00, 0x00, 0x01, 0xa2, 0x56, 0x60, 0x00, 0x03, 0x5e, 0xa2, 0x56, 0x6f, 0x9f, 0xce,
  0xb1, 0xc2, 0x56, 0x64, 0x4c, 0xa0, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0x80, 0x00, 0x00, 0x01, 0xc3, 0x62, 0x24, 0x30, 0x18, 0x60, 0xa5, 0xe3, 0x08, 0xe0, 0x51, 0x40,
  0x00, 0x00, 0x1a, 0x63, 0xe5, 0xaf, 0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,
  0x02, 0x00, 0xf8, 0x0e, 0x15, 0x10, 0x30, 0x08, 0x00, 0x4e, 0x22, 0x66, 0x80, 0x80, 0x50, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0xc0, 0x7d, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x05,
  0x30, 0x1c, 0x00, 0x40, 0x04, 0x00, 0x02, 0x09, 0x00, 0x7f, 0xc0, 0x00, 0x45, 0x0b, 0x01, 0x00,
  0x00, 0x00,
};
// bit offsets of the fields in kRpuTemplate (MSB first)
constexpr int BIT_SOURCE_MIN_PQ = 775; // 12 bits, vdr_dm_data
constexpr int BIT_SOURCE_MAX_PQ = 787; // 12
constexpr int BIT_L1_MIN = 829;        // 12, level 1
constexpr int BIT_L1_MAX = 841;        // 12
constexpr int BIT_L1_AVG = 853;        // 12
constexpr int BIT_L6_MDL_MAX = 955;    // 16, level 6
constexpr int BIT_L6_MDL_MIN = 971;    // 16
constexpr int BIT_L6_MAX_CLL = 987;    // 16
constexpr int BIT_L6_MAX_FALL = 1003;  // 16

// libdovi's level 1 limits (CM v4.0)
constexpr uint16_t L1_MIN_PQ_MAX_VALUE = 12;
constexpr uint16_t L1_MAX_PQ_MIN_VALUE = 2081;
constexpr uint16_t L1_MAX_PQ_MAX_VALUE = 4095;
constexpr uint16_t L1_AVG_PQ_MIN_VALUE = 819; // Kodi's own clamp (CM v2.9's)
constexpr uint16_t L1_AVG_PQ_MIN_VALUE_CMV40 = 1229;
constexpr uint16_t MAX_PQ_LUMINANCE = 10000; // level 6

// ST 2084
constexpr double ST2084_Y_MAX = 10000.0;
constexpr double ST2084_M1 = 2610.0 / 16384.0;
constexpr double ST2084_M2 = (2523.0 / 4096.0) * 128.0;
constexpr double ST2084_C1 = 3424.0 / 4096.0;
constexpr double ST2084_C2 = (2413.0 / 4096.0) * 32.0;
constexpr double ST2084_C3 = (2392.0 / 4096.0) * 32.0;

double NitsToPq(double nits)
{
  const double y = nits / ST2084_Y_MAX;
  return std::pow((ST2084_C1 + ST2084_C2 * std::pow(y, ST2084_M1)) / (1.0 + ST2084_C3 * std::pow(y, ST2084_M1)),
                  ST2084_M2);
}

uint16_t CastPq(double nits)
{
  return static_cast<uint16_t>(std::round(NitsToPq(nits) * 4095.0));
}

uint16_t Clamp16(uint16_t d, uint16_t min, uint16_t max)
{
  const uint16_t t = d < min ? min : d;
  return t > max ? max : t;
}

std::vector<uint8_t> Unescape(const uint8_t* p, size_t size)
{
  std::vector<uint8_t> out;
  out.reserve(size);
  int zeros = 0;
  for (size_t i = 0; i < size; i++)
  {
    if (zeros >= 2 && p[i] == 3)
    {
      zeros = 0;
      continue;
    }
    zeros = p[i] ? 0 : zeros + 1;
    out.push_back(p[i]);
  }
  return out;
}

class BitReader
{
public:
  BitReader(const uint8_t* p, size_t size) : m_p(p), m_bits(size * 8) {}
  uint32_t Read(int n)
  {
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
    {
      v <<= 1;
      if (m_pos < m_bits)
        v |= (m_p[m_pos >> 3] >> (7 - (m_pos & 7))) & 1;
      else
        m_over = true;
      m_pos++;
    }
    return v;
  }
  void Skip(size_t n)
  {
    m_pos += n;
    if (m_pos > m_bits)
      m_over = true;
  }
  bool Over() const { return m_over; }

private:
  const uint8_t* m_p;
  size_t m_bits;
  size_t m_pos{0};
  bool m_over{false};
};

// user_data_registered_itu_t_t35 of HDR10+ (ST 2094-40, as Kodi's
// hdr10plus_sei_to_metadata reads it: window 0's values kept)
bool ParseT35(const uint8_t* p, size_t size, Meta& meta)
{
  BitReader br(p, size);
  const uint32_t country = br.Read(8);
  const uint32_t provider = br.Read(16);
  const uint32_t oriented = br.Read(16);
  const uint32_t appId = br.Read(8);
  const uint32_t appVersion = br.Read(8);
  if (country != 0xB5 || provider != 0x003C || oriented != 0x0001 || appId != 4 || appVersion > 1)
    return false;
  Meta m;
  m.numWindows = br.Read(2);
  for (int i = 1; i < m.numWindows; i++)
    br.Skip(16 * 6 + 8 + 16 * 3 + 1); // processing window i
  br.Skip(27); // targeted_system_display_maximum_luminance
  if (br.Read(1))
  {
    const int rows = br.Read(5), cols = br.Read(5);
    br.Skip(size_t(rows) * cols * 4);
  }
  for (int w = 0; w < m.numWindows; w++)
  {
    uint32_t maxscl[3];
    for (auto& v : maxscl)
      v = br.Read(17);
    const uint32_t avg = br.Read(17);
    const int n = br.Read(4);
    uint8_t percentage[16];
    uint32_t percentile[16];
    for (int i = 0; i < n; i++)
    {
      percentage[i] = br.Read(7);
      percentile[i] = br.Read(17);
    }
    br.Skip(10); // fraction_bright_pixels
    if (w == 0)
    {
      std::copy(maxscl, maxscl + 3, m.maxscl);
      m.averageMaxrgb = avg;
      m.numPercentiles = n;
      std::copy(percentage, percentage + n, m.percentage);
      std::copy(percentile, percentile + n, m.percentile);
    }
  }
  if (br.Over())
    return false;
  meta = m;
  return true;
}

uint16_t MaximumPq(const Meta& meta)
{
  // HistogramPlus = Histogram: the highest percentile
  if (meta.numWindows == 0 || meta.numPercentiles == 0)
    return 0;
  uint32_t max = meta.percentile[0];
  for (int i = 1; i < meta.numPercentiles; i++)
    max = std::max(max, meta.percentile[i]);
  return CastPq(static_cast<double>(max) / 10.0);
}

uint16_t AveragePq(const Meta& meta)
{
  if (meta.numWindows == 0)
    return 0;
  // HistogramPlus: a mean over the percentiles, weighted by their spans
  if (meta.numPercentiles == 9 && meta.percentage[1] == 5 && meta.percentage[2] == 10)
  {
    auto pq = [&](int i) { return NitsToPq(static_cast<double>(meta.percentile[i]) / 10.0); };
    const double pq1 = pq(0), pq2 = pq(3), pq3 = pq(4), pq4 = pq(5), pq5 = pq(6), pq6 = pq(7), pq7 = pq(8);
    const double mean = (pq1 + pq2) / 2.0 * 0.2400 + (pq2 + pq3) / 2.0 * 0.2500 + (pq3 + pq4) / 2.0 * 0.2500 +
                        (pq4 + pq5) / 2.0 * 0.1500 + (pq5 + pq6) / 2.0 * 0.0500 + (pq6 + pq7) / 2.0 * 0.0498;
    return static_cast<uint16_t>(std::round(mean * 4095.0));
  }
  return CastPq(static_cast<double>(meta.averageMaxrgb) / 10.0);
}

void PutBits(uint8_t* p, int pos, int n, uint32_t v)
{
  for (int i = 0; i < n; i++, pos++)
  {
    const uint8_t bit = (v >> (n - 1 - i)) & 1;
    const uint8_t mask = 0x80 >> (pos & 7);
    p[pos >> 3] = bit ? (p[pos >> 3] | mask) : (p[pos >> 3] & ~mask);
  }
}

// CRC-32/MPEG-2
uint32_t Crc32(const uint8_t* p, size_t size)
{
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < size; i++)
  {
    crc ^= uint32_t(p[i]) << 24;
    for (int b = 0; b < 8; b++)
      crc = crc & 0x80000000 ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
  }
  return crc;
}

} // namespace

bool Values::operator==(const Values& o) const
{
  return sourceMinPq == o.sourceMinPq && sourceMaxPq == o.sourceMaxPq && minPq == o.minPq && maxPq == o.maxPq &&
         avgPq == o.avgPq && mdlMax == o.mdlMax && mdlMin == o.mdlMin && maxCll == o.maxCll && maxFall == o.maxFall;
}

bool ParseSei(const uint8_t* nal, size_t size, Static& st, Meta& meta)
{
  if (size < 3 || ((nal[0] >> 1) & 0x3f) != 39)
    return false;
  const std::vector<uint8_t> rbsp = Unescape(nal + 2, size - 2);
  const size_t n = rbsp.size();
  size_t pos = 0;
  bool found = false;
  while (pos < n)
  {
    // rbsp_trailing_bits
    if (pos + 1 == n && rbsp[pos] == 0x80)
      break;
    uint32_t type = 0, len = 0;
    while (pos < n && rbsp[pos] == 0xFF)
      type += 255, pos++;
    if (pos >= n)
      break;
    type += rbsp[pos++];
    while (pos < n && rbsp[pos] == 0xFF)
      len += 255, pos++;
    if (pos >= n)
      break;
    len += rbsp[pos++];
    if (pos + len > n)
      break;
    const uint8_t* p = rbsp.data() + pos;
    if (type == 137 && len >= 24) // mastering_display_colour_volume
    {
      const uint32_t maxRaw = (uint32_t(p[16]) << 24) | (uint32_t(p[17]) << 16) | (uint32_t(p[18]) << 8) | p[19];
      const uint32_t minRaw = (uint32_t(p[20]) << 24) | (uint32_t(p[21]) << 16) | (uint32_t(p[22]) << 8) | p[23];
      st.hasMdcv = true;
      st.maxLum = maxRaw / 10000;
      st.minLum = minRaw;
    }
    else if (type == 144 && len >= 4) // content_light_level_info
    {
      st.hasCll = true;
      st.maxCll = uint16_t((p[0] << 8) | p[1]);
      st.maxFall = uint16_t((p[2] << 8) | p[3]);
    }
    else if (type == 4 && ParseT35(p, len, meta))
    {
      found = true;
    }
    pos += len;
  }
  return found;
}

Values Convert(const Meta& meta, const Static& st)
{
  uint16_t minPq;
  if (st.minLum == 0)
    minPq = 0;
  else if (st.minLum < 2)
    minPq = 7;
  else if (st.minLum < 5)
    minPq = 10;
  else if (st.minLum < 10)
    minPq = 17;
  else if (st.minLum < 20)
    minPq = 26;
  else if (st.minLum < 50)
    minPq = 38;
  else
    minPq = 62;

  uint16_t sourceMaxPq;
  switch (st.maxLum)
  {
    case 0:
    case 1000: sourceMaxPq = 3079; break;
    case 2000: sourceMaxPq = 3388; break;
    case 4000: sourceMaxPq = 3696; break;
    case 10000: sourceMaxPq = 4095; break;
    default: sourceMaxPq = CastPq(st.maxLum); break;
  }

  uint16_t maxPq = MaximumPq(meta);
  if (maxPq == 0)
  {
    switch (st.maxLum)
    {
      case 2000: maxPq = 3388; break;
      case 4000: maxPq = 3696; break;
      case 10000: maxPq = 4095; break;
      default: maxPq = 3079; break;
    }
  }
  const uint16_t avgPq = AveragePq(meta);

  Values v{};
  v.sourceMinPq = minPq;
  v.sourceMaxPq = sourceMaxPq;
  v.minPq = minPq;
  // the scene peak no higher than the source peak (Kodi: an L1 max above
  // source_max_pq breaks the tone mapping, a red screen on TV-led displays)
  v.maxPq = Clamp16(maxPq, L1_MAX_PQ_MIN_VALUE, sourceMaxPq);
  v.avgPq = Clamp16(avgPq, L1_AVG_PQ_MIN_VALUE, v.maxPq - 1);
  // level 6 as libdovi accepts it (Kodi's generation fails above 10000)
  v.mdlMax = uint16_t(std::min<uint32_t>(st.maxLum, MAX_PQ_LUMINANCE));
  v.mdlMin = uint16_t(std::min<uint32_t>(st.minLum, MAX_PQ_LUMINANCE));
  v.maxCll = std::min(st.maxCll, MAX_PQ_LUMINANCE);
  v.maxFall = std::min(st.maxFall, MAX_PQ_LUMINANCE);

  // libdovi's generator clamps level 1 (CM v4.0)
  v.minPq = std::min(v.minPq, L1_MIN_PQ_MAX_VALUE);
  v.maxPq = std::clamp(v.maxPq, L1_MAX_PQ_MIN_VALUE, L1_MAX_PQ_MAX_VALUE);
  v.avgPq = std::clamp(v.avgPq, L1_AVG_PQ_MIN_VALUE_CMV40, uint16_t(v.maxPq - 1));
  return v;
}

std::vector<uint8_t> WriteRpu(const Values& v)
{
  uint8_t rbsp[sizeof(kRpuTemplate) + 5];
  std::copy(kRpuTemplate, kRpuTemplate + sizeof(kRpuTemplate), rbsp);
  PutBits(rbsp, BIT_SOURCE_MIN_PQ, 12, v.sourceMinPq);
  PutBits(rbsp, BIT_SOURCE_MAX_PQ, 12, v.sourceMaxPq);
  PutBits(rbsp, BIT_L1_MIN, 12, v.minPq);
  PutBits(rbsp, BIT_L1_MAX, 12, v.maxPq);
  PutBits(rbsp, BIT_L1_AVG, 12, v.avgPq);
  PutBits(rbsp, BIT_L6_MDL_MAX, 16, v.mdlMax);
  PutBits(rbsp, BIT_L6_MDL_MIN, 16, v.mdlMin);
  PutBits(rbsp, BIT_L6_MAX_CLL, 16, v.maxCll);
  PutBits(rbsp, BIT_L6_MAX_FALL, 16, v.maxFall);
  // rpu_data_crc32 over the RPU after its 0x19 prefix, then the stop bit byte
  const uint32_t crc = Crc32(rbsp + 1, sizeof(kRpuTemplate) - 1);
  size_t n = sizeof(kRpuTemplate);
  rbsp[n++] = uint8_t(crc >> 24);
  rbsp[n++] = uint8_t(crc >> 16);
  rbsp[n++] = uint8_t(crc >> 8);
  rbsp[n++] = uint8_t(crc);
  rbsp[n++] = 0x80;

  // emulation prevention exactly as libdovi's writer (from the 4th byte)
  std::vector<uint8_t> out;
  out.reserve(n + 16);
  out.push_back(0x7C); // NAL unit type 62
  out.push_back(0x01);
  for (size_t i = 0; i < n; i++)
  {
    const size_t k = out.size() - 2; // index in the escaped RPU
    if (k > 2 && out[out.size() - 2] == 0 && out[out.size() - 1] == 0 && rbsp[i] <= 3)
      out.push_back(3);
    out.push_back(rbsp[i]);
  }
  return out;
}

} // namespace hdr10plus

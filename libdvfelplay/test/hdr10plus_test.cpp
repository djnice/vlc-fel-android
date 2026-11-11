/*
 *  Copyright (C) 2026 djnice <djnice@freemail.hu>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// host test of src/hdr10plus.cpp:
//   g++ -std=c++17 -O1 -Isrc test/hdr10plus_test.cpp src/hdr10plus.cpp -o /tmp/h10t
//   /tmp/h10t test/hdr10plus_vectors.txt [stream.hevc]
// vectors: "smin smax mdl_max mdl_min cll fall l1min l1max l1avg <dovi_tool RPU hex>" per line (the
// RPU as dovi_tool generate writes it: escaped, after the start code, without the NAL header); an
// Annex-B HEVC stream: its HDR10+ SEIs converted, the values of the first ones printed.
#include "hdr10plus.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace hdr10plus;

static int TestVectors(const char* path)
{
  std::ifstream f(path);
  std::string line;
  int n = 0, bad = 0;
  while (std::getline(f, line))
  {
    std::istringstream s(line);
    unsigned a[9];
    std::string hex;
    for (auto& x : a)
      s >> x;
    s >> hex;
    if (hex.empty())
      continue;
    Values v{uint16_t(a[0]), uint16_t(a[1]), uint16_t(a[6]), uint16_t(a[7]), uint16_t(a[8]),
             uint16_t(a[2]), uint16_t(a[3]), uint16_t(a[4]), uint16_t(a[5])};
    const std::vector<uint8_t> rpu = WriteRpu(v);
    std::string got;
    char b[3];
    for (size_t i = 2; i < rpu.size(); i++)
    {
      snprintf(b, sizeof(b), "%02x", rpu[i]);
      got += b;
    }
    n++;
    if (got != hex)
    {
      bad++;
      printf("MISMATCH line %d\n  want %s\n  got  %s\n", n, hex.c_str(), got.c_str());
    }
  }
  printf("vectors: %d, mismatches %d\n", n, bad);
  return bad || !n;
}

static int TestStream(const char* path)
{
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Static st;
  int seis = 0, printed = 0;
  size_t i = 0;
  while (i + 3 < d.size())
  {
    if (!(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1))
    {
      i++;
      continue;
    }
    const size_t start = i + 3;
    size_t j = start;
    while (j + 3 < d.size() && !(d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1))
      j++;
    size_t end = j + 3 < d.size() ? j : d.size();
    while (end > start && d[end - 1] == 0)
      end--;
    Meta meta;
    if (ParseSei(d.data() + start, end - start, st, meta))
    {
      seis++;
      const Values v = Convert(meta, st);
      if (printed++ < 8)
        printf("HDR10+ #%d: windows %d maxscl %u %u %u avg %u pct %d [%u..%u] -> src %u..%u L1 %u %u %u L6 %u %u %u %u\n",
               seis, meta.numWindows, meta.maxscl[0], meta.maxscl[1], meta.maxscl[2], meta.averageMaxrgb,
               meta.numPercentiles, meta.percentile[0], meta.percentile[meta.numPercentiles ? meta.numPercentiles - 1 : 0],
               v.sourceMinPq, v.sourceMaxPq, v.minPq, v.maxPq, v.avgPq, v.mdlMax, v.mdlMin, v.maxCll, v.maxFall);
    }
    i = j;
  }
  printf("stream: %d HDR10+ SEIs, mdcv %d (%u / %u) cll %d (%u / %u)\n", seis, st.hasMdcv, st.maxLum, st.minLum,
         st.hasCll, st.maxCll, st.maxFall);
  return seis == 0;
}

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: %s vectors.txt [stream.hevc]\n", argv[0]);
    return 2;
  }
  int r = TestVectors(argv[1]);
  if (argc > 2)
    r |= TestStream(argv[2]);
  return r;
}

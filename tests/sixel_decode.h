// A small Sixel decoder for tests: handles the parts vtpen writes.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "vtpen.h"

namespace sixeltest {

using vtpen::Rgb;

struct Decoded {
  int w = 0, h = 0;
  std::vector<Rgb> pix;
  std::vector<char> drawn;
  bool transparent = false;
  bool ok = false;
};

// Decode one picture that starts at the DCS at position `at` of s (which must
// be "ESC P"). Returns the picture; `end` is set just after its ST.
inline Decoded decode_at(const std::string& s, size_t at, size_t* end = nullptr) {
  Decoded d;
  size_t i = at;
  if (s.compare(i, 2, "\x1bP") != 0) return d;
  const size_t q = s.find('q', i);
  if (q == std::string::npos) return d;
  d.transparent = s.compare(i, q - i + 1, "\x1bP0;1;0q") == 0;
  i = q + 1;
  auto number = [&](size_t& k) {
    int v = 0;
    while (k < s.size() && isdigit(static_cast<unsigned char>(s[k]))) v = v * 10 + (s[k++] - '0');
    return v;
  };
  if (i >= s.size() || s[i] != '"') return d;
  i++;
  number(i); i++; number(i); i++;
  d.w = number(i); i++;
  d.h = number(i);
  d.pix.assign(static_cast<size_t>(d.w) * d.h, Rgb{});
  d.drawn.assign(d.pix.size(), 0);
  std::map<int, Rgb> pal;
  int cur = 0, x = 0, y = 0;
  auto put = [&](char ch, int times) {
    for (int t = 0; t < times; t++, x++)
      for (int b = 0; b < 6; b++)
        if ((ch - 63) >> b & 1 && x < d.w && y + b < d.h) {
          d.pix[static_cast<size_t>(y + b) * d.w + x] = pal[cur];
          d.drawn[static_cast<size_t>(y + b) * d.w + x] = 1;
        }
  };
  while (i < s.size()) {
    char c = s[i];
    if (c == '#') {
      i++;
      cur = number(i);
      if (i < s.size() && s[i] == ';') {
        i++; number(i); i++;
        int r = number(i); i++;
        int g = number(i); i++;
        int b = number(i);
        pal[cur] = {static_cast<uint8_t>(r * 255 / 100), static_cast<uint8_t>(g * 255 / 100), static_cast<uint8_t>(b * 255 / 100)};
      }
    } else if (c == '!') {
      i++;
      int n = number(i);
      put(s[i++], n);
    } else if (c == '$') { x = 0; i++; }
    else if (c == '-') { x = 0; y += 6; i++; }
    else if (c == '\x1b') { i += 2; break; }
    else if (c >= 63 && c <= 126) { put(c, 1); i++; }
    else i++;
  }
  d.ok = true;
  if (end) *end = i;
  return d;
}

inline Decoded decode(const std::string& s) { return decode_at(s, 0); }

inline int dist(Rgb a, Rgb b) {
  return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

}  // namespace sixeltest

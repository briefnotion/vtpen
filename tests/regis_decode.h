// A small ReGIS interpreter for tests: it understands only what vtpen_regis.h writes
// (ESC P 1 p ... ESC \, S(E), S(M..), W(..), P, V, C, C(A-n), F(V(B)..(E)), F(C..)),
// draws it on an 800x480 grid of colour numbers, and sets ok = false at anything else
// or at a position off the screen. It is a reading of the DEC manual, so it checks the
// emitter against that reading, not against a real terminal.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace registest {

struct Screen {
  static constexpr int W = 800, H = 480;
  std::vector<uint8_t> px = std::vector<uint8_t>(static_cast<size_t>(W) * H, 0);  // 0 = background
  std::array<std::array<int, 3>, 16> hls{};   // hue, lightness, saturation as sent
  std::array<bool, 16> defined{};
  long long cx = 0, cy = 0;
  bool erase = false;
  int colour = 1;
  bool ok = true;
  int dcs = 0;            // how many complete ESC P 1 p ... ESC \ were read
  std::string error;

  uint8_t at(int x, int y) const { return x < 0 || y < 0 || x >= W || y >= H ? 0 : px[static_cast<size_t>(y) * W + x]; }
  int ink() const { int n = 0; for (uint8_t p : px) n += p != 0; return n; }

  void set(long long x, long long y) {
    if (x < 0 || y < 0 || x >= W || y >= H) { fail("pixel off the screen"); return; }
    px[static_cast<size_t>(y) * W + x] = erase ? 0 : static_cast<uint8_t>(colour);
  }
  void fail(const std::string& why) { if (ok) error = why; ok = false; }

  void line(long long x0, long long y0, long long x1, long long y1) {
    const long long dx = std::llabs(x1 - x0), dy = -std::llabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    long long err = dx + dy;
    for (;;) {
      set(x0, y0);
      if (x0 == x1 && y0 == y1) break;
      const long long e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  void fill_polygon(const std::vector<std::pair<long long, long long>>& v) {
    long long ymin = v[0].second, ymax = v[0].second;
    for (const auto& p : v) { ymin = std::min(ymin, p.second); ymax = std::max(ymax, p.second); }
    for (long long y = ymin; y <= ymax; y++) {
      std::vector<double> xs;
      for (size_t i = 0; i < v.size(); i++) {
        const auto a = v[i], b = v[(i + 1) % v.size()];
        if ((a.second <= y && b.second > y) || (b.second <= y && a.second > y))
          xs.push_back(a.first + static_cast<double>(y - a.second) * (b.first - a.first) / (b.second - a.second));
      }
      std::sort(xs.begin(), xs.end());
      for (size_t i = 0; i + 1 < xs.size(); i += 2)
        for (long long x = static_cast<long long>(std::ceil(xs[i] - 1e-9)); x <= static_cast<long long>(std::floor(xs[i + 1] + 1e-9)); x++) set(x, y);
    }
    for (size_t i = 0; i < v.size(); i++) line(v[i].first, v[i].second, v[(i + 1) % v.size()].first, v[(i + 1) % v.size()].second);
  }
};

class Reader {
 public:
  Reader(const std::string& s, Screen& scr) : s_(s), scr_(scr) {}

  void run() {
    size_t i = 0;
    while (i < s_.size()) {
      if (s_.compare(i, 4, "\x1bP1p") != 0) { scr_.fail("not a ReGIS start"); return; }
      const size_t end = s_.find("\x1b\\", i);
      if (end == std::string::npos) { scr_.fail("no ST"); return; }
      body(i + 4, end);
      if (!scr_.ok) return;
      scr_.dcs++;
      i = end + 2;
    }
  }

 private:
  const std::string& s_;
  Screen& scr_;
  size_t i_ = 0, end_ = 0;

  bool more() const { return i_ < end_; }
  char peek() const { return i_ < end_ ? s_[i_] : '\0'; }
  void skip() { while (more() && (s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == ' ')) i_++; }
  bool eat(char c) { skip(); if (peek() == c) { i_++; return true; } return false; }
  void need(char c) { if (!eat(c)) scr_.fail(std::string("expected ") + c + " at " + std::to_string(i_)); }
  long long number() {
    skip();
    bool neg = false;
    if (peek() == '-') { neg = true; i_++; }
    if (!isdigit(static_cast<unsigned char>(peek()))) { scr_.fail("number expected"); return 0; }
    long long v = 0;
    while (isdigit(static_cast<unsigned char>(peek()))) { v = v * 10 + (s_[i_++] - '0'); if (v > 1000000000) v = 1000000000; }
    return neg ? -v : v;
  }
  // [x,y] absolute, on the screen; or [] (returns false)
  bool position(long long& x, long long& y) {
    need('[');
    if (eat(']')) return false;
    x = number(); need(','); y = number(); need(']');
    if (scr_.ok && (x < 0 || y < 0 || x >= Screen::W || y >= Screen::H)) scr_.fail("position off the screen: " + std::to_string(x) + "," + std::to_string(y));
    return true;
  }

  void body(size_t from, size_t to) {
    i_ = from; end_ = to;
    while (scr_.ok) {
      skip();
      if (!more()) return;
      const char c = s_[i_++];
      long long x = 0, y = 0;
      switch (c) {
        case 'P':
          while (peek() == '[') { if (position(x, y)) { scr_.cx = x; scr_.cy = y; } }
          break;
        case 'V':
          while (scr_.ok && peek() == '[') {
            if (position(x, y)) { scr_.line(scr_.cx, scr_.cy, x, y); scr_.cx = x; scr_.cy = y; }
            else scr_.set(scr_.cx, scr_.cy);
          }
          break;
        case 'C': circle(); break;
        case 'F': fill(); break;
        case 'W': write(); break;
        case 'S': screen(); break;
        default: scr_.fail(std::string("unknown command ") + c);
      }
    }
  }

  void circle() {
    long long sweep = 360;
    bool arc = false;
    if (eat('(')) { need('A'); sweep = -number(); arc = true; need(')'); }
    long long x, y;
    if (!position(x, y)) { scr_.fail("circle without a point"); return; }
    const double r = std::hypot(static_cast<double>(x - scr_.cx), static_cast<double>(y - scr_.cy));
    const long long ri = std::llround(r);
    const double a0 = std::atan2(static_cast<double>(y - scr_.cy), static_cast<double>(x - scr_.cx));
    if (!arc) sweep = 360;
    long long px = x, py = y;
    for (long long k = 1; k <= sweep && scr_.ok; k++) {
      const double a = a0 + k * 3.14159265358979 / 180;
      const long long nx = scr_.cx + std::llround(ri * std::cos(a)), ny = scr_.cy + std::llround(ri * std::sin(a));
      scr_.line(px, py, nx, ny);
      px = nx; py = ny;
    }
    scr_.set(x, y);
  }

  void fill() {
    need('(');
    skip();
    if (eat('C')) {
      long long x, y;
      if (!position(x, y)) { scr_.fail("fill circle without a point"); return; }
      const long long r = std::llround(std::hypot(static_cast<double>(x - scr_.cx), static_cast<double>(y - scr_.cy)));
      for (long long dy = -r; dy <= r; dy++)
        for (long long dx = -r; dx <= r; dx++)
          if (dx * dx + dy * dy <= r * r + r) { if (scr_.cx + dx >= 0 && scr_.cx + dx < Screen::W && scr_.cy + dy >= 0 && scr_.cy + dy < Screen::H) scr_.set(scr_.cx + dx, scr_.cy + dy); else scr_.fail("fill off the screen"); }
      need(')');
      return;
    }
    need('V'); need('('); need('B'); need(')');
    std::vector<std::pair<long long, long long>> v{{scr_.cx, scr_.cy}};
    long long x, y;
    while (scr_.ok && peek() == '[') { if (position(x, y)) v.push_back({x, y}); }
    need('('); need('E'); need(')'); need(')');
    if (!scr_.ok) return;
    if (v.size() < 3) { scr_.fail("fill needs three corners"); return; }
    if (v.size() > 256) { scr_.fail("fill has more than 256 corners"); return; }
    scr_.fill_polygon(v);
  }

  void write() {
    need('(');
    while (scr_.ok && !eat(')')) {
      skip();
      const char o = peek();
      i_++;
      if (o == 'V') scr_.erase = false;
      else if (o == 'E') scr_.erase = true;
      else if (o == 'P') number();
      else if (o == 'I') { scr_.colour = static_cast<int>(number()); if (scr_.colour < 1 || scr_.colour > 15 || !scr_.defined[static_cast<size_t>(scr_.colour)]) scr_.fail("colour not in the map"); }
      else scr_.fail(std::string("unknown write option ") + o);
      eat(',');
    }
  }

  void screen() {
    need('(');
    if (eat('E')) { std::fill(scr_.px.begin(), scr_.px.end(), static_cast<uint8_t>(0)); need(')'); return; }
    need('M');
    while (scr_.ok && peek() != ')') {
      const long long n = number();
      if (n < 1 || n > 15) { scr_.fail("map entry out of range"); return; }
      need('('); need('A'); need('H');
      const long long h = number(); need('L'); const long long l = number(); need('S'); const long long s = number(); need(')');
      if (h < 0 || h > 359 || l < 0 || l > 100 || s < 0 || s > 100) scr_.fail("HLS out of range");
      scr_.hls[static_cast<size_t>(n)] = {static_cast<int>(h), static_cast<int>(l), static_cast<int>(s)};
      scr_.defined[static_cast<size_t>(n)] = true;
      skip();
    }
    need(')');
  }
};

inline Screen read(const std::string& s) {
  Screen scr;
  Reader(s, scr).run();
  return scr;
}
inline void read_into(Screen& scr, const std::string& s) { Reader(s, scr).run(); }

}  // namespace registest

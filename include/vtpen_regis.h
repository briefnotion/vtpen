// vtpen ReGIS (experimental: not yet run on a real terminal): show a Drawing on a
// terminal that understands ReGIS (DEC's vector graphics language: VT125, VT240,
// VT340 and emulators such as xterm built with it).
//
//   vtpen::Drawing d(800, 480);
//   d.set_colour({255, 200, 0});
//   d.polyline({{100, 100}, {400, 380}, {700, 100}});
//
//   vtpen::Regis regis;                    // the whole 800x480 ReGIS screen
//   write(fd, regis.draw(d));              // or: regis.frame(d) for an animation
//
// The strings returned are complete (they start with ESC P 1 p and end with ESC \),
// so they can be written at any time. ReGIS has a screen of its own that is not tied
// to the text cells and does not scroll with the text.
//
// What it does with a Drawing:
//   - Lines and outlines are 1 pixel wide. Thicker ones (1.5 and up) are drawn as
//     filled strips, with round discs at the bends and ends of lines 3 or more pixels wide.
//   - Circles and arcs that are round and fit on the screen are sent as ReGIS circles
//     and arcs; ellipses, curves and everything partly off the screen become lines.
//   - Fills are ReGIS polygon fills. ReGIS cannot fill a shape that crosses itself or
//     has more than 256 corners, so those are cut into horizontal strips first. The
//     result is the even-odd fill Canvas does.
//   - Colours: ReGIS shows a few at a time. The palette (up to 15 colours, set in
//     RegisOptions or taken from the first drawing) goes into the terminal's colour
//     map 1 to 15; every colour is drawn as the nearest palette colour. Entry 0 is the
//     background and is left alone.
//   - Nothing is drawn outside the rectangle the drawing was given on the screen, and
//     there is no smoothing.
//
// Header only, no terminal calls.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "vtpen_drawing.h"

namespace vtpen {

struct RegisOptions {
  // Colours to use, at most 15. Empty: taken from the first drawing (the most used
  // colours) and then kept, because a changed colour map recolours what is already there.
  std::vector<Rgb> palette;
  // Where on the ReGIS screen (0..799 across, 0..479 down, top left is 0,0) the
  // drawing goes. The drawing's whole space is stretched to this rectangle.
  int x = 0, y = 0, width = 800, height = 480;
  // Lines and curves are cut down to fewer points: a point that lies within this many
  // pixels of the straight line between its neighbours is left out, which hardly shows
  // and makes the text much shorter. 0 keeps every point.
  double simplify = 0.5;
};

class Regis {
 public:
  static constexpr int SCREEN_W = 800, SCREEN_H = 480;
  static constexpr size_t MAX_COLOURS = 15;
  static constexpr size_t MAX_FILL_CORNERS = 256;

  explicit Regis(RegisOptions options = RegisOptions()) : opt_(std::move(options)) {
    opt_.x = std::clamp(opt_.x, 0, SCREEN_W - 1);
    opt_.y = std::clamp(opt_.y, 0, SCREEN_H - 1);
    opt_.width = std::clamp(opt_.width, 1, SCREEN_W - opt_.x);
    opt_.height = std::clamp(opt_.height, 1, SCREEN_H - opt_.y);
    if (opt_.palette.size() > MAX_COLOURS) opt_.palette.resize(MAX_COLOURS);
    if (!(opt_.simplify > 0) || !std::isfinite(opt_.simplify)) opt_.simplify = 0;
    palette_ = opt_.palette;
    frozen_ = !palette_.empty();
  }

  // Draw the shapes on top of what is on the screen.
  std::string draw(const Drawing& d) {
    std::string out = "\x1bP1p";
    define_palette(d, out);
    out += "W(V,P1)\n";
    body(d, false, out);
    return out + "\x1b\\";
  }

  // Take the shapes away again: draw them in the background colour. Anything else
  // that crossed them loses the pixels they shared.
  std::string erase(const Drawing& d) const {
    std::string out = "\x1bP1pW(E,P1)\n";
    body(d, true, out);
    return out + "\x1b\\";
  }

  // For animation: take away what the last call drew, then draw this one, in one
  // piece. The first call only draws.
  std::string frame(const Drawing& next) {
    std::string out = "\x1bP1p";
    define_palette(next, out);
    if (have_previous_) {
      out += "W(E,P1)\n";
      body(previous_, true, out);
    }
    out += "W(V,P1)\n";
    body(next, false, out);
    previous_ = next;
    have_previous_ = true;
    return out + "\x1b\\";
  }

  // Wipe the ReGIS screen. Forgets the drawing frame() remembers.
  std::string clear() {
    have_previous_ = false;
    previous_.clear();
    return "\x1bP1pS(E)\x1b\\";
  }

  // The terminal was reset or is a different one: send the colour map again and forget the last frame.
  void reset() {
    colours_sent_ = false;
    have_previous_ = false;
    previous_.clear();
  }

  // The colours in use (register i + 1 holds palette()[i]); empty until the first drawing.
  const std::vector<Rgb>& palette() const { return palette_; }

 private:
  struct IPt {
    long long x, y;
    bool operator==(const IPt& o) const { return x == o.x && y == o.y; }
  };

  static constexpr double LIMIT = 1e9;

  RegisOptions opt_;
  std::vector<Rgb> palette_;
  bool frozen_ = false, colours_sent_ = false, have_previous_ = false;
  Drawing previous_;

  // ---- the colour map ----------------------------------------------------------------

  void define_palette(const Drawing& d, std::string& out) {
    if (!frozen_ && !d.commands_.empty()) {
      std::vector<std::pair<uint32_t, int>> seen;  // colour, how many shapes
      for (const Drawing::Command& c : d.commands_) {
        const uint32_t k = static_cast<uint32_t>(c.colour.r) << 16 | static_cast<uint32_t>(c.colour.g) << 8 | c.colour.b;
        auto it = std::find_if(seen.begin(), seen.end(), [&](const std::pair<uint32_t, int>& e) { return e.first == k; });
        if (it == seen.end()) seen.push_back({k, 1}); else it->second++;
      }
      std::stable_sort(seen.begin(), seen.end(), [](const std::pair<uint32_t, int>& a, const std::pair<uint32_t, int>& b) { return a.second > b.second; });
      if (seen.size() > MAX_COLOURS) seen.resize(MAX_COLOURS);
      for (const auto& e : seen)
        palette_.push_back({static_cast<uint8_t>(e.first >> 16), static_cast<uint8_t>(e.first >> 8), static_cast<uint8_t>(e.first)});
      frozen_ = true;
    }
    if (colours_sent_ || palette_.empty()) return;
    out += "S(M";
    for (size_t i = 0; i < palette_.size(); i++) {
      int h, l, s;
      hls(palette_[i], h, l, s);
      out += std::to_string(i + 1) + "(AH" + std::to_string(h) + "L" + std::to_string(l) + "S" + std::to_string(s) + ")";
    }
    out += ")\n";
    colours_sent_ = true;
  }

  // ReGIS hue: blue is 0, red 120, green 240 (the usual wheel turned by 120 degrees).
  static void hls(Rgb c, int& h, int& l, int& s) {
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn, light = (mx + mn) / 2;
    double sat = 0, hue = 0;
    if (d > 0) {
      sat = light > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
      if (mx == r) hue = (g - b) / d + (g < b ? 6 : 0);
      else if (mx == g) hue = (b - r) / d + 2;
      else hue = (r - g) / d + 4;
      hue *= 60;
    }
    h = static_cast<int>(std::lround(hue + 120)) % 360;
    l = static_cast<int>(std::lround(light * 100));
    s = static_cast<int>(std::lround(sat * 100));
  }

  int nearest(Rgb c) const {
    int best = 0;
    long long best_d = -1;
    for (size_t i = 0; i < palette_.size(); i++) {
      const long long dr = c.r - palette_[i].r, dg = c.g - palette_[i].g, db = c.b - palette_[i].b;
      const long long dist = dr * dr + dg * dg + db * db;
      if (best_d < 0 || dist < best_d) { best_d = dist; best = static_cast<int>(i); }
    }
    return best + 1;
  }

  // ---- the shapes ----------------------------------------------------------------------

  void body(const Drawing& d, bool erasing, std::string& out) const {
    const double kx = static_cast<double>(opt_.width) / d.w_, ky = static_cast<double>(opt_.height) / d.h_;
    auto map = [&](Point p) { return Point{opt_.x + p.x * kx, opt_.y + p.y * ky}; };
    auto mapped = [&](const std::vector<Point>& pts) {  // not-a-number points are dropped, huge ones pulled in
      std::vector<Point> r;
      r.reserve(pts.size());
      for (const Point& p : pts) {
        const Point q = map(p);
        if (std::isfinite(q.x) && std::isfinite(q.y)) r.push_back({std::clamp(q.x, -LIMIT, LIMIT), std::clamp(q.y, -LIMIT, LIMIT)});
      }
      return r;
    };
    int current = -1;
    for (const Drawing::Command& c : d.commands_) {
      if (!erasing && !palette_.empty()) {
        const int idx = nearest(c.colour);
        if (idx != current) { out += "W(I" + std::to_string(idx) + ")\n"; current = idx; }
      }
      const double t = std::isfinite(c.thickness) ? c.thickness : 1.0;
      switch (c.kind) {
        case Drawing::Kind::Polyline: polyline(mapped(c.pts), t, out); break;
        case Drawing::Kind::Polygon: {
          std::vector<Point> p = mapped(c.pts);
          if (!p.empty()) p.push_back(p[0]);
          polyline(p, t, out);
          break;
        }
        case Drawing::Kind::FillPolygon: fill(mapped(c.pts), out); break;
        case Drawing::Kind::Arc: {
          const std::vector<Point> m = mapped({c.pts[0]});
          if (m.empty() || !std::isfinite(c.rx) || !std::isfinite(c.ry) || !std::isfinite(c.a0) || !std::isfinite(c.a1)) break;
          arc(m[0].x, m[0].y, std::min(std::fabs(c.rx * kx), LIMIT), std::min(std::fabs(c.ry * ky), LIMIT), c.a0, c.a1, t, out);
          break;
        }
        case Drawing::Kind::FillEllipse: {
          const std::vector<Point> m = mapped({c.pts[0]});
          if (m.empty() || !std::isfinite(c.rx) || !std::isfinite(c.ry)) break;
          fill_ellipse(m[0].x, m[0].y, std::min(std::fabs(c.rx * kx), LIMIT), std::min(std::fabs(c.ry * ky), LIMIT), out);
          break;
        }
        case Drawing::Kind::Bezier: {
          const std::vector<Point> p = mapped(c.pts);
          if (p.size() != 4) break;  // a corner was not a number
          std::vector<Point> path{p[0]};
          append_bezier_path(path, p[0], p[1], p[2], p[3]);
          polyline(path, t, out);
          break;
        }
        case Drawing::Kind::Spline: polyline(spline_path(mapped(c.pts)), t, out); break;
      }
    }
  }

  // The rectangle that may be drawn in, as pixel numbers.
  double x0() const { return opt_.x; }
  double y0() const { return opt_.y; }
  double x1() const { return opt_.x + opt_.width - 1; }
  double y1() const { return opt_.y + opt_.height - 1; }

  static long long rnd(double v) { return static_cast<long long>(std::floor(std::clamp(v, -LIMIT, LIMIT) + 0.5)); }
  IPt pixel(Point p) const {
    return {std::clamp(rnd(p.x), static_cast<long long>(x0()), static_cast<long long>(x1())),
            std::clamp(rnd(p.y), static_cast<long long>(y0()), static_cast<long long>(y1()))};
  }
  static std::string at(IPt p) { return "[" + std::to_string(p.x) + "," + std::to_string(p.y) + "]"; }
  bool inside(Point p) const { return p.x >= x0() && p.x <= x1() && p.y >= y0() && p.y <= y1(); }

  // A circle of whole-pixel radius r around (cx, cy) lies wholly on the drawable rectangle.
  bool circle_fits(long long cx, long long cy, long long r) const {
    return cx - r >= static_cast<long long>(x0()) && cx + r <= static_cast<long long>(x1()) &&
           cy - r >= static_cast<long long>(y0()) && cy + r <= static_cast<long long>(y1());
  }

  // Cut the segment a-b down to the part inside the rectangle (Liang-Barsky). False if none is.
  bool clip_segment(Point& a, Point& b) const {
    double t0 = 0, t1 = 1;
    const double dx = b.x - a.x, dy = b.y - a.y;
    auto clip = [&](double p, double q) {
      if (p == 0) return q >= 0;
      const double r = q / p;
      if (p < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
      else { if (r < t0) return false; if (r < t1) t1 = r; }
      return true;
    };
    if (!clip(-dx, a.x - x0()) || !clip(dx, x1() - a.x) || !clip(-dy, a.y - y0()) || !clip(dy, y1() - a.y)) return false;
    const Point na{a.x + t0 * dx, a.y + t0 * dy}, nb{a.x + t1 * dx, a.y + t1 * dy};
    a = na;
    b = nb;
    return true;
  }

  // ---- lines ---------------------------------------------------------------------------------

  void polyline(const std::vector<Point>& points, double thickness, std::string& out) const {
    if (points.empty()) return;
    const std::vector<Point> p = simplified(points);
    if (thickness >= 1.5) thick_line(p, thickness, out);
    else thin_line(p, out);
  }

  // Douglas-Peucker: keep the ends, and any point that is further than opt_.simplify from the
  // line between the points kept around it.
  std::vector<Point> simplified(const std::vector<Point>& p) const {
    if (opt_.simplify <= 0 || p.size() < 3) return p;
    auto gap = [](Point q, Point a, Point b) {  // how far q is from the segment a-b
      const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
      const double t = len2 > 0 ? std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / len2, 0.0, 1.0) : 0;
      return std::hypot(q.x - (a.x + t * dx), q.y - (a.y + t * dy));
    };
    std::vector<char> keep(p.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> todo{{0, p.size() - 1}};
    while (!todo.empty()) {
      const auto range = todo.back();
      todo.pop_back();
      double far = -1;
      size_t at = range.first;
      for (size_t i = range.first + 1; i < range.second; i++) {
        const double d = gap(p[i], p[range.first], p[range.second]);
        if (d > far) { far = d; at = i; }
      }
      if (far > opt_.simplify) {
        keep[at] = 1;
        todo.push_back({range.first, at});
        todo.push_back({at, range.second});
      }
    }
    std::vector<Point> r;
    for (size_t i = 0; i < p.size(); i++) if (keep[i]) r.push_back(p[i]);
    return r;
  }

  // 1 pixel wide: P to the start, then V through the points. A line that leaves the
  // rectangle and comes back starts a new P.
  void thin_line(const std::vector<Point>& p, std::string& out) const {
    if (p.size() == 1) {
      if (inside(p[0])) out += "P" + at(pixel(p[0])) + "V[]\n";
      return;
    }
    std::vector<IPt> run;
    auto flush = [&]() {
      if (run.empty()) return;
      out += "P" + at(run[0]) + "V";
      for (size_t i = 1; i < run.size(); i++) out += at(run[i]);
      out += "\n";
      run.clear();
    };
    for (size_t i = 0; i + 1 < p.size(); i++) {
      Point a = p[i], b = p[i + 1];
      if (!clip_segment(a, b)) { flush(); continue; }
      const IPt ia = pixel(a), ib = pixel(b);
      if (!run.empty() && run.back() == ia) run.push_back(ib);
      else { flush(); run = {ia, ib}; }
    }
    flush();
  }

  // Wider: every piece of the line is a filled strip, and bends of wide lines get a disc.
  void thick_line(const std::vector<Point>& p, double thickness, std::string& out) const {
    const double h = thickness / 2;
    bool drew = false;
    for (size_t i = 0; i + 1 < p.size(); i++) {
      const double dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y, len = std::hypot(dx, dy);
      if (!(len > 1e-9)) continue;
      const double nx = -dy / len * h, ny = dx / len * h;
      fill({{p[i].x + nx, p[i].y + ny}, {p[i + 1].x + nx, p[i + 1].y + ny}, {p[i + 1].x - nx, p[i + 1].y - ny}, {p[i].x - nx, p[i].y - ny}}, out);
      drew = true;
      if (h >= 1.5 && i + 2 < p.size()) {
        const double ex = p[i + 2].x - p[i + 1].x, ey = p[i + 2].y - p[i + 1].y, elen = std::hypot(ex, ey);
        if (elen > 1e-9 && std::fabs(dx * ey - dy * ex) / (len * elen) > 0.15) fill_ellipse(p[i + 1].x, p[i + 1].y, h, h, out);
      }
    }
    if (!drew) fill_ellipse(p[0].x, p[0].y, h, h, out);  // a dot
    else if (h >= 1.5) {                                  // round ends, as Canvas has
      fill_ellipse(p.front().x, p.front().y, h, h, out);
      fill_ellipse(p.back().x, p.back().y, h, h, out);
    }
  }

  // ---- circles and arcs ----------------------------------------------------------------------

  // Our angles grow clockwise on the screen from "right"; ReGIS counts degrees
  // anticlockwise, so a sweep of +90 for us is A-90 there.
  void arc(double cx, double cy, double rx, double ry, double a0, double a1, double thickness, std::string& out) const {
    const double sweep = a1 - a0, r = (rx + ry) / 2;
    const long long cxi = rnd(cx), cyi = rnd(cy), ri = rnd(r);
    if (thickness < 1.5 && std::fabs(rx - ry) <= 0.5 && ri >= 1 && sweep > 0 && circle_fits(cxi, cyi, ri)) {
      if (sweep >= 360) {
        out += "P" + at({cxi, cyi}) + "C" + at({cxi + ri, cyi}) + "\n";
      } else {
        const double a = a0 * 3.14159265358979 / 180;
        const IPt start{cxi + rnd(ri * std::cos(a)), cyi + rnd(ri * std::sin(a))};
        out += "P" + at({cxi, cyi}) + "C(A-" + std::to_string(std::clamp(rnd(sweep), 1LL, 359LL)) + ")" + at(start) + "\n";
      }
      return;
    }
    polyline(arc_path(cx, cy, rx, ry, a0, a1), thickness, out);
  }

  void fill_ellipse(double cx, double cy, double rx, double ry, std::string& out) const {
    const long long cxi = rnd(cx), cyi = rnd(cy), ri = rnd((rx + ry) / 2);
    if (std::fabs(rx - ry) <= 0.5 && ri >= 1 && circle_fits(cxi, cyi, ri)) {
      out += "P" + at({cxi, cyi}) + "F(C" + at({cxi + ri, cyi}) + ")\n";
      return;
    }
    fill(arc_path(cx, cy, rx, ry, 0, 360), out);
  }

  // ---- fills ----------------------------------------------------------------------------------

  // Cut a polygon to the rectangle (Sutherland-Hodgman).
  std::vector<Point> clip_polygon(std::vector<Point> poly) const {
    for (int edge = 0; edge < 4 && !poly.empty(); edge++) {
      auto in = [&](Point p) {
        switch (edge) {
          case 0: return p.x >= x0();
          case 1: return p.x <= x1();
          case 2: return p.y >= y0();
          default: return p.y <= y1();
        }
      };
      auto cut = [&](Point a, Point b) {
        const double v = edge == 0 ? x0() : edge == 1 ? x1() : edge == 2 ? y0() : y1();
        if (edge < 2) { const double t = (v - a.x) / (b.x - a.x); return Point{v, a.y + t * (b.y - a.y)}; }
        const double t = (v - a.y) / (b.y - a.y);
        return Point{a.x + t * (b.x - a.x), v};
      };
      std::vector<Point> next;
      for (size_t i = 0; i < poly.size(); i++) {
        const Point a = poly[i], b = poly[(i + 1) % poly.size()];
        const bool ia = in(a), ib = in(b);
        if (ia && ib) next.push_back(b);
        else if (ia && !ib) next.push_back(cut(a, b));
        else if (!ia && ib) { next.push_back(cut(a, b)); next.push_back(b); }
      }
      poly = std::move(next);
    }
    return poly;
  }

  void fill(const std::vector<Point>& poly, std::string& out) const {
    std::vector<IPt> v;
    for (const Point& p : clip_polygon(poly)) {
      const IPt q = pixel(p);
      if (v.empty() || !(v.back() == q)) v.push_back(q);
    }
    while (v.size() > 1 && v.front() == v.back()) v.pop_back();  // (the clipping may turn the start around)
    if (v.size() < 3 || area2(v) == 0) return;
    if (v.size() <= MAX_FILL_CORNERS && simple(v)) corner_fill(v, out);
    else strips(v, out);
  }

  static long long cross(IPt a, IPt b, IPt c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
  static long long area2(const std::vector<IPt>& v) {
    long long s = 0;
    for (size_t i = 0; i < v.size(); i++) s += v[i].x * v[(i + 1) % v.size()].y - v[(i + 1) % v.size()].x * v[i].y;
    return s;
  }

  static bool on_segment(IPt a, IPt b, IPt p) {  // p is known to be on the line a-b
    return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= p.y && p.y <= std::max(a.y, b.y);
  }
  static bool touch(IPt a, IPt b, IPt c, IPt d) {
    const long long d1 = cross(a, b, c), d2 = cross(a, b, d), d3 = cross(c, d, a), d4 = cross(c, d, b);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
    return (d1 == 0 && on_segment(a, b, c)) || (d2 == 0 && on_segment(a, b, d)) || (d3 == 0 && on_segment(c, d, a)) || (d4 == 0 && on_segment(c, d, b));
  }

  // No edge touches another except next to it at the shared corner. (An edge that doubles
  // back over the one before it leaves a corner of the next edge on it, which counts.)
  static bool simple(const std::vector<IPt>& v) {
    const size_t n = v.size();
    for (size_t i = 0; i < n; i++) {
      const IPt a = v[i], b = v[(i + 1) % n];
      for (size_t j = i + 2; j < n; j++) {
        if (i == 0 && j == n - 1) continue;  // the last edge is next to the first
        if (touch(a, b, v[j], v[(j + 1) % n])) return false;
      }
    }
    return true;
  }

  static void corner_fill(const std::vector<IPt>& v, std::string& out) {
    out += "P" + at(v[0]) + "F(V(B)";
    for (size_t i = 1; i < v.size(); i++) out += at(v[i]);
    out += "(E))\n";
  }

  // Even-odd fill of any polygon as a pile of four-cornered strips: cut at every
  // corner height and at every height where two edges cross; between two cuts the
  // edges keep their order, and every second gap between them is inside.
  void strips(const std::vector<IPt>& v, std::string& out) const {
    struct Edge { double xa, ya, xb, yb; };
    std::vector<Edge> edges;
    std::vector<double> ys;
    const size_t n = v.size();
    for (size_t i = 0; i < n; i++) {
      const IPt a = v[i], b = v[(i + 1) % n];
      ys.push_back(static_cast<double>(a.y));
      if (a.y != b.y) edges.push_back({static_cast<double>(a.x), static_cast<double>(a.y), static_cast<double>(b.x), static_cast<double>(b.y)});
    }
    for (size_t i = 0; i < edges.size(); i++)
      for (size_t j = i + 1; j < edges.size(); j++) {
        const Edge &p = edges[i], &q = edges[j];
        const double rx = p.xb - p.xa, ry = p.yb - p.ya, sx = q.xb - q.xa, sy = q.yb - q.ya, den = rx * sy - ry * sx;
        if (den == 0) continue;
        const double t = ((q.xa - p.xa) * sy - (q.ya - p.ya) * sx) / den, u = ((q.xa - p.xa) * ry - (q.ya - p.ya) * rx) / den;
        if (t > 0 && t < 1 && u > 0 && u < 1) ys.push_back(p.ya + t * ry);
      }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end(), [](double a, double b) { return b - a < 1e-9; }), ys.end());
    auto x_at = [](const Edge& e, double y) { return e.xa + (y - e.ya) * (e.xb - e.xa) / (e.yb - e.ya); };
    for (size_t k = 0; k + 1 < ys.size(); k++) {
      const double ya = ys[k], yb = ys[k + 1], ym = (ya + yb) / 2;
      std::vector<const Edge*> across;
      for (const Edge& e : edges)
        if (std::min(e.ya, e.yb) <= ya + 1e-9 && std::max(e.ya, e.yb) >= yb - 1e-9) across.push_back(&e);
      std::sort(across.begin(), across.end(), [&](const Edge* a, const Edge* b) { return x_at(*a, ym) < x_at(*b, ym); });
      for (size_t i = 0; i + 1 < across.size(); i += 2) {
        const Edge &l = *across[i], &r = *across[i + 1];
        std::vector<IPt> quad;
        for (const IPt& q : {IPt{rnd(x_at(l, ya)), rnd(ya)}, IPt{rnd(x_at(r, ya)), rnd(ya)}, IPt{rnd(x_at(r, yb)), rnd(yb)}, IPt{rnd(x_at(l, yb)), rnd(yb)}})
          if (quad.empty() || !(quad.back() == q)) quad.push_back(q);
        while (quad.size() > 1 && quad.front() == quad.back()) quad.pop_back();
        if (quad.size() >= 3 && area2(quad) != 0) corner_fill(quad, out);
      }
    }
  }
};

}  // namespace vtpen

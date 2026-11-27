// vtpen: draw shapes on a small pixel canvas and turn the canvas into a Sixel
// picture a terminal can show. Header only, standard C++17, no terminal calls:
// the caller moves the cursor and writes sixel() out.
//
//   vtpen::Canvas c(700, 420, 800, 480);   // 700x420 pixels, drawn in 800x480 units
//   c.set_colour({255, 200, 0});
//   c.set_thickness(3);                      // in pixels
//   c.polyline({{100, 100}, {400, 380}, {700, 100}});
//   c.fill_circle(400, 240, 60);             // fills can use a Pattern (set_fill_pattern)
//   write(fd, c.sixel());
//
// Shapes: lines, polylines, rectangles, polygons, circles, ellipses, arcs, Bezier
// curves, splines, and filled polygons/rectangles/circles/ellipses. Edges are
// smoothed in a few steps (set_edge_levels). A picture can use at most 255
// colours; with more, the 255 most used are kept and the others get the nearest
// one. Numbers that are not usable (NaN, infinity) are skipped, never crash.
// See ../README.md, and vtpen_scene.h for movable objects and sending only changes.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <string>
#include <vector>

namespace vtpen {

struct Point {
  double x, y;
};

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const Rgb& o) const { return !(*this == o); }
};

// ---- Curve helpers (shared with the scene) ----------------------------------------
// These turn arcs and curves into points. px and py are how many pixels one
// drawing unit is across and down, so a curve gets as many pieces as it needs
// to look round at its final size.

inline int curve_steps(double radius_pixels, double sweep_radians) {
  if (!std::isfinite(radius_pixels) || !std::isfinite(sweep_radians)) return 1;
  const double r = std::max(radius_pixels, 0.5);
  const double per_step = std::max(2 * std::acos(std::max(-1.0, 1 - 0.1 / r)), 1e-3);
  const double pi = 3.14159265358979;
  return static_cast<int>(std::clamp(std::ceil(std::fabs(sweep_radians) / std::min(per_step, pi / 6)), 1.0, 4000.0));
}

inline std::vector<Point> arc_path(double cx, double cy, double rx, double ry, double start_deg, double end_deg,
                                   double px = 1, double py = 1) {
  const double pi = 3.14159265358979;
  const double a0 = start_deg * pi / 180, a1 = end_deg * pi / 180;
  int n = curve_steps(std::max(std::fabs(rx) * px, std::fabs(ry) * py), a1 - a0);
  if (std::fabs(end_deg - start_deg) >= 360 && n % 2 == 1) n++;  // a full circle comes out mirror symmetric
  std::vector<Point> pts;
  for (int i = 0; i <= n; i++) {
    const double a = a0 + (a1 - a0) * i / n;
    pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
  }
  return pts;
}

// Adds the points of a cubic Bezier curve from p0 (not repeated) to p1.
inline void append_bezier_path(std::vector<Point>& pts, Point p0, Point c0, Point c1, Point p1, double px = 1, double py = 1) {
  const double len = std::hypot((c0.x - p0.x) * px, (c0.y - p0.y) * py) + std::hypot((c1.x - c0.x) * px, (c1.y - c0.y) * py) +
                     std::hypot((p1.x - c1.x) * px, (p1.y - c1.y) * py);
  const int n = std::isfinite(len) ? static_cast<int>(std::clamp(std::ceil(len / 3), 4.0, 500.0)) : 4;
  for (int i = 1; i <= n; i++) {
    const double t = static_cast<double>(i) / n, u = 1 - t;
    const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    pts.push_back({b0 * p0.x + b1 * c0.x + b2 * c1.x + b3 * p1.x, b0 * p0.y + b1 * c0.y + b2 * c1.y + b3 * p1.y});
  }
}

// A smooth path through every point (Catmull-Rom).
inline std::vector<Point> spline_path(const std::vector<Point>& points, double px = 1, double py = 1) {
  if (points.size() < 3) return points;
  std::vector<Point> pts{points[0]};
  const size_t n = points.size();
  for (size_t i = 0; i + 1 < n; i++) {
    const Point a = points[i == 0 ? 0 : i - 1], b = points[i], c = points[i + 1], d = points[std::min(i + 2, n - 1)];
    append_bezier_path(pts, b, {b.x + (c.x - a.x) / 6, b.y + (c.y - a.y) / 6},
                       {c.x - (d.x - b.x) / 6, c.y - (d.y - b.y) / 6}, c, px, py);
  }
  return pts;
}

// ---- Pattern: a small tile that fills shapes -------------------------------------------
//
// A pattern is made once and can fill any number of shapes. It is fixed to the
// picture, like wallpaper behind the shapes: pixel (x, y) of the canvas always
// gets the pattern's pixel (x mod width, y mod height), so neighbouring shapes
// line up and a shape that turns or moves does not drag the pattern with it.
// Each pixel of a pattern has a strength from 0 (nothing: what is below shows)
// to 255 (the full colour).

class Pattern {
 public:
  Pattern(int width, int height)
      : w_(std::max(1, width)), h_(std::max(1, height)), alpha_(static_cast<size_t>(w_) * h_, 0) {}

  int width() const { return w_; }
  int height() const { return h_; }

  void set(int x, int y, uint8_t strength) {
    if (x >= 0 && y >= 0 && x < w_ && y < h_) alpha_[static_cast<size_t>(y) * w_ + x] = strength;
  }

  // The strength at any canvas pixel; the tile repeats in every direction.
  uint8_t at(long long x, long long y) const {
    const long long px = ((x % w_) + w_) % w_, py = ((y % h_) + h_) % h_;
    return alpha_[static_cast<size_t>(py) * w_ + static_cast<size_t>(px)];
  }

  // Every pixel.
  static Pattern solid() { return Pattern(1, 1).filled(); }

  // Lines across: one row in every `spacing`, `thickness` rows thick.
  static Pattern horizontal(int spacing = 4, int thickness = 1) {
    Pattern p(1, std::max(1, spacing));
    for (int y = 0; y < p.h_; y++) p.alpha_[static_cast<size_t>(y)] = y < std::max(1, thickness) ? 255 : 0;
    return p;
  }

  // Lines up and down.
  static Pattern vertical(int spacing = 4, int thickness = 1) {
    Pattern p(std::max(1, spacing), 1);
    for (int x = 0; x < p.w_; x++) p.alpha_[static_cast<size_t>(x)] = x < std::max(1, thickness) ? 255 : 0;
    return p;
  }

  // Slanted lines: "/" when forward is true, "\" when not.
  static Pattern diagonal(int spacing = 6, int thickness = 1, bool forward = true) {
    const int n = std::max(1, spacing);
    Pattern p(n, n);
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++) {
        const int d = forward ? (x + y) % n : ((x - y) % n + n) % n;
        p.alpha_[static_cast<size_t>(y) * n + x] = d < std::max(1, thickness) ? 255 : 0;
      }
    return p;
  }

  // Lines both ways: "+" when upright, "x" when not.
  static Pattern crosshatch(int spacing = 6, int thickness = 1, bool upright = true) {
    const int n = std::max(1, spacing);
    Pattern p(n, n);
    const int t = std::max(1, thickness);
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++) {
        const bool on = upright ? (x < t || y < t) : ((x + y) % n < t || ((x - y) % n + n) % n < t);
        p.alpha_[static_cast<size_t>(y) * n + x] = on ? 255 : 0;
      }
    return p;
  }

  // Dots, `size` pixels across, one in every `spacing` pixels each way.
  static Pattern dots(int spacing = 4, int size = 1) {
    const int n = std::max(1, spacing);
    Pattern p(n, n);
    for (int y = 0; y < std::min(n, std::max(1, size)); y++)
      for (int x = 0; x < std::min(n, std::max(1, size)); x++) p.alpha_[static_cast<size_t>(y) * n + x] = 255;
    return p;
  }

  // A chessboard of squares `cell` pixels wide.
  static Pattern checker(int cell = 2) {
    const int c = std::max(1, cell);
    Pattern p(2 * c, 2 * c);
    for (int y = 0; y < 2 * c; y++)
      for (int x = 0; x < 2 * c; x++) p.alpha_[static_cast<size_t>(y) * 2 * c + x] = ((x / c + y / c) % 2 == 0) ? 255 : 0;
    return p;
  }

  // A grey level from 0 (nothing) to 16 (solid), made of single dots spread evenly
  // (a 4x4 ordered dither).
  static Pattern gray(int level) {
    static const int order[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const int l = std::clamp(level, 0, 16);
    Pattern p(4, 4);
    for (int i = 0; i < 16; i++) p.alpha_[static_cast<size_t>(i)] = order[i] < l ? 255 : 0;
    return p;
  }

  // From text: one string per row, the character `on` is a pixel.
  static Pattern from_rows(const std::vector<std::string>& rows, char on = 'X') {
    size_t width = 1;
    for (const std::string& r : rows) width = std::max(width, r.size());
    Pattern p(static_cast<int>(width), static_cast<int>(std::max<size_t>(1, rows.size())));
    for (size_t y = 0; y < rows.size(); y++)
      for (size_t x = 0; x < rows[y].size(); x++) p.alpha_[y * static_cast<size_t>(p.w_) + x] = rows[y][x] == on ? 255 : 0;
    return p;
  }

 private:
  Pattern& filled() {
    std::fill(alpha_.begin(), alpha_.end(), 255);
    return *this;
  }

  int w_, h_;
  std::vector<uint8_t> alpha_;
};

class Canvas {
 public:
  // virt_w and virt_h are the size of the space you draw in; 0 means "same as
  // the pixel size".
  Canvas(int width, int height, double virt_w = 0, double virt_h = 0)
      : w_(std::max(1, width)), h_(std::max(1, height)),
        sx_(virt_w > 0 ? w_ / virt_w : 1.0), sy_(virt_h > 0 ? h_ / virt_h : 1.0),
        pix_(static_cast<size_t>(w_) * h_) {}

  int width() const { return w_; }
  int height() const { return h_; }

  void set_colour(Rgb c) { colour_ = c; }
  // Not a number counts as 1; more than 4096 is cut to 4096.
  void set_thickness(double pixels) { thickness_ = std::isfinite(pixels) ? std::clamp(pixels, 0.0, 4096.0) : 1.0; }

  // What fills are made of. Null (the default) is solid colour. Lines and
  // outlines are always solid.
  void set_fill_pattern(std::shared_ptr<const Pattern> pattern) { pattern_ = std::move(pattern); }
  const std::shared_ptr<const Pattern>& fill_pattern() const { return pattern_; }

  // How many steps the soft edge of a line has. 1 = hard edges (smallest
  // picture, stair-steps on slanted lines), 4 = the default (looks nearly as
  // smooth as it gets, about 40% smaller), 0 = every shade (largest).
  void set_edge_levels(int levels) { edge_levels_ = std::max(0, levels); }

  // Background colour; clear() fills with it. When transparent (the default)
  // pixels that still have the background colour are not sent, so the
  // terminal's own background shows through.
  void set_background(Rgb c) { background_ = c; }
  void set_transparent(bool t) { transparent_ = t; }
  void clear() { std::fill(pix_.begin(), pix_.end(), background_); }

  // The pixels of row y, left to right (width() of them).
  const Rgb* row(int y) const { return &pix_[static_cast<size_t>(y) * w_]; }
  Rgb background() const { return background_; }
  bool transparent() const { return transparent_; }
  double pixels_per_unit_x() const { return sx_; }
  double pixels_per_unit_y() const { return sy_; }

  Rgb pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return background_;
    return pix_[static_cast<size_t>(y) * w_ + x];
  }

  void line(Point a, Point b) { polyline({a, b}); }

  // Points are joined in order. One point draws a dot. Joins are round. Points
  // that are not numbers (NaN, infinity) are skipped, and ones beyond a billion
  // pixels are pulled in to that, which is far outside any canvas.
  void polyline(const std::vector<Point>& points) {
    const std::vector<Point> p = to_pixels(points);
    if (p.empty()) return;

    // Coverage goes in one mask for the whole line (the strongest value wins),
    // so a joint is not blended twice. The mask is kept between calls and only
    // the pixels touched are visited and cleared.
    ensure_scratch();
    const double half = std::max(thickness_, 1.0) / 2.0;
    const double reach = half + 1;
    // Long segments are cut up so a slanted one only looks at pixels near it,
    // not at its whole bounding box; thick ones in longer pieces.
    const double piece = std::max(12.0, thickness_);
    for (size_t i = 0; i < p.size(); i++) {
      Point a = p[i], b = p[std::min(i + 1, p.size() - 1)];
      // Only the part of the segment that can touch the canvas matters.
      if (!clip_to_box(a, b, -reach, -reach, w_ + reach, h_ + reach)) continue;
      const double len = std::hypot(b.x - a.x, b.y - a.y);
      const int pieces = std::max(1, static_cast<int>(std::ceil(len / piece)));
      for (int k = 0; k < pieces; k++) {
        const Point pa{a.x + (b.x - a.x) * k / pieces, a.y + (b.y - a.y) * k / pieces};
        const Point pb{a.x + (b.x - a.x) * (k + 1) / pieces, a.y + (b.y - a.y) * (k + 1) / pieces};
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min(pa.x, pb.x) - reach)));
        const int x1 = std::min(w_ - 1, static_cast<int>(std::ceil(std::max(pa.x, pb.x) + reach)));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min(pa.y, pb.y) - reach)));
        const int y1 = std::min(h_ - 1, static_cast<int>(std::ceil(std::max(pa.y, pb.y) + reach)));
        for (int y = y0; y <= y1; y++) {
          for (int x = x0; x <= x1; x++) {
            const double d = distance_to_segment(x + 0.5, y + 0.5, pa, pb);
            const float cov = static_cast<float>(std::clamp(half + 0.5 - d, 0.0, 1.0));
            if (cov <= 0.0f) continue;
            add_coverage(static_cast<size_t>(y) * w_ + x, cov);
          }
        }
      }
    }
    flush_mask();
  }

  // ---- Shapes -------------------------------------------------------------------
  // Coordinates are in the drawing space given to the constructor. Angles are in
  // degrees; they grow clockwise on the screen, and 0 points right. Outlines use
  // the current colour and thickness; fills use the current colour.

  void polygon(const std::vector<Point>& points) {  // closed outline
    if (points.empty()) return;
    std::vector<Point> p = points;
    p.push_back(points[0]);
    polyline(p);
  }

  void rectangle(double x, double y, double w, double h) {
    polygon({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}});
  }

  void circle(double cx, double cy, double r) { ellipse(cx, cy, r, r); }

  void ellipse(double cx, double cy, double rx, double ry) { polyline(arc_path(cx, cy, rx, ry, 0, 360, sx_, sy_)); }

  void arc(double cx, double cy, double r, double start_deg, double end_deg) {
    arc(cx, cy, r, r, start_deg, end_deg);
  }

  void arc(double cx, double cy, double rx, double ry, double start_deg, double end_deg) {
    polyline(arc_path(cx, cy, rx, ry, start_deg, end_deg, sx_, sy_));
  }

  // A cubic Bezier curve from p0 to p1, pulled towards c0 and c1.
  void bezier(Point p0, Point c0, Point c1, Point p1) {
    std::vector<Point> pts{p0};
    append_bezier_path(pts, p0, c0, c1, p1, sx_, sy_);
    polyline(pts);
  }

  // A smooth curve that passes through every point (Catmull-Rom).
  void spline(const std::vector<Point>& points) { polyline(spline_path(points, sx_, sy_)); }

  // Filled shapes. A polygon is filled by the even-odd rule: a region the
  // outline crosses twice is left empty (the centre of a pentagram, for example).
  void fill_polygon(const std::vector<Point>& points) {
    const std::vector<Point> p = to_pixels(points);
    if (p.size() < 3) return;
    double ymin = p[0].y, ymax = p[0].y;
    for (const Point& q : p) { ymin = std::min(ymin, q.y); ymax = std::max(ymax, q.y); }
    const int y0 = std::max(0, static_cast<int>(std::floor(ymin)));
    const int y1 = std::min(h_ - 1, static_cast<int>(std::ceil(ymax)));
    if (y0 > y1) return;
    ensure_scratch();
    constexpr int SUB = 4;  // sub-rows per pixel row; across a row the coverage is exact
    std::vector<double> xs;
    for (int y = y0; y <= y1; y++) {
      int lo = w_, hi = -1;
      for (int sub = 0; sub < SUB; sub++) {
        const double ys = y + (sub + 0.5) / SUB;
        xs.clear();
        for (size_t i = 0; i < p.size(); i++) {
          const Point a = p[i], b = p[(i + 1) % p.size()];
          if ((a.y <= ys && b.y > ys) || (b.y <= ys && a.y > ys))
            xs.push_back(a.x + (ys - a.y) / (b.y - a.y) * (b.x - a.x));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
          const double xl = std::max(0.0, xs[k]), xr = std::min(static_cast<double>(w_), xs[k + 1]);
          if (xr <= xl) continue;
          const int i0 = static_cast<int>(std::floor(xl)), i1 = static_cast<int>(std::ceil(xr)) - 1;
          for (int x = i0; x <= i1; x++)
            row_[static_cast<size_t>(x)] += static_cast<float>((std::min(xr, x + 1.0) - std::max(xl, static_cast<double>(x))) / SUB);
          lo = std::min(lo, i0);
          hi = std::max(hi, i1);
        }
      }
      for (int x = lo; x <= hi; x++) {
        const float cov = std::min(1.0f, row_[static_cast<size_t>(x)]);
        row_[static_cast<size_t>(x)] = 0.0f;
        if (cov > 0.0f) add_coverage(static_cast<size_t>(y) * w_ + x, cov);
      }
    }
    flush_mask(true);
  }

  void fill_rectangle(double x, double y, double w, double h) {
    fill_polygon({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}});
  }

  void fill_circle(double cx, double cy, double r) { fill_ellipse(cx, cy, r, r); }

  void fill_ellipse(double cx, double cy, double rx, double ry) { fill_polygon(arc_path(cx, cy, rx, ry, 0, 360, sx_, sy_)); }

  // The whole picture as a Sixel string (DCS ... ST).
  std::string sixel() const { return encode(pix_, w_, h_, background_, transparent_); }

  // Just a rectangle of the picture (clipped to the canvas) as its own Sixel
  // picture, to be drawn with the cursor at the rectangle's top left cell.
  std::string sixel(int x, int y, int w, int h) const {
    int rx, ry, rw, rh;
    if (!clip(x, y, w, h, rx, ry, rw, rh))
      return encode(std::vector<Rgb>(1, background_), 1, 1, background_, transparent_);
    std::vector<Rgb> sub(static_cast<size_t>(rw) * rh);
    for (int j = 0; j < rh; j++)
      for (int i = 0; i < rw; i++) sub[static_cast<size_t>(j) * rw + i] = pix_[static_cast<size_t>(ry + j) * w_ + rx + i];
    return encode(sub, rw, rh, background_, transparent_);
  }

  // True when nothing would be drawn in the rectangle.
  bool empty(int x, int y, int w, int h) const {
    int rx, ry, rw, rh;
    if (!clip(x, y, w, h, rx, ry, rw, rh)) return true;
    for (int j = 0; j < rh; j++)
      for (int i = 0; i < rw; i++) {
        const Rgb& p = pix_[static_cast<size_t>(ry + j) * w_ + rx + i];
        if (!(transparent_ && p == background_)) return false;
      }
    return true;
  }

 private:
  static std::string encode(const std::vector<Rgb>& pix, int w, int h, Rgb bg, bool transparent) {
    // 1. Palette. Exact when there are at most 255 colours, else rounded.
    std::set<uint32_t> seen;
    uint32_t previous = 0xffffffffu;  // runs of one colour are the usual case: look once
    for (const Rgb& c : pix) {
      if (transparent && c == bg) continue;
      const uint32_t k = key(c);
      if (k == previous) continue;
      previous = k;
      seen.insert(k);
      if (seen.size() > 255) break;
    }
    const bool exact = seen.size() <= 255;
    std::vector<Rgb> palette;
    std::vector<int> index(pix.size(), -1);  // -1: not drawn
    if (exact) {
      std::vector<uint32_t> keys(seen.begin(), seen.end());
      for (uint32_t k : keys) palette.push_back({static_cast<uint8_t>(k >> 16), static_cast<uint8_t>(k >> 8), static_cast<uint8_t>(k)});
      uint32_t last_key = 0xffffffffu;
      int last_index = -1;
      for (size_t i = 0; i < pix.size(); i++) {
        if (transparent && pix[i] == bg) continue;
        const uint32_t k = key(pix[i]);
        if (k != last_key) {
          last_key = k;
          last_index = static_cast<int>(std::lower_bound(keys.begin(), keys.end(), k) - keys.begin());
        }
        index[i] = last_index;
      }
    } else {
      // Too many colours. Keep the 255 most used (grouped in bins of 8 levels
      // per channel, each shown by the average of what fell in it) and give
      // every other pixel the nearest of those. Smooth edges keep their soft shades.
      struct Bin {
        uint32_t n = 0, r = 0, g = 0, b = 0;
      };
      std::vector<Bin> bins(32768);
      auto bin_of = [](const Rgb& c) { return (c.r >> 3) << 10 | (c.g >> 3) << 5 | (c.b >> 3); };
      for (size_t i = 0; i < pix.size(); i++) {
        const Rgb& c = pix[i];
        if (transparent && c == bg) continue;
        Bin& bn = bins[static_cast<size_t>(bin_of(c))];
        bn.n++; bn.r += c.r; bn.g += c.g; bn.b += c.b;
      }
      std::vector<int> used;
      for (int k = 0; k < 32768; k++) if (bins[static_cast<size_t>(k)].n) used.push_back(k);
      std::stable_sort(used.begin(), used.end(), [&](int x, int y) { return bins[static_cast<size_t>(x)].n > bins[static_cast<size_t>(y)].n; });
      const size_t keep = std::min<size_t>(255, used.size());
      auto average = [&](int k) {
        const Bin& bn = bins[static_cast<size_t>(k)];
        return Rgb{static_cast<uint8_t>(bn.r / bn.n), static_cast<uint8_t>(bn.g / bn.n), static_cast<uint8_t>(bn.b / bn.n)};
      };
      std::vector<int> pal_of_bin(32768, -1);
      for (size_t i = 0; i < keep; i++) {
        palette.push_back(average(used[i]));
        pal_of_bin[static_cast<size_t>(used[i])] = static_cast<int>(i);
      }
      for (size_t i = keep; i < used.size(); i++) {  // the rest: nearest kept colour
        const Rgb c = average(used[i]);
        int best = 0, best_d = 1 << 30;
        for (size_t j = 0; j < keep; j++) {
          const int dr = c.r - palette[j].r, dg = c.g - palette[j].g, db = c.b - palette[j].b;
          const int d = dr * dr + dg * dg + db * db;
          if (d < best_d) { best_d = d; best = static_cast<int>(j); }
        }
        pal_of_bin[static_cast<size_t>(used[i])] = best;
      }
      for (size_t i = 0; i < pix.size(); i++) {
        const Rgb& c = pix[i];
        if (transparent && c == bg) continue;
        index[i] = pal_of_bin[static_cast<size_t>(bin_of(c))];
      }
    }

    // 2. Header and palette (Sixel colours are percentages).
    std::string s = transparent ? "\x1bP0;1;0q" : "\x1bP0;0;0q";
    s += "\"1;1;" + std::to_string(w) + ";" + std::to_string(h);
    for (size_t i = 0; i < palette.size(); i++)
      s += "#" + std::to_string(i) + ";2;" + std::to_string(pct(palette[i].r)) + ";" +
           std::to_string(pct(palette[i].g)) + ";" + std::to_string(pct(palette[i].b));

    // 3. Bands of six pixel rows, one pass per colour used in the band. Each
    //    colour's row is kept between bands (all '?', meaning "no pixels"), and
    //    only the stretch from its first to its last pixel is visited and cleared.
    std::vector<std::string> rows(palette.size());
    std::vector<char> used(palette.size(), 0);
    std::vector<int> first_x(palette.size(), 0), last_x(palette.size(), 0);
    for (int top = 0; top < h; top += 6) {
      std::vector<size_t> order;
      for (int b = 0; b < 6 && top + b < h; b++) {
        const int* row_index = &index[static_cast<size_t>(top + b) * w];
        for (int x = 0; x < w; x++) {
          const int c = row_index[x];
          if (c < 0) continue;
          if (!used[c]) {
            used[c] = 1;
            if (rows[c].empty()) rows[c].assign(static_cast<size_t>(w), '?');
            first_x[c] = last_x[c] = x;
            order.push_back(static_cast<size_t>(c));
          } else {
            if (x < first_x[c]) first_x[c] = x;
            if (x > last_x[c]) last_x[c] = x;
          }
          rows[c][x] = static_cast<char>(rows[c][x] + (1 << b));
        }
      }
      std::sort(order.begin(), order.end());
      bool first = true;
      for (size_t c : order) {
        std::string& row = rows[c];
        const size_t lo = static_cast<size_t>(first_x[c]), hi = static_cast<size_t>(last_x[c]) + 1;
        if (!first) s += "$";  // back to the left edge of the same band
        first = false;
        s += "#" + std::to_string(c);
        if (lo > 3) s += "!" + std::to_string(lo) + "?";  // empty stretch before the first pixel
        else s.append(lo, '?');
        for (size_t i = lo; i < hi;) {
          size_t j = i;
          while (j < hi && row[j] == row[i]) j++;
          if (j - i > 3) s += "!" + std::to_string(j - i) + row[i];
          else s.append(row, i, j - i);
          i = j;
        }
        std::fill(row.begin() + static_cast<std::ptrdiff_t>(lo), row.begin() + static_cast<std::ptrdiff_t>(hi), '?');
        used[c] = 0;
      }
      if (top + 6 < h) s += "-";  // next band
    }
    return s + "\x1b\\";
  }

  static constexpr double LIMIT = 1e9;   // pixels; beyond this a coordinate is pulled in

  // Points in drawing units -> pixels, dropping any that are not finite numbers.
  std::vector<Point> to_pixels(const std::vector<Point>& points) const {
    std::vector<Point> p;
    p.reserve(points.size());
    for (const Point& q : points) {
      const double x = q.x * sx_, y = q.y * sy_;
      if (std::isfinite(x) && std::isfinite(y)) p.push_back({std::clamp(x, -LIMIT, LIMIT), std::clamp(y, -LIMIT, LIMIT)});
    }
    return p;
  }

  // Cut the segment a-b down to the part inside the box (Liang-Barsky). False if none is.
  static bool clip_to_box(Point& a, Point& b, double x0, double y0, double x1, double y1) {
    double t0 = 0, t1 = 1;
    const double dx = b.x - a.x, dy = b.y - a.y;
    auto clip = [&](double p, double q) {
      if (p == 0) return q >= 0;
      const double r = q / p;
      if (p < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
      else { if (r < t0) return false; if (r < t1) t1 = r; }
      return true;
    };
    if (!clip(-dx, a.x - x0) || !clip(dx, x1 - a.x) || !clip(-dy, a.y - y0) || !clip(dy, y1 - a.y)) return false;
    const Point na{a.x + t0 * dx, a.y + t0 * dy}, nb{a.x + t1 * dx, a.y + t1 * dy};
    a = na;
    b = nb;
    return true;
  }

  void ensure_scratch() {
    if (mask_.size() != pix_.size()) mask_.assign(pix_.size(), 0.0f);
    if (row_.size() != static_cast<size_t>(w_)) row_.assign(static_cast<size_t>(w_), 0.0f);
    touched_.clear();
  }

  void add_coverage(size_t idx, float cov) {
    float& m = mask_[idx];
    if (m == 0.0f) touched_.push_back(idx);
    if (cov > m) m = cov;
  }

  // Blend the colour into every pixel in the mask, by its coverage, and clear the mask.
  // With `patterned` (fills), each pixel's strength also goes through the fill pattern.
  void flush_mask(bool patterned = false) {
    const Pattern* pat = patterned ? pattern_.get() : nullptr;
    for (size_t idx : touched_) {
      float m = mask_[idx];
      mask_[idx] = 0.0f;
      if (edge_levels_ > 0) m = std::round(m * edge_levels_) / edge_levels_;
      if (pat) m *= pat->at(static_cast<long long>(idx % static_cast<size_t>(w_)), static_cast<long long>(idx / static_cast<size_t>(w_))) / 255.0f;
      if (m <= 0.0f) continue;
      Rgb& d = pix_[idx];
      d.r = mix(d.r, colour_.r, m);
      d.g = mix(d.g, colour_.g, m);
      d.b = mix(d.b, colour_.b, m);
    }
    touched_.clear();
  }

  // Cut a rectangle down to the canvas. False when nothing is left.
  bool clip(int x, int y, int w, int h, int& rx, int& ry, int& rw, int& rh) const {
    const long long x0 = std::max<long long>(0, x), y0 = std::max<long long>(0, y);
    const long long x1 = std::min<long long>(w_, static_cast<long long>(x) + w), y1 = std::min<long long>(h_, static_cast<long long>(y) + h);
    if (x1 <= x0 || y1 <= y0) return false;
    rx = static_cast<int>(x0); ry = static_cast<int>(y0);
    rw = static_cast<int>(x1 - x0); rh = static_cast<int>(y1 - y0);
    return true;
  }

  static uint32_t key(const Rgb& c) { return (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b; }
  static int pct(int v) { return (v * 100 + 127) / 255; }
  static uint8_t mix(uint8_t under, uint8_t over, float a) {
    return static_cast<uint8_t>(std::lround(under + (over - under) * a));
  }
  static double distance_to_segment(double px, double py, Point a, Point b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double ex = a.x + t * dx - px, ey = a.y + t * dy - py;
    return std::sqrt(ex * ex + ey * ey);
  }

  int w_, h_;
  double sx_, sy_;
  std::vector<Rgb> pix_;
  Rgb colour_{255, 255, 255};
  Rgb background_{0, 0, 0};
  bool transparent_ = true;
  double thickness_ = 1.0;
  int edge_levels_ = 4;
  std::shared_ptr<const Pattern> pattern_;
  std::vector<float> mask_;        // scratch for polyline(): all zero between calls
  std::vector<size_t> touched_;    // the pixels the current shape has put coverage on
  std::vector<float> row_;         // scratch for fill_polygon(): all zero between calls
};

}  // namespace vtpen

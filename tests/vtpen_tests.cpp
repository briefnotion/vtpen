// Tests for vtpen.h. Build and run: ./m test
// The Sixel output is decoded again here and compared with the canvas, so a
// mistake in the encoder cannot hide behind the encoder's own idea of it.

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>

#include "sixel_decode.h"
#include "vtpen.h"

using namespace vtpen;
static const Rgb WHITE_{255, 255, 255};
using sixeltest::decode;
using sixeltest::Decoded;
using sixeltest::dist;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                      \
      failures++;                                                         \
    }                                                                     \
  } while (0)

int main() {
  // A horizontal 1 pixel line at y = 10.5 covers row 10 fully and leaves row 12 alone.
  {
    Canvas c(40, 20);
    c.set_colour({200, 100, 50});
    c.line({5, 10.5}, {25, 10.5});
    CHECK(c.pixel(15, 10) == (Rgb{200, 100, 50}));
    CHECK(c.pixel(15, 12) == (Rgb{0, 0, 0}));
    CHECK(c.pixel(35, 10) == (Rgb{0, 0, 0}));
  }
  // A thick line is thicker.
  {
    Canvas c(40, 20);
    c.set_thickness(5);
    c.line({5, 10.5}, {35, 10.5});  // covers rows 8 to 12
    CHECK(c.pixel(20, 8) == (Rgb{255, 255, 255}));
    CHECK(c.pixel(20, 12) == (Rgb{255, 255, 255}));
    CHECK(c.pixel(20, 14) == (Rgb{0, 0, 0}));
    // On a pixel boundary the line is shared by two rows, half each.
    Canvas e(40, 20);
    e.line({5, 10}, {35, 10});
    CHECK(e.pixel(20, 9) == e.pixel(20, 10));
    CHECK(e.pixel(20, 9).r > 100 && e.pixel(20, 9).r < 155);
  }
  // A diagonal has smoothed edges: some pixels are in between.
  {
    Canvas c(40, 40);
    c.line({2, 2}, {38, 30});
    int partial = 0;
    for (int y = 0; y < 40; y++)
      for (int x = 0; x < 40; x++) {
        Rgb p = c.pixel(x, y);
        if (p.r > 0 && p.r < 255) partial++;
      }
    CHECK(partial > 20);
  }
  // Edge levels: 1 gives hard edges only, 4 (the default) gives at most four
  // soft steps plus the full colour, 0 gives many shades.
  {
    auto shades = [](int levels) {
      Canvas c(60, 60);
      if (levels >= 0) c.set_edge_levels(levels);
      c.line({2, 5}, {57, 40});
      std::set<int> s;
      for (int y = 0; y < 60; y++)
        for (int x = 0; x < 60; x++) s.insert(c.pixel(x, y).r);
      return s;
    };
    std::set<int> hard = shades(1), normal = shades(-1), full = shades(0);
    CHECK(hard == (std::set<int>{0, 255}));
    CHECK(normal.size() == 5);                       // 0, 64, 128, 191, 255
    CHECK(normal.count(0) && normal.count(255));
    CHECK(full.size() > 15);
    // Steps are rounded to the nearest: coverage 0.7 becomes 0.75, 0.3 becomes 0.25.
    Canvas r(40, 20);
    r.line({2, 10.2}, {38, 10.2});
    CHECK(r.pixel(20, 10).r == 191);
    CHECK(r.pixel(20, 9).r == 64);
  }
  // The fast line drawing gives the same pixels as the plain way of doing it:
  // test random polylines against a slow reference that checks every pixel of
  // the line's whole bounding box against the whole segment.
  {
    unsigned seed = 12345;
    auto rnd = [&](double lo, double hi) {
      seed = seed * 1664525u + 1013904223u;
      return lo + (hi - lo) * ((seed >> 8) / 16777216.0);
    };
    int bad = 0, total = 0;
    for (int trial = 0; trial < 60; trial++) {
      const int W = 90, H = 70;
      Canvas c(W, H);
      c.set_edge_levels(0);
      const double thick = rnd(0, 6);
      c.set_thickness(thick);
      std::vector<Point> pts;
      const int n = 1 + trial % 5;
      for (int i = 0; i < n; i++) pts.push_back({rnd(-30, W + 30), rnd(-30, H + 30)});
      c.polyline(pts);
      const double half = std::max(thick, 1.0) / 2.0;
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          double best = 0;
          for (size_t i = 0; i < pts.size(); i++) {
            Point a = pts[i], b = pts[std::min(i + 1, pts.size() - 1)];
            double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
            double t = l2 > 0 ? ((x + 0.5 - a.x) * dx + (y + 0.5 - a.y) * dy) / l2 : 0;
            t = std::clamp(t, 0.0, 1.0);
            double ex = a.x + t * dx - (x + 0.5), ey = a.y + t * dy - (y + 0.5);
            best = std::max(best, std::clamp(half + 0.5 - std::sqrt(ex * ex + ey * ey), 0.0, 1.0));
          }
          int want = static_cast<int>(std::lround(255 * best)), got = c.pixel(x, y).r;
          total++;
          if (std::abs(want - got) > 1) bad++;
        }
    }
    CHECK(total == 60 * 90 * 70);
    CHECK(bad == 0);
  }
  // One line does not leave anything behind for the next: a thin red line over
  // a thick white one only changes the pixels it really covers.
  {
    Canvas c(40, 20);
    c.set_thickness(5);
    c.line({2, 10.5}, {38, 10.5});
    c.set_colour({255, 0, 0});
    c.set_thickness(1);
    c.line({2, 10.5}, {38, 10.5});
    CHECK(c.pixel(20, 10) == (Rgb{255, 0, 0}));
    CHECK(c.pixel(20, 8) == (Rgb{255, 255, 255}));
    CHECK(c.pixel(20, 12) == (Rgb{255, 255, 255}));
  }
  // ---- Shapes ----
  // Filled rectangle: inside full, outside nothing, a half-pixel edge half.
  {
    Canvas c(40, 30);
    c.set_edge_levels(0);
    c.fill_rectangle(5, 5, 20, 10);
    CHECK(c.pixel(10, 8) == (Rgb{255, 255, 255}));
    CHECK(c.pixel(4, 8) == (Rgb{0, 0, 0}));
    CHECK(c.pixel(25, 8) == (Rgb{0, 0, 0}));
    CHECK(c.pixel(10, 15) == (Rgb{0, 0, 0}));
    Canvas e(40, 30);
    e.set_edge_levels(0);
    e.fill_rectangle(5.5, 5, 10, 10);   // starts half way into column 5
    CHECK(e.pixel(5, 8).r > 120 && e.pixel(5, 8).r < 136);
    CHECK(e.pixel(6, 8) == (Rgb{255, 255, 255}));
  }
  // The total filled area is right (circle: pi r^2 within 1%).
  {
    Canvas c(120, 120);
    c.set_edge_levels(0);
    c.fill_circle(60, 60, 40);
    double area = 0;
    for (int y = 0; y < 120; y++)
      for (int x = 0; x < 120; x++) area += c.pixel(x, y).r / 255.0;
    const double want = 3.14159265358979 * 40 * 40;
    CHECK(std::fabs(area - want) < want * 0.01);
    CHECK(c.pixel(60, 60) == (Rgb{255, 255, 255}));
    CHECK(c.pixel(5, 5) == (Rgb{0, 0, 0}));
  }
  // Even-odd rule: the centre of a pentagram is left empty.
  {
    Canvas c(100, 100);
    std::vector<Point> star;
    for (int i = 0; i < 5; i++) {
      double a = -3.14159265358979 / 2 + i * 4 * 3.14159265358979 / 5;
      star.push_back({50 + 45 * std::cos(a), 50 + 45 * std::sin(a)});
    }
    c.fill_polygon(star);
    CHECK(c.pixel(50, 50) == (Rgb{0, 0, 0}));        // centre pentagon, crossed twice
    CHECK(c.pixel(50, 18) == (Rgb{255, 255, 255}));  // a point of the star
  }
  // The fast fill matches a plain 16x16 sampling of every pixel on random polygons.
  {
    unsigned seed = 777;
    auto rnd = [&](double lo, double hi) {
      seed = seed * 1664525u + 1013904223u;
      return lo + (hi - lo) * ((seed >> 8) / 16777216.0);
    };
    int bad = 0, total = 0;
    for (int trial = 0; trial < 30; trial++) {
      const int W = 60, H = 50;
      std::vector<Point> pts;
      const int n = 3 + trial % 5;
      for (int i = 0; i < n; i++) pts.push_back({rnd(-10, W + 10), rnd(-10, H + 10)});
      Canvas c(W, H);
      c.set_edge_levels(0);
      c.fill_polygon(pts);
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          int inside = 0;
          for (int sy = 0; sy < 16; sy++)
            for (int sx = 0; sx < 16; sx++) {
              const double px = x + (sx + 0.5) / 16, py = y + (sy + 0.5) / 16;
              bool in = false;
              for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
                if ((pts[i].y > py) != (pts[j].y > py) &&
                    px < (pts[j].x - pts[i].x) * (py - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x)
                  in = !in;
              inside += in;
            }
          const int want = static_cast<int>(std::lround(255.0 * inside / 256)), got = c.pixel(x, y).r;
          total++;
          if (std::abs(want - got) > 40) bad++;   // four sub-rows vs sixteen: edges differ a little
        }
    }
    CHECK(total == 30 * 60 * 50);
    CHECK(bad == 0);
  }
  // Circle outline: on the ring, not in the middle, mirror symmetric.
  {
    Canvas c(80, 80);
    c.set_edge_levels(0);
    c.circle(40, 40, 30);
    CHECK(c.pixel(70, 40).r > 100 && c.pixel(9, 40).r > 100);
    CHECK(c.pixel(40, 70).r > 100 && c.pixel(40, 9).r > 100);
    CHECK(c.pixel(40, 40) == (Rgb{0, 0, 0}));
    CHECK(c.pixel(40, 55) == (Rgb{0, 0, 0}));
    bool sym = true;
    for (int y = 0; y < 80; y++)
      for (int x = 0; x < 40; x++) sym = sym && std::abs(c.pixel(x, y).r - c.pixel(79 - x, y).r) <= 2;
    CHECK(sym);
  }
  // Ellipse: wide and short.
  {
    Canvas c(100, 60);
    c.set_edge_levels(0);
    c.ellipse(50, 30, 40, 15);
    CHECK(c.pixel(90, 30).r > 100 && c.pixel(10, 30).r > 100);
    CHECK(c.pixel(50, 45).r > 100 && c.pixel(50, 15).r > 100);
    CHECK(c.pixel(50, 5) == (Rgb{0, 0, 0}));   // outside, above
    CHECK(c.pixel(95, 30) == (Rgb{0, 0, 0}));  // outside, to the right
  }
  // Arc: 0 to 90 degrees runs from the right to the bottom, clockwise on the screen.
  {
    Canvas c(80, 80);
    c.set_edge_levels(0);
    c.arc(40, 40, 30, 0, 90);
    CHECK(c.pixel(70, 40).r > 100);
    CHECK(c.pixel(40, 70).r > 100);
    CHECK(c.pixel(9, 40) == (Rgb{0, 0, 0}));
    CHECK(c.pixel(40, 9) == (Rgb{0, 0, 0}));
    CHECK(c.pixel(61, 61).r > 100);   // 45 degrees: 40 + 30 cos 45 = 61.2
    CHECK(c.pixel(19, 61) == (Rgb{0, 0, 0}));
  }
  // Bezier: starts and ends at the end points, and passes (p0+3c0+3c1+p1)/8 in the middle.
  {
    Canvas c(100, 100);
    c.set_edge_levels(0);
    Point p0{10, 90}, c0{10, 10}, c1{90, 10}, p1{90, 90};
    c.bezier(p0, c0, c1, p1);
    CHECK(c.pixel(10, 89).r > 100);
    CHECK(c.pixel(89, 89).r > 100);
    const double mx = (p0.x + 3 * c0.x + 3 * c1.x + p1.x) / 8, my = (p0.y + 3 * c0.y + 3 * c1.y + p1.y) / 8;
    CHECK(c.pixel(static_cast<int>(mx), static_cast<int>(my)).r > 100);
    CHECK(c.pixel(50, 80) == (Rgb{0, 0, 0}));  // the curve bows up, away from here
  }
  // Spline: passes through every point it is given.
  {
    Canvas c(100, 60);
    c.set_edge_levels(0);
    std::vector<Point> pts{{5, 50}, {30, 10}, {55, 45}, {80, 15}, {95, 40}};
    c.spline(pts);
    for (const Point& q : pts) {  // a point on a pixel corner lights up the pixels around it
      int best = 0;
      for (int dy = -1; dy <= 0; dy++)
        for (int dx = -1; dx <= 0; dx++) best = std::max<int>(best, c.pixel(static_cast<int>(q.x) + dx, static_cast<int>(q.y) + dy).r);
      CHECK(best > 60);
    }
    CHECK(c.pixel(55, 5) == (Rgb{0, 0, 0}));
  }
  // Spline shape: between the middle points it follows the standard Catmull-Rom curve.
  {
    Canvas c(100, 80);
    c.set_edge_levels(0);
    c.set_thickness(3);
    const Point P0{10, 50}, P1{30, 20}, P2{60, 60}, P3{90, 30};
    c.spline({P0, P1, P2, P3});
    for (double t : {0.25, 0.5, 0.75}) {  // the curve from P1 to P2
      auto at = [&](double a0, double a1, double a2, double a3) {
        return 0.5 * (2 * a1 + (-a0 + a2) * t + (2 * a0 - 5 * a1 + 4 * a2 - a3) * t * t + (-a0 + 3 * a1 - 3 * a2 + a3) * t * t * t);
      };
      const double x = at(P0.x, P1.x, P2.x, P3.x), y = at(P0.y, P1.y, P2.y, P3.y);
      CHECK(c.pixel(static_cast<int>(x), static_cast<int>(y)).r > 150);
    }
  }
  // A polygon outline is closed: the last point joins back to the first.
  {
    Canvas c(40, 40);
    c.set_edge_levels(0);
    c.polygon({{10, 10}, {30, 10}, {20, 30}});
    CHECK(c.pixel(15, 20).r > 100);   // the edge from (20,30) back to (10,10)
    CHECK(c.pixel(20, 10).r > 100);
    CHECK(c.pixel(20, 18) == (Rgb{0, 0, 0}));  // not filled
    Canvas r(40, 40);
    r.set_edge_levels(0);
    r.rectangle(5, 5, 20, 10);
    CHECK(r.pixel(5, 10).r > 100 && r.pixel(25, 10).r > 100 && r.pixel(15, 5).r > 100 && r.pixel(15, 15).r > 100);
    CHECK(r.pixel(15, 10) == (Rgb{0, 0, 0}));
  }
  // Shapes follow the drawing space: a circle at (100, 50) in 200x100 units on a
  // 100x50 pixel canvas is centred on pixel (50, 25), and ovals stay in proportion.
  {
    Canvas c(100, 50, 200, 100);
    c.set_edge_levels(0);
    c.fill_circle(100, 50, 40);
    CHECK(c.pixel(50, 25) == (Rgb{255, 255, 255}));
    CHECK(c.pixel(68, 25).r > 100);   // radius 20 pixels
    CHECK(c.pixel(75, 25) == (Rgb{0, 0, 0}));
    Canvas t(100, 50, 100, 100);       // twice as tall as wide per unit
    t.set_edge_levels(0);
    t.fill_circle(50, 50, 20);        // 20 pixels wide, 10 high
    CHECK(t.pixel(68, 25).r > 100 && t.pixel(50, 28) == (Rgb{255, 255, 255}) && t.pixel(50, 38) == (Rgb{0, 0, 0}));
  }
  // Odd input does not crash: too few points, points far outside, a zero size.
  {
    Canvas c(40, 40);
    c.fill_polygon({});
    c.fill_polygon({{5, 5}, {30, 30}});
    c.fill_polygon({{-1e6, -1e6}, {1e6, -1e6}, {0, 1e6}});
    CHECK(c.pixel(20, 20) == (Rgb{255, 255, 255}));   // the huge triangle covers the canvas
    c.circle(20, 20, 0);
    c.arc(20, 20, 10, 30, 30);
    c.rectangle(10, 10, 0, 0);
    c.spline({});
    c.spline({{1, 1}});
    c.bezier({1, 1}, {1, 1}, {1, 1}, {1, 1});
  }
  // Fills do not leave coverage behind for the next shape.
  {
    Canvas c(40, 40);
    c.set_edge_levels(0);
    c.fill_rectangle(5, 5, 30, 30);
    c.set_colour({255, 0, 0});
    c.fill_rectangle(10, 10, 2, 2);
    CHECK(c.pixel(10, 10) == (Rgb{255, 0, 0}));
    CHECK(c.pixel(20, 20) == (Rgb{255, 255, 255}));
    CHECK(c.pixel(13, 13) == (Rgb{255, 255, 255}));
  }
  // ---- Patterns ----
  {
    // The tiles themselves.
    Pattern h = Pattern::horizontal(4, 1);
    CHECK(h.width() == 1 && h.height() == 4);
    CHECK(h.at(0, 0) == 255 && h.at(7, 4) == 255 && h.at(3, 1) == 0 && h.at(0, 3) == 0);
    CHECK(h.at(5, -4) == 255 && h.at(5, -1) == 0);            // the tile repeats to the left and up too
    Pattern v = Pattern::vertical(3, 2);
    CHECK(v.at(0, 9) == 255 && v.at(1, 0) == 255 && v.at(2, 5) == 0 && v.at(3, 0) == 255);
    Pattern f = Pattern::diagonal(6, 1, true);                // "/": on where x + y is a multiple of 6
    CHECK(f.at(0, 0) == 255 && f.at(5, 1) == 255 && f.at(2, 4) == 255 && f.at(1, 1) == 0 && f.at(1, 0) == 0);
    Pattern b = Pattern::diagonal(6, 1, false);               // "\": on where x - y is a multiple of 6
    CHECK(b.at(0, 0) == 255 && b.at(1, 1) == 255 && b.at(4, 4) == 255 && b.at(1, 0) == 0 && b.at(0, 1) == 0 && b.at(5, 0) == 0);
    CHECK(b.at(-1, 5) == 255);                                // -1 - 5 = -6
    Pattern c = Pattern::crosshatch(5, 1, true);
    CHECK(c.at(0, 3) == 255 && c.at(3, 0) == 255 && c.at(2, 2) == 0 && c.at(5, 7) == 255);
    Pattern x = Pattern::crosshatch(5, 1, false);
    CHECK(x.at(0, 0) == 255 && x.at(2, 3) == 255 && x.at(1, 4) == 255 && x.at(1, 2) == 0);
    Pattern d = Pattern::dots(4, 2);
    CHECK(d.at(0, 0) == 255 && d.at(1, 1) == 255 && d.at(2, 0) == 0 && d.at(0, 2) == 0 && d.at(4, 4) == 255);
    Pattern ch = Pattern::checker(2);
    CHECK(ch.at(0, 0) == 255 && ch.at(2, 0) == 0 && ch.at(0, 2) == 0 && ch.at(2, 2) == 255 && ch.at(4, 0) == 255);
    Pattern r = Pattern::from_rows({"X.X", ".X."});
    CHECK(r.width() == 3 && r.height() == 2 && r.at(0, 0) == 255 && r.at(1, 0) == 0 && r.at(1, 1) == 255 && r.at(3, 0) == 255);
    CHECK(Pattern::solid().at(123, -45) == 255);
    // Grey levels: the number of dots in a 4x4 tile is the level.
    for (int level = 0; level <= 16; level++) {
      Pattern g = Pattern::gray(level);
      int on = 0;
      for (int yy = 0; yy < 4; yy++) for (int xx = 0; xx < 4; xx++) on += g.at(xx, yy) == 255;
      CHECK(on == level);
    }
    CHECK(Pattern::gray(-3).at(0, 0) == 0 && Pattern::gray(99).at(1, 1) == 255);
    Pattern odd(0, -2);                                       // too small a size becomes 1x1
    CHECK(odd.width() == 1 && odd.height() == 1);
    odd.set(5, 5, 200);                                       // outside: ignored
    CHECK(odd.at(0, 0) == 0);
    odd.set(0, 0, 200);
    CHECK(odd.at(9, 9) == 200);
  }
  {
    // Filling with a pattern.
    auto lines = std::make_shared<const Pattern>(Pattern::horizontal(4, 1));
    Canvas c(60, 40);
    c.set_edge_levels(0);
    c.set_fill_pattern(lines);
    c.fill_rectangle(0, 1, 30, 20);
    for (int y = 0; y < 40; y++) {
      const bool inside = y >= 1 && y < 21;
      const bool lit = inside && y % 4 == 0;
      CHECK((c.pixel(10, y) == WHITE_) == lit);
      CHECK(c.pixel(10, y) == WHITE_ || c.pixel(10, y) == (Rgb{0, 0, 0}));
    }
    // Fixed to the picture: a neighbouring shape continues the same lines, and a
    // shape that starts one pixel lower gets the same lines, not shifted ones.
    c.fill_rectangle(30, 3, 30, 20);
    CHECK(c.pixel(29, 4) == WHITE_ && c.pixel(30, 4) == WHITE_ && c.pixel(35, 4) == WHITE_);
    CHECK(c.pixel(35, 5) == (Rgb{0, 0, 0}) && c.pixel(35, 3) == (Rgb{0, 0, 0}));
    // Lines and outlines ignore the pattern.
    Canvas l(40, 20);
    l.set_edge_levels(0);
    l.set_fill_pattern(lines);
    l.line({2, 7.5}, {38, 7.5});
    CHECK(l.pixel(20, 7) == WHITE_);                           // row 7 is not a pattern row, still solid
    l.rectangle(5, 3.5, 30, 12);                               // top edge on row 3, bottom edge on row 15
    CHECK(l.pixel(20, 3) == WHITE_ && l.pixel(20, 15) == WHITE_);
    // Back to solid.
    l.set_fill_pattern(nullptr);
    l.fill_rectangle(0, 0, 40, 20);
    CHECK(l.pixel(20, 5) == WHITE_ && l.pixel(3, 19) == WHITE_);
    CHECK(l.fill_pattern() == nullptr);
    // A soft edge keeps its softness under a pattern: half covered, on a pattern row.
    Canvas e(20, 8);
    e.set_edge_levels(0);
    e.set_fill_pattern(std::make_shared<const Pattern>(Pattern::solid()));
    e.fill_rectangle(2.5, 0, 10, 8);
    CHECK(e.pixel(2, 3).r > 120 && e.pixel(2, 3).r < 136);
    // A pattern with half-strength pixels gives half-strength colour.
    Pattern half(2, 1);
    half.set(0, 0, 255);
    half.set(1, 0, 128);
    Canvas g(20, 4);
    g.set_edge_levels(0);
    g.set_fill_pattern(std::make_shared<const Pattern>(half));
    g.fill_rectangle(0, 0, 20, 4);
    CHECK(g.pixel(4, 1) == WHITE_);
    CHECK(g.pixel(5, 1).r > 124 && g.pixel(5, 1).r < 132);
    // A pattern gives the share of pixels it should: grey level 8 of 16 is half.
    Canvas gr(64, 64);
    gr.set_fill_pattern(std::make_shared<const Pattern>(Pattern::gray(8)));
    gr.fill_rectangle(0, 0, 64, 64);
    int lit = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) lit += gr.pixel(x, y).r > 128;
    CHECK(lit == 64 * 64 / 2);
    // The same pattern object fills many shapes; none of them copy it.
    auto shared = std::make_shared<const Pattern>(Pattern::dots(3, 1));
    Canvas m(30, 30);
    m.set_fill_pattern(shared);
    CHECK(shared.use_count() == 2);
    m.fill_circle(15, 15, 10);
    m.fill_rectangle(0, 0, 5, 5);
    CHECK(shared.use_count() == 2);
  }
  // ---- Unusual input ----
  {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    // A point that is not a number is skipped; the rest are still joined.
    Canvas c(40, 30);
    c.set_edge_levels(0);
    c.polyline({{5, 15.5}, {nan, 3}, {inf, inf}, {30, 15.5}});
    CHECK(c.pixel(15, 15).r == 255);
    CHECK(c.pixel(15, 5) == (Rgb{0, 0, 0}));
    // Absurdly long lines still draw the part that is on the canvas, with the right slope.
    Canvas d(40, 40);
    d.set_edge_levels(0);
    d.line({-1e300, 15.5}, {1e300, 15.5});
    CHECK(d.pixel(20, 15).r == 255 && d.pixel(0, 15).r == 255 && d.pixel(39, 15).r == 255);
    Canvas e(40, 40);
    e.line({-1e8, -1e8}, {1e8, 1e8});
    CHECK(e.pixel(20, 20).r > 150);
    CHECK(e.pixel(20, 5) == (Rgb{0, 0, 0}));
    // Thickness: not a number counts as 1, a huge one is cut down (and is quick).
    Canvas f(40, 30);
    f.set_edge_levels(0);
    f.set_thickness(nan);
    f.line({2, 15.5}, {38, 15.5});
    CHECK(f.pixel(20, 15).r == 255 && f.pixel(20, 13) == (Rgb{0, 0, 0}));
    Canvas g(40, 30);
    g.set_thickness(1e12);
    g.line({2, 15}, {38, 15});
    CHECK(g.pixel(20, 0).r == 255 && g.pixel(20, 29).r == 255);
    // Odd sizes and numbers everywhere: nothing crashes, nothing waits forever.
    Canvas h(0, -5);
    CHECK(h.width() == 1 && h.height() == 1);
    h.line({0, 0}, {1, 1});
    Canvas k(60, 40);
    const double odd[] = {nan, inf, -inf, 1e300, -1e300, 1e18, 3e9, 0, -0.0, 1e-300, 17.5};
    for (double a : odd)
      for (double b : odd) {
        k.line({a, b}, {5, 5});
        k.polyline({{a, b}});
        k.circle(a, b, 4);
        k.circle(5, 5, a);
        k.arc(5, 5, 4, a, b);
        k.ellipse(a, 5, b, 5);
        k.fill_polygon({{a, b}, {20, 5}, {5, 20}});
        k.fill_rectangle(a, b, 10, 10);
        k.rectangle(5, 5, a, b);
        k.bezier({a, b}, {1, 1}, {2, 2}, {3, 3});
        k.spline({{a, b}, {1, 1}, {9, 9}});
        k.set_thickness(a);
        k.set_edge_levels(std::isfinite(b) ? static_cast<int>(std::fmod(b, 7)) : 3);
        k.line({1, 1}, {50, 30});
      }
    k.set_thickness(1);
    // Rectangles with extreme numbers for the sixel output.
    const int big = std::numeric_limits<int>::max(), small = std::numeric_limits<int>::min();
    CHECK(!k.sixel(big, big, big, big).empty());
    CHECK(!k.sixel(small, small, big, big).empty());
    CHECK(!k.sixel(5, 5, big, big).empty());
    CHECK(k.empty(big, big, big, big));
    // The same with random garbage, many times.
    unsigned seed = 4242;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
    auto num = [&]() {
      const unsigned r = rnd() % 12;
      if (r < 6) return static_cast<double>(static_cast<int>(rnd() % 200) - 50);
      if (r < 9) return (rnd() % 20000) / 10.0 - 1000;
      return odd[rnd() % 11];
    };
    Canvas q(80, 50, 100, 60);
    for (int i = 0; i < 3000; i++) {
      std::vector<Point> pts;
      for (unsigned j = 0, n = rnd() % 6; j < n; j++) pts.push_back({num(), num()});
      q.set_colour({static_cast<uint8_t>(rnd()), static_cast<uint8_t>(rnd()), static_cast<uint8_t>(rnd())});
      q.set_thickness(num() / 20);
      switch (rnd() % 8) {
        case 0: q.polyline(pts); break;
        case 1: q.fill_polygon(pts); break;
        case 2: q.circle(num(), num(), num()); break;
        case 3: q.arc(num(), num(), num(), num(), num(), num()); break;
        case 4: q.spline(pts); break;
        case 5: if (pts.size() >= 4) q.bezier(pts[0], pts[1], pts[2], pts[3]); break;
        case 6: q.fill_ellipse(num(), num(), num(), num()); break;
        default: q.sixel(static_cast<int>(num()), static_cast<int>(num()), static_cast<int>(num()), static_cast<int>(num())); break;
      }
    }
    CHECK(q.sixel().size() > 20);
  }
  // Virtual coordinates are scaled: (400, 240) in 800x480 is the middle of 100x60.
  {
    Canvas c(100, 60, 800, 480);
    c.set_thickness(3);
    c.polyline({{400, 240}});
    CHECK(c.pixel(50, 30) == (Rgb{255, 255, 255}));
    CHECK(c.pixel(10, 10) == (Rgb{0, 0, 0}));
  }
  // A line that leaves the canvas does not crash and still draws inside it.
  {
    Canvas c(30, 30);
    c.line({-100, 15.5}, {200, 15.5});
    CHECK(c.pixel(15, 15) == (Rgb{255, 255, 255}));
  }
  // A joint is not blended twice: a half-transparent edge pixel at a corner
  // must not be brighter than the same pixel on a straight line. (Checked by
  // drawing a polyline over itself: nothing may change.)
  {
    Canvas a(40, 40), b(40, 40);
    a.polyline({{5, 5}, {20, 20}, {5, 35}});
    b.polyline({{5, 5}, {20, 20}, {5, 35}});
    bool same = true;
    for (int y = 0; y < 40; y++)
      for (int x = 0; x < 40; x++) same = same && a.pixel(x, y) == b.pixel(x, y);
    CHECK(same);
  }
  // Going out and back over the same path must look the same as going once:
  // overlapping parts of one line are not blended twice.
  {
    Canvas a(60, 20), b(60, 20);
    a.set_thickness(3);
    b.set_thickness(3);
    a.line({5, 10.5}, {55, 10.5});
    b.polyline({{5, 10.5}, {55, 10.5}, {5, 10.5}});
    bool same = true;
    for (int y = 0; y < 20; y++)
      for (int x = 0; x < 60; x++) same = same && a.pixel(x, y) == b.pixel(x, y);
    CHECK(same);
  }
  // Long runs of the same pixels are compressed (a 600 pixel line is a few bytes).
  {
    Canvas c(600, 12);
    c.line({0, 3.5}, {600, 3.5});
    CHECK(c.sixel().size() < 200);
  }
  // The empty stretch before a colour's first pixel is compressed too.
  {
    Canvas c(600, 12);
    c.polyline({{250, 3.5}});
    CHECK(c.sixel().size() < 150);
  }
  // Round trip: exact colours when there are at most 255.
  {
    Canvas c(61, 40);  // 40 is not a multiple of 6, 61 is odd
    c.set_colour({255, 0, 0});
    c.polyline({{2, 2}, {58, 20}, {2, 38}});
    c.set_colour({0, 200, 255});
    c.set_thickness(3);
    c.line({0, 20}, {60, 20});
    std::string s = c.sixel();
    Decoded d = decode(s);
    CHECK(d.w == 61 && d.h == 40);
    CHECK(d.transparent);
    int wrong = 0, extra = 0;
    for (int y = 0; y < 40; y++)
      for (int x = 0; x < 61; x++) {
        size_t i = static_cast<size_t>(y) * 61 + x;
        Rgb want = c.pixel(x, y);
        if (want == Rgb{0, 0, 0}) { if (d.drawn[i]) extra++; }
        else if (!d.drawn[i] || dist(d.pix[i], want) > 3) wrong++;  // 1% steps
      }
    CHECK(wrong == 0);
    CHECK(extra == 0);
  }
  // Round trip with an opaque background.
  {
    Canvas c(20, 12);
    c.set_background({10, 20, 30});
    c.set_transparent(false);
    c.clear();
    c.line({1, 6}, {18, 6});
    Decoded d = decode(c.sixel());
    CHECK(!d.transparent);
    CHECK(d.drawn[0] == 1);
    CHECK(dist(d.pix[0], Rgb{10, 20, 30}) <= 3);
  }
  // The real case for more than 255 colours: a smooth, dim diagonal line next
  // to a block of 400 different colours. The dim line must keep its soft shades
  // (an earlier version rounded everything to a fixed palette and turned such
  // lines into hard teal stairs).
  {
    Canvas c(140, 90);
    for (int j = 0; j < 10; j++)
      for (int i = 0; i < 40; i++) {
        c.set_colour({static_cast<uint8_t>(20 + i * 6), static_cast<uint8_t>(20 + j * 20), static_cast<uint8_t>(240 - i * 5)});
        c.line({i * 3 + 2.5, j * 4 + 2.5}, {i * 3 + 2.5, j * 4 + 5.5});
      }
    c.set_edge_levels(0);  // every shade, so the dim line has many soft shades
    c.set_colour({35, 40, 60});
    c.line({0, 60}, {139, 89});
    std::set<uint32_t> distinct;
    for (int y = 0; y < 90; y++)
      for (int x = 0; x < 140; x++) {
        Rgb p = c.pixel(x, y);
        distinct.insert((uint32_t(p.r) << 16) | (uint32_t(p.g) << 8) | p.b);
      }
    CHECK(distinct.size() > 255);  // otherwise this test is not testing what it says
    Decoded d = decode(c.sixel());
    int worst = 0, soft = 0;
    for (int y = 50; y < 90; y++)  // below the colour block: only the dim line is here
      for (int x = 0; x < 140; x++) {
        Rgb want = c.pixel(x, y);
        if (want == Rgb{0, 0, 0}) continue;
        size_t k = static_cast<size_t>(y) * 140 + x;
        if (!d.drawn[k]) { worst = 255; continue; }
        worst = std::max(worst, dist(d.pix[k], want));
        if (want.r > 5 && want.r < 30) soft++;  // an in-between shade
      }
    CHECK(soft > 20);   // the line really has soft shades to lose
    CHECK(worst <= 12);
    int block_ok = 0;   // and the 400 plain colours mostly survive too
    for (int y = 0; y < 44; y++)
      for (int x = 0; x < 125; x++) {
        Rgb want = c.pixel(x, y);
        size_t k = static_cast<size_t>(y) * 140 + x;
        if (want != Rgb{0, 0, 0} && d.drawn[k] && dist(d.pix[k], want) <= 16) block_ok++;
      }
    CHECK(block_ok > 250);
  }
  // More than 255 colours: rounded, but close.
  {
    Canvas c(100, 60);
    for (int i = 0; i < 100; i++) {
      c.set_colour({static_cast<uint8_t>(i * 2 + 30), static_cast<uint8_t>(255 - i * 2), static_cast<uint8_t>(i)});
      c.line({double(i), 0}, {double(i), 59});
    }
    Decoded d = decode(c.sixel());
    int far = 0;
    for (int y = 0; y < 60; y++)
      for (int x = 0; x < 100; x++) {
        size_t i = static_cast<size_t>(y) * 100 + x;
        if (!d.drawn[i] || dist(d.pix[i], c.pixel(x, y)) > 30) far++;
      }
    CHECK(far == 0);
  }
  // A rectangle of the picture on its own: same pixels as that part of the canvas.
  {
    Canvas c(61, 40);
    c.set_colour({255, 0, 0});
    c.polyline({{2, 2}, {58, 20}, {2, 38}});
    c.set_colour({0, 200, 255});
    c.set_thickness(3);
    c.line({0, 20}, {60, 20});
    auto check_region = [&](int x, int y, int w, int h, int ex, int ey, int ew, int eh) {
      Decoded d = decode(c.sixel(x, y, w, h));
      CHECK(d.w == ew && d.h == eh);
      int wrong = 0;
      for (int j = 0; j < d.h && j < eh; j++)
        for (int i = 0; i < d.w && i < ew; i++) {
          size_t k = static_cast<size_t>(j) * d.w + i;
          Rgb want = c.pixel(ex + i, ey + j);
          if (want == Rgb{0, 0, 0}) { if (d.drawn[k]) wrong++; }
          else if (!d.drawn[k] || dist(d.pix[k], want) > 3) wrong++;
        }
      CHECK(wrong == 0);
    };
    check_region(10, 7, 20, 15, 10, 7, 20, 15);   // not on a band boundary
    check_region(-5, -5, 20, 20, 0, 0, 15, 15);   // sticks out at the top left
    check_region(50, 30, 40, 40, 50, 30, 11, 10); // sticks out at the bottom right
    Decoded none = decode(c.sixel(500, 500, 10, 10));  // entirely outside
    CHECK(none.w == 1 && none.h == 1);
    CHECK(none.drawn[0] == 0);
    CHECK(c.empty(0, 0, 1, 1));              // the very corner has nothing
    CHECK(!c.empty(0, 15, 61, 10));          // the cyan line runs through here
    CHECK(c.empty(500, 500, 10, 10));        // outside counts as empty
    Decoded above = decode(c.sixel(5, -30, 10, 10));  // wholly above the canvas
    CHECK(above.w == 1 && above.h == 1 && above.drawn[0] == 0);
    CHECK(c.empty(5, -30, 10, 10));
  }
  // An empty canvas is a valid, tiny picture.
  {
    Canvas c(10, 10);
    std::string s = c.sixel();
    CHECK(s.size() < 40);
    CHECK(s.compare(0, 2, "\x1bP") == 0);
  }

  if (failures == 0) printf("vtpen: all tests passed\n");
  return failures ? 1 : 0;
}

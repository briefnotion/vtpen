// Tests for vtpen_drawing.h (Drawing and its Sixel backend). Build and run: ./m test
// The Sixel backend is checked against the same shapes drawn straight on a Canvas:
// every pixel, and the Sixel text, must be the same.

#include <cstdio>
#include <limits>

#include "vtpen_drawing.h"

using namespace vtpen;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                      \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static bool same_pixels(const Canvas& a, const Canvas& b) {
  if (a.width() != b.width() || a.height() != b.height()) return false;
  for (int y = 0; y < a.height(); y++)
    for (int x = 0; x < a.width(); x++)
      if (a.pixel(x, y) != b.pixel(x, y)) return false;
  return true;
}

static int drawn(const Canvas& c) {
  int n = 0;
  for (int y = 0; y < c.height(); y++)
    for (int x = 0; x < c.width(); x++)
      if (c.pixel(x, y) != c.background()) n++;
  return n;
}

// One of every shape, in different colours and thicknesses. The same calls are made
// on a Drawing and on a Canvas (both written as a template so they cannot drift apart).
template <class T>
static void everything(T& t) {
  t.set_colour({255, 200, 0});
  t.set_thickness(3);
  t.polyline({{40, 40}, {200, 300}, {420, 60}});
  t.set_colour({90, 200, 255});
  t.set_thickness(1);
  t.line({10, 450}, {790, 430});
  t.rectangle(500, 40, 120, 80);
  t.polygon({{650, 40}, {760, 90}, {700, 160}});
  t.set_colour({255, 80, 80});
  t.set_thickness(2);
  t.circle(150, 380, 50);
  t.ellipse(400, 380, 90, 30);
  t.arc(600, 380, 60, 30, 250);
  t.arc(720, 300, 40, 25, 10, 200);
  t.set_colour({120, 255, 120});
  t.set_thickness(1.5);
  t.bezier({40, 200}, {120, 120}, {200, 280}, {300, 200});
  t.spline({{320, 220}, {380, 150}, {450, 240}, {520, 170}});
  t.set_colour({200, 120, 255});
  t.fill_polygon({{560, 200}, {620, 330}, {500, 250}, {620, 250}, {500, 330}});  // a star: even-odd hole
  t.fill_rectangle(30, 330, 50, 40);
  t.fill_circle(300, 330, 30);
  t.fill_ellipse(240, 420, 50, 20);
}

int main() {
  // The Sixel backend gives the same picture as drawing straight on a Canvas.
  {
    Drawing d(800, 480);
    everything(d);
    Canvas direct(400, 240, 800, 480);
    everything(direct);
    Canvas via = d.render(400, 240);
    CHECK(drawn(direct) > 1000);                       // the test really draws something
    CHECK(same_pixels(direct, via));
    CHECK(direct.sixel() == via.sixel());
    CHECK(d.size() == 14);                             // one entry per call, even for rectangle() and circle()
  }
  // The same, at a size where one drawing unit is more than one pixel, and with
  // soft edges off and a coloured opaque background.
  {
    Drawing d(800, 480);
    everything(d);
    Canvas direct(1600, 960, 800, 480), via(1600, 960, 800, 480);
    for (Canvas* c : {&direct, &via}) { c->set_edge_levels(1); c->set_background({10, 20, 30}); c->set_transparent(false); c->clear(); }
    everything(direct);
    d.draw(via);
    CHECK(same_pixels(direct, via));
    CHECK(direct.sixel() == via.sixel());
  }
  // A canvas with a different drawing space: the drawing is stretched to it.
  {
    Drawing d(800, 480);
    everything(d);
    Canvas direct(400, 240, 800, 480);                 // 0.5 pixel per unit
    everything(direct);
    Canvas other(400, 240, 400, 240);                  // 1 pixel per unit: the same picture needs half the numbers
    d.draw(other);
    CHECK(same_pixels(direct, other));
    Canvas plain(400, 240);                            // no drawing space given: units are pixels
    d.draw(plain);
    CHECK(same_pixels(direct, plain));
  }
  // A canvas whose space is stretched one way only (twice as many units across as it has):
  // the same picture, so x and y must each use their own scale.
  {
    Drawing d(800, 480);
    everything(d);
    Canvas direct(400, 240, 800, 480);
    everything(direct);
    Canvas wide(400, 240, 400, 480);                   // 1 pixel per unit across, 0.5 down
    d.draw(wide);
    CHECK(same_pixels(direct, wide));
    Canvas tall(400, 240, 800, 240);                   // 0.5 across, 1 down
    d.draw(tall);
    CHECK(same_pixels(direct, tall));
  }
  // Many sizes whose scale is not a tidy number: still the same pixels as drawing straight.
  {
    Drawing d(800, 480);
    everything(d);
    int bad = 0;
    for (int w = 97; w < 700; w += 37) {
      const int h = w * 3 / 5 + 1;
      Canvas direct(w, h, 800, 480);
      everything(direct);
      if (!same_pixels(direct, d.render(w, h))) bad++;
    }
    CHECK(bad == 0);
  }
  // Squeezed one way: a circle becomes an oval, as it does in Canvas.
  {
    Drawing d(100, 100);
    d.circle(50, 50, 40);
    d.fill_circle(50, 50, 10);
    Canvas direct(200, 100, 100, 100);
    direct.circle(50, 50, 40);
    direct.fill_circle(50, 50, 10);
    Canvas via = d.render(200, 100);
    CHECK(same_pixels(direct, via));
    CHECK(via.pixel(20, 50) != via.background());      // far left of the oval (centre 100, x radius 80)
    CHECK(via.pixel(179, 50) != via.background());     // far right: x is stretched
    CHECK(via.pixel(100, 10) != via.background());     // top (y radius 40)
    CHECK(via.pixel(100, 3) == via.background());      // nothing beyond it
  }
  // Colour and thickness are taken when the shape is added, not when it is drawn.
  {
    Drawing d(100, 100);
    d.set_colour({255, 0, 0});
    d.set_thickness(1);
    d.line({10, 20.5}, {90, 20.5});
    d.set_colour({0, 0, 255});
    d.set_thickness(7);
    d.line({10, 60.5}, {90, 60.5});
    d.set_colour({0, 255, 0});                         // nothing after this draws
    Canvas c = d.render(100, 100);
    CHECK(c.pixel(50, 20) == (Rgb{255, 0, 0}));
    CHECK(c.pixel(50, 19) == c.background());
    CHECK(c.pixel(50, 60) == (Rgb{0, 0, 255}));
    CHECK(c.pixel(50, 58) == (Rgb{0, 0, 255}));
    CHECK(c.pixel(50, 40) == c.background());
  }
  // Shapes land in the order they were added; draw() keeps what the canvas had and
  // forgets nothing between calls.
  {
    Drawing d(60, 60);
    d.set_colour({255, 0, 0});
    d.fill_rectangle(10, 10, 30, 30);
    d.set_colour({0, 0, 255});
    d.fill_rectangle(20, 20, 30, 30);
    Canvas c(60, 60);
    c.set_colour({0, 255, 0});
    c.fill_rectangle(0, 0, 60, 8);                     // already on the canvas
    d.draw(c);
    CHECK(c.pixel(30, 4) == (Rgb{0, 255, 0}));         // kept
    CHECK(c.pixel(15, 15) == (Rgb{255, 0, 0}));
    CHECK(c.pixel(30, 30) == (Rgb{0, 0, 255}));        // later shape on top
    CHECK(c.pixel(55, 55) == c.background());
    const int before = drawn(c);
    d.draw(c);                                         // twice: the same picture
    CHECK(drawn(c) == before);
  }
  // Fills are solid: a pattern left on the canvas does not leak into a Drawing.
  {
    Drawing d(40, 40);
    d.set_colour({255, 255, 255});
    d.fill_rectangle(0, 0, 40, 40);
    Canvas c(40, 40);
    c.set_fill_pattern(std::make_shared<const Pattern>(Pattern::checker(2)));
    d.draw(c);
    CHECK(drawn(c) == 40 * 40);
    CHECK(c.fill_pattern() == nullptr);
  }
  // The whole drawing space is covered, to its corners.
  {
    Drawing d(800, 480);
    d.set_colour({255, 255, 255});
    d.fill_rectangle(0, 0, 800, 480);
    Canvas c = d.render(80, 48);
    CHECK(c.width() == 80 && c.height() == 48);
    CHECK(c.pixel(0, 0) != c.background());
    CHECK(c.pixel(79, 0) != c.background());
    CHECK(c.pixel(0, 47) != c.background());
    CHECK(c.pixel(79, 47) != c.background());
    CHECK(drawn(c) == 80 * 48);
  }
  // clear(), size(), empty(); the colour and thickness set before stay.
  {
    Drawing d(50, 50);
    CHECK(d.empty() && d.size() == 0);
    d.set_colour({1, 2, 3});
    d.set_thickness(4);
    d.line({0, 0}, {10, 10});
    CHECK(!d.empty() && d.size() == 1);
    d.clear();
    CHECK(d.empty());
    d.line({0, 25}, {49, 25});
    Canvas c = d.render(50, 50);
    CHECK(c.pixel(25, 25) == (Rgb{1, 2, 3}));
    CHECK(c.pixel(25, 23) == (Rgb{1, 2, 3}));          // still 4 thick
    CHECK(Canvas(10, 10).width() == 10 && drawn(Drawing(5, 5).render(10, 10)) == 0);  // nothing added, nothing drawn
  }
  // Sizes that are not a size give the default; bad numbers do not crash or hang.
  {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    CHECK(Drawing(0, -3).width() == 800 && Drawing(0, -3).height() == 480);
    CHECK(Drawing(nan, inf).width() == 800 && Drawing(nan, inf).height() == 480);
    CHECK(Drawing(inf, 5).width() == 800 && Drawing(5, inf).height() == 480);
    CHECK(Drawing(120, 70).width() == 120 && Drawing(120, 70).height() == 70);
    Drawing d(100, 100);
    d.set_thickness(nan);
    d.polyline({{nan, 5}, {10, 10}, {inf, 3}, {50, 50}, {1e300, -1e300}});
    d.set_thickness(1e9);
    d.circle(nan, 5, 10);
    d.circle(50, 50, inf);
    d.arc(50, 50, 20, nan, 90);
    d.fill_polygon({{nan, nan}, {1e300, 1e300}, {-1e300, 5}});
    d.fill_ellipse(50, 50, -5, 1e300);
    d.bezier({0, 0}, {nan, 0}, {inf, inf}, {100, 100});
    d.spline({{0, 0}});
    d.polyline({});
    d.polygon({});
    d.fill_polygon({});
    d.spline({});
    Canvas c = d.render(40, 40);                       // survives, whatever it drew
    CHECK(c.width() == 40);
    Canvas tiny = d.render(0, -4);                     // Canvas keeps at least 1x1
    CHECK(tiny.width() == 1 && tiny.height() == 1);
  }

  printf("%s (%d failures)\n", failures ? "FAILED" : "all drawing tests passed", failures);
  return failures ? 1 : 0;
}

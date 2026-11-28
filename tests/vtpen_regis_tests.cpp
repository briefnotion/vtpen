// Tests for vtpen_regis.h (the ReGIS emitter). Build and run: ./m test
// The text it writes is compared with exact expectations for simple shapes, and read
// back by a small ReGIS interpreter (regis_decode.h) to compare the picture it makes
// with the same Drawing drawn on a Canvas.

#include <cstdio>
#include <limits>

#include "regis_decode.h"
#include "vtpen_regis.h"

using namespace vtpen;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                      \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static const std::string DCS = "\x1bP1p";
static const std::string ST = "\x1b\\";
static const std::string HEAD = "W(V,P1)\nW(I1)\n";   // what a one-colour white drawing starts with

static Regis with_palette(std::vector<Rgb> p) {
  RegisOptions o;
  o.palette = std::move(p);
  return Regis(o);
}
static Regis white() { return with_palette({{255, 255, 255}}); }

// The text of one drawing, without the DCS, the ST and the colour map.
static std::string body(const std::string& s) {
  std::string b = s.substr(4, s.size() - 6);
  if (b.compare(0, 3, "S(M") == 0) b = b.substr(b.find('\n') + 1);
  return b;
}

static registest::Screen read(const std::string& s) {
  registest::Screen scr = registest::read(s);
  if (!scr.ok) printf("  (the reader said: %s)\n", scr.error.c_str());
  return scr;
}

// What a Canvas draws for the same Drawing, hard edges: a flag for every pixel with ink.
static std::vector<char> canvas_ink(const Drawing& d) {
  Canvas c(800, 480, d.width(), d.height());
  c.set_edge_levels(1);
  d.draw(c);
  std::vector<char> v(800 * 480, 0);
  for (int y = 0; y < 480; y++)
    for (int x = 0; x < 800; x++) v[static_cast<size_t>(y) * 800 + x] = c.pixel(x, y) != c.background();
  return v;
}
static std::vector<char> regis_ink(const registest::Screen& s) {
  std::vector<char> v(800 * 480, 0);
  for (size_t i = 0; i < v.size(); i++) v[i] = s.px[i] != 0;
  return v;
}
static int count(const std::vector<char>& v) { int n = 0; for (char c : v) n += c != 0; return n; }
// How many pixels of a have no pixel of b within tol.
static int far_from(const std::vector<char>& a, const std::vector<char>& b, int tol) {
  int bad = 0;
  for (int y = 0; y < 480; y++)
    for (int x = 0; x < 800; x++) {
      if (!a[static_cast<size_t>(y) * 800 + x]) continue;
      bool near = false;
      for (int dy = -tol; dy <= tol && !near; dy++)
        for (int dx = -tol; dx <= tol; dx++) {
          const int xx = x + dx, yy = y + dy;
          if (xx >= 0 && yy >= 0 && xx < 800 && yy < 480 && b[static_cast<size_t>(yy) * 800 + xx]) { near = true; break; }
        }
      if (!near) bad++;
    }
  return bad;
}
// The ReGIS picture and the Canvas picture of the same drawing agree to within tol pixels everywhere.
static bool alike(const Drawing& d, int tol, int* area_canvas = nullptr, int* area_regis = nullptr, registest::Screen* keep = nullptr) {
  Regis r;
  const registest::Screen s = read(r.draw(d));
  const std::vector<char> a = canvas_ink(d), b = regis_ink(s);
  if (area_canvas) *area_canvas = count(a);
  if (area_regis) *area_regis = count(b);
  if (keep) *keep = s;
  const int x = far_from(a, b, tol), y = far_from(b, a, tol);
  if (x || y) printf("  (%d canvas pixels and %d ReGIS pixels are more than %d from the other picture)\n", x, y, tol);
  return s.ok && !x && !y;
}
static bool areas_close(int a, int b, double fraction) { return std::fabs(a - b) <= fraction * std::max(a, b) + 2; }

int main() {
  // ---- the text, exactly ----
  {
    Regis r = white();
    Drawing d;
    d.set_colour({255, 255, 255});
    d.polyline({{10, 10}, {20, 10}, {20, 20}});
    CHECK(r.draw(d) == DCS + "S(M1(AH120L100S0))\nW(V,P1)\nW(I1)\nP[10,10]V[20,10][20,20]\n" + ST);
    CHECK(r.draw(d) == DCS + "W(V,P1)\nW(I1)\nP[10,10]V[20,10][20,20]\n" + ST);  // the map goes once
    r.reset();
    CHECK(r.draw(d).find("S(M1(AH120L100S0))") != std::string::npos);            // unless reset
    CHECK(r.clear() == DCS + "S(E)" + ST);
    CHECK(r.erase(d) == DCS + "W(E,P1)\nP[10,10]V[20,10][20,20]\n" + ST);        // no colour when erasing
  }
  {
    Regis r = with_palette({{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {128, 128, 128}});
    Drawing d;
    d.set_colour({250, 10, 10});
    d.line({0, 0}, {5, 0});
    d.set_colour({0, 0, 255});
    d.line({0, 1}, {5, 1});
    d.line({0, 2}, {5, 2});  // same colour: no second W(I)
    d.set_colour({1, 250, 1});
    d.line({0, 3}, {5, 3});
    d.set_colour({140, 140, 130});
    d.line({0, 4}, {5, 4});
    const std::string s = r.draw(d);
    CHECK(s.find("S(M1(AH120L50S100)2(AH240L50S100)3(AH0L50S100)4(AH120L50S0))") != std::string::npos);  // red, green, blue, grey
    CHECK(body(s) == "W(V,P1)\nW(I1)\nP[0,0]V[5,0]\nW(I3)\nP[0,1]V[5,1]\nP[0,2]V[5,2]\nW(I2)\nP[0,3]V[5,3]\nW(I4)\nP[0,4]V[5,4]\n");
    CHECK(r.palette().size() == 4);
  }
  {
    Drawing d;
    d.circle(100, 100, 50);
    CHECK(body(white().draw(d)) == HEAD + "P[100,100]C[150,100]\n");
    Drawing a;
    a.arc(100, 100, 50, 0, 90);
    CHECK(body(white().draw(a)) == HEAD + "P[100,100]C(A-90)[150,100]\n");
    Drawing b;
    b.arc(100, 100, 50, 90, 200);
    CHECK(body(white().draw(b)) == HEAD + "P[100,100]C(A-110)[100,150]\n");
    Drawing c;
    c.fill_circle(100, 100, 50);
    CHECK(body(white().draw(c)) == HEAD + "P[100,100]F(C[150,100])\n");
    Drawing e;
    e.fill_rectangle(10, 10, 20, 10);
    CHECK(body(white().draw(e)) == HEAD + "P[10,10]F(V(B)[30,10][30,20][10,20](E))\n");
    Drawing f;
    f.polyline({{30, 40}});
    CHECK(body(white().draw(f)) == HEAD + "P[30,40]V[]\n");
    Drawing g;
    g.rectangle(10, 10, 20, 10);
    CHECK(body(white().draw(g)) == HEAD + "P[10,10]V[30,10][30,20][10,20][10,10]\n");
    Drawing h;
    h.polyline({});
    h.polygon({});
    h.fill_polygon({});
    h.fill_polygon({{1, 1}, {5, 5}});         // not a shape
    h.fill_polygon({{1, 1}, {5, 5}, {9, 9}});  // no area
    CHECK(body(white().draw(h)) == "W(V,P1)\n" "W(I1)\n");
  }
  // ---- clipping to the screen ----
  {
    Drawing d;
    d.line({-100, -100}, {-10, 50});
    CHECK(body(white().draw(d)) == HEAD);                    // nothing of it is on the screen
    Drawing e;
    e.line({-100, 10}, {50, 10});
    CHECK(body(white().draw(e)) == HEAD + "P[0,10]V[50,10]\n");
    Drawing f;
    f.polyline({{10, 10}, {-50, 10}, {-50, 30}, {20, 30}});  // leaves, then comes back in
    CHECK(body(white().draw(f)) == HEAD + "P[10,10]V[0,10]\nP[0,30]V[20,30]\n");
    Drawing g;
    g.line({700, 100}, {900, 100});
    CHECK(body(white().draw(g)) == HEAD + "P[700,100]V[799,100]\n");
    Drawing h;
    h.circle(5, 100, 50);                                    // does not fit: lines instead of a ReGIS circle
    CHECK(body(white().draw(h)).find("C[") == std::string::npos);
    CHECK(read(white().draw(h)).ok);
  }
  // ---- shape details ----
  {
    Drawing a;
    a.set_thickness(2);
    a.circle(100, 100, 50);                                  // 2 pixels wide: not a ReGIS circle
    const std::string t = body(white().draw(a));
    CHECK(t.find("C[") == std::string::npos && t.find("F(V(B)") != std::string::npos);
    Drawing b;
    b.circle(50, 100, 50);                                   // touches the left edge of the screen exactly
    CHECK(body(white().draw(b)) == HEAD + "P[50,100]C[100,100]\n");
    Drawing c;
    c.circle(49, 100, 50);                                   // one pixel beyond it
    CHECK(body(white().draw(c)).find("C[") == std::string::npos);
    // Two corners meet at one point: not one simple fill, so strips.
    Drawing d;
    d.fill_polygon({{0, 0}, {100, 0}, {50, 50}, {100, 100}, {0, 100}, {50, 50}});
    const std::string pinched = body(white().draw(d));
    CHECK(pinched.find("F(V(B)") != pinched.rfind("F(V(B)"));
    CHECK(read(white().draw(d)).ok);
    // A closing corner that repeats the first changes nothing.
    Drawing e, f;
    e.fill_polygon({{10, 10}, {90, 20}, {50, 80}});
    f.fill_polygon({{10, 10}, {90, 20}, {50, 80}, {10, 10}});
    CHECK(read(white().draw(e)).px == read(white().draw(f)).px && read(white().draw(e)).ink() > 1000);
    // Wide lines: a disc at each end, and one at a bend only when it is sharp.
    auto discs = [](const Drawing& x) {
      const std::string s = body(white().draw(x));
      int n = 0;
      for (size_t i = s.find("F(C["); i != std::string::npos; i = s.find("F(C[", i + 1)) n++;
      return n;
    };
    Drawing g, h;
    g.set_thickness(6);
    g.polyline({{100, 100}, {200, 100}, {300, 105}});         // a bend of about 3 degrees
    h.set_thickness(6);
    h.polyline({{100, 100}, {200, 100}, {250, 200}});         // a sharp one
    CHECK(discs(g) == 2 && discs(h) == 3);
    Drawing k;
    k.set_thickness(6);
    k.line({100, 100}, {200, 100});
    CHECK(discs(k) == 2);
    // A dot beyond the screen is nothing; a line out to the ends of the numbers still crosses it.
    Drawing m, n;
    m.polyline({{-5, 5}});
    CHECK(body(white().draw(m)) == HEAD);
    n.line({-1e308, 100}, {1e308, 100});
    const registest::Screen sn = read(white().draw(n));
    int row = 0;
    for (int x = 0; x < 800; x++) row += sn.at(x, 100) != 0;
    CHECK(sn.ok && row == 800 && sn.ink() == 800);
  }
  // ---- a rectangle of the screen for the drawing ----
  {
    RegisOptions o;
    o.palette = {{255, 255, 255}};
    o.x = 100; o.y = 50; o.width = 200; o.height = 100;
    Drawing d(800, 480);
    d.fill_rectangle(0, 0, 800, 480);
    d.set_colour({255, 255, 255});
    registest::Screen s = read(Regis(o).draw(d));
    CHECK(s.ok);
    int x0 = 9999, x1 = -1, y0 = 9999, y1 = -1;
    for (int y = 0; y < 480; y++)
      for (int x = 0; x < 800; x++)
        if (s.at(x, y)) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    CHECK(x0 == 100 && x1 == 299 && y0 == 50 && y1 == 149);
    CHECK(s.ink() == 200 * 100);
    // A line across the whole drawing space spans the whole rectangle, and stays inside it.
    Drawing e(800, 480);
    e.line({-300, 240}, {1100, 240});
    s = read(Regis(o).draw(e));
    CHECK(s.at(100, 100) && s.at(299, 100) && !s.at(99, 100) && !s.at(300, 100) && s.ink() == 200);
    // Odd options are put right.
    RegisOptions w;
    w.x = -5; w.y = 9999; w.width = 0; w.height = -3;
    Drawing f;
    f.fill_rectangle(0, 0, 800, 480);
    s = read(Regis(w).draw(f));
    CHECK(s.ok && s.ink() <= 2);                        // a rectangle of one pixel: nothing much to draw, and no crash
    RegisOptions big;
    big.x = 700; big.width = 5000; big.height = 5000;
    s = read(Regis(big).draw(f));
    CHECK(s.ok && s.ink() == 100 * 480);
  }
  // ---- the picture agrees with the Canvas ----
  {
    Drawing d;
    d.set_colour({255, 255, 255});
    d.polyline({{40, 40}, {200, 300}, {420, 60}});
    d.line({10, 450}, {790, 430});
    d.rectangle(500, 40, 120, 80);
    d.polygon({{650, 40}, {760, 90}, {700, 160}});
    d.circle(150, 380, 50);
    d.ellipse(400, 380, 90, 30);
    d.arc(600, 380, 60, 30, 250);
    d.arc(720, 300, 40, 25, 10, 200);
    d.bezier({40, 200}, {120, 120}, {200, 280}, {300, 200});
    d.spline({{320, 220}, {380, 150}, {450, 240}, {520, 170}});
    int ac, ar;
    CHECK(alike(d, 2, &ac, &ar));
    CHECK(ac > 1500 && areas_close(ac, ar, 0.35));
  }
  {
    Drawing d;
    d.set_colour({255, 255, 255});
    d.fill_polygon({{50, 50}, {300, 80}, {200, 200}, {330, 330}, {40, 300}, {120, 180}});   // a dent
    d.fill_rectangle(400, 40, 150, 90);
    d.fill_circle(650, 100, 60);
    d.fill_ellipse(500, 300, 100, 40);
    d.fill_polygon({{600, 250}, {780, 260}, {700, 420}});
    int ac, ar;
    CHECK(alike(d, 2, &ac, &ar));
    CHECK(ac > 20000 && areas_close(ac, ar, 0.03));
  }
  {
    // A star: the fill has a hole in the middle (even-odd), which ReGIS cannot do in one fill.
    Drawing d;
    d.set_colour({255, 255, 255});
    std::vector<Point> star;
    for (int i = 0; i < 5; i++) {
      const double a = (i * 144 - 90) * 3.14159265358979 / 180;
      star.push_back({400 + 200 * std::cos(a), 240 + 200 * std::sin(a)});
    }
    d.fill_polygon(star);
    int ac, ar;
    registest::Screen s;
    CHECK(alike(d, 2, &ac, &ar, &s));
    CHECK(areas_close(ac, ar, 0.03));
    CHECK(!s.at(400, 240));                                   // the middle is a hole
    const std::string text = body(white().draw(d));
    CHECK(text.find("F(V(B)") != std::string::npos);
    CHECK(text.find("F(V(B)") != text.rfind("F(V(B)"));       // more than one strip
  }
  {
    // More than 256 corners: ReGIS cannot take that in one fill either.
    Drawing d;
    d.set_colour({255, 255, 255});
    std::vector<Point> ring;
    for (int i = 0; i < 400; i++) {
      const double a = i * 2 * 3.14159265358979 / 400;
      ring.push_back({400 + 220 * std::cos(a), 240 + 200 * std::sin(a)});
    }
    d.fill_polygon(ring);
    int ac, ar;
    CHECK(alike(d, 2, &ac, &ar));                              // the reader fails any fill over 256 corners
    CHECK(areas_close(ac, ar, 0.03));
  }
  {
    // Lines 5 pixels wide are strips; bends of wide lines are rounded with discs.
    Drawing d;
    d.set_colour({255, 255, 255});
    d.set_thickness(5);
    d.polyline({{100, 100}, {400, 120}, {300, 300}, {600, 380}});
    d.set_thickness(2);
    d.circle(650, 150, 80);
    d.set_thickness(9);
    d.line({50, 440}, {750, 440});
    int ac, ar;
    registest::Screen s;
    CHECK(alike(d, 3, &ac, &ar, &s));
    CHECK(areas_close(ac, ar, 0.12));
    int across = 0;
    for (int y = 400; y < 480; y++) across += s.at(400, y) != 0;
    CHECK(across >= 8 && across <= 10);                       // the 9 pixel line
    const std::string text = body(white().draw(d));
    CHECK(text.find("F(C[") != std::string::npos);            // a disc at a sharp bend
    // A wide line that is a single point is a dot.
    Drawing e;
    e.set_thickness(7);
    e.polyline({{100, 100}});
    s = read(white().draw(e));
    CHECK(s.ok && s.ink() >= 30 && s.ink() <= 70 && s.at(100, 100));
  }
  // ---- fewer points ----
  {
    auto with = [](double simplify) {
      RegisOptions o;
      o.palette = {{255, 255, 255}};
      o.simplify = simplify;
      return Regis(o);
    };
    Drawing straight;
    for (int i = 0; i <= 9; i++) straight.polyline({{i * 10.0, 10}, {i * 10.0 + 10, 10}});
    Drawing line;
    std::vector<Point> pts;
    for (int i = 0; i <= 9; i++) pts.push_back({i * 10.0, 10});
    line.polyline(pts);
    CHECK(body(with(0.5).draw(line)) == HEAD + "P[0,10]V[90,10]\n");
    CHECK(body(with(0).draw(line)) == HEAD + "P[0,10]V[10,10][20,10][30,10][40,10][50,10][60,10][70,10][80,10][90,10]\n");
    CHECK(body(with(std::numeric_limits<double>::quiet_NaN()).draw(line)) == body(with(0).draw(line)));
    CHECK(body(with(-3).draw(line)) == body(with(0).draw(line)));
    // A wobble of 0.4 pixel goes, one of 1 pixel stays.
    Drawing small, big;
    std::vector<Point> a, b;
    for (int i = 0; i <= 4; i++) { a.push_back({i * 10.0, i % 2 ? 10.4 : 10.0}); b.push_back({i * 10.0, i % 2 ? 11.0 : 10.0}); }
    small.polyline(a);
    big.polyline(b);
    CHECK(body(with(0.5).draw(small)) == HEAD + "P[0,10]V[40,10]\n");
    CHECK(body(with(0.5).draw(big)) == HEAD + "P[0,10]V[10,11][20,10][30,11][40,10]\n");
    CHECK(body(with(std::numeric_limits<double>::infinity()).draw(line)) == body(with(0).draw(line)));
    // Three points on a line lose the middle one; half a pixel off exactly is the limit (kept only when further).
    Drawing three, edge, further, back;
    three.polyline({{0, 10}, {50, 10}, {100, 10}});
    edge.polyline({{0, 10}, {50, 10.5}, {100, 10}});
    further.polyline({{0, 10}, {50, 10.6}, {100, 10}});
    CHECK(body(with(0.5).draw(three)) == HEAD + "P[0,10]V[100,10]\n");
    CHECK(body(with(0.5).draw(edge)) == HEAD + "P[0,10]V[100,10]\n");
    CHECK(body(with(0.5).draw(further)) == HEAD + "P[0,10]V[50,11][100,10]\n");
    // A line that runs out and comes back along itself keeps its far end.
    back.polyline({{0, 10}, {100, 10}, {20, 10}});
    CHECK(body(with(0.5).draw(back)) == HEAD + "P[0,10]V[100,10][20,10]\n");
    // The turning points of an outline stay, whatever the tolerance.
    Drawing box;
    box.rectangle(10, 10, 20, 10);
    CHECK(body(with(5).draw(box)) == HEAD + "P[10,10]V[30,10][30,20][10,20][10,10]\n");
    // Wide lines are strips: fewer points, fewer strips.
    Drawing wide;
    wide.set_thickness(4);
    std::vector<Point> mid;
    for (int i = 0; i <= 9; i++) mid.push_back({100 + i * 10.0, 100});   // away from the screen's edge, where the end discs fit
    wide.polyline(mid);
    auto strips = [&](double tol) {
      const std::string t = body(with(tol).draw(wide));
      int n = 0;
      for (size_t i = t.find("F(V(B)"); i != std::string::npos; i = t.find("F(V(B)", i + 1)) n++;
      return n;
    };
    CHECK(strips(0.5) == 1 && strips(0) == 9);
    // Curves keep their shape to within a pixel and the text gets much shorter.
    Drawing curves;
    curves.set_colour({255, 255, 255});
    curves.ellipse(400, 240, 300, 150);
    curves.circle(400, 240, 90);
    curves.bezier({40, 200}, {120, 20}, {200, 400}, {300, 200});
    curves.spline({{320, 220}, {380, 150}, {450, 240}, {520, 170}, {700, 400}});
    curves.arc(600, 380, 60, 30, 250);
    curves.set_thickness(3);
    curves.arc(150, 380, 80, 20, 300);
    const std::string full = with(0).draw(curves), cut = with(0.5).draw(curves);
    const registest::Screen sf = read(full), sc = read(cut);
    CHECK(sf.ok && sc.ok && far_from(regis_ink(sf), regis_ink(sc), 1) == 0 && far_from(regis_ink(sc), regis_ink(sf), 1) == 0);
    CHECK(cut.size() * 10 < full.size() * 7);
    CHECK(sf.ink() > 1500);
  }
  // ---- taking shapes away, and animation ----
  {
    Drawing a;
    a.set_colour({255, 255, 255});
    a.polyline({{40, 40}, {200, 300}, {420, 60}});
    a.fill_circle(600, 200, 50);
    a.circle(300, 300, 40);
    a.set_thickness(4);
    a.line({10, 440}, {790, 450});
    Regis r = white();
    registest::Screen s;
    registest::read_into(s, r.draw(a));
    CHECK(s.ok && s.ink() > 1000);
    registest::read_into(s, r.erase(a));
    CHECK(s.ok && s.ink() == 0);

    Drawing b;
    b.set_colour({255, 255, 255});
    b.fill_rectangle(100, 100, 300, 200);
    b.line({0, 0}, {799, 479});
    // frame(): the first only draws, the next takes the last away first.
    Regis f = white();
    registest::Screen t;
    registest::read_into(t, f.frame(a));
    registest::read_into(t, f.frame(b));
    const registest::Screen fresh = read(white().draw(b));
    CHECK(t.ok && fresh.ok && t.px == fresh.px);
    // A frame is one piece, and the second erases before it draws.
    Regis g = white();
    g.frame(a);
    const std::string second = g.frame(b);
    CHECK(second.compare(0, 4, DCS) == 0 && second.find(ST) == second.size() - 2);
    CHECK(second.find("W(E,P1)") < second.find("W(V,P1)"));
    // clear() wipes the screen and forgets the frame.
    registest::read_into(t, g.clear());
    CHECK(t.ink() == 0);
    CHECK(g.frame(b).find("W(E,P1)") == std::string::npos);
  }
  // ---- colours ----
  {
    Regis r;                                                    // no palette: taken from the first drawing
    Drawing d;
    std::vector<Rgb> twenty;
    for (int i = 0; i < 20; i++) twenty.push_back({static_cast<uint8_t>(i * 12), static_cast<uint8_t>(255 - i * 12), 77});
    for (int i = 0; i < 20; i++) { d.set_colour(twenty[static_cast<size_t>(i)]); d.line({0, i * 5.0}, {50, i * 5.0}); }
    d.set_colour(twenty[3]);                                    // used twice, and the last colour four times: both must be kept
    d.line({0, 200}, {50, 200});
    d.set_colour(twenty[19]);
    for (int i = 0; i < 4; i++) d.line({0, 210.0 + i}, {50, 210.0 + i});
    const std::string s = r.draw(d);
    CHECK(r.palette().size() == 15);
    CHECK(std::find(r.palette().begin(), r.palette().end(), twenty[3]) != r.palette().end());
    CHECK(std::find(r.palette().begin(), r.palette().end(), twenty[19]) != r.palette().end());
    CHECK(r.palette()[0] == twenty[19] && r.palette()[1] == twenty[3]);  // the most used first
    registest::Screen scr = read(s);
    CHECK(scr.ok && scr.ink() == 25 * 51);                      // every line drawn, in some colour of the map
    // The map is fixed after the first drawing: a new colour takes the nearest one, nothing is redefined.
    Drawing e;
    e.set_colour({3, 3, 3});
    e.line({0, 0}, {10, 0});
    const std::string t = r.draw(e);
    CHECK(t.find("S(M") == std::string::npos);
    CHECK(r.palette().size() == 15);
    registest::read_into(scr, t);
    CHECK(scr.ok);
    // An empty first drawing does not fix an empty palette.
    Regis q;
    q.draw(Drawing());
    CHECK(q.palette().empty());
    q.draw(e);
    CHECK(q.palette().size() == 1);
    // More than 15 colours in the options are cut to 15.
    RegisOptions o;
    for (int i = 0; i < 40; i++) o.palette.push_back({static_cast<uint8_t>(i), 0, 0});
    CHECK(Regis(o).palette().size() == 15);
    // Hue is turned by 120 degrees, light and saturation as percent.
    Regis h = with_palette({{255, 255, 0}, {0, 255, 255}, {255, 0, 255}, {0, 0, 0}, {64, 64, 192}});
    Drawing x;
    x.line({0, 0}, {1, 0});
    const std::string m = h.draw(x);
    CHECK(m.find("S(M1(AH180L50S100)2(AH300L50S100)3(AH60L50S100)4(AH120L0S0)5(AH") != std::string::npos);
    Regis pk = with_palette({{255, 160, 160}, {40, 10, 10}});               // light (saturation from the other end) and dark
    CHECK(pk.draw(x).find("S(M1(AH120L81S100)2(AH120L10S60))") != std::string::npos);
    // A colour halfway between two palette entries takes the first.
    Regis tie = with_palette({{0, 0, 0}, {10, 0, 0}});
    Drawing ty;
    ty.set_colour({5, 0, 0});
    ty.line({0, 0}, {1, 0});
    CHECK(body(tie.draw(ty)) == "W(V,P1)\nW(I1)\nP[0,0]V[1,0]\n");
  }
  // ---- bad numbers, and plenty of random drawings ----
  {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    Drawing d(0, nan);
    d.set_thickness(nan);
    d.polyline({{nan, 5}, {10, 10}, {inf, 3}, {50, 50}, {1e300, -1e300}});
    d.set_thickness(1e9);
    d.circle(nan, 5, 10);
    d.circle(50, 50, inf);
    d.arc(50, 50, 20, nan, 90);
    d.arc(50, 50, 20, 0, inf);
    d.fill_polygon({{nan, nan}, {1e300, 1e300}, {-1e300, 5}});
    d.fill_ellipse(50, 50, -5, 1e300);
    d.fill_circle(50, 50, nan);
    d.bezier({0, 0}, {nan, 0}, {inf, inf}, {100, 100});
    d.spline({{0, 0}});
    d.spline({});
    d.set_thickness(3);
    d.polyline({{1e300, 1e300}, {-1e300, 5}, {400, 400}});
    registest::Screen s = read(Regis().draw(d));
    CHECK(s.ok);
  }
  {
    unsigned seed = 4242;
    auto rnd = [&](double lo, double hi) {
      seed = seed * 1664525u + 1013904223u;
      return lo + (hi - lo) * ((seed >> 8) / 16777216.0);
    };
    auto wild = [&]() {
      const double r = rnd(0, 1);
      if (r < 0.04) return std::numeric_limits<double>::quiet_NaN();
      if (r < 0.06) return std::numeric_limits<double>::infinity();
      if (r < 0.10) return rnd(-1e12, 1e12);
      return rnd(-300, 1100);
    };
    int bad = 0, off_rect = 0;
    for (int trial = 0; trial < 400; trial++) {
      RegisOptions o;
      o.x = static_cast<int>(rnd(0, 700)); o.y = static_cast<int>(rnd(0, 400));
      o.width = static_cast<int>(rnd(1, 800)); o.height = static_cast<int>(rnd(1, 480));
      Regis r(o);
      Drawing d(rnd(1, 900), rnd(1, 600));
      const int n = 1 + trial % 7;
      for (int i = 0; i < n; i++) {
        d.set_colour({static_cast<uint8_t>(rnd(0, 255)), static_cast<uint8_t>(rnd(0, 255)), static_cast<uint8_t>(rnd(0, 255))});
        d.set_thickness(rnd(0, 12));
        std::vector<Point> pts;
        const int k = static_cast<int>(rnd(0, 9));
        for (int j = 0; j < k; j++) pts.push_back({wild(), wild()});
        switch (static_cast<int>(rnd(0, 10))) {
          case 0: d.polyline(pts); break;
          case 1: d.polygon(pts); break;
          case 2: d.fill_polygon(pts); break;
          case 3: d.circle(wild(), wild(), std::fabs(rnd(0, 200))); break;
          case 4: d.arc(wild(), wild(), rnd(0, 300), rnd(0, 300), rnd(-400, 400), rnd(-400, 400)); break;
          case 5: d.fill_ellipse(wild(), wild(), rnd(0, 300), rnd(0, 300)); break;
          case 6: d.bezier({wild(), wild()}, {wild(), wild()}, {wild(), wild()}, {wild(), wild()}); break;
          case 7: d.spline(pts); break;
          case 8: d.fill_circle(rnd(-100, 900), rnd(-100, 600), rnd(0, 400)); break;
          default: d.fill_rectangle(rnd(-100, 900), rnd(-100, 600), rnd(0, 900), rnd(0, 600)); break;
        }
      }
      registest::Screen s;
      registest::read_into(s, r.frame(d));
      registest::read_into(s, r.frame(d));
      registest::read_into(s, r.erase(d));
      if (!s.ok) { bad++; printf("  (trial %d: %s)\n", trial, s.error.c_str()); continue; }
      registest::Screen t = read(Regis(o).draw(d));
      for (int y = 0; y < 480; y++)
        for (int x = 0; x < 800; x++)
          if (t.at(x, y) && (x < o.x || x >= o.x + o.width || y < o.y || y >= o.y + o.height)) off_rect++;
      if (!t.ok) bad++;
    }
    CHECK(bad == 0);
    CHECK(off_rect == 0);                                       // nothing outside the rectangle given
  }

  printf("%s (%d failures)\n", failures ? "FAILED" : "all ReGIS tests passed", failures);
  return failures ? 1 : 0;
}

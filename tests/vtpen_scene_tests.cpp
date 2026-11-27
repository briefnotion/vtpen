// Tests for vtpen_scene.h (Symbol, Scene, CellSender). Build and run: ./m test
// CellSender is checked by playing its output into a pretend screen and
// comparing that screen with the canvas, so a wrong cell or position shows up.

#include <cstdio>

#include "sixel_decode.h"
#include "vtpen_scene.h"

using namespace vtpen;
using sixeltest::decode_at;
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

static const Rgb BLACK{0, 0, 0}, WHITE{255, 255, 255};

// What CellSender wrote, split up: pictures with where they go, and runs of spaces.
struct Piece {
  int row, col;
  bool picture;
  int spaces;
  Decoded image;
};

static std::vector<Piece> parse(const std::string& s) {
  std::vector<Piece> v;
  int row = 0, col = 0;
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      int r = 0, c = 0;
      while (j < s.size() && isdigit(static_cast<unsigned char>(s[j]))) r = r * 10 + (s[j++] - '0');
      j++;  // ;
      while (j < s.size() && isdigit(static_cast<unsigned char>(s[j]))) c = c * 10 + (s[j++] - '0');
      row = r; col = c;
      i = j + 1;  // H
    } else if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == 'P') {
      size_t end = 0;
      Piece p{row, col, true, 0, decode_at(s, i, &end)};
      v.push_back(p);
      i = end;
    } else if (s[i] == ' ') {
      int n = 0;
      while (i < s.size() && s[i] == ' ') { n++; i++; }
      v.push_back({row, col, false, n, {}});
    } else {
      i++;
    }
  }
  return v;
}

// A pretend terminal screen: pictures replace the pixels of the cells they cover.
struct Screen {
  int w, h, cw, ch, top, left;
  std::vector<Rgb> px;
  Screen(int W, int H, int CW, int CH, int T, int L) : w(W), h(H), cw(CW), ch(CH), top(T), left(L), px(static_cast<size_t>(W) * H) {}
  void apply(const std::string& out) {
    for (const Piece& p : parse(out)) {
      if (!p.picture) continue;  // spaces: an opaque canvas never needs them
      const int x0 = (p.col - left) * cw, y0 = (p.row - top) * ch;
      for (int y = 0; y < p.image.h; y++)
        for (int x = 0; x < p.image.w; x++) {
          const size_t k = static_cast<size_t>(y) * p.image.w + x;
          if (p.image.drawn[k] && x0 + x < w && y0 + y < h) px[static_cast<size_t>(y0 + y) * w + x0 + x] = p.image.pix[k];
        }
    }
  }
};

static bool screen_matches(const Screen& s, const Canvas& c, int tolerance = 3) {
  for (int y = 0; y < c.height(); y++)
    for (int x = 0; x < c.width(); x++)
      if (dist(s.px[static_cast<size_t>(y) * c.width() + x], c.pixel(x, y)) > tolerance) return false;
  return true;
}

int main() {
  // ---- Scene: placing ----
  {
    Symbol dot;
    dot.fill_circle(10, 0, 3);   // a dot 10 units to the right of the centre
    Canvas c(100, 100);
    c.set_edge_levels(0);
    Scene sc;
    int id = sc.add(dot, 50, 50);
    sc.render(c);
    CHECK(c.pixel(60, 50) == WHITE);
    CHECK(c.pixel(50, 60) == BLACK);
    sc.rotate(id, 90);           // clockwise on the screen: right becomes down
    sc.render(c);
    CHECK(c.pixel(50, 60) == WHITE);
    CHECK(c.pixel(60, 50) == BLACK);   // the old place is clear again
    sc.rotate(id, 180);
    sc.render(c);
    CHECK(c.pixel(40, 50) == WHITE);
    sc.rotate(id, 0);
    sc.scale(id, 2);             // twice as far out and twice as big
    sc.render(c);
    CHECK(c.pixel(70, 50) == WHITE);
    CHECK(c.pixel(73, 50).r > 100);    // radius 6, not 3
    CHECK(c.pixel(60, 50) == BLACK);
    sc.scale(id, 1);
    sc.move(id, 20, 80);
    sc.render(c);
    CHECK(c.pixel(30, 80) == WHITE);
    CHECK(c.pixel(60, 50) == BLACK);
  }
  // ---- Scene: curves turn with their object ----
  {
    Symbol a;
    a.set_thickness(3);
    a.arc(0, 0, 20, 0, 90);      // from the right round to the bottom
    Canvas c(100, 100);
    c.set_edge_levels(0);
    Scene sc;
    int id = sc.add(a, 50, 50);
    sc.render(c);
    CHECK(c.pixel(69, 50).r > 100 && c.pixel(50, 69).r > 100);
    CHECK(c.pixel(30, 50) == BLACK && c.pixel(50, 30) == BLACK);
    sc.rotate(id, 90);
    sc.render(c);
    CHECK(c.pixel(50, 69).r > 100 && c.pixel(30, 50).r > 100);   // now bottom to left
    CHECK(c.pixel(69, 50) == BLACK);
  }
  // ---- Scene: colour ----
  {
    Symbol s;
    s.fill_rectangle(-5, -5, 10, 10);          // no colour of its own
    s.set_colour({255, 0, 0});
    s.fill_rectangle(10, -5, 10, 10);          // red whatever the object's colour
    Canvas c(60, 30);
    c.set_edge_levels(0);
    Scene sc;
    int id = sc.add(s, 15, 15);
    sc.render(c);
    CHECK(c.pixel(15, 15) == WHITE);           // the object's default colour
    CHECK(c.pixel(30, 15) == (Rgb{255, 0, 0}));
    sc.set_colour(id, {0, 0, 255});
    sc.render(c);
    CHECK(c.pixel(15, 15) == (Rgb{0, 0, 255}));
    CHECK(c.pixel(30, 15) == (Rgb{255, 0, 0}));
  }
  // ---- Scene: order, visibility, removal ----
  {
    Symbol red, blue;
    red.set_colour({255, 0, 0});
    red.fill_rectangle(0, 0, 20, 20);
    blue.set_colour({0, 0, 255});
    blue.fill_rectangle(0, 0, 20, 20);
    Canvas c(60, 40);
    c.set_edge_levels(0);
    Scene sc;
    int r = sc.add(red, 10, 10);
    int b = sc.add(blue, 20, 10);      // overlaps red, added later: on top
    sc.render(c);
    CHECK(c.pixel(25, 15) == (Rgb{0, 0, 255}));
    CHECK(c.pixel(12, 15) == (Rgb{255, 0, 0}));
    sc.set_layer(r, 5);                // a higher layer goes on top whatever the order
    sc.render(c);
    CHECK(c.pixel(25, 15) == (Rgb{255, 0, 0}));
    sc.show(r, false);
    sc.render(c);
    CHECK(c.pixel(25, 15) == (Rgb{0, 0, 255}));
    CHECK(c.pixel(12, 15) == BLACK);
    sc.show(r, true);
    CHECK(sc.size() == 2);
    CHECK(sc.remove(b));
    CHECK(!sc.remove(b));
    CHECK(!sc.exists(b) && sc.exists(r));
    CHECK(sc.size() == 1);
    sc.render(c);
    CHECK(c.pixel(25, 15) == (Rgb{255, 0, 0}));
    CHECK(c.pixel(35, 15) == BLACK);
    CHECK(!sc.move(b, 1, 1) && !sc.rotate(b, 1) && !sc.scale(b, 1) && !sc.set_colour(b, WHITE) && !sc.show(b, true));
    Scene empty;
    empty.render(c);
    CHECK(c.pixel(15, 15) == BLACK);   // nothing in the scene: a clear canvas
  }
  // One symbol can be placed many times.
  {
    Symbol d;
    d.fill_circle(0, 0, 4);
    Canvas c(60, 20);
    c.set_edge_levels(0);
    Scene sc;
    auto shared = std::make_shared<const Symbol>(d);
    for (int i = 0; i < 5; i++) sc.add(shared, 6 + i * 12, 10);
    sc.render(c);
    for (int i = 0; i < 5; i++) CHECK(c.pixel(6 + i * 12, 10) == WHITE);
    CHECK(c.pixel(12, 10) == BLACK);
  }

  // ---- Scene: patterns ----
  {
    auto lines = std::make_shared<const Pattern>(Pattern::horizontal(4, 1));
    Symbol hatch;
    hatch.set_fill_pattern(lines);
    hatch.fill_rectangle(-20, -10, 40, 20);
    const long with_symbol = lines.use_count();
    CHECK(with_symbol > 1);              // the symbol holds on to the pattern (it never copies it)
    Canvas c(100, 60);
    c.set_edge_levels(0);
    Scene sc;
    int id = sc.add(hatch, 30, 30);
    sc.add(hatch, 75, 30);
    CHECK(lines.use_count() > with_symbol);   // the objects hold on to the same one too
    sc.render(c);
    // Filled lines on rows that are multiples of 4, inside the rectangles only.
    for (int y = 0; y < 60; y++) {
      const bool inside = y >= 20 && y < 40;
      CHECK((c.pixel(30, y) == WHITE) == (inside && y % 4 == 0));
      CHECK((c.pixel(75, y) == WHITE) == (inside && y % 4 == 0));
    }
    // Turning the object turns the shape but not the lines: the pattern stays with the picture.
    sc.rotate(id, 90);
    sc.render(c);
    for (int y = 0; y < 60; y++) {
      const bool inside = y >= 10 && y < 50;      // now 20 wide, 40 tall, centred at 30
      CHECK((c.pixel(30, y) == WHITE) == (inside && y % 4 == 0));
    }
    CHECK(c.pixel(22, 12) == WHITE);              // a row-12 pixel inside the turned shape
    CHECK(c.pixel(22, 13) == BLACK);
    // A symbol with no pattern fills solid, and lines are solid whatever the pattern.
    Symbol plain;
    plain.fill_rectangle(-5, -5, 10, 10);
    plain.set_fill_pattern(lines);
    plain.line({-20, 0.5}, {20, 0.5});
    Scene s2;
    s2.add(plain, 50, 30);
    Canvas d(100, 60);
    d.set_edge_levels(0);
    s2.render(d);
    CHECK(d.pixel(35, 30) == WHITE);              // the line is solid on a non-pattern row
  }
  // ---- CellSender ----
  {
    // Cells are 10x20, the picture is 40x40: 4 columns by 2 rows. Terminal position (3, 2).
    Canvas c(40, 40);
    c.set_background({0, 0, 0});
    c.set_transparent(false);
    c.clear();
    c.set_colour({200, 100, 50});
    c.fill_rectangle(5, 5, 30, 10);
    CellSender tx(10, 20);
    Screen screen(40, 40, 10, 20, 3, 2);

    std::string first = tx.update(c, 3, 2);
    CHECK(!first.empty());
    screen.apply(first);
    CHECK(screen_matches(screen, c));
    CHECK(tx.changed().size() == 8);

    CHECK(tx.update(c, 3, 2).empty());      // nothing changed: nothing sent
    CHECK(tx.changed().empty());

    // One pixel in cell (2, 1) changes: only that cell is sent, at the right place.
    c.set_colour({10, 200, 10});
    c.fill_rectangle(22, 25, 1, 1);
    std::string one = tx.update(c, 3, 2);
    std::vector<Piece> pieces = parse(one);
    CHECK(pieces.size() == 1);
    if (pieces.size() == 1) {
      CHECK(pieces[0].picture);
      CHECK(pieces[0].row == 3 + 1 && pieces[0].col == 2 + 2);   // row 1, column 2 of the picture
      CHECK(pieces[0].image.w == 10 && pieces[0].image.h == 20);
    }
    CHECK(tx.changed().size() == 1 && tx.changed()[0] == std::make_pair(2, 1));
    screen.apply(one);
    CHECK(screen_matches(screen, c));

    // Two neighbouring cells in one row go together as one picture; a separate one is its own.
    c.set_colour({255, 255, 0});
    c.fill_rectangle(2, 2, 1, 1);    // cell (0, 0)
    c.fill_rectangle(12, 2, 1, 1);   // cell (1, 0)
    c.fill_rectangle(32, 2, 1, 1);   // cell (3, 0)
    std::string runs = tx.update(c, 3, 2);
    pieces = parse(runs);
    CHECK(pieces.size() == 2);
    if (pieces.size() == 2) {
      CHECK(pieces[0].image.w == 20 && pieces[0].col == 2);
      CHECK(pieces[1].image.w == 10 && pieces[1].col == 2 + 3);
    }
    screen.apply(runs);
    CHECK(screen_matches(screen, c));

    // After redraw() everything is sent again, even though nothing changed.
    tx.redraw();
    std::string again = tx.update(c, 3, 2);
    CHECK(!again.empty());
    CHECK(tx.changed().size() == 8);
    CHECK(tx.update(c, 3, 2).empty());

    // A new cell size (the terminal was resized, the font changed) starts over, cut up the new way.
    CHECK(tx.cell_width() == 10 && tx.cell_height() == 20);
    tx.set_cell_size(5, 10);
    CHECK(tx.cell_width() == 5 && tx.cell_height() == 10);
    std::string resized = tx.update(c, 3, 2);
    CHECK(!resized.empty());
    CHECK(tx.changed().size() == 8 * 4);                 // 40x40 in 5x10 cells is 8 columns by 4 rows
    Screen small(40, 40, 5, 10, 3, 2);
    small.apply(resized);
    CHECK(screen_matches(small, c));
    CHECK(tx.update(c, 3, 2).empty());                   // and steady again afterwards
    c.set_colour({200, 10, 200});
    c.fill_rectangle(7, 3, 1, 1);                        // in the new cell at column 1, row 0
    std::vector<Piece> after = parse(tx.update(c, 3, 2));
    CHECK(after.size() == 1);
    if (after.size() == 1) CHECK(after[0].image.w == 5 && after[0].image.h == 10 && after[0].col == 2 + 1 && after[0].row == 3 + 0);
    tx.set_cell_size(0, -4);                             // nonsense counts as 1x1
    CHECK(tx.cell_width() == 1 && tx.cell_height() == 1);
    tx.set_cell_size(10, 20);
    // A different size starts over.
    Canvas big(50, 40);
    big.set_transparent(false);
    big.clear();
    CHECK(!tx.update(big, 3, 2).empty());
  }
  // A picture whose size is not a whole number of cells: the edge cells are smaller.
  {
    Canvas c(25, 30);
    c.set_transparent(false);
    c.clear();
    CellSender tx(10, 20);
    Screen screen(25, 30, 10, 20, 1, 1);
    screen.apply(tx.update(c, 1, 1));
    c.set_colour({255, 0, 0});
    c.fill_rectangle(24, 29, 1, 1);
    std::string s = tx.update(c, 1, 1);
    std::vector<Piece> p = parse(s);
    CHECK(p.size() == 1);
    if (p.size() == 1) CHECK(p[0].image.w == 5 && p[0].image.h == 10 && p[0].row == 2 && p[0].col == 3);
    screen.apply(s);
    CHECK(screen_matches(screen, c));
  }
  // A transparent canvas has its cells wiped with spaces before the pictures go in.
  {
    Canvas c(40, 40);                    // transparent by default
    c.fill_rectangle(5, 5, 20, 10);
    CellSender tx(10, 20);
    tx.update(c, 1, 1);
    c.set_colour({255, 0, 0});
    c.fill_rectangle(12, 25, 3, 3);      // cell (1, 1)
    std::vector<Piece> p = parse(tx.update(c, 1, 1));
    CHECK(p.size() == 2);
    if (p.size() == 2) {
      CHECK(!p[0].picture && p[0].spaces == 1 && p[0].row == 2 && p[0].col == 2);
      CHECK(p[1].picture && p[1].row == 2 && p[1].col == 2);
    }
  }
  // Most of the picture changing is sent as one picture, not cell by cell.
  {
    Canvas c(40, 40);
    c.set_transparent(false);
    c.clear();
    CellSender tx(10, 20);
    tx.update(c, 1, 1);
    c.set_colour({0, 90, 200});
    c.fill_rectangle(0, 0, 40, 40);
    std::vector<Piece> p = parse(tx.update(c, 1, 1));
    CHECK(p.size() == 1);
    if (p.size() == 1) CHECK(p[0].image.w == 40 && p[0].image.h == 40 && p[0].row == 1 && p[0].col == 1);
  }

  // ---- The whole thing together: an arrow turning over a map, played into a pretend screen ----
  {
    Symbol arrow;
    arrow.set_colour({255, 140, 40});
    arrow.fill_polygon({{-40, -10}, {10, -10}, {10, -25}, {45, 0}, {10, 25}, {10, 10}, {-40, 10}});
    Symbol road;
    road.set_colour({90, 200, 120});
    road.set_thickness(3);
    road.polyline({{0, 120}, {200, 150}, {400, 60}, {599, 100}});
    road.set_colour({70, 140, 255});
    road.polyline({{300, 0}, {320, 200}, {300, 399}});

    Canvas c(600, 400);
    c.set_background({0, 0, 0});
    c.set_transparent(false);
    Scene sc;
    sc.add(road, 0, 0, 0, 1, 0);
    int a = sc.add(arrow, 300, 150, 0, 1, 1);
    CellSender tx(10, 20);
    Screen screen(600, 400, 10, 20, 5, 4);
    size_t full_bytes = 0, step_bytes = 0;
    int steps = 0;
    for (int deg = 0; deg <= 360; deg += 15) {
      sc.rotate(a, deg);
      if (deg % 60 == 0) sc.move(a, 300 + deg / 2.0, 150 - deg / 6.0);
      sc.render(c);
      std::string out = tx.update(c, 5, 4);
      if (deg == 0) full_bytes = out.size(); else { step_bytes += out.size(); steps++; }
      screen.apply(out);
      CHECK(screen_matches(screen, c));          // after every step the screen shows the canvas
    }
    CHECK(steps == 24);
    CHECK(step_bytes / static_cast<size_t>(steps) < full_bytes / 3);   // a step is much cheaper than the first picture
  }

  if (failures == 0) printf("vtpen scene: all tests passed\n");
  return failures ? 1 : 0;
}

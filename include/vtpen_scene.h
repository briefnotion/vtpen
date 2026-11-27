// vtpen scene: keep what is drawn as a list of objects, so any of them can be moved,
// turned, hidden or removed later, and send only the cells that changed.
//
//   vtpen::Symbol arrow;                         // drawn once, around its own centre (0, 0)
//   arrow.set_colour({255, 200, 0});               // optional: leave it out and each object picks its own
//   arrow.fill_polygon({{-40, -10}, {10, -10}, {10, -25}, {45, 0}, {10, 25}, {10, 10}, {-40, 10}});
//
//   vtpen::Scene scene;
//   int id = scene.add(arrow, 400, 240);           // placed at (400, 240)
//   scene.rotate(id, 30);                          // turned 30 degrees clockwise
//
//   vtpen::Canvas canvas(900, 540, 800, 480);
//   scene.render(canvas);                          // clear, then draw every object in order
//
//   vtpen::CellSender sender(10, 20);            // one character cell is 10x20 pixels
//   write(fd, sender.update(canvas, 3, 2));        // only the cells that differ from last time
//
// Header only, no terminal calls except the strings CellSender returns.

#pragma once

#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vtpen.h"

namespace vtpen {

// ---- Symbol: a reusable drawing -------------------------------------------------

class Symbol {
 public:
  // The colour and thickness the next command uses. Without a colour, a command
  // takes the colour of the object it ends up in, so one symbol can be red here
  // and blue there.
  void set_colour(Rgb c) { colour_ = c; }
  void inherit_colour() { colour_.reset(); }
  void set_thickness(double pixels) { thickness_ = pixels; }
  // What the next filled shapes are filled with (null: solid colour). One
  // pattern can be shared by any number of symbols.
  void set_fill_pattern(std::shared_ptr<const Pattern> p) { pattern_ = std::move(p); }

  void line(Point a, Point b) { polyline({a, b}); }
  void polyline(const std::vector<Point>& pts) { add(Kind::Polyline, pts); }
  void polygon(const std::vector<Point>& pts) { add(Kind::Polygon, pts); }
  void fill_polygon(const std::vector<Point>& pts) { add(Kind::FillPolygon, pts); }
  void rectangle(double x, double y, double w, double h) { polygon({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}); }
  void fill_rectangle(double x, double y, double w, double h) { fill_polygon({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}); }
  void circle(double cx, double cy, double r) { ellipse(cx, cy, r, r); }
  void fill_circle(double cx, double cy, double r) { fill_ellipse(cx, cy, r, r); }
  void ellipse(double cx, double cy, double rx, double ry) { arc(cx, cy, rx, ry, 0, 360); }
  void fill_ellipse(double cx, double cy, double rx, double ry) {
    Command c = make(Kind::FillEllipse, {{cx, cy}});
    c.rx = rx; c.ry = ry;
    commands_.push_back(c);
  }
  void arc(double cx, double cy, double r, double start_deg, double end_deg) { arc(cx, cy, r, r, start_deg, end_deg); }
  void arc(double cx, double cy, double rx, double ry, double start_deg, double end_deg) {
    Command c = make(Kind::Arc, {{cx, cy}});
    c.rx = rx; c.ry = ry; c.a0 = start_deg; c.a1 = end_deg;
    commands_.push_back(c);
  }
  void bezier(Point p0, Point c0, Point c1, Point p1) { add(Kind::Bezier, {p0, c0, c1, p1}); }
  void spline(const std::vector<Point>& pts) { add(Kind::Spline, pts); }

  size_t size() const { return commands_.size(); }

 private:
  friend class Scene;
  enum class Kind { Polyline, Polygon, FillPolygon, Arc, FillEllipse, Bezier, Spline };
  struct Command {
    Kind kind;
    std::vector<Point> pts;
    std::optional<Rgb> colour;
    double thickness = 1;
    std::shared_ptr<const Pattern> pattern;
    double rx = 0, ry = 0, a0 = 0, a1 = 0;
  };

  Command make(Kind k, std::vector<Point> pts) const {
    Command c;
    c.kind = k;
    c.pts = std::move(pts);
    c.colour = colour_;
    c.thickness = thickness_;
    c.pattern = pattern_;
    return c;
  }
  void add(Kind k, std::vector<Point> pts) { commands_.push_back(make(k, std::move(pts))); }

  std::vector<Command> commands_;
  std::optional<Rgb> colour_;
  double thickness_ = 1;
  std::shared_ptr<const Pattern> pattern_;
};

// ---- Scene: placed, movable, turnable objects ---------------------------------------

class Scene {
 public:
  using Id = int;

  // Put a symbol in the scene. x, y is where its centre (0, 0) goes, rotation is
  // in degrees clockwise, scale multiplies its size. Objects with a higher layer
  // are drawn on top; the same layer is drawn in the order added.
  Id add(const Symbol& symbol, double x, double y, double rotation_deg = 0, double scale = 1, int layer = 0) {
    return add(std::make_shared<const Symbol>(symbol), x, y, rotation_deg, scale, layer);
  }
  Id add(std::shared_ptr<const Symbol> symbol, double x, double y, double rotation_deg = 0, double scale = 1, int layer = 0) {
    Object o;
    o.id = next_++;
    o.symbol = std::move(symbol);
    o.x = x; o.y = y; o.rotation = rotation_deg; o.scale = scale; o.layer = layer;
    objects_.push_back(std::move(o));
    return objects_.back().id;
  }

  bool exists(Id id) const { return find(id) != nullptr; }
  size_t size() const { return objects_.size(); }

  // Each returns false when there is no such object.
  bool move(Id id, double x, double y) { Object* o = find(id); if (!o) return false; o->x = x; o->y = y; return true; }
  bool rotate(Id id, double degrees) { Object* o = find(id); if (!o) return false; o->rotation = degrees; return true; }
  bool scale(Id id, double s) { Object* o = find(id); if (!o) return false; o->scale = s; return true; }
  bool set_colour(Id id, Rgb c) { Object* o = find(id); if (!o) return false; o->colour = c; return true; }
  bool set_layer(Id id, int layer) { Object* o = find(id); if (!o) return false; o->layer = layer; return true; }
  bool show(Id id, bool visible) { Object* o = find(id); if (!o) return false; o->visible = visible; return true; }
  bool remove(Id id) {
    for (size_t i = 0; i < objects_.size(); i++)
      if (objects_[i].id == id) { objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(i)); return true; }
    return false;
  }

  // Clear the canvas and draw every visible object, lowest layer first. The
  // canvas's colour and thickness settings are left changed.
  void render(Canvas& canvas) const {
    canvas.clear();
    std::vector<const Object*> order;
    for (const Object& o : objects_) if (o.visible) order.push_back(&o);
    std::stable_sort(order.begin(), order.end(), [](const Object* a, const Object* b) { return a->layer < b->layer; });
    for (const Object* o : order) draw(canvas, *o);
  }

 private:
  struct Object {
    Id id = 0;
    std::shared_ptr<const Symbol> symbol;
    double x = 0, y = 0, rotation = 0, scale = 1;
    int layer = 0;
    bool visible = true;
    Rgb colour{255, 255, 255};
  };

  Object* find(Id id) { for (Object& o : objects_) if (o.id == id) return &o; return nullptr; }
  const Object* find(Id id) const { for (const Object& o : objects_) if (o.id == id) return &o; return nullptr; }

  static void draw(Canvas& canvas, const Object& o) {
    const double pi = 3.14159265358979;
    const double ca = std::cos(o.rotation * pi / 180), sa = std::sin(o.rotation * pi / 180);
    auto place = [&](Point p) {
      const double x = p.x * o.scale, y = p.y * o.scale;
      return Point{o.x + x * ca - y * sa, o.y + x * sa + y * ca};
    };
    auto place_all = [&](const std::vector<Point>& pts) {
      std::vector<Point> r;
      r.reserve(pts.size());
      for (const Point& p : pts) r.push_back(place(p));
      return r;
    };
    // Curves get as many pieces as they need at their final size.
    const double px = canvas.pixels_per_unit_x() * o.scale, py = canvas.pixels_per_unit_y() * o.scale;

    for (const Symbol::Command& c : o.symbol->commands_) {
      canvas.set_colour(c.colour.value_or(o.colour));
      canvas.set_thickness(c.thickness);
      canvas.set_fill_pattern(c.pattern);
      switch (c.kind) {
        case Symbol::Kind::Polyline: canvas.polyline(place_all(c.pts)); break;
        case Symbol::Kind::Polygon: {
          std::vector<Point> p = place_all(c.pts);
          if (!p.empty()) p.push_back(p[0]);
          canvas.polyline(p);
          break;
        }
        case Symbol::Kind::FillPolygon: canvas.fill_polygon(place_all(c.pts)); break;
        case Symbol::Kind::Arc:
          canvas.polyline(place_all(arc_path(c.pts[0].x, c.pts[0].y, c.rx, c.ry, c.a0, c.a1, px, py)));
          break;
        case Symbol::Kind::FillEllipse:
          canvas.fill_polygon(place_all(arc_path(c.pts[0].x, c.pts[0].y, c.rx, c.ry, 0, 360, px, py)));
          break;
        case Symbol::Kind::Bezier: {
          std::vector<Point> p{c.pts[0]};
          append_bezier_path(p, c.pts[0], c.pts[1], c.pts[2], c.pts[3], px, py);
          canvas.polyline(place_all(p));
          break;
        }
        case Symbol::Kind::Spline: canvas.polyline(place_all(spline_path(c.pts, px, py))); break;
      }
    }
  }

  std::vector<Object> objects_;
  Id next_ = 1;
};

// ---- CellSender: send only what changed -------------------------------------------------
//
// The terminal shows a Sixel picture in character cells, and writing into a cell
// replaces that cell's pixels. So the canvas is cut into cells, each is compared
// with what was sent last time, and only the cells that differ are sent, with
// neighbours in a row sent together as one small picture.

class CellSender {
 public:
  CellSender(int cell_w, int cell_h) : cw_(std::max(1, cell_w)), ch_(std::max(1, cell_h)) {}

  // Bytes that make the terminal show `canvas` with its top left corner at the
  // 1-based terminal row and column given. An empty string means nothing changed.
  // If the canvas has a transparent background the cells are first wiped with
  // spaces; an opaque canvas replaces them directly.
  std::string update(const Canvas& canvas, int term_row, int term_col) {
    changed_.clear();
    const int w = canvas.width(), h = canvas.height();
    const int cols = (w + cw_ - 1) / cw_, rows = (h + ch_ - 1) / ch_;
    if (w != sent_w_ || h != sent_h_) {  // a new size: nothing sent so far is valid
      sent_.assign(static_cast<size_t>(w) * h, Rgb{});
      known_.assign(static_cast<size_t>(cols) * rows, 0);
      sent_w_ = w; sent_h_ = h;
    }

    // Which cells differ from what the terminal shows?
    std::vector<char> dirty(static_cast<size_t>(cols) * rows, 0);
    size_t dirty_count = 0;
    for (int r = 0; r < rows; r++) {
      for (int c = 0; c < cols; c++) {
        const int x0 = c * cw_, y0 = r * ch_, x1 = std::min(w, x0 + cw_), y1 = std::min(h, y0 + ch_);
        bool same = known_[static_cast<size_t>(r) * cols + c] != 0;
        for (int y = y0; same && y < y1; y++)
          same = std::memcmp(canvas.row(y) + x0, &sent_[static_cast<size_t>(y) * w + x0], static_cast<size_t>(x1 - x0) * sizeof(Rgb)) == 0;
        if (!same) { dirty[static_cast<size_t>(r) * cols + c] = 1; dirty_count++; changed_.push_back({c, r}); }
      }
    }
    if (dirty_count == 0) return "";

    std::string out;
    if (dirty_count * 10 > static_cast<size_t>(cols) * rows * 6) {  // most of it changed: one picture
      if (canvas.transparent()) wipe(out, term_row, term_col, cols, rows);
      out += at(term_row, term_col) + canvas.sixel();
    } else {
      for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols;) {
          if (!dirty[static_cast<size_t>(r) * cols + c]) { c++; continue; }
          int end = c;
          while (end < cols && dirty[static_cast<size_t>(r) * cols + end]) end++;  // a run of changed cells
          if (canvas.transparent()) out += at(term_row + r, term_col + c) + std::string(static_cast<size_t>(end - c), ' ');
          out += at(term_row + r, term_col + c) + canvas.sixel(c * cw_, r * ch_, (end - c) * cw_, ch_);
          c = end;
        }
      }
    }

    // Remember what was sent.
    for (int y = 0; y < h; y++) std::memcpy(&sent_[static_cast<size_t>(y) * w], canvas.row(y), static_cast<size_t>(w) * sizeof(Rgb));
    std::fill(known_.begin(), known_.end(), 1);
    return out;
  }

  // Ask for a full redraw: the next update sends the whole picture again, even
  // though nothing changed. Call it whenever the terminal may no longer show what
  // was sent, for example after the window was resized (SIGWINCH), the screen was
  // cleared or something else wrote over the picture.
  void redraw() { sent_w_ = sent_h_ = -1; }

  // The size of a character cell in pixels, if the terminal now says it is
  // different (the window was resized, the font changed). Implies redraw().
  void set_cell_size(int cell_w, int cell_h) {
    cw_ = std::max(1, cell_w);
    ch_ = std::max(1, cell_h);
    redraw();
  }

  int cell_width() const { return cw_; }
  int cell_height() const { return ch_; }

  // The cells the last update sent, as (column, row) inside the picture, counting
  // from 0. Text written over those cells has to be written again.
  const std::vector<std::pair<int, int>>& changed() const { return changed_; }

 private:
  static std::string at(int row, int col) { return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H"; }
  static void wipe(std::string& out, int row, int col, int cols, int rows) {
    for (int r = 0; r < rows; r++) out += at(row + r, col) + std::string(static_cast<size_t>(cols), ' ');
  }

  int cw_, ch_;
  int sent_w_ = -1, sent_h_ = -1;
  std::vector<Rgb> sent_;
  std::vector<char> known_;
  std::vector<std::pair<int, int>> changed_;
};

}  // namespace vtpen

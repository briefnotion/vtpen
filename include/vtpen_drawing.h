// vtpen drawing: describe a picture as a list of shapes, then let a backend show it.
//
//   vtpen::Drawing d(800, 480);               // you draw in 800x480 units
//   d.set_colour({255, 200, 0});
//   d.set_thickness(2);
//   d.polyline({{100, 100}, {400, 380}, {700, 100}});
//   d.fill_circle(400, 240, 60);
//
//   vtpen::Canvas canvas = d.render(700, 420);   // Sixel: replay onto a 700x420 pixel canvas
//   std::string picture = canvas.sixel();
//
// Unlike Canvas, which turns every shape into pixels at once, a Drawing keeps the
// shapes as they were asked for. That is what lets a second backend (ReGIS, which
// takes lines and curves, not pixels) show the same drawing. This header has the
// list and the Sixel backend only.
//
// What a Drawing can say is what every backend can do: lines, outlines, arcs and
// curves, solid fills, one colour and one thickness per shape. There are no fill
// patterns and no pixels to read back; use Canvas for those.
//
// Header only, no terminal calls.

#pragma once

#include <cmath>
#include <utility>
#include <vector>

#include "vtpen.h"

namespace vtpen {

class Drawing {
 public:
  // The size of the space you draw in. Not a positive number counts as the default, 800x480.
  explicit Drawing(double width = 800, double height = 480)
      : w_(std::isfinite(width) && width > 0 ? width : 800), h_(std::isfinite(height) && height > 0 ? height : 480) {}

  double width() const { return w_; }
  double height() const { return h_; }

  // What the next shapes use. Thickness is in pixels, as in Canvas.
  void set_colour(Rgb c) { colour_ = c; }
  void set_thickness(double pixels) { thickness_ = pixels; }

  void line(Point a, Point b) { polyline({a, b}); }
  void polyline(const std::vector<Point>& pts) { add(Kind::Polyline, pts); }
  void polygon(const std::vector<Point>& pts) { add(Kind::Polygon, pts); }  // outline, closes itself
  void rectangle(double x, double y, double w, double h) { polygon({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}); }
  void circle(double cx, double cy, double r) { arc(cx, cy, r, r, 0, 360); }
  void ellipse(double cx, double cy, double rx, double ry) { arc(cx, cy, rx, ry, 0, 360); }
  void arc(double cx, double cy, double r, double start_deg, double end_deg) { arc(cx, cy, r, r, start_deg, end_deg); }
  void arc(double cx, double cy, double rx, double ry, double start_deg, double end_deg) {
    Command c = make(Kind::Arc, {{cx, cy}});
    c.rx = rx; c.ry = ry; c.a0 = start_deg; c.a1 = end_deg;
    commands_.push_back(std::move(c));
  }
  void bezier(Point p0, Point c0, Point c1, Point p1) { add(Kind::Bezier, {p0, c0, c1, p1}); }
  void spline(const std::vector<Point>& pts) { add(Kind::Spline, pts); }

  // Filled, even-odd rule (the middle of a star is a hole), as in Canvas.
  void fill_polygon(const std::vector<Point>& pts) { add(Kind::FillPolygon, pts); }
  void fill_rectangle(double x, double y, double w, double h) { fill_polygon({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}); }
  void fill_circle(double cx, double cy, double r) { fill_ellipse(cx, cy, r, r); }
  void fill_ellipse(double cx, double cy, double rx, double ry) {
    Command c = make(Kind::FillEllipse, {{cx, cy}});
    c.rx = rx; c.ry = ry;
    commands_.push_back(std::move(c));
  }

  // Forget every shape. The colour and thickness stay as they were.
  void clear() { commands_.clear(); }
  size_t size() const { return commands_.size(); }
  bool empty() const { return commands_.empty(); }

  // ---- Sixel backend ----------------------------------------------------------

  // Draw every shape, in the order they were added, on top of what the canvas
  // already holds (call canvas.clear() first for a fresh picture). The drawing's
  // units are stretched to the canvas's own drawing space, so a Drawing of 800x480
  // fills a canvas made as Canvas(px, py, 400, 240) just the same. Like
  // Scene::render, this leaves the canvas's colour, thickness and fill pattern changed.
  void draw(Canvas& canvas) const {
    const double kx = canvas.width() / (canvas.pixels_per_unit_x() * w_);
    const double ky = canvas.height() / (canvas.pixels_per_unit_y() * h_);
    auto at = [&](Point p) { return Point{p.x * kx, p.y * ky}; };
    auto all = [&](const std::vector<Point>& pts) {
      std::vector<Point> r;
      r.reserve(pts.size());
      for (const Point& p : pts) r.push_back(at(p));
      return r;
    };

    canvas.set_fill_pattern(nullptr);
    for (const Command& c : commands_) {
      canvas.set_colour(c.colour);
      canvas.set_thickness(c.thickness);
      switch (c.kind) {
        case Kind::Polyline: canvas.polyline(all(c.pts)); break;
        case Kind::Polygon: canvas.polygon(all(c.pts)); break;
        case Kind::FillPolygon: canvas.fill_polygon(all(c.pts)); break;
        case Kind::Arc: {
          const Point m = at(c.pts[0]);
          canvas.arc(m.x, m.y, c.rx * kx, c.ry * ky, c.a0, c.a1);
          break;
        }
        case Kind::FillEllipse: {
          const Point m = at(c.pts[0]);
          canvas.fill_ellipse(m.x, m.y, c.rx * kx, c.ry * ky);
          break;
        }
        case Kind::Bezier: {
          const std::vector<Point> p = all(c.pts);
          canvas.bezier(p[0], p[1], p[2], p[3]);
          break;
        }
        case Kind::Spline: canvas.spline(all(c.pts)); break;
      }
    }
  }

  // A new canvas of the given size (pixels), covering this drawing's whole space,
  // with the drawing on it, using Canvas's defaults. To change the edge softness,
  // background or transparency, make the Canvas yourself, set those, then call draw().
  Canvas render(int pixel_w, int pixel_h) const {
    Canvas canvas(pixel_w, pixel_h, w_, h_);
    draw(canvas);
    return canvas;
  }

 private:
  friend class Regis;  // vtpen_regis.h reads the shapes
  enum class Kind { Polyline, Polygon, FillPolygon, Arc, FillEllipse, Bezier, Spline };
  struct Command {
    Kind kind;
    std::vector<Point> pts;
    Rgb colour;
    double thickness = 1;
    double rx = 0, ry = 0, a0 = 0, a1 = 0;
  };

  Command make(Kind k, std::vector<Point> pts) const {
    Command c;
    c.kind = k;
    c.pts = std::move(pts);
    c.colour = colour_;
    c.thickness = thickness_;
    return c;
  }
  void add(Kind k, std::vector<Point> pts) { commands_.push_back(make(k, std::move(pts))); }

  double w_, h_;
  Rgb colour_{255, 255, 255};
  double thickness_ = 1;
  std::vector<Command> commands_;
};

}  // namespace vtpen

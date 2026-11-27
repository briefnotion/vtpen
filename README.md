# vtpen

Draw vector shapes in a terminal. You describe a drawing (lines, circles, curves,
filled shapes, patterns); the library turns it into a Sixel picture and sends only
the parts of the screen that changed. Header-only, standard C++17, no dependencies,
no terminal calls of its own.

Two headers:

| Header | What it gives you |
|---|---|
| `include/vtpen.h` | `Canvas` (draw shapes, get Sixel), `Pattern` (fill tiles) |
| `include/vtpen_scene.h` | `Symbol` and `Scene` (objects you can move, turn and remove), `CellSender` (send only what changed) |
| `include/vtpen_probe.h` | `Probe` and `Capabilities`: ask the terminal whether it can show pictures (no input or output of its own) |
| `include/vtpen_probe_posix.h` | `probe_terminal()`: does the asking and reading on a Unix terminal |

Also in this folder: `tools/vtprobe.cpp`, a program you run inside a terminal to see
what it supports and to try the library (ten pages), and `tests/`.

## Quick start

```cpp
#include "vtpen.h"

vtpen::Canvas c(700, 420, 800, 480);      // 700x420 pixels, you draw in 800x480 units
c.set_colour({255, 200, 0});
c.set_thickness(3);
c.polyline({{100, 100}, {400, 380}, {700, 100}});
c.set_colour({90, 200, 255});
c.fill_circle(400, 240, 60);

std::string picture = c.sixel();            // write it to the terminal at the cursor
```

A moving drawing:

```cpp
#include "vtpen_scene.h"
using namespace vtpen;

Symbol arrow;                               // drawn around its own centre (0, 0)
arrow.fill_polygon({{-30, -8}, {6, -8}, {6, -20}, {34, 0}, {6, 20}, {6, 8}, {-30, 8}});

Scene scene;
int id = scene.add(arrow, 400, 240);        // where its centre goes
scene.set_colour(id, {255, 235, 60});

Canvas canvas(900, 540, 800, 480);
CellSender sender(10, 20);                  // one character cell is 10x20 pixels

for (int deg = 0; deg < 360; deg += 5) {
  scene.rotate(id, deg);
  scene.render(canvas);                     // clear, then draw every object
  write(fd, sender.update(canvas, 3, 2));  // only cells that changed; top left at row 3, column 2
}
```

Find the size of a character cell by asking the terminal (`ESC [ 16 t` answers
`ESC [ 6 ; height ; width t`). `tools/vtprobe.cpp` shows how.

## Ideas to know

- **Drawing space.** The two last arguments of `Canvas` are the size of the space you
  draw in. Everything you pass in is in those units and is scaled to the pixels, so
  the same drawing works at any size. If the drawing space and the picture have
  different shapes, circles become ovals (on purpose): keep the shapes equal.
- **Angles** are in degrees, grow clockwise on the screen, and 0 points right.
- **Edges are smooth.** `set_edge_levels(n)`: 4 (default) looks nearly as good as
  it gets and is about 40% smaller than 0 (every shade); 1 gives hard edges and the
  smallest pictures but slanted lines show stair-steps.
- **At most 255 colours** in one picture. With more, the 255 most used are kept and the
  rest take the nearest. Smooth edges make many shades, so a busy picture can get close.
- **Opaque or transparent.** By default pixels you did not draw on are not sent, so
  the terminal's own background and text show through. For something that is redrawn
  every frame use `set_background(...)`, `set_transparent(false)` and `clear()`, so
  each frame fully replaces the last.
- **Patterns are fixed to the picture,** like wallpaper behind the shapes: neighbouring
  shapes line up and a turning shape does not drag its pattern around. Build one
  `Pattern`, share it between as many shapes as you like.
- **Bad numbers are skipped, not fatal.** NaN and infinity points are dropped, huge
  ones are pulled in, thickness is limited. Nothing hangs or crashes.

## Asking the terminal what it can do

Check before you draw. A terminal that does not know Sixel would print garbage.

```cpp
#include "vtpen_probe_posix.h"

vtpen::Capabilities cap = vtpen::probe_terminal();   // asks on stdin/stdout, 1 second at most
if (cap.usable()) {
  // draw with cap.cell_w and cap.cell_h
} else {
  // plain text instead
}
```

`usable()` means the terminal answered, says it can draw Sixel, and told us how big a
character cell is. Without the POSIX helper, send `vtpen::Probe::questions()` yourself,
read until `Probe::complete(reply)`, and call `Probe::parse(reply)`.

What `Capabilities` holds: whether Sixel is available; the largest picture and the
number of colours (0 when the terminal does not say); window and cell size in cells and
pixels; the terminal's name (`XTVERSION`), whether tmux or screen is in between;
whether synchronized output exists; and `summary()`, a few lines of text for people.

Optionally (`cursor_test = true`) it also finds out where the cursor ends up after a
picture, by drawing a small black picture in the terminal's alternate screen. That is
off by default: switching screens made Windows Terminal show an extra copy of the line
above, and the colour and mode questions asked right after it got no answer at all
(they were answered once the cursor test was left out). vtpen never needs the answer
because `CellSender` always moves the cursor before it draws. If you use it, do it once,
at the very start, and do not rely on the terminal answering other questions straight
afterwards.

What it **cannot** find out: a terminal cannot report whether writing a letter over a
picture removes the picture's pixels in that cell, or whether a new picture replaces
the old one cleanly. `cap.behaviour` holds what is known from terminals someone has
looked at. Today that is Windows Terminal only, and it is recognised by how it answers,
so `behaviour.guessed` is true. Anything else has `behaviour.known == false`: test it
(`vtprobe` pages 4, 5 and 9 show these things) before relying on `CellSender` there.
The terminals in the world differ here.

If nothing answers within the time, `answered` is false and everything else is empty.
Keys typed while it waits are read along with the answers: they are dropped, unless you pass a string as the last argument of `probe_terminal`, which then receives them in order (`Probe::typed(reply)` does the same for your own reading), so a program can put them back in front of whatever it starts. Every number the terminal sends is limited, and
garbage is ignored.

## When the window is resized

The terminal does not repaint a Sixel picture for you after a resize. When you get
SIGWINCH: ask the terminal for the new cell size (`ESC [ 16 t`) and window size, make a
canvas that fits (or keep the old one), call `sender.set_cell_size(w, h)` (or just
`sender.redraw()` if the cell size did not change), clear the screen, write your text
again, and call `update` as usual. The next update sends the whole picture once.

## Terminals and text

A Sixel picture lives in the character cells it covers. Measured in Windows Terminal
over ssh (other terminals differ, check with `vtprobe`):

- Writing a letter into a cell **replaces that cell's pixels.** So after a cell is
  re-sent, text over it has to be written again. `CellSender::changed()` lists the
  cells it sent for exactly that reason.
- The picture scrolls with the text, and erasing a cell erases only that cell's pixels.
- Text drawn this way stays real, selectable text; the library never draws letters.

## Reference

### `Canvas` (vtpen.h)

| | |
|---|---|
| `Canvas(w, h, virt_w = 0, virt_h = 0)` | pixel size, and the size of your drawing space (0 = the same) |
| `set_colour(rgb)`, `set_thickness(px)` | for what you draw next (thickness is in pixels) |
| `set_edge_levels(n)` | softness steps; 4 default, 0 = every shade, 1 = hard |
| `set_fill_pattern(shared_ptr<const Pattern>)` | what fills are made of; null = solid |
| `set_background(rgb)`, `set_transparent(bool)`, `clear()` | background handling |
| `line(a, b)`, `polyline(points)` | one point draws a dot; joints are round |
| `rectangle(x, y, w, h)`, `polygon(points)` | outlines (a polygon closes itself) |
| `circle(cx, cy, r)`, `ellipse(cx, cy, rx, ry)` | outlines |
| `arc(cx, cy, r, start, end)`, `arc(cx, cy, rx, ry, start, end)` | degrees |
| `bezier(p0, c0, c1, p1)` | cubic curve |
| `spline(points)` | smooth curve through every point (Catmull-Rom) |
| `fill_polygon(points)`, `fill_rectangle`, `fill_circle`, `fill_ellipse` | filled; even-odd rule (the middle of a star is a hole) |
| `sixel()` | the whole picture |
| `sixel(x, y, w, h)`, `empty(x, y, w, h)` | a rectangle of pixels as its own picture; whether it has anything |
| `pixel(x, y)`, `row(y)`, `width()`, `height()` | read back |

### `Pattern` (vtpen.h)

`Pattern::solid()`, `horizontal(spacing, thickness)`, `vertical(...)`,
`diagonal(spacing, thickness, forward)`, `crosshatch(spacing, thickness, upright)`,
`dots(spacing, size)`, `checker(cell)`, `gray(0..16)`, `from_rows({"X.X", ".X."})`, or
`Pattern(w, h)` with `set(x, y, strength)`. Fills only; lines are always solid.

Size in Sixel (a 165x105 field, from `vtprobe` page 10): solid 0.3 KB, horizontal lines
0.8 KB, everything else (vertical, diagonal, crosshatch, dots, checker, grey) 3 to 4 KB.
Horizontal runs compress; anything that changes along a row does not.

### `Symbol`, `Scene` (vtpen_scene.h)

`Symbol` has the same shape commands as `Canvas` (`line`, `polyline`, `polygon`,
`rectangle`, `circle`, `ellipse`, `arc`, `bezier`, `spline`, and the `fill_*` ones) plus
`set_colour`, `inherit_colour`, `set_thickness`, `set_fill_pattern` for what follows.
A command with no colour of its own takes the colour of the object it is in.

`Scene`: `add(symbol, x, y, rotation = 0, scale = 1, layer = 0)` returns an id;
`move`, `rotate`, `scale`, `set_colour`, `set_layer`, `show`, `remove`, `exists`, `size`;
`render(canvas)` clears the canvas and draws every visible object, lowest layer first
(same layer: the order added). Curves get as many pieces as they need at their final size.

### `Probe`, `Capabilities` (vtpen_probe.h) and `probe_terminal` (vtpen_probe_posix.h)

`Probe::questions(cursor_test = false)` the bytes to send (the last question is the plain
"what are you", which every terminal answers); `Probe::complete(reply)` true once the
answer to it has arrived; `Probe::parse(reply)` returns `Capabilities`.
`probe_terminal(in_fd = 0, out_fd = 1, timeout_ms = 1000, cursor_test = false, typed = nullptr)` does all of
it, putting the terminal in raw mode meanwhile and back afterwards.

`Capabilities`: `answered`, `sixel`, `usable()`, `max_width`, `max_height`,
`colour_registers`, `cols`, `rows`, `cell_w`, `cell_h`, `window_w`, `window_h`, `name`,
`multiplexer`, `da1`, `da2`, `supports_sync_output()`, `cursor_after`
(`Stays`, `Right`, `OnLastRow`, `Below`, `Unknown`; only with the cursor test) with
`cursor_row_delta` and `cursor_col_delta`,
`behaviour`, `summary()`.

### `CellSender` (vtpen_scene.h)

`CellSender(cell_w, cell_h)`. `update(canvas, row, col)` returns the bytes that make the
terminal show the canvas with its top left at that terminal row and column (1-based):
cells that differ from last time only, neighbours in a row as one picture, or one picture
when most of it changed; an empty string when nothing changed. A transparent canvas has
its cells wiped with spaces first. `changed()` lists the cells sent (column, row in the
picture). `redraw()` makes the next update send everything again: call it when the
window was resized, the screen was cleared, or something else wrote over the picture.
`set_cell_size(w, h)` tells it the character cell is now a different size in pixels (the
window or font changed) and implies `redraw()`. A canvas of a different size also starts over.

## What it costs

Measured on a 900x540 picture (about 2,400 cells), 20 frames a second:

| | per frame |
|---|---|
| the first picture | about 40 KB |
| a turning arrow over a map (17 cells change) | about 2.6 KB |
| the whole map redrawn and sent every frame | about 65 KB |

Drawing and encoding a full-window (2060x880) picture takes about 30 ms on a normal
machine. The picture can be as big as you like in memory; the terminal's own Sixel size
limit is another matter (ask with `ESC [ ? 2 ; 1 ; 0 S`).

## Building and testing

```
./m            build the vtprobe tool
./m test       also build and run the tests, with address, undefined-behaviour and
               float-conversion sanitizers
```

The tests decode the Sixel the library writes and compare it with the canvas; fills are
checked against a plain 16x16 sampling of every pixel; `CellSender` is checked by playing
its output into a pretend screen after every change. The code is also fed NaN, infinity,
huge and random values.

## Not done

Drawn text, write modes (overwrite, erase, complement), rotated text, saved macros,
reading ReGIS or any text command language, and more than one pattern colour.

## Licence

MIT (see `LICENSE`), the same as vtscreen.

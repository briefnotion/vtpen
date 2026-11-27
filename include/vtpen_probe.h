// vtpen probe: find out whether the terminal can show vtpen's pictures, and a few
// facts vtpen needs (cell size, picture size limit, where the cursor ends up).
// Standard C++17, no input or output of its own:
//
//   std::string q = vtpen::Probe::questions();     // write this to the terminal
//   ... read what comes back, until the terminal's answer to the last question ...
//   vtpen::Capabilities c = vtpen::Probe::parse(reply);
//   if (c.usable()) { /* draw pictures, using c.cell_w and c.cell_h */ }
//
// vtpen_probe_posix.h does the writing and reading for you on a Unix terminal.
//
// Everything the terminal says is taken with a pinch of salt: replies are only
// read, never trusted for sizes or counts without limits, and any garbage is
// ignored.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace vtpen {

// Where the cursor ended up after a Sixel picture a few rows tall, drawn at the top left.
enum class CursorAfterImage {
  Unknown,
  Stays,      // where it started
  Right,      // on the same row, to the right of the picture
  OnLastRow,  // on the picture's last row
  Below,      // on the row just below the picture (the usual "next line" behaviour)
};

// What is known about how the terminal treats pictures and text sharing cells.
// A terminal cannot report these, so they come from a short list of terminals
// somebody has actually looked at.
struct Behaviour {
  bool known = false;
  bool cell_write_wipes_pixels = false;  // writing a letter into a cell removes that cell's picture
  bool picture_replaces_cell = false;    // a new picture in a cell fully replaces the old one
  bool scrolls_with_text = false;        // the picture moves when the text scrolls
  bool guessed = false;                  // matched by how it answers, not by its own name
  std::string basis;                     // where this comes from
};

struct Capabilities {
  bool answered = false;        // the terminal answered at all
  bool sixel = false;           // it says it can draw Sixel pictures
  int max_width = 0;            // largest picture in pixels, 0 = it did not say
  int max_height = 0;
  int colour_registers = 0;     // most colours in one picture, 0 = it did not say
  int cols = 0, rows = 0;       // the window in character cells
  int cell_w = 0, cell_h = 0;   // one character cell in pixels
  int window_w = 0, window_h = 0;
  std::string name;             // what it calls itself (XTVERSION), may be empty
  std::string multiplexer;      // "tmux" or "screen" if one is in between, else empty
  std::vector<int> da1, da2;    // the raw answers to "what are you" (class, then features / model, version)
  int sync_output_mode = -1;    // DECRQM answers: -1 no answer, 0 unknown, 1 on, 2 off, 3 always on, 4 always off
  int sixel_display_mode = -1;  // mode 80
  int sixel_cursor_mode = -1;   // mode 8452
  CursorAfterImage cursor_after = CursorAfterImage::Unknown;
  int cursor_row_delta = 0, cursor_col_delta = 0;
  Behaviour behaviour;

  // Enough to draw: it can show Sixel and we know how big a cell is.
  bool usable() const { return answered && sixel && cell_w > 0 && cell_h > 0; }
  bool supports_sync_output() const { return sync_output_mode >= 1 && sync_output_mode <= 3; }

  // A few lines of plain text describing all of this.
  std::string summary() const {
    std::string s;
    auto line = [&](const std::string& t) { s += t + "\n"; };
    if (!answered) {
      line("The terminal did not answer.");
      return s;
    }
    line(std::string("Sixel pictures: ") + (sixel ? "yes" : "no"));
    if (!name.empty()) line("Calls itself: " + name);
    if (!multiplexer.empty()) line("A multiplexer is in between: " + multiplexer);
    if (cell_w > 0) line("One character cell: " + std::to_string(cell_w) + "x" + std::to_string(cell_h) + " pixels");
    if (cols > 0) line("Window: " + std::to_string(cols) + " columns, " + std::to_string(rows) + " rows");
    if (window_w > 0) line("Window in pixels: " + std::to_string(window_w) + "x" + std::to_string(window_h));
    if (max_width > 0) line("Largest picture: " + std::to_string(max_width) + "x" + std::to_string(max_height));
    else line("Largest picture: not reported");
    if (colour_registers > 0) line("Colours in one picture: " + std::to_string(colour_registers));
    else line("Colours in one picture: not reported");
    switch (cursor_after) {
      case CursorAfterImage::Stays: line("After a picture the cursor stays where it was"); break;
      case CursorAfterImage::Right: line("After a picture the cursor is to its right, on the same row"); break;
      case CursorAfterImage::OnLastRow:
        line("After a picture the cursor is on the picture's last row (" + std::to_string(cursor_row_delta) + " row(s) down, " +
             std::to_string(cursor_col_delta) + " column(s) across)");
        break;
      case CursorAfterImage::Below:
        line("After a picture the cursor is on the row below it (" + std::to_string(cursor_row_delta) + " row(s) down, " +
             std::to_string(cursor_col_delta) + " column(s) across)");
        break;
      case CursorAfterImage::Unknown: line("Where the cursor ends up after a picture: not known"); break;
    }
    line(std::string("Synchronized output: ") + (supports_sync_output() ? "yes" : "no or not reported"));
    if (behaviour.known)
      line(std::string("Known behaviour: text wipes picture pixels in its cell: ") + (behaviour.cell_write_wipes_pixels ? "yes" : "no") +
           "; a new picture replaces the old one: " + (behaviour.picture_replaces_cell ? "yes" : "no") + "; picture scrolls with text: " +
           (behaviour.scrolls_with_text ? "yes" : "no") + (behaviour.guessed ? " (a guess from how it answers)" : "") + " [" + behaviour.basis + "]");
    else line("Behaviour with text over pictures: not known for this terminal; test it");
    return s;
  }
};

class Probe {
 public:
  // The bytes to send. The last question is the plain "what are you" (ESC [ c)
  // that every terminal answers, so its answer tells you the others are done: read
  // until a reply of the form ESC [ ? ... c arrives (or give up after a moment).
  //
  // With cursor_test the probe draws a small black picture, 60 pixels tall, at the
  // top left of the terminal's alternate screen (and leaves that screen again), to
  // see where the cursor ends up. It is off by default: asking nothing but questions
  // changes nothing on screen, while switching screens did make Windows Terminal
  // show an extra copy of the line above. vtpen itself never needs the answer,
  // because it always moves the cursor before drawing.
  static std::string questions(bool cursor_test = false) {
    std::string q;
    if (cursor_test) {
      q += "\x1b[?1049h\x1b[1;1H\x1b[6n";                                   // alternate screen, home, where am I
      q += "\x1bP0;1;0q\"1;1;6;60#0;2;0;0;0";                              // a picture 6 wide, 60 tall (ten bands of six rows)
      for (int band = 0; band < 10; band++) q += band < 9 ? "#0~~~~~~-" : "#0~~~~~~";
      q += "\x1b\\";
      q += "\x1b[6n";                                                       // where am I now
      q += "\x1b[1;1H \x1b[?1049l";                                         // wipe the cell, back to the normal screen
    }
    q += "\x1b[>0q";                                                        // what is your name
    q += "\x1b[>c";                                                         // secondary attributes
    q += "\x1b[?1;1;0S\x1b[?2;1;0S";                                        // colours, largest picture
    q += "\x1b[18t\x1b[14t\x1b[16t";                                        // size in cells, in pixels, one cell
    q += "\x1b[?2026$p\x1b[?80$p\x1b[?8452$p";                              // some modes
    q += "\x1b[c";                                                          // the sentinel, and the feature list
    return q;
  }

  // True once `reply` contains the answer to the last question, so reading can stop.
  static bool complete(const std::string& reply) {
    for (const Csi& c : find_csi(reply))
      if (c.final == 'c' && c.lead == '?') return true;
    return false;
  }

  // The keys that were typed while the terminal was answering: `reply` with the terminal's answers taken out, everything else
  // left as it was, in order (letters, Enter, and the sequences keys send, such as the arrows). The answers are the ESC [ ... c, S,
  // t and y sequences of the questions above (and R, the cursor position, when the cursor test was asked for) and the ESC P ... ST
  // string that gives the terminal's name. Use it to put the typing back in front of the program that asked.
  static std::string typed(const std::string& reply, bool cursor_test = false) {
    std::string t;
    size_t i = 0;
    while (i < reply.size()) {
      if (reply[i] == '\x1b' && i + 1 < reply.size() && reply[i + 1] == '[') {
        size_t j = i + 2;
        while (j < reply.size() && reply[j] >= 0x20 && reply[j] <= 0x3f) j++;           // parameters, then
        if (j < reply.size() && reply[j] >= 0x40 && reply[j] <= 0x7e) {                  // the final byte
          const char f = reply[j];
          const char first = i + 2 < j ? reply[i + 2] : 0;
          const bool dollar = reply.find('$', i + 2) < j;
          const bool answer = (f == 'c' && (first == '?' || first == '>')) || (f == 'S' && first == '?') ||
                              (f == 't' && first >= '0' && first <= '9') || (f == 'y' && dollar) ||
                              (f == 'R' && cursor_test && first >= '0' && first <= '9');
          if (!answer) t.append(reply, i, j + 1 - i);
          i = j + 1;
          continue;
        }
      } else if (reply[i] == '\x1b' && i + 1 < reply.size() && reply[i + 1] == 'P') {     // a string: the terminal's name
        size_t j = i + 2;
        while (j < reply.size() && reply[j] != '\x07' && !(reply[j] == '\x1b' && j + 1 < reply.size() && reply[j + 1] == '\\')) j++;
        if (j >= reply.size()) break;                                                    // cut short: it is the terminal's, not typing
        i = j + (reply[j] == '\x07' ? 1 : 2);
        continue;
      }
      t += reply[i++];
    }
    return t;
  }

  // Read everything the terminal sent back.
  static Capabilities parse(const std::string& reply) {
    Capabilities cap;
    std::vector<std::pair<int, int>> positions;  // answers to "where am I", in order
    bool da1_seen = false;
    for (const Csi& c : find_csi(reply)) {
      const std::vector<long> n = c.numbers;
      switch (c.final) {
        case 'c':
          if (c.lead == '?') { cap.da1 = ints(n); da1_seen = true; }
          else if (c.lead == '>') cap.da2 = ints(n);
          break;
        case 'R':
          if (c.lead == 0 && n.size() >= 2) positions.push_back({clamp(n[0]), clamp(n[1])});
          break;
        case 'S':  // colours and picture size: ESC [ ? item ; status ; value ... S, status 0 is "ok"
          if (c.lead == '?' && n.size() >= 3 && n[1] == 0) {
            if (n[0] == 1) cap.colour_registers = clamp(n[2]);
            if (n[0] == 2 && n.size() >= 4) { cap.max_width = clamp(n[2]); cap.max_height = clamp(n[3]); }
          }
          break;
        case 't':
          if (c.lead == 0 && n.size() >= 3) {
            if (n[0] == 8) { cap.rows = clamp(n[1]); cap.cols = clamp(n[2]); }
            if (n[0] == 4) { cap.window_h = clamp(n[1]); cap.window_w = clamp(n[2]); }
            if (n[0] == 6) { cap.cell_h = clamp(n[1]); cap.cell_w = clamp(n[2]); }
          }
          break;
        case 'y':  // mode answers: ESC [ ? mode ; value $ y
          if (c.lead == '?' && c.dollar && n.size() >= 2 && n[1] >= 0 && n[1] <= 4) {
            if (n[0] == 2026) cap.sync_output_mode = static_cast<int>(n[1]);
            if (n[0] == 80) cap.sixel_display_mode = static_cast<int>(n[1]);
            if (n[0] == 8452) cap.sixel_cursor_mode = static_cast<int>(n[1]);
          }
          break;
        default: break;
      }
    }
    cap.answered = da1_seen;
    cap.name = xtversion(reply);

    // Does it draw Sixel? Its feature list says so, or it answered the picture questions.
    for (size_t i = 1; i < cap.da1.size(); i++) if (cap.da1[i] == 4) cap.sixel = true;
    if (cap.max_width > 0 || cap.colour_registers > 0) cap.sixel = true;

    // The size of a cell, if it only gave us the window in pixels and in cells.
    if (cap.cell_w <= 0 && cap.window_w > 0 && cap.cols > 0 && cap.window_h > 0 && cap.rows > 0) {
      cap.cell_w = cap.window_w / cap.cols;
      cap.cell_h = cap.window_h / cap.rows;
    }

    // Is there something in between?
    if (cap.da2.size() >= 1 && cap.da2[0] == 84) cap.multiplexer = "tmux";
    else if (cap.da2.size() >= 1 && cap.da2[0] == 83) cap.multiplexer = "screen";
    if (cap.name.compare(0, 4, "tmux") == 0) cap.multiplexer = "tmux";

    // Where the cursor went after the picture.
    if (positions.size() >= 2) {
      const int dr = cap.cursor_row_delta = positions[1].first - positions[0].first;
      const int dc = cap.cursor_col_delta = positions[1].second - positions[0].second;
      // The picture is 60 pixels tall: how many character rows is that?
      const int tall = cap.cell_h > 0 ? (60 + cap.cell_h - 1) / cap.cell_h : 0;
      if (dr == 0 && dc == 0) cap.cursor_after = CursorAfterImage::Stays;
      else if (dr == 0 && dc > 0) cap.cursor_after = CursorAfterImage::Right;
      else if (tall == 0 && dr > 0) cap.cursor_after = CursorAfterImage::Below;   // cell size unknown: cannot tell the two apart
      else if (tall > 0 && dr == tall) cap.cursor_after = CursorAfterImage::Below;
      else if (tall > 1 && dr == tall - 1) cap.cursor_after = CursorAfterImage::OnLastRow;
    }

    cap.behaviour = known_behaviour(cap);
    return cap;
  }

 private:
  struct Csi {
    char final = 0;
    char lead = 0;            // '?' or '>' or '=' if the reply starts with one, else 0
    bool dollar = false;      // had a '$' before the final letter
    std::vector<long> numbers;
  };

  static int clamp(long v) { return static_cast<int>(std::clamp(v, 0L, 1000000L)); }
  static std::vector<int> ints(const std::vector<long>& v) {
    std::vector<int> r;
    for (long x : v) r.push_back(clamp(x));
    return r;
  }

  // Every ESC [ ... reply in the text.
  static std::vector<Csi> find_csi(const std::string& s) {
    std::vector<Csi> v;
    size_t i = 0;
    while ((i = s.find("\x1b[", i)) != std::string::npos) {
      size_t j = i + 2;
      Csi c;
      if (j < s.size() && (s[j] == '?' || s[j] == '>' || s[j] == '=')) c.lead = s[j++];
      long cur = -1;
      bool bad = false;
      while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7e)) {
        const char ch = s[j];
        if (ch >= '0' && ch <= '9') {
          cur = (cur < 0 ? 0 : cur) * 10 + (ch - '0');
          if (cur > 100000000L) cur = 100000000L;
        } else if (ch == ';' || ch == ':') {
          c.numbers.push_back(cur < 0 ? 0 : cur);
          cur = -1;
        } else if (ch == '$') {
          c.dollar = true;
        } else if (ch < 0x20 || ch >= 0x7f) {
          bad = true;
          break;
        }
        j++;
      }
      if (j >= s.size()) break;
      if (!bad) {
        if (cur >= 0) c.numbers.push_back(cur);
        c.final = s[j];
        v.push_back(std::move(c));
      }
      i = bad ? i + 2 : j + 1;
    }
    return v;
  }

  // ESC P > | text ESC \  (or ended with BEL)
  static std::string xtversion(const std::string& s) {
    const size_t at = s.find("\x1bP>|");
    if (at == std::string::npos) return "";
    std::string t;
    for (size_t i = at + 4; i < s.size() && t.size() < 200; i++) {
      if (s[i] == '\x1b' || s[i] == '\x07') break;
      if (static_cast<unsigned char>(s[i]) >= 0x20 && static_cast<unsigned char>(s[i]) < 0x7f) t += s[i];
    }
    return t;
  }

  static Behaviour known_behaviour(const Capabilities& cap) {
    Behaviour b;
    // Windows Terminal answers "what are you" as ESC [ > 0 ; 10 ; 1 c and does not give a name.
    // Seen with vtpen's own probe tool (pages 4, 5 and 9), by eye, over ssh, in 2026.
    if (cap.sixel && cap.name.empty() && cap.multiplexer.empty() && cap.da2.size() == 3 && cap.da2[0] == 0 && cap.da2[1] == 10 && cap.da2[2] == 1) {
      b.known = true;
      b.guessed = true;
      b.cell_write_wipes_pixels = true;
      b.picture_replaces_cell = true;
      b.scrolls_with_text = true;
      b.basis = "Windows Terminal, looked at with vtprobe pages 4, 5 and 9";
    }
    return b;
  }
};

}  // namespace vtpen

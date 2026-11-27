// vtprobe: a small program to run inside a terminal (Windows Terminal, over
// ssh or WSL) and see what it supports.
//
// A test is a function that writes escape sequences, optionally asks the
// terminal something with query(), and returns a Result. Add it to tests()
// and it shows up on the report page.

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "vtpen_probe_posix.h"
#include "vtpen_scene.h"
#include "vtpen.h"

enum class Status { Pass, Fail, Unknown, Look };  // Look: judge by eye

struct Result {
  Status status;
  std::string note;
  std::vector<std::string> details = {};  // extra lines, shown indented
};

struct Test {
  std::string name;
  std::function<Result()> run;
};

// ---- terminal plumbing ----------------------------------------------------

static termios saved_termios;
static bool raw_on = false;

static void raw_off() {
  if (raw_on) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    raw_on = false;
  }
}

static bool raw_start() {
  if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) return false;
  termios t = saved_termios;
  cfmakeraw(&t);
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return false;
  raw_on = true;
  atexit(raw_off);
  return true;
}

static size_t bytes_out = 0;  // everything written to the terminal, for bandwidth numbers

static void out(const std::string& s) {
  bytes_out += s.size();
  size_t done = 0;
  while (done < s.size()) {
    ssize_t n = write(STDOUT_FILENO, s.data() + done, s.size() - done);
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
}

// Send a sequence and collect the reply until `last` ends it or the time is
// up. Returns what arrived (empty means the terminal did not answer).
[[maybe_unused]] static std::string query(const std::string& seq, char last, int timeout_ms = 500) {
  tcflush(STDIN_FILENO, TCIFLUSH);  // drop stale input
  out(seq);
  std::string reply;
  for (;;) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) break;
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) break;
    reply += c;
    if (c == last) break;
  }
  return reply;
}

[[maybe_unused]] static std::string show(const std::string& s) {  // make escapes readable
  std::string r;
  for (unsigned char c : s) {
    if (c == 0x1b) r += "ESC";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "^%c", c + 64); r += b; }
    else r += static_cast<char>(c);
  }
  return r;
}

static const char* status_name(Status s) {
  switch (s) {
    case Status::Pass: return "PASS   ";
    case Status::Fail: return "FAIL   ";
    case Status::Unknown: return "UNKNOWN";
    case Status::Look: return "LOOK   ";
  }
  return "?";
}

// ---- asking with a sentinel -------------------------------------------------
//
// Terminals ignore sequences they do not know, so "no answer" would cost a
// full timeout. Instead ask() sends the question followed by a Primary Device
// Attributes request, which every terminal answers. When that answer arrives
// we know everything before it is the whole reply to the question (an empty
// string means the terminal does not know it).

struct Asked {
  std::string answer;  // what came back before the sentinel
  bool alive;          // the sentinel itself came back
};

static std::vector<int> numbers(const std::string& s) {
  std::vector<int> v;
  int cur = -1;
  for (char c : s) {
    if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
    else { if (cur >= 0) v.push_back(cur); cur = -1; }
  }
  if (cur >= 0) v.push_back(cur);
  return v;
}

static Asked ask(const std::string& seq, int timeout_ms = 700) {
  tcflush(STDIN_FILENO, TCIFLUSH);
  out(seq + "\x1b[c");
  std::string buf;
  for (;;) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return {buf, false};
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return {buf, false};
    buf += c;
    if (c != 'c') continue;
    size_t at = buf.rfind("\x1b[?");  // the sentinel reply is ESC [ ? digits c
    if (at == std::string::npos) continue;
    bool ok = true;
    for (size_t i = at + 3; i + 1 < buf.size(); i++)
      if (!((buf[i] >= '0' && buf[i] <= '9') || buf[i] == ';')) ok = false;
    if (ok) return {buf.substr(0, at), true};
  }
}

// ---- Primary Device Attributes (DA1) ----------------------------------------
// Reply: ESC [ ? class ; feature ; feature ... c

struct Da1 {
  bool ok = false;
  std::string raw;
  std::vector<int> params;
};

static const Da1& da1() {
  static Da1 d;
  static bool done = false;
  if (!done) {
    done = true;
    d.raw = query("\x1b[c", 'c');
    size_t at = d.raw.find("\x1b[?");
    if (at != std::string::npos && d.raw.back() == 'c') {
      d.params = numbers(d.raw.substr(at + 3));
      d.ok = !d.params.empty();
    }
  }
  return d;
}

static bool has_feature(const Da1& d, int f) {
  for (size_t i = 1; i < d.params.size(); i++)
    if (d.params[i] == f) return true;
  return false;
}

static std::string da1_class_name(int c) {
  switch (c) {
    case 1: return "VT100";
    case 6: return "VT102";
    case 7: return "VT131";
    case 12: return "VT125";
    case 62: return "VT220 (level 2)";
    case 63: return "VT320 (level 3)";
    case 64: return "VT420 (level 4)";
    case 65: return "VT500 series or later (level 5)";
  }
  return "unknown class";
}

static Result test_identity() {
  const Da1& d = da1();
  if (!d.ok) return {Status::Unknown, "no answer to ESC [ c"};
  Result r{Status::Pass, "answered " + show(d.raw)};
  r.details.push_back("claims to be a " + da1_class_name(d.params[0]) +
                      " (class " + std::to_string(d.params[0]) + ")");
  return r;
}

static Result test_features() {
  const Da1& d = da1();
  if (!d.ok) return {Status::Unknown, "no answer to ESC [ c"};
  struct F { int n; const char* text; };
  static const F known[] = {
      {1, "132 columns"},
      {2, "printer port"},
      {3, "ReGIS graphics (vector)"},
      {4, "Sixel graphics (bitmap)"},
      {6, "selective erase"},
      {7, "soft character sets (DRCS)"},
      {8, "user-defined keys"},
      {9, "national replacement character sets"},
      {15, "technical character set"},
      {16, "locator port"},
      {17, "terminal state interrogation"},
      {18, "windowing"},
      {21, "horizontal scrolling"},
      {22, "ANSI colour"},
      {28, "rectangular editing"},
      {29, "ANSI text locator"},
      {52, "clipboard access"},
  };
  Result r{Status::Pass, ""};
  int listed = 0;
  for (size_t i = 1; i < d.params.size(); i++) {
    const char* text = "not in my table";
    for (const F& f : known) if (f.n == d.params[i]) text = f.text;
    // A VT100 (class 1) lists option codes, not the feature numbers above.
    if (d.params[0] == 1) text = d.params[i] == 2   ? "advanced video option"
             : d.params[i] == 4 ? "graphics option (Sixel on modern terminals)"
                                : "VT100 option code";
    r.details.push_back(std::to_string(d.params[i]) + ": " + text);
    listed++;
  }
  r.note = std::to_string(listed) + " features listed";
  if (listed == 0) r.note = "none listed";
  return r;
}

// ---- Secondary Device Attributes (DA2) --------------------------------------
// Reply: ESC [ > model ; firmware version ; ROM cartridge c

static std::string da2_model_name(int m) {
  switch (m) {
    case 0: return "VT100 (xterm and many others also say 0)";
    case 1: return "VT220";
    case 2: return "VT240";
    case 18: return "VT330";
    case 19: return "VT340";
    case 24: return "VT320";
    case 32: return "VT382";
    case 41: return "VT420";
    case 61: return "VT510";
    case 64: return "VT520";
    case 65: return "VT525";
  }
  return "not in my table";
}

static Result test_secondary() {
  std::string reply = query("\x1b[>c", 'c');
  size_t at = reply.find("\x1b[>");
  if (at == std::string::npos || reply.back() != 'c')
    return {Status::Unknown, "no answer to ESC [ > c"};
  std::vector<int> p = numbers(reply.substr(at + 3));
  Result r{Status::Pass, "answered " + show(reply)};
  if (!p.empty()) r.details.push_back("model " + std::to_string(p[0]) + ": " + da2_model_name(p[0]));
  if (p.size() > 1) r.details.push_back("version number " + std::to_string(p[1]));
  return r;
}

// ---- The terminal's own name (XTVERSION) ------------------------------------
// Reply: ESC P > | name version, then ESC and a backslash

static Result test_xtversion() {
  Asked a = ask("\x1b[>0q");
  if (!a.alive) return {Status::Unknown, "the terminal stopped answering"};
  size_t at = a.answer.find("\x1bP>|");
  if (at == std::string::npos)
    return {Status::Fail, "does not tell its name (no answer to ESC [ > 0 q)"};
  std::string name = a.answer.substr(at + 4);
  size_t end = name.find("\x1b\\");
  if (end != std::string::npos) name.resize(end);
  return {Status::Pass, "says it is: " + name};
}

// ---- Graphics ---------------------------------------------------------------
// Bitmap: Sixel (DA1 feature 4), the Kitty graphics protocol.
// Vector: ReGIS (DA1 feature 3). Tektronix 4014 mode cannot be asked about.

static Result test_graphics() {
  const Da1& d = da1();
  Result r{Status::Fail, "no graphics found"};
  std::vector<std::string> found;

  if (d.ok && has_feature(d, 4)) found.push_back("Sixel (bitmap), claimed by DA1");
  if (d.ok && has_feature(d, 3)) found.push_back("ReGIS (vector), claimed by DA1");

  // Sixel geometry: ESC [ ? 2 ; 1 ; 0 S  ->  ESC [ ? 2 ; 0 ; width ; height S
  Asked g = ask("\x1b[?2;1;0S");
  if (g.alive && g.answer.find("\x1b[?2;0;") != std::string::npos) {
    std::vector<int> p = numbers(g.answer);
    if (p.size() >= 4)
      found.push_back("Sixel picture size up to " + std::to_string(p[2]) + "x" +
                      std::to_string(p[3]) + " pixels (asked, not claimed)");
  }

  // Kitty graphics: a 1 pixel query. Reply: ESC _ G i=31 ; OK ESC \ .
  Asked k = ask("\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\");
  if (k.alive && k.answer.find("\x1b_G") != std::string::npos &&
      k.answer.find("OK") != std::string::npos)
    found.push_back("Kitty graphics protocol (bitmap)");

  // Cell size in pixels: ESC [ 16 t  ->  ESC [ 6 ; height ; width t
  Asked c = ask("\x1b[16t");
  std::vector<int> cp = numbers(c.answer);
  if (c.alive && cp.size() >= 3 && cp[0] == 6)
    r.details.push_back("one character cell is " + std::to_string(cp[2]) + "x" +
                        std::to_string(cp[1]) + " pixels");

  if (found.empty() && !d.ok && !g.alive && !k.alive && !c.alive)
    return {Status::Unknown, "the terminal answered nothing, so graphics are not known"};
  if (!found.empty()) {
    r.status = Status::Pass;
    r.note = std::to_string(found.size()) + " kind(s) found";
    r.details.insert(r.details.begin(), found.begin(), found.end());
  }
  return r;
}

// ---- Reading replies -----------------------------------------------------------

struct Csi {
  char final;                // the last byte, such as 't' or 'y'
  std::string lead;          // anything before the numbers: ? > and so on
  std::vector<int> n;        // the numbers
};

// Pick every ESC [ ... reply out of a string.
static std::vector<Csi> csi_replies(const std::string& s) {
  std::vector<Csi> v;
  size_t i = 0;
  while ((i = s.find("\x1b[", i)) != std::string::npos) {
    size_t j = i + 2;
    std::string body;
    while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7e)) body += s[j++];
    if (j >= s.size()) break;
    Csi c;
    c.final = s[j];
    if (!body.empty() && (body[0] == '?' || body[0] == '>' || body[0] == '=')) c.lead = body.substr(0, 1);
    c.n = numbers(body);
    v.push_back(c);
    i = j + 1;
  }
  return v;
}

// ---- Window size ---------------------------------------------------------------

static Result test_window_size() {
  Asked a = ask("\x1b[18t\x1b[14t\x1b[16t");
  if (!a.alive) return {Status::Unknown, "the terminal stopped answering"};
  Result r{Status::Fail, "does not report its size"};
  for (const Csi& c : csi_replies(a.answer)) {
    if (c.final != 't' || c.n.size() < 3) continue;
    if (c.n[0] == 8) r.details.push_back(std::to_string(c.n[2]) + " columns x " + std::to_string(c.n[1]) + " rows");
    if (c.n[0] == 4) r.details.push_back("window " + std::to_string(c.n[2]) + " x " + std::to_string(c.n[1]) + " pixels");
    if (c.n[0] == 6) r.details.push_back("cell " + std::to_string(c.n[2]) + " x " + std::to_string(c.n[1]) + " pixels");
  }
  if (!r.details.empty()) { r.status = Status::Pass; r.note = ""; }
  return r;
}

// ---- Colours the terminal really uses (OSC 4, 10, 11) --------------------------

static bool rgb_of(const std::string& s, size_t from, std::string& hex) {
  size_t at = s.find("rgb:", from);
  if (at == std::string::npos) return false;
  std::string part[3];
  size_t k = at + 4;
  for (int i = 0; i < 3; i++) {
    while (k < s.size() && isxdigit(static_cast<unsigned char>(s[k]))) part[i] += s[k++];
    if (i < 2) { if (k < s.size() && s[k] == '/') k++; else return false; }
  }
  for (int i = 0; i < 3; i++) if (part[i].size() < 2) return false;
  hex = "#" + part[0].substr(0, 2) + part[1].substr(0, 2) + part[2].substr(0, 2);
  return true;
}

static Result test_colours() {
  std::string q = "\x1b]10;?\x1b\\\x1b]11;?\x1b\\";
  for (int i = 0; i < 16; i++) q += "\x1b]4;" + std::to_string(i) + ";?\x1b\\";
  Asked a = ask(q, 900);
  if (!a.alive) return {Status::Unknown, "the terminal stopped answering"};
  Result r{Status::Fail, "does not report its colours"};
  std::string hex;
  size_t at = 0;
  std::string line16;
  while ((at = a.answer.find("\x1b]", at)) != std::string::npos) {
    size_t semi = a.answer.find(';', at);
    if (semi == std::string::npos) break;
    std::string id = a.answer.substr(at + 2, semi - at - 2);
    size_t next = a.answer.find("\x1b]", at + 2);
    std::string body = a.answer.substr(semi + 1, next == std::string::npos ? std::string::npos : next - semi - 1);
    if (rgb_of(body, 0, hex)) {
      if (id == "10") r.details.push_back("text colour " + hex);
      else if (id == "11") r.details.push_back("background  " + hex);
      else if (id == "4") {
        size_t s2 = body.find(';');  // body is N;rgb:...
        std::string idx = body.substr(0, s2);
        line16 += idx + "=" + hex + " ";
        if (idx == "7" || idx == "15") { r.details.push_back("palette " + line16); line16.clear(); }
      }
    }
    at = semi;
  }
  if (!line16.empty()) r.details.push_back("palette " + line16);
  if (!r.details.empty()) { r.status = Status::Pass; r.note = ""; }
  return r;
}

// ---- Modes (DECRQM) ------------------------------------------------------------

static Result test_modes() {
  struct M { int n; const char* text; };
  static const M modes[] = {
      {1, "application cursor keys"},
      {3, "132/80 column switch (DECCOLM)"},
      {7, "autowrap"},
      {25, "cursor shown"},
      {40, "allow 80/132 switch"},
      {80, "Sixel scrolling"},
      {1000, "mouse: clicks"},
      {1002, "mouse: drag"},
      {1003, "mouse: all motion"},
      {1004, "focus in/out events"},
      {1006, "mouse: SGR format"},
      {1049, "alternate screen"},
      {2004, "bracketed paste"},
      {2026, "synchronized output"},
      {2027, "grapheme clustering"},
      {9001, "Windows key input (win32-input-mode)"},
  };
  std::string q;
  for (const M& m : modes) q += "\x1b[?" + std::to_string(m.n) + "$p";
  Asked a = ask(q, 900);
  if (!a.alive) return {Status::Unknown, "the terminal stopped answering"};
  std::vector<Csi> rep = csi_replies(a.answer);
  Result r{Status::Pass, ""};
  int known = 0;
  for (const M& m : modes) {
    std::string state = "does not know it";
    for (const Csi& c : rep) {
      if (c.final != 'y' || c.n.size() < 2 || c.n[0] != m.n) continue;
      switch (c.n[1]) {
        case 1: state = "supported, now on"; known++; break;
        case 2: state = "supported, now off"; known++; break;
        case 3: state = "supported, always on"; known++; break;
        case 4: state = "always off"; break;
        default: break;
      }
    }
    r.details.push_back(std::to_string(m.n) + " " + m.text + ": " + state);
  }
  if (rep.empty()) return {Status::Fail, "does not answer mode questions (DECRQM)"};
  r.note = std::to_string(known) + " of " + std::to_string(sizeof modes / sizeof modes[0]) + " supported";
  return r;
}

// ---- Keyboard protocol ----------------------------------------------------------

static Result test_kitty_keys() {
  Asked a = ask("\x1b[?u");
  if (!a.alive) return {Status::Unknown, "the terminal stopped answering"};
  for (const Csi& c : csi_replies(a.answer))
    if (c.final == 'u' && c.lead == "?")
      return {Status::Pass, "supports the Kitty keyboard protocol (flags now " +
                                std::to_string(c.n.empty() ? 0 : c.n[0]) + ")"};
  return {Status::Fail, "no Kitty keyboard protocol"};
}

// ---- True colour accepted? (DECRQSS) -------------------------------------------

static Result test_truecolor_accepted() {
  Asked a = ask("\x1b[38;2;10;20;30m\x1bP$qm\x1b\\\x1b[0m");
  if (!a.alive) return {Status::Unknown, "the terminal stopped answering"};
  size_t at = a.answer.find("\x1bP");
  if (at == std::string::npos) return {Status::Unknown, "no answer; look at the colour samples instead"};
  std::string body = a.answer.substr(at);
  if (body.find("$r") != std::string::npos && body.find("38") != std::string::npos &&
      body.find("20") != std::string::npos)
    return {Status::Pass, "took 24-bit colour and reports it back: " + show(body)};
  return {Status::Fail, "did not keep 24-bit colour: " + show(body)};
}

// ---- Character widths, measured with the cursor ----------------------------------

static int width_of(const std::string& text) {
  out("\r\x1b[2K" + text);
  std::string r = query("\x1b[6n", 'R');
  out("\r\x1b[2K");
  std::vector<Csi> v = csi_replies(r);
  if (v.empty() || v[0].n.size() < 2) return -1;
  return v[0].n[1] - 1;
}

static Result test_widths() {
  struct W { const char* text; int expect; const char* name; };
  static const W cases[] = {
      {"A", 1, "plain letter"},
      {"\xe4\xbd\xa0", 2, "Chinese character"},
      {"\xe3\x81\x82", 2, "Japanese kana"},
      {"\xed\x95\x9c", 2, "Korean hangul"},
      {"\xf0\x9f\x98\x80", 2, "emoji (grinning face)"},
      {"e\xcc\x81", 1, "letter + combining accent"},
      {"\xe2\x9d\xa4\xef\xb8\x8f", 2, "heart + emoji selector"},
      {"\xf0\x9f\x87\xba\xf0\x9f\x87\xb8", 2, "flag (two regional letters)"},
      {"\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7", 2, "family (emoji joined with ZWJ)"},
      {"\xce\xb1", 1, "Greek alpha (ambiguous width)"},
  };
  Result r{Status::Pass, ""};
  int bad = 0, silent = 0;
  for (const W& c : cases) {
    int w = width_of(c.text);
    std::string verdict;
    if (w < 0) { verdict = "no answer"; silent++; }
    else if (w == c.expect) verdict = "ok";
    else { verdict = "MISMATCH, expected " + std::to_string(c.expect); bad++; }
    r.details.push_back(std::string(c.name) + ": " + (w < 0 ? "?" : std::to_string(w)) +
                        " column(s), " + verdict);
  }
  if (silent == static_cast<int>(sizeof cases / sizeof cases[0]))
    return {Status::Unknown, "the terminal does not report the cursor position"};
  if (bad) { r.status = Status::Fail; r.note = std::to_string(bad) + " width(s) differ from the usual table"; }
  return r;
}

// ---- Things to judge by eye ----------------------------------------------------------

static Result look(std::vector<std::string> lines, const std::string& note = "judge by eye") {
  return Result{Status::Look, note, std::move(lines)};
}

static Result test_attributes() {
  return look({
      "\x1b[1mbold\x1b[0m \x1b[2mdim\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m "
      "\x1b[5mblink\x1b[0m \x1b[7mreverse\x1b[0m \x1b[9mstrike\x1b[0m \x1b[21mdouble-underline\x1b[0m "
      "\x1b[53moverline\x1b[0m [\x1b[8mhidden\x1b[0m] (a gap between the brackets is right)",
      "\x1b[1;3;4mbold italic underline\x1b[0m  \x1b[31;1mbold red\x1b[0m  \x1b[2;32mdim green\x1b[0m"});
}

static Result test_underlines() {
  return look({
      "\x1b[4:1msingle\x1b[0m \x1b[4:2mdouble\x1b[0m \x1b[4:3mcurly\x1b[0m "
      "\x1b[4:4mdotted\x1b[0m \x1b[4:5mdashed\x1b[0m",
      "\x1b[4:3m\x1b[58;2;255;80;80mcurly red underline\x1b[0m  "
      "\x1b[4m\x1b[58;2;80;160;255mblue underline\x1b[0m"});
}

static Result test_colour_samples() {
  std::vector<std::string> v;
  std::string a, b;
  for (int i = 0; i < 8; i++) a += "\x1b[4" + std::to_string(i) + "m  ";
  for (int i = 0; i < 8; i++) b += "\x1b[10" + std::to_string(i) + "m  ";
  v.push_back("16 colours:  " + a + "\x1b[0m " + b);
  for (int row = 0; row < 6; row++) {
    std::string s = row == 0 ? "256 colours: " : "             ";
    for (int i = 0; i < 36; i++) s += "\x1b[48;5;" + std::to_string(16 + row * 36 + i) + "m ";
    v.push_back(s);
  }
  std::string g = "grays:       ";
  for (int i = 232; i < 256; i++) g += "\x1b[48;5;" + std::to_string(i) + "m  ";
  v.push_back(g);
  std::string t = "true colour: ";
  for (int i = 0; i < 64; i++)
    t += "\x1b[48;2;" + std::to_string(i * 4) + ";" + std::to_string(255 - i * 4) + ";128m ";
  v.push_back(t + "(should be a smooth blend, no banding)");
  return look(v, "smooth rows mean it works");
}

static Result test_blocks() {
  return look({
      "boxes:   \xe2\x94\x8c\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90  \xe2\x95\x94\xe2\x95\x90\xe2\x95\xa6\xe2\x95\x90\xe2\x95\x97  "
      "\xe2\x95\xad\xe2\x94\x80\xe2\x95\xae  \xe2\x94\x82 \xe2\x94\x9c\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\xa4  \xe2\x94\x94\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x98",
      "blocks:  \xe2\x96\x91\xe2\x96\x92\xe2\x96\x93\xe2\x96\x88 \xe2\x96\x80\xe2\x96\x84\xe2\x96\x8c\xe2\x96\x90 "
      "\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\xe2\x96\x88 (lines up with no gaps between rows)",
      "braille: \xe2\xa0\x81\xe2\xa0\x82\xe2\xa0\x84\xe2\xa1\x80\xe2\xa2\x80\xe2\xa0\xa0\xe2\xa0\x90\xe2\xa0\x88 \xe2\xa3\xbf\xe2\xa3\xb6\xe2\xa3\xa4\xe2\xa3\x80",
      "sextants (newer, may show as empty boxes): \xf0\x9f\xac\x80\xf0\x9f\xac\x81\xf0\x9f\xac\x82\xf0\x9f\xac\x83\xf0\x9f\xac\x84\xf0\x9f\xac\x85"});
}

static Result test_wide_sample() {
  return look({
      "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c|  <- 4 wide letters, then a bar",
      "AAAAAAAA|  <- 8 narrow letters; the two bars should line up",
      "\xe6\xbc\xa2\xe5\xad\x97 \xe3\x81\x8b\xe3\x81\xaa \xed\x95\x9c\xea\xb8\x80 \xf0\x9f\x98\x80\xf0\x9f\x8e\x89\xf0\x9f\x9a\x80 "
      "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 \xf0\x9f\x87\xba\xf0\x9f\x87\xb8 \xe2\x9d\xa4\xef\xb8\x8f"});
}

static Result test_hyperlink() {
  return look({"\x1b]8;;https://example.com\x1b\\this is a link (hold Ctrl and click)\x1b]8;;\x1b\\"});
}

static Result test_sixel_picture() {
  const Da1& d = da1();
  Asked g = ask("\x1b[?2;1;0S");
  bool claims = (d.ok && has_feature(d, 4)) || (g.alive && g.answer.find("\x1b[?2;0;") != std::string::npos);
  if (!claims) return {Status::Unknown, "the terminal does not claim Sixel, so nothing was drawn"};
  return look({"red, green and blue bars, two rows tall, should be right below this line:",
               "\x1bPq\"1;1;48;12#1;2;100;0;0#2;2;0;100;0#3;2;0;0;100"
               "#1!16~#2!16~#3!16~-#1!16~#2!16~#3!16~\x1b\\",
               "", "", ""});
}

static Result test_regis_picture() {
  const Da1& d = da1();
  if (!(d.ok && has_feature(d, 3)))
    return {Status::Unknown, "the terminal does not claim ReGIS (vector), so nothing was drawn"};
  return look({"a box with a diagonal line should be drawn below:",
               "\x1bPpS(E)P[20,20]V[200,20][200,120][20,120][20,20][200,120]\x1b\\",
               "", "", "", "", ""});
}

// What vtpen's own probe says about this terminal.
static Result test_vtpen_probe() {
  vtpen::Capabilities c = vtpen::probe_terminal(STDIN_FILENO, STDOUT_FILENO, 1500);
  Result r{c.usable() ? Status::Pass : (c.answered ? Status::Fail : Status::Unknown),
           c.usable() ? "vtpen can draw here" : c.answered ? "vtpen cannot draw here (no Sixel, or the size of a cell is not known)" : "no answer"};
  const std::string text = c.summary();
  for (size_t start = 0; start < text.size();) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    if (end > start) r.details.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return r;
}

static std::vector<Test> tests() {
  return {
      {"Identifies as (DA1)", test_identity},
      {"Feature list (DA1)", test_features},
      {"Secondary identity (DA2)", test_secondary},
      {"Terminal name (XTVERSION)", test_xtversion},
      {"vtpen probe (what the library sees)", test_vtpen_probe},
      {"Window and cell size", test_window_size},
      {"Colours it really uses", test_colours},
      {"Modes it knows (DECRQM)", test_modes},
      {"Kitty keyboard protocol", test_kitty_keys},
      {"24-bit colour accepted", test_truecolor_accepted},
      {"Character widths (measured)", test_widths},
      {"Graphics (bitmap and vector)", test_graphics},
      {"Text attributes", test_attributes},
      {"Underline styles", test_underlines},
      {"Colour samples", test_colour_samples},
      {"Lines, blocks, braille", test_blocks},
      {"Wide letters and emoji", test_wide_sample},
      {"Hyperlinks (OSC 8)", test_hyperlink},
      {"Sixel picture (bitmap)", test_sixel_picture},
      {"ReGIS picture (vector)", test_regis_picture},
  };
}

// ---- Page 2: a big circle drawn with Sixel -------------------------------------
//
// We draw the circle ourselves into a bitmap (with smoothed edges), then send
// the bitmap as Sixel. Colours: 1..8 are the ring in 8 brightness steps,
// 9..16 the inside, so the edge can fade into the background.

static std::string sixel_circle(int size) {
  const int W = size, H = size;
  const double cx = W / 2.0, cy = H / 2.0;
  const double outer = size / 2.0 - 2, inner = outer - std::max(6.0, size / 40.0);
  const int N = 4;  // samples per pixel side, for smooth edges

  std::vector<unsigned char> pix(static_cast<size_t>(W) * H, 0);  // 0 = nothing
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      int ring = 0, fill = 0;
      for (int sy = 0; sy < N; sy++) {
        for (int sx = 0; sx < N; sx++) {
          double dx = x + (sx + 0.5) / N - cx, dy = y + (sy + 0.5) / N - cy;
          double d = std::sqrt(dx * dx + dy * dy);
          if (d <= inner) fill++;
          else if (d <= outer) ring++;
        }
      }
      int total = N * N;
      int level = ((ring > fill ? ring : fill) * 8 + total / 2) / total;
      if (level > 0) pix[static_cast<size_t>(y) * W + x] = static_cast<unsigned char>((ring > fill ? 0 : 8) + level);
    }
  }

  std::string s = "\x1bP0;1;0q\"1;1;" + std::to_string(W) + ";" + std::to_string(H);
  for (int k = 1; k <= 8; k++) {  // sixel colours are percentages
    int f = k * 100 / 8;
    s += "#" + std::to_string(k) + ";2;" + std::to_string(f) + ";" + std::to_string(f * 6 / 10) + ";0";
    s += "#" + std::to_string(k + 8) + ";2;0;" + std::to_string(f * 4 / 10) + ";" + std::to_string(f);
  }

  for (int top = 0; top < H; top += 6) {
    bool first = true;
    for (int c = 1; c <= 16; c++) {
      std::string row;
      bool any = false;
      for (int x = 0; x < W; x++) {
        int bits = 0;
        for (int b = 0; b < 6 && top + b < H; b++)
          if (pix[static_cast<size_t>(top + b) * W + x] == c) bits |= 1 << b;
        if (bits) any = true;
        row += static_cast<char>(63 + bits);
      }
      if (!any) continue;
      if (!first) s += "$";  // back to the left edge, same band
      first = false;
      s += "#" + std::to_string(c);
      for (size_t i = 0; i < row.size();) {  // run-length encode
        size_t j = i;
        while (j < row.size() && row[j] == row[i]) j++;
        if (j - i > 3) s += "!" + std::to_string(j - i) + row[i];
        else s += row.substr(i, j - i);
        i = j;
      }
    }
    s += "-";  // next band
  }
  return s + "\x1b\\";
}

static void page_circle() {
  winsize ws{};
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  int rows = ws.ws_row ? ws.ws_row : 24;

  int cell_h = 20;  // fall back when the terminal will not say
  Asked a = ask("\x1b[16t");
  for (const Csi& c : csi_replies(a.answer))
    if (c.final == 't' && c.n.size() >= 3 && c.n[0] == 6 && c.n[1] > 0) cell_h = c.n[1];

  int size = (rows - 4) * cell_h;  // leave room for the two text lines
  if (size > 700) size = 700;
  if (size < 60) size = 60;

  out("\x1b[?1049h\x1b[2J\x1b[H");  // second screen, so page 1 is still there after
  out("Page 2: a circle drawn as a Sixel bitmap (" + std::to_string(size) + "x" +
      std::to_string(size) + " pixels).\r\n");
  out("A round circle with a smooth edge means it works. Press any key to go back.\r\n");
  out(sixel_circle(size));
  char c;
  (void)!read(STDIN_FILENO, &c, 1);
  out("\x1b[?1049l");
}

// ---- Page 3: lines drawn by the vector-to-Sixel engine ---------------------------

static vtpen::Rgb hue(double h) {  // h in 0..1, full colour
  double r = std::clamp(std::fabs(h * 6 - 3) - 1, 0.0, 1.0);
  double g = std::clamp(2 - std::fabs(h * 6 - 2), 0.0, 1.0);
  double b = std::clamp(2 - std::fabs(h * 6 - 4), 0.0, 1.0);
  return {static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255)};
}

static void page_lines() {
  winsize ws{};
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  int rows = ws.ws_row ? ws.ws_row : 24, cols = ws.ws_col ? ws.ws_col : 80;

  int cell_w = 10, cell_h = 20;  // fall back when the terminal will not say
  Asked a = ask("\x1b[16t");
  for (const Csi& c : csi_replies(a.answer))
    if (c.final == 't' && c.n.size() >= 3 && c.n[0] == 6 && c.n[1] > 0 && c.n[2] > 0) {
      cell_h = c.n[1];
      cell_w = c.n[2];
    }

  int ph = (rows - 4) * cell_h;  // the drawing space is 800x480, a 5:3 box
  int pw = ph * 5 / 3;
  if (pw > (cols - 1) * cell_w) { pw = (cols - 1) * cell_w; ph = pw * 3 / 5; }
  if (pw > 1000) { pw = 1000; ph = 600; }
  if (pw < 100) { pw = 100; ph = 60; }

  using vtpen::Point;
  vtpen::Canvas c(pw, ph, 800, 480);
  const double pi = 3.14159265358979;

  // Frame and a faint grid.
  c.set_thickness(1);
  c.set_colour({50, 50, 70});
  for (int x = 80; x < 800; x += 80) c.line({double(x), 0}, {double(x), 480});
  for (int y = 80; y < 480; y += 80) c.line({0, double(y)}, {800, double(y)});
  c.set_colour({140, 140, 170});
  c.set_thickness(2);
  c.polyline({{2, 2}, {798, 2}, {798, 478}, {2, 478}, {2, 2}});

  // A sine wave.
  std::vector<Point> wave;
  for (int x = 0; x <= 800; x += 4) wave.push_back({double(x), 240 + 90 * std::sin(x * 2 * pi / 400)});
  c.set_colour({60, 220, 255});
  c.set_thickness(3);
  c.polyline(wave);

  // A five pointed star.
  std::vector<Point> star;
  for (int i = 0; i <= 5; i++) {
    double ang = -pi / 2 + i * 4 * pi / 5;
    star.push_back({160 + 110 * std::cos(ang), 150 + 110 * std::sin(ang)});
  }
  c.set_colour({255, 210, 40});
  c.set_thickness(4);
  c.polyline(star);

  // A spiral.
  std::vector<Point> spiral;
  for (int i = 0; i <= 400; i++) {
    double ang = i * 0.1, r = 2 + i * 0.22;
    spiral.push_back({620 + r * std::cos(ang), 360 + r * 0.9 * std::sin(ang)});
  }
  c.set_colour({255, 90, 200});
  c.set_thickness(2);
  c.polyline(spiral);

  // A fan of lines from the bottom left, every colour, getting thicker.
  for (int i = 0; i < 12; i++) {
    c.set_colour(hue(i / 12.0));
    c.set_thickness(1 + i * 0.4);
    double ang = -pi / 2 * (i / 11.0);
    c.line({30, 450}, {30 + 330 * std::cos(ang), 450 + 330 * std::sin(ang)});
  }

  std::string image = c.sixel();

  out("\x1b[?1049h\x1b[2J\x1b[H");
  out("Page 3: lines drawn by the vector-to-Sixel engine (" + std::to_string(pw) + "x" +
      std::to_string(ph) + " pixels, " + std::to_string(image.size()) + " bytes).\r\n");
  out("Straight lines, smooth curves from many short lines, thickness from 1 pixel up. Press any key to go back.\r\n");
  out(image);
  char k;
  (void)!read(STDIN_FILENO, &k, 1);
  out("\x1b[?1049l");
}

// ---- Page 4: Sixel pictures and text sharing the same cells --------------------
//
// A Sixel picture lives in the character cells it covers. These small tests
// do things to the text around and over a picture, and ask what you saw.

struct Cells {
  int w = 10, h = 20, rows = 24, cols = 80;
};

static Cells cell_info() {
  Cells c;
  winsize ws{};
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  if (ws.ws_row) c.rows = ws.ws_row;
  if (ws.ws_col) c.cols = ws.ws_col;
  Asked a = ask("\x1b[16t");
  for (const Csi& r : csi_replies(a.answer))
    if (r.final == 't' && r.n.size() >= 3 && r.n[0] == 6 && r.n[1] > 0 && r.n[2] > 0) {
      c.h = r.n[1];
      c.w = r.n[2];
    }
  return c;
}

static const int PIC_COLS = 40, PIC_ROWS = 10, PIC_TOP = 4, PIC_LEFT = 2, PROMPT_ROW = 17;

// A picture exactly PIC_COLS x PIC_ROWS cells: a frame, diagonals and ticks.
static std::string test_picture(const Cells& c) {
  using vtpen::Point;
  const int pw = PIC_COLS * c.w, ph = PIC_ROWS * c.h;
  vtpen::Canvas v(pw, ph);
  v.set_colour({60, 220, 255});
  v.set_thickness(3);
  v.polyline({{2, 2}, {pw - 2.0, 2}, {pw - 2.0, ph - 2.0}, {2, ph - 2.0}, {2, 2}});
  v.set_colour({255, 150, 40});
  v.set_thickness(2);
  v.line({2, 2}, {pw - 2.0, ph - 2.0});
  v.line({pw - 2.0, 2}, {2, ph - 2.0});
  v.set_colour({120, 255, 120});
  v.set_thickness(1);
  for (int i = 1; i < PIC_COLS; i++) v.line({double(i * c.w), 0}, {double(i * c.w), 6.0});
  for (int j = 1; j < PIC_ROWS; j++) v.line({0, double(j * c.h)}, {6.0, double(j * c.h)});
  return v.sixel();
}

static void goto_rc(int r, int c) { out("\x1b[" + std::to_string(r) + ";" + std::to_string(c) + "H"); }
static void pause_ms(int ms) { usleep(static_cast<useconds_t>(ms) * 1000); }

static void begin_test(const std::string& title, const std::string& what) {
  out("\x1b[0m\x1b[2J\x1b[H" + title + "\r\n" + what + "\r\n");
}

static void put_picture(const Cells& c) {
  goto_rc(PIC_TOP, PIC_LEFT);
  out(test_picture(c));
}

// Show a question on the prompt row and wait for one of `keys`.
static char choose(const std::string& question, const std::string& keys, int row = PROMPT_ROW) {
  goto_rc(row, 1);
  out("\x1b[0m\x1b[J" + question);
  for (;;) {
    char k;
    if (read(STDIN_FILENO, &k, 1) != 1) return '?';
    if (keys.find(k) != std::string::npos) return k;
  }
}

static void page_overlap() {
  Cells c = cell_info();
  if (c.rows < 20 || c.cols < 50) {
    out("\x1b[0m\r\nPage 4 needs a window of at least 50 columns and 20 rows. Make it bigger and try again.\r\n");
    return;
  }
  std::vector<std::string> results;
  out("\x1b[?1049h");

  begin_test("Page 4: Sixel pictures and text sharing the same cells",
             "Five small tests. Each draws a picture, does something to the text, and asks what you saw.\r\n"
             "Press any key to start.");
  { char k; (void)!read(STDIN_FILENO, &k, 1); }

  // A: text written over a picture.
  begin_test("Test A of 5: text written over the picture",
             "A picture is drawn, then a line of text (white on red) is written across its middle row.");
  put_picture(c);
  pause_ms(1200);
  goto_rc(PIC_TOP + PIC_ROWS / 2, PIC_LEFT + 6);
  out("\x1b[1;37;41m TEXT WRITTEN OVER THE PICTURE \x1b[0m");
  char a = choose("1 = text shows, picture still there around it   2 = text shows, picture gone where the text is\r\n"
                  "3 = picture gone completely   4 = something else: ", "1234");
  results.push_back(std::string("A text over picture: ") + a +
                    (a == '1' ? " (text on top, picture kept)" : a == '2' ? " (text replaces the picture cells)" :
                     a == '3' ? " (whole picture lost)" : " (other)"));

  // B: a picture drawn over text.
  begin_test("Test B of 5: a picture drawn over text",
             "Rows of X are written first, then the picture (empty parts transparent) is drawn on top.");
  for (int r = 0; r < PIC_ROWS; r++) {
    goto_rc(PIC_TOP + r, PIC_LEFT);
    out("\x1b[32m" + std::string(PIC_COLS, 'X') + "\x1b[0m");
  }
  pause_ms(1200);
  put_picture(c);
  char b = choose("1 = X's still visible between the lines   2 = X's gone, picture on empty background\r\n"
                  "3 = something else: ", "123");
  results.push_back(std::string("B picture over text: ") + b +
                    (b == '1' ? " (text shows through the empty parts)" : b == '2' ? " (picture replaces the text)" : " (other)"));

  // C: text changing below the picture.
  begin_test("Test C of 5: text changing beside the picture",
             "The picture stays; a counter line under it is rewritten ten times.");
  put_picture(c);
  for (int i = 1; i <= 10; i++) {
    goto_rc(PIC_TOP + PIC_ROWS + 1, PIC_LEFT);
    out("\x1b[2Kcounter " + std::to_string(i) + " of 10");
    pause_ms(200);
  }
  char cc = choose("1 = picture stayed steady   2 = picture flickered or blinked\r\n"
                   "3 = picture got damaged or moved   4 = something else: ", "1234");
  results.push_back(std::string("C text changing beside it: ") + cc +
                    (cc == '1' ? " (steady)" : cc == '2' ? " (flicker)" : cc == '3' ? " (damaged)" : " (other)"));

  // E: erasing a line through the picture.
  begin_test("Test D of 5: erasing part of a row through the picture",
             "Row 3 of the picture is erased from its middle to the right (ESC [ K); a moment later row 7 is erased entirely (ESC [ 2 K).");
  put_picture(c);
  pause_ms(1500);
  goto_rc(PIC_TOP + 2, PIC_LEFT + PIC_COLS / 2);
  out("\x1b[K");
  pause_ms(1500);
  goto_rc(PIC_TOP + 6, 1);
  out("\x1b[2K");
  char e = choose("1 = only those parts of the picture vanished   2 = the whole picture vanished\r\n"
                  "3 = nothing vanished   4 = something else: ", "1234");
  results.push_back(std::string("D erasing rows through it: ") + e +
                    (e == '1' ? " (only the erased cells lost their pixels)" : e == '2' ? " (whole picture lost)" :
                     e == '3' ? " (erase did not touch it)" : " (other)"));

  // F: scrolling.
  begin_test("Test E of 5: scrolling the screen",
             "j = scroll up one line by newline   u = ESC [ S (scroll up)   d = ESC [ T (scroll down)   q = done");
  put_picture(c);
  std::string hint = "j scroll up (newline)   u ESC[S   d ESC[T   q done";
  goto_rc(c.rows, 1);
  out(hint);
  for (;;) {
    char k;
    if (read(STDIN_FILENO, &k, 1) != 1) break;
    if (k == 'q') break;
    if (k == 'j') { goto_rc(c.rows, 1); out("\r\n\x1b[2K" + hint); }
    if (k == 'u') { out("\x1b[S"); goto_rc(c.rows, 1); out("\x1b[2K" + hint); }
    if (k == 'd') { out("\x1b[T"); goto_rc(c.rows, 1); out("\x1b[2K" + hint); }
  }
  char f = choose("Newline scroll (j): 1 = picture moved up with the text   2 = stayed where it was\r\n"
                  "3 = vanished or garbled   4 = did not try: ", "1234");
  char g = choose("ESC [ S / ESC [ T (u, d): 1 = picture moved with the text   2 = stayed where it was\r\n"
                  "3 = vanished or garbled   4 = did not try: ", "1234");
  results.push_back(std::string("E1 scroll by newline: ") + f +
                    (f == '1' ? " (picture scrolls with the text)" : f == '2' ? " (picture stays put)" :
                     f == '3' ? " (vanished or garbled)" : " (not tried)"));
  results.push_back(std::string("E2 scroll by ESC[S / ESC[T: ") + g +
                    (g == '1' ? " (picture scrolls with the text)" : g == '2' ? " (picture stays put)" :
                     g == '3' ? " (vanished or garbled)" : " (not tried)"));

  out("\x1b[?1049l");
  out("\x1b[0m\r\nPage 4 results (paste these back):\r\n");
  for (const std::string& r : results) out("  " + r + "\r\n");
  out("\r\n");
}

// ---- Page 5: a map (one picture) with real text on top ----------------------------
//
// The map is drawn once as a Sixel picture. The place names are ordinary text
// written over it. A marker (an @) then moves along a road as text, so moving
// it costs a few bytes, not a new picture. Writing into a cell wipes the
// picture pixels in that cell, so when the marker leaves, that one cell of the
// picture is sent again (a small "patch") after the space that clears the @.

struct UV {
  double u, v;  // 0..1 across and down the map
};

static const std::vector<UV> MAP_COAST = {{0, .35}, {.08, .30}, {.15, .34}, {.22, .25}, {.30, .22}, {.36, .28},
    {.42, .20}, {.50, .18}, {.58, .24}, {.66, .22}, {.74, .28}, {.80, .38}, {.86, .46}, {.92, .58}, {.97, .70}, {1, .74}};
static const std::vector<UV> MAP_ISLAND = {{.12, .10}, {.18, .07}, {.26, .09}, {.28, .14}, {.22, .17}, {.14, .15}, {.12, .10}};
static const std::vector<UV> MAP_RIVER = {{.45, .22}, {.40, .35}, {.46, .48}, {.42, .62}, {.48, .75}, {.50, 1}};
static const std::vector<UV> MAP_ROAD_A = {{.18, .36}, {.30, .45}, {.45, .50}, {.62, .46}, {.78, .50}, {.90, .62}};
static const std::vector<UV> MAP_ROAD_B = {{.45, .50}, {.52, .70}, {.60, .90}};
static const std::vector<UV> MAP_ROAD_C = {{.30, .45}, {.25, .65}, {.20, .88}};

struct City {
  const char* name;
  double u, v;
};
static const City CITIES[] = {{"Port Hale", .18, .36}, {"Millbrook", .30, .45}, {"Riverton", .45, .50},
    {"Eastgate", .62, .46}, {"Farhaven", .90, .62}, {"Dunmore", .52, .70}, {"Salt Lick", .60, .90},
    {"Oakridge", .25, .65}};

// Draws the whole map into `m` (pw x ph pixels), turned by `angle` radians
// about the middle of the picture. The frame stays still.
static void draw_map(vtpen::Canvas& m, int pw, int ph, double angle) {
  using vtpen::Point;
  const double cx = (pw - 1) / 2.0, cy = (ph - 1) / 2.0;
  const double ca = std::cos(angle), sa = std::sin(angle);
  auto px = [&](UV a) {
    const double x = a.u * (pw - 1) - cx, y = a.v * (ph - 1) - cy;
    return Point{cx + x * ca - y * sa, cy + x * sa + y * ca};
  };
  auto path = [&](const std::vector<UV>& v) {
    std::vector<Point> p;
    for (const UV& a : v) p.push_back(px(a));
    return p;
  };
  m.set_thickness(1);
  m.set_colour({35, 40, 60});  // faint grid, long enough to cover the corners when turned
  for (int i = -5; i <= 15; i++) {
    if (i == 0 || i == 10) continue;
    m.line(px({i / 10.0, -0.6}), px({i / 10.0, 1.6}));
    m.line(px({-0.6, i / 10.0}), px({1.6, i / 10.0}));
  }
  m.set_colour({150, 150, 180});
  m.set_thickness(2);
  m.polyline({{1, 1}, {pw - 2.0, 1}, {pw - 2.0, ph - 2.0}, {1, ph - 2.0}, {1, 1}});
  m.set_colour({90, 200, 120});  // land edge
  m.set_thickness(3);
  m.polyline(path(MAP_COAST));
  m.polyline(path(MAP_ISLAND));
  m.set_colour({70, 140, 255});  // river
  m.set_thickness(2);
  m.polyline(path(MAP_RIVER));
  m.set_colour({230, 140, 50});  // roads
  m.set_thickness(2);
  m.polyline(path(MAP_ROAD_A));
  m.set_thickness(1);
  m.polyline(path(MAP_ROAD_B));
  m.polyline(path(MAP_ROAD_C));
  m.set_colour({255, 255, 255});  // towns
  m.set_thickness(2);
  for (const City& t : CITIES) {
    Point a = px({t.u, t.v});
    m.line({a.x - 5, a.y}, {a.x + 5, a.y});
    m.line({a.x, a.y - 5}, {a.x, a.y + 5});
  }
}

static void page_map() {
  Cells c = cell_info();
  const int cols = std::min(c.cols - 3, 72), rows = std::min(c.rows - 9, 22);
  if (cols < 40 || rows < 12) {
    out("\x1b[0m\r\nPage 5 needs a window of at least 43 columns and 21 rows. Make it bigger and try again.\r\n");
    return;
  }
  const int top = 3, left = 2;
  const int pw = cols * c.w, ph = rows * c.h;
  vtpen::Canvas m(pw, ph);
  draw_map(m, pw, ph, 0.0);
  const std::string picture = m.sixel();

  auto cell_of = [&](double u, double v, int& col, int& row) {
    col = std::clamp(static_cast<int>(u * cols), 0, cols - 1);
    row = std::clamp(static_cast<int>(v * rows), 0, rows - 1);
  };

  // The text layer: what is written over the picture, kept so we can restore it.
  std::vector<std::string> layer(static_cast<size_t>(rows), std::string(static_cast<size_t>(cols), ' '));
  for (const City& t : CITIES) {
    int col, row;
    cell_of(t.u, t.v, col, row);
    std::string name = t.name;
    int at = col + 1;
    if (at + static_cast<int>(name.size()) > cols) at = col - static_cast<int>(name.size());
    if (at < 0) continue;
    layer[static_cast<size_t>(row)].replace(static_cast<size_t>(at), name.size(), name);
  }
  size_t tiles = 0, tile_bytes = 0;
  auto put_cell = [&](int col, int row, bool marker) {
    goto_rc(top + row, left + col);
    if (marker) { out("\x1b[1;93m@\x1b[0m"); return; }
    char ch = layer[static_cast<size_t>(row)][static_cast<size_t>(col)];
    if (ch != ' ') { out(std::string("\x1b[1;37m") + ch + "\x1b[0m"); return; }
    // A blank cell: write a space (this also removes the @), then patch the
    // picture back into just this cell if there was anything drawn in it.
    out("\x1b[0m ");
    const int x = col * c.w, y = row * c.h;
    if (!m.empty(x, y, c.w, c.h)) {
      const size_t before = bytes_out;
      goto_rc(top + row, left + col);
      out(m.sixel(x, y, c.w, c.h));
      tiles++;
      tile_bytes += bytes_out - before;
    }
  };

  out("\x1b[?1049h\x1b[0m\x1b[2J\x1b[H");
  out("Page 5: a map drawn once, place names and a moving marker as text, cells patched behind the marker. Any key stops it.\r\n");
  const size_t before_picture = bytes_out;
  goto_rc(top, left);
  out(picture);
  const size_t picture_bytes = bytes_out - before_picture;
  for (int r = 0; r < rows; r++)
    for (int col = 0; col < cols; col++)
      if (layer[static_cast<size_t>(r)][static_cast<size_t>(col)] != ' ') put_cell(col, r, false);
  const int status_row = top + rows + 1, prompt_row = top + rows + 3;
  goto_rc(status_row, left);
  out("unit 7 ready");
  pause_ms(1500);

  // Walk along the main road, 100 ms per step, 14 s end to end.
  std::vector<UV> route = MAP_ROAD_A;
  std::vector<double> len(1, 0.0);
  for (size_t i = 1; i < route.size(); i++) {
    using vtpen::Point;
    Point a{route[i - 1].u * (pw - 1), route[i - 1].v * (ph - 1)}, b{route[i].u * (pw - 1), route[i].v * (ph - 1)};
    len.push_back(len.back() + std::hypot(b.x - a.x, b.y - a.y));
  }
  const int steps = 140;
  int last_col = -1, last_row = -1, cells_moved = 0;
  const size_t before_walk = bytes_out;
  for (int step = 0; step <= steps; step++) {
    double d = len.back() * step / steps;
    size_t seg = 1;
    while (seg + 1 < route.size() && len[seg] < d) seg++;
    double f = (d - len[seg - 1]) / std::max(1e-9, len[seg] - len[seg - 1]);
    double u = route[seg - 1].u + (route[seg].u - route[seg - 1].u) * f;
    double v = route[seg - 1].v + (route[seg].v - route[seg - 1].v) * f;
    int col, row;
    cell_of(u, v, col, row);
    if (col != last_col || row != last_row) {
      if (last_col >= 0) put_cell(last_col, last_row, false);
      put_cell(col, row, true);
      last_col = col;
      last_row = row;
      cells_moved++;
      goto_rc(status_row, left);
      out("\x1b[2Kunit 7  cell " + std::to_string(col) + "," + std::to_string(row) + "  step " +
          std::to_string(step) + "/" + std::to_string(steps));
    }
    pollfd pf{STDIN_FILENO, POLLIN, 0};
    if (poll(&pf, 1, 100) > 0) { char k; (void)!read(STDIN_FILENO, &k, 1); break; }
  }
  const size_t walk_bytes = bytes_out - before_walk;

  std::string stats = "picture " + std::to_string(picture_bytes) + " bytes once; marker over " +
                      std::to_string(cells_moved) + " cell moves: " + std::to_string(walk_bytes) + " bytes (" +
                      std::to_string(tiles) + " cells patched, " + std::to_string(tile_bytes) + " bytes of that)";
  goto_rc(prompt_row - 1, left);
  out("\x1b[2K" + stats);
  char a = choose("Behind the marker's path: 1 = map intact   2 = holes in the lines   3 = a stray @ left behind   4 = other: ",
                  "1234", prompt_row);
  char b = choose("Place names: 1 = always readable   2 = hidden or garbled   3 = other: ", "123", prompt_row + 1);
  out("\x1b[?1049l");
  out("\x1b[0m\r\nPage 5 results (paste these back):\r\n");
  out("  " + stats + "\r\n");
  out(std::string("  F map behind marker: ") + a +
      (a == '1' ? " (intact, cells patched)" : a == '2' ? " (holes remain)" : a == '3' ? " (stray @ left)" : " (other)") + "\r\n");
  out(std::string("  G place names: ") + b +
      (b == '1' ? " (readable)" : b == '2' ? " (hidden or garbled)" : " (other)") + "\r\n\r\n");
}

// ---- Page 6: the map turning, and what that costs in bandwidth -------------------
//
// Every frame the map is drawn again at a new angle and the whole picture is
// sent. The background is opaque so each frame fully replaces the last.

// full = false: page 6, a picture up to 72x22 cells. full = true: page 7, a
// picture that fills the window.
static void rotate_run(bool full) {
  const char* page = full ? "Page 7" : "Page 6";
  Cells c = cell_info();
  const int cols = full ? c.cols - 3 : std::min(c.cols - 3, 72);
  const int rows = full ? c.rows - 7 : std::min(c.rows - 9, 22);
  if (cols < 40 || rows < 12) {
    out(std::string("\x1b[0m\r\n") + page + " needs a window of at least 43 columns and " +
        (full ? "19" : "21") + " rows. Make it bigger and try again.\r\n");
    return;
  }
  const int top = 3, left = 2, pw = cols * c.w, ph = rows * c.h;
  const int status_row = top + rows + 1, prompt_row = top + rows + 3;

  struct Stage {
    const char* name;
    int fps;     // 0 = as fast as it will go
    bool sync;   // wrap each frame in synchronized output (mode 2026)
  };
  std::vector<Stage> stages = {{"5 frames/s", 5, false}, {"10 frames/s", 10, false}, {"20 frames/s", 20, false},
                               {"as fast as it will go", 0, false}, {"as fast as it will go, synchronized output", 0, true}};
  if (full) stages.pop_back();  // page 7 skips the synchronized output stage
  const double seconds = 4.0, degrees_per_second = 30.0, pi = 3.14159265358979;

  std::string limit_note;
  if (full) {  // what the terminal says its largest Sixel picture is
    Asked g = ask("\x1b[?2;1;0S");
    for (const Csi& r : csi_replies(g.answer))
      if (r.final == 'S' && r.lead == "?" && r.n.size() >= 4 && r.n[0] == 2 && r.n[1] == 0) {
        limit_note = "The terminal says its largest Sixel picture is " + std::to_string(r.n[2]) + "x" +
                     std::to_string(r.n[3]) + (pw > r.n[2] || ph > r.n[3] ? " (SMALLER than ours)" : " (ours fits)") + ".";
      }
    if (limit_note.empty()) limit_note = "The terminal does not report a largest Sixel picture size.";
  }
  const std::string size_note = "The picture is " + std::to_string(pw) + "x" + std::to_string(ph) + " pixels (" +
                                std::to_string(cols) + "x" + std::to_string(rows) + " cells).";
  const size_t n_stages = stages.size();
  out("\x1b[?1049h\x1b[0m\x1b[2J\x1b[H\x1b[?25l");
  out(std::string(page) + ": the map turns (30 degrees a second) and every frame is sent whole, at " +
      (n_stages == 5 ? "five" : "four") + " speeds, 4 s each.\r\n" + size_note + (full ? " " + limit_note : "") + "\r\n"
      "After each one you rate how it looked. Press q at any time to stop. Press any key to start.");
  { char k; (void)!read(STDIN_FILENO, &k, 1); }

  using Clock = std::chrono::steady_clock;
  auto secs = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
  std::vector<std::string> results;
  bool quit = false;
  for (const Stage& st : stages) {
    if (quit) break;
    out(std::string("\x1b[0m\x1b[2J\x1b[H") + "Stage: " + st.name + "\r\n");
    size_t frames = 0;
    const size_t bytes_before = bytes_out;
    double compute = 0;
    const auto t0 = Clock::now();
    for (;;) {
      const double now = secs(Clock::now() - t0);
      if (now >= seconds) break;
      if (st.fps > 0) {
        const double due = static_cast<double>(frames) / st.fps;
        if (due > now) { pause_ms(static_cast<int>((due - now) * 1000)); continue; }
      }
      const auto c0 = Clock::now();
      vtpen::Canvas m(pw, ph);
      m.set_background({0, 0, 0});
      m.set_transparent(false);
      m.clear();
      draw_map(m, pw, ph, now * degrees_per_second * pi / 180.0);
      std::string image = m.sixel();
      compute += secs(Clock::now() - c0);
      std::string frame;
      if (st.sync) frame += "\x1b[?2026h";
      frame += "\x1b[" + std::to_string(top) + ";" + std::to_string(left) + "H" + image;
      if (st.sync) frame += "\x1b[?2026l";
      out(frame);
      frames++;
      pollfd pf{STDIN_FILENO, POLLIN, 0};
      if (poll(&pf, 1, 0) > 0) { char k; if (read(STDIN_FILENO, &k, 1) == 1 && k == 'q') { quit = true; break; } }
    }
    const double wall = secs(Clock::now() - t0);
    const size_t bytes = bytes_out - bytes_before;
    if (frames == 0) break;
    char line[256];
    snprintf(line, sizeof line, "%zu frames in %.1f s = %.1f frames/s, %.1f KB a frame, %.0f KB/s, drawing and encoding %.1f ms a frame",
             frames, wall, frames / wall, bytes / 1024.0 / frames, bytes / 1024.0 / wall, compute * 1000 / frames);
    goto_rc(status_row, left);
    out(std::string("\x1b[0m\x1b[2K") + line);
    if (quit) break;
    char r = choose("How did it look? 1 = smooth   2 = jerky or slow   3 = flickers   4 = kept moving after it stopped (lag): ",
                    "1234", prompt_row);
    results.push_back(std::string(st.name) + ": " + line + "; looked " + r +
                      (r == '1' ? " (smooth)" : r == '2' ? " (jerky or slow)" : r == '3' ? " (flickers)" : " (lagged)"));
  }
  out(std::string("\x1b[?25h\x1b[?1049l\x1b[0m\r\n") + page + " results (paste these back):\r\n");
  out("  " + size_note + (full ? " " + limit_note : "") + "\r\n");
  for (const std::string& r : results) out("  " + r + "\r\n");
  out("\r\n");
}

static void page_rotate() { rotate_run(false); }
static void page_rotate_full() { rotate_run(true); }

// ---- Page 8: every shape the engine can draw ---------------------------------------
//
// One picture, drawn once. The labels are ordinary terminal text over it.

static void page_shapes() {
  Cells c = cell_info();
  const int cols = std::min(c.cols - 3, 90), rows = std::min(c.rows - 6, 30);
  if (cols < 60 || rows < 18) {
    out("\x1b[0m\r\nPage 8 needs a window of at least 63 columns and 24 rows. Make it bigger and try again.\r\n");
    return;
  }
  // The picture has the same shape as the 800x480 drawing space, so circles stay round.
  const int top = 3, left = 2, pw = std::min(cols * c.w, rows * c.h * 5 / 3), ph = pw * 3 / 5;
  using vtpen::Point;
  vtpen::Canvas m(pw, ph, 800, 480);
  const double pi = 3.14159265358979;

  auto colour = [&](int r, int g, int b) { m.set_colour({static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)}); };
  const double X[5] = {80, 240, 400, 560, 720};

  // Row 1: outlines.
  m.set_thickness(3);
  colour(90, 200, 255);
  m.circle(X[0], 70, 50);
  m.ellipse(X[1], 70, 68, 36);
  m.rectangle(X[2] - 55, 25, 110, 90);
  m.polygon({{X[3], 20}, {X[3] + 58, 112}, {X[3] - 58, 112}});
  colour(255, 200, 80);
  m.set_thickness(6);
  m.arc(X[4], 70, 48, -30, 240);

  // Row 2: filled.
  colour(235, 70, 70);
  m.fill_circle(X[0], 235, 52);
  colour(80, 210, 120);
  m.fill_ellipse(X[1], 235, 68, 36);
  colour(90, 120, 255);
  m.fill_rectangle(X[2] - 55, 190, 110, 90);
  colour(255, 220, 60);
  std::vector<Point> star;  // a five pointed star: the middle is left empty (even-odd)
  for (int i = 0; i < 5; i++) {
    double a = -pi / 2 + i * 4 * pi / 5;
    star.push_back({X[3] + 58 * std::cos(a), 238 + 58 * std::sin(a)});
  }
  m.fill_polygon(star);
  colour(255, 140, 40);
  m.fill_polygon({{X[4] - 55, 220}, {X[4] + 5, 220}, {X[4] + 5, 195}, {X[4] + 58, 235}, {X[4] + 5, 275}, {X[4] + 5, 250}, {X[4] - 55, 250}});

  // Row 3: curves.
  m.set_thickness(3);
  colour(255, 120, 200);
  m.bezier({X[0] - 55, 440}, {X[0] - 55, 330}, {X[0] + 55, 450}, {X[0] + 55, 340});
  colour(150, 255, 150);
  m.spline({{X[1] - 70, 400}, {X[1] - 35, 340}, {X[1], 430}, {X[1] + 35, 340}, {X[1] + 70, 400}});
  colour(255, 255, 255);
  m.set_thickness(2);
  for (int i = 0; i < 4; i++) m.arc(X[2], 390, 14 + i * 11, 20 + i * 70, 200 + i * 70);
  colour(120, 200, 255);
  m.set_thickness(4);
  m.bezier({X[3] - 60, 350}, {X[3] + 90, 330}, {X[3] - 90, 450}, {X[3] + 60, 430});
  colour(255, 180, 120);
  m.set_thickness(2);
  std::vector<Point> sp;
  for (int i = 0; i <= 28; i++) sp.push_back({X[4] + (i * 5.0 - 70) , 390 + 45 * std::sin(i * 0.9)});
  m.polyline(sp);

  const std::string image = m.sixel();
  out("\x1b[?1049h\x1b[0m\x1b[2J\x1b[H");
  out("Page 8: every shape (" + std::to_string(pw) + "x" + std::to_string(ph) + " pixels, " + std::to_string(image.size()) +
      " bytes). The labels are real text. Press any key to go back.\r\n");
  goto_rc(top, left);
  out(image);

  struct Label { double x, y; const char* text; };
  const Label labels[] = {
      {X[0], 135, "circle"}, {X[1], 135, "ellipse"}, {X[2], 135, "rectangle"}, {X[3], 135, "polygon"}, {X[4], 135, "arc (thick)"},
      {X[0], 300, "fill_circle"}, {X[1], 300, "fill_ellipse"}, {X[2], 300, "fill_rectangle"}, {X[3], 300, "star, even-odd"}, {X[4], 300, "fill_polygon (arrow)"},
      {X[0], 470, "bezier"}, {X[1], 470, "spline"}, {X[2], 470, "arcs"}, {X[3], 470, "bezier, thick"}, {X[4], 470, "polyline (wave)"}};
  for (const Label& l : labels) {
    const int col = static_cast<int>(l.x / 800 * pw / c.w) - static_cast<int>(strlen(l.text)) / 2;
    const int row = static_cast<int>(l.y / 480 * ph / c.h);
    goto_rc(top + row, left + std::max(0, col));
    out(std::string("\x1b[1;37m") + l.text + "\x1b[0m");
  }
  char k;
  (void)!read(STDIN_FILENO, &k, 1);
  out("\x1b[?1049l");
}

// ---- Page 9: a scene with a turning arrow over a map, only changed cells sent --------
//
// The map and the arrow are objects in a Scene. Every frame the whole scene is
// drawn, and CellSender works out which character cells differ from what the
// terminal already shows and sends only those. The place names are real text, so
// after a cell is replaced the letters in it are written again.

static void page_scene() {
  Cells c = cell_info();
  const int cols = std::min(c.cols - 3, 90), rows = std::min(c.rows - 8, 30);
  if (cols < 60 || rows < 18) {
    out("\x1b[0m\r\nPage 9 needs a window of at least 63 columns and 26 rows. Make it bigger and try again.\r\n");
    return;
  }
  using namespace vtpen;
  const int top = 3, left = 2, pw = std::min(cols * c.w, rows * c.h * 5 / 3), ph = pw * 3 / 5;
  const int pcols = (pw + c.w - 1) / c.w, prows = (ph + c.h - 1) / c.h;

  auto P = [](UV a) { return Point{a.u * 799, a.v * 479}; };
  auto path = [&](const std::vector<UV>& v) {
    std::vector<Point> p;
    for (const UV& a : v) p.push_back(P(a));
    return p;
  };
  Symbol grid, land, water, roads, towns;
  grid.set_colour({35, 40, 60});
  for (int i = 1; i < 10; i++) {
    grid.line({i * 79.9, 0}, {i * 79.9, 479});
    grid.line({0, i * 47.9}, {799, i * 47.9});
  }
  land.set_colour({90, 200, 120});
  land.set_thickness(3);
  land.polyline(path(MAP_COAST));
  land.polyline(path(MAP_ISLAND));
  water.set_colour({70, 140, 255});
  water.set_thickness(2);
  water.polyline(path(MAP_RIVER));
  roads.set_colour({230, 140, 50});
  roads.set_thickness(2);
  roads.polyline(path(MAP_ROAD_A));
  roads.set_thickness(1);
  roads.polyline(path(MAP_ROAD_B));
  roads.polyline(path(MAP_ROAD_C));
  towns.set_colour({255, 255, 255});
  towns.set_thickness(2);
  for (const City& t : CITIES) {
    Point a = P({t.u, t.v});
    towns.line({a.x - 7, a.y}, {a.x + 7, a.y});
    towns.line({a.x, a.y - 7}, {a.x, a.y + 7});
  }
  Symbol arrow;  // drawn around its own centre, pointing right
  arrow.fill_polygon({{-30, -8}, {6, -8}, {6, -20}, {34, 0}, {6, 20}, {6, 8}, {-30, 8}});

  Scene scene;
  scene.add(grid, 0, 0, 0, 1, 0);
  scene.add(land, 0, 0, 0, 1, 0);
  scene.add(water, 0, 0, 0, 1, 0);
  scene.add(roads, 0, 0, 0, 1, 0);
  scene.add(towns, 0, 0, 0, 1, 0);
  const int unit = scene.add(arrow, 0, 0, 0, 1, 1);
  scene.set_colour(unit, {255, 235, 60});

  Canvas m(pw, ph, 800, 480);
  m.set_background({0, 0, 0});
  m.set_transparent(false);
  CellSender sender(c.w, c.h);

  // Place names as real text, remembered per cell.
  std::vector<std::string> layer(static_cast<size_t>(prows), std::string(static_cast<size_t>(pcols), ' '));
  for (const City& t : CITIES) {
    int col = static_cast<int>(t.u * 799 / 800.0 * pw / c.w) + 1, row = static_cast<int>(t.v * 479 / 480.0 * ph / c.h);
    std::string name = t.name;
    if (col + static_cast<int>(name.size()) > pcols) col = pcols - static_cast<int>(name.size());
    if (row < prows && col >= 0) layer[static_cast<size_t>(row)].replace(static_cast<size_t>(col), name.size(), name);
  }
  auto write_labels = [&](const std::vector<std::pair<int, int>>& cells) {
    for (const auto& cell : cells) {
      const char ch = layer[static_cast<size_t>(cell.second)][static_cast<size_t>(cell.first)];
      if (ch == ' ') continue;
      goto_rc(top + cell.second, left + cell.first);
      out(std::string("\x1b[1;37m") + ch + "\x1b[0m");
    }
  };

  out("\x1b[?1049h\x1b[0m\x1b[2J\x1b[H\x1b[?25l");
  out("Page 9: a map and a yellow arrow as scene objects; only the cells that change are sent. Press q to stop.\r\n");
  std::vector<std::pair<int, int>> all_cells;
  for (int r = 0; r < prows; r++) for (int col = 0; col < pcols; col++) all_cells.push_back({col, r});

  using Clock = std::chrono::steady_clock;
  auto secs = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
  const double pi = 3.14159265358979;
  const int status_row = top + prows + 1, prompt_row = top + prows + 3;
  std::vector<std::string> results;
  bool quit = false;

  // First picture: sent whole, then the place names.
  scene.move(unit, P({.45, .50}).x, P({.45, .50}).y);
  scene.render(m);
  const size_t before_first = bytes_out;
  goto_rc(top, left);
  out(sender.update(m, top, left));
  write_labels(all_cells);
  const size_t first_bytes = bytes_out - before_first;
  const size_t full_frame = m.sixel().size();

  struct Phase { const char* name; double seconds; bool drive; };
  const Phase phases[] = {{"the arrow turns on the spot (90 degrees a second)", 5, false},
                          {"the arrow drives along the road, pointing the way it goes", 9, true}};
  // Arc length along road A, for driving at an even speed.
  std::vector<Point> route = path(MAP_ROAD_A);
  std::vector<double> len(1, 0.0);
  for (size_t i = 1; i < route.size(); i++) len.push_back(len.back() + std::hypot(route[i].x - route[i - 1].x, route[i].y - route[i - 1].y));

  for (const Phase& ph_ : phases) {
    if (quit) break;
    size_t frames = 0, bytes = 0, cells = 0;
    const auto t0 = Clock::now();
    for (;;) {
      const double now = secs(Clock::now() - t0);
      if (now >= ph_.seconds) break;
      const double due = static_cast<double>(frames) / 20;   // 20 frames a second
      if (due > now) { pause_ms(static_cast<int>((due - now) * 1000)); continue; }
      if (!ph_.drive) {
        scene.rotate(unit, now * 90);
      } else {
        const double d = len.back() * now / ph_.seconds;
        size_t seg = 1;
        while (seg + 1 < route.size() && len[seg] < d) seg++;
        const double f = (d - len[seg - 1]) / std::max(1e-9, len[seg] - len[seg - 1]);
        const Point a = route[seg - 1], b = route[seg];
        scene.move(unit, a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f);
        scene.rotate(unit, std::atan2(b.y - a.y, b.x - a.x) * 180 / pi);
      }
      scene.render(m);
      const size_t before = bytes_out;
      out(sender.update(m, top, left));
      write_labels(sender.changed());
      bytes += bytes_out - before;
      cells += sender.changed().size();
      frames++;
      pollfd pf{STDIN_FILENO, POLLIN, 0};
      if (poll(&pf, 1, 0) > 0) { char k; if (read(STDIN_FILENO, &k, 1) == 1 && k == 'q') { quit = true; break; } }
    }
    if (frames == 0) break;
    char line[300];
    snprintf(line, sizeof line, "%s: %zu frames, %.1f KB a frame, %.1f cells a frame (of %d), whole picture would be %.1f KB",
             ph_.name, frames, bytes / 1024.0 / frames, static_cast<double>(cells) / frames, pcols * prows, full_frame / 1024.0);
    goto_rc(status_row, left);
    out(std::string("\x1b[0m\x1b[2K") + line);
    results.push_back(line);
    if (quit) break;
    char r = choose("How did it look? 1 = clean   2 = bits of the old arrow or gaps in the map left behind   3 = flickers   4 = other: ",
                    "1234", prompt_row);
    results.back() += std::string("; looked ") + r + (r == '1' ? " (clean)" : r == '2' ? " (leftovers or gaps)" : r == '3' ? " (flickers)" : " (other)");
  }
  out("\x1b[?25h\x1b[?1049l\x1b[0m\r\nPage 9 results (paste these back):\r\n");
  out("  first picture: " + std::to_string(first_bytes) + " bytes (" + std::to_string(pw) + "x" + std::to_string(ph) + " pixels)\r\n");
  for (const std::string& r : results) out("  " + r + "\r\n");
  out("\r\n");
}

// ---- Page 10: pattern fills, and what each costs in bytes ----------------------------

static void page_patterns() {
  Cells c = cell_info();
  const int cols = std::min(c.cols - 3, 90), rows = std::min(c.rows - 6, 30);
  if (cols < 60 || rows < 18) {
    out("\x1b[0m\r\nPage 10 needs a window of at least 63 columns and 24 rows. Make it bigger and try again.\r\n");
    return;
  }
  using namespace vtpen;
  const int top = 3, left = 2, pw = std::min(cols * c.w, rows * c.h * 5 / 3), ph = pw * 3 / 5;
  Canvas m(pw, ph, 800, 480);
  const double sx = static_cast<double>(pw) / 800, sy = static_cast<double>(ph) / 480;

  struct Field { const char* name; std::shared_ptr<const Pattern> pattern; };
  auto P = [](Pattern p) { return std::make_shared<const Pattern>(std::move(p)); };
  const Field fields[] = {
      {"solid", nullptr}, {"horizontal", P(Pattern::horizontal(4, 1))}, {"vertical", P(Pattern::vertical(4, 1))},
      {"diagonal /", P(Pattern::diagonal(6, 1, true))}, {"diagonal \\", P(Pattern::diagonal(6, 1, false))}, {"crosshatch +", P(Pattern::crosshatch(6, 1, true))},
      {"crosshatch x", P(Pattern::crosshatch(6, 1, false))}, {"dots", P(Pattern::dots(4, 2))}, {"checker", P(Pattern::checker(3))},
      {"grey 4/16", P(Pattern::gray(4))}, {"grey 8/16", P(Pattern::gray(8))}, {"grey 12/16", P(Pattern::gray(12))}};
  const int nf = 12;
  const uint8_t colours[12][3] = {{235, 70, 70}, {90, 200, 255}, {90, 200, 255}, {255, 200, 80}, {255, 200, 80}, {120, 230, 120},
                                  {120, 230, 120}, {255, 130, 200}, {200, 160, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};

  struct Placed { int x, y, w, h; std::string label; };
  std::vector<Placed> placed;
  for (int i = 0; i < nf; i++) {
    const int col = i % 4, row = i / 4;
    const double fx = 20 + col * 195, fy = 20 + row * 155, fw = 165, fh = 105;
    m.set_colour({colours[i][0], colours[i][1], colours[i][2]});
    m.set_fill_pattern(fields[i].pattern);
    if (i % 2 == 0) m.fill_rectangle(fx, fy, fw, fh); else m.fill_ellipse(fx + fw / 2, fy + fh / 2, fw / 2, fh / 2);
    // Bytes for just this field's part of the picture.
    const int px = static_cast<int>(fx * sx), py = static_cast<int>(fy * sy);
    const size_t bytes = m.sixel(px, py, static_cast<int>(fw * sx) + 1, static_cast<int>(fh * sy) + 1).size();
    char buf[80];
    snprintf(buf, sizeof buf, "%s  %.1f KB", fields[i].name, bytes / 1024.0);
    placed.push_back({static_cast<int>((fx + fw / 2) * sx), static_cast<int>((fy + fh + 14) * sy), 0, 0, buf});
  }
  m.set_fill_pattern(nullptr);
  const std::string image = m.sixel();

  out("\x1b[?1049h\x1b[0m\x1b[2J\x1b[H");
  out("Page 10: pattern fills (" + std::to_string(pw) + "x" + std::to_string(ph) + " pixels, " + std::to_string(image.size()) +
      " bytes whole). Each label shows what that one field costs. Press any key to go back.\r\n");
  goto_rc(top, left);
  out(image);
  std::vector<std::string> results;
  for (const Placed& p : placed) {
    const int col = p.x / c.w - static_cast<int>(p.label.size()) / 2, row = p.y / c.h;
    goto_rc(top + row, left + std::max(0, col));
    out("\x1b[1;37m" + p.label + "\x1b[0m");
    results.push_back(p.label);
  }
  char k;
  (void)!read(STDIN_FILENO, &k, 1);
  out("\x1b[?1049l\x1b[0m\r\nPage 10 sizes (a 165x105 field in drawing units):\r\n");
  for (const std::string& r : results) out("  " + r + "\r\n");
  out("\r\n");
}

// ---- the report page ------------------------------------------------------

int main() {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "vtprobe: run it in a terminal.\n");
    return 1;
  }
  if (!raw_start()) {
    perror("vtprobe: raw mode");
    return 1;
  }

  winsize ws{};
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  const char* term = getenv("TERM");

  // In raw mode a newline needs an explicit return.
  auto line = [](const std::string& s) { out(s + "\r\n"); };

  line("vtprobe");
  line("TERM=" + std::string(term ? term : "(unset)") + "  size=" +
       std::to_string(ws.ws_col) + "x" + std::to_string(ws.ws_row));
  line("");

  std::vector<Test> all = tests();
  if (all.empty()) line("No tests yet.");

  int count[4] = {0, 0, 0, 0};
  for (const Test& t : all) {
    Result r = t.run();
    count[static_cast<int>(r.status)]++;
    line(std::string(status_name(r.status)) + "  " + t.name +
         (r.note.empty() ? "" : "  - " + r.note));
    for (const std::string& d : r.details) line("           " + d + "\x1b[0m");
  }

  line("");
  line("Done: " + std::to_string(count[0]) + " pass, " + std::to_string(count[1]) +
       " fail, " + std::to_string(count[2]) + " unknown, " +
       std::to_string(count[3]) + " to judge by eye.");
  line("");
  line("Press 2 or Space for page 2 (a Sixel circle), 3 for page 3 (lines), 4 for page 4 (pictures and text), 5 for page 5 (map), 6 for page 6 (turning map), 7 for page 7 (turning map, full window), 8 for page 8 (shapes), 9 for page 9 (turning arrow), 0 for page 10 (patterns), any other key to quit.");
  char key;
  while (read(STDIN_FILENO, &key, 1) == 1) {
    if (key == '2' || key == ' ') page_circle();
    else if (key == '3') page_lines();
    else if (key == '4') page_overlap();
    else if (key == '5') page_map();
    else if (key == '6') page_rotate();
    else if (key == '7') page_rotate_full();
    else if (key == '8') page_shapes();
    else if (key == '9') page_scene();
    else if (key == '0') page_patterns();
    else break;
  }
  return 0;
}

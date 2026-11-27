// Tests for vtpen_probe.h: canned answers from different kinds of terminal, answers in
// another order, answers cut short, and plain garbage. Build and run: ./m test

#include <algorithm>
#include <cstdio>
#include <random>

#include "vtpen_probe.h"

using namespace vtpen;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                      \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static const std::string E = "\x1b";

int main() {
  // ---- What we ask ----
  {
    CHECK(Probe::questions() == Probe::questions(false));                       // the cursor test is off unless asked for
    const std::string q = Probe::questions(true);
    CHECK(q.size() > 3 && q.compare(q.size() - 3, 3, "\x1b[c") == 0);          // the sentinel is last
    for (const char* part : {"\x1b[>0q", "\x1b[>c", "\x1b[?1;1;0S", "\x1b[?2;1;0S", "\x1b[18t", "\x1b[14t", "\x1b[16t",
                             "\x1b[?2026$p", "\x1b[?80$p", "\x1b[?8452$p"})
      CHECK(q.find(part) != std::string::npos);
    // The picture test happens on the alternate screen and leaves it again.
    const size_t on = q.find("\x1b[?1049h"), off = q.find("\x1b[?1049l");
    CHECK(on != std::string::npos && off != std::string::npos && on < off);
    CHECK(q.find("\x1bP0;1;0q") > on && q.find("\x1bP0;1;0q") < off);
    // The picture is 6 pixels wide and 60 tall: ten bands of six rows.
    const size_t pic = q.find("\x1bP0;1;0q"), pic_end = q.find("\x1b\\", pic);
    const std::string head = "\x1bP0;1;0q\"1;1;6;60#0;2";
    CHECK(q.compare(pic, head.size(), head) == 0);
    CHECK(std::count(q.begin() + static_cast<std::ptrdiff_t>(pic), q.begin() + static_cast<std::ptrdiff_t>(pic_end), '-') == 9);
    CHECK(q.find("\x1b[6n") > on && q.find("\x1b[6n") < off);
    const std::string plain = Probe::questions();
    CHECK(plain.find("1049") == std::string::npos && plain.find("\x1bP") == std::string::npos && plain.find("\x1b[6n") == std::string::npos);
    CHECK(plain.compare(plain.size() - 3, 3, "\x1b[c") == 0);
    // The tiny picture's cell is wiped before leaving the alternate screen.
    const size_t picture_end = q.find("\x1b\\", q.find("\x1bP0;1;0q"));
    CHECK(picture_end != std::string::npos && q.find("\x1b[1;1H \x1b[?1049l", picture_end) != std::string::npos);
  }
  // ---- When to stop reading ----
  {
    CHECK(!Probe::complete(""));
    CHECK(!Probe::complete(E + "[?64;4"));                                      // cut short
    CHECK(!Probe::complete(E + "[>0;10;1c"));                                   // the secondary answer is not the last one
    CHECK(!Probe::complete(E + "[?2;0;1000;800S"));
    CHECK(!Probe::complete(E + "[8;40;100t" + E + "[?1;0;256S"));
    CHECK(Probe::complete(E + "[>0;10;1c" + E + "[?61;4c"));
    CHECK(Probe::complete(E + "[?1c"));
    CHECK(Probe::complete("junk" + E + "[8;40;100t" + E + "[?62;4;6c"));
  }
  // ---- A terminal like Windows Terminal ----
  {
    const std::string r = E + "[1;1R" + E + "[4;1R" + E + "[>0;10;1c" + E + "[8;40;110t" + E + "[4;800;1100t" + E + "[6;20;10t" +
                          E + "[?2026;2$y" + E + "[?80;0$y" + E + "[?61;4;6;7;14;21;22;23;24;28;32;42c";
    Capabilities c = Probe::parse(r);
    CHECK(c.answered && c.sixel && c.usable());
    CHECK(c.cell_w == 10 && c.cell_h == 20 && c.cols == 110 && c.rows == 40 && c.window_w == 1100 && c.window_h == 800);
    CHECK(c.name.empty() && c.multiplexer.empty());
    CHECK(c.da2 == (std::vector<int>{0, 10, 1}));
    CHECK(c.da1.size() == 12 && c.da1[0] == 61);
    CHECK(c.max_width == 0 && c.colour_registers == 0);                         // it did not say
    CHECK(c.cursor_after == CursorAfterImage::Below && c.cursor_row_delta == 3 && c.cursor_col_delta == 0);   // 60 pixels is 3 rows of 20
    CHECK(c.sync_output_mode == 2 && c.supports_sync_output());                // it knows the mode (and it is off, which is normal)
    CHECK(!Probe::parse(E + "[?2026;4$y" + E + "[?62c").supports_sync_output());   // permanently off: not supported
    CHECK(!Probe::parse(E + "[?2026;0$y" + E + "[?62c").supports_sync_output());   // does not know the mode
    CHECK(c.sixel_display_mode == 0 && c.sixel_cursor_mode == -1);
    CHECK(c.behaviour.known && c.behaviour.guessed && c.behaviour.cell_write_wipes_pixels &&
          c.behaviour.picture_replaces_cell && c.behaviour.scrolls_with_text && !c.behaviour.basis.empty());
    const std::string text = c.summary();
    CHECK(text.find("Sixel pictures: yes") != std::string::npos && text.find("10x20") != std::string::npos);
    CHECK(text.find("not reported") != std::string::npos);
  }
  // ---- tmux ----
  {
    const std::string r = E + "[1;1R" + E + "[1;1R" + E + "P>|tmux 3.4" + E + "\\" + E + "[>84;0;0c" + E + "[8;70;100t" +
                          E + "[4;2240;1600t" + E + "[6;32;16t" + E + "[?1;2;4c";
    Capabilities c = Probe::parse(r);
    CHECK(c.answered && c.sixel && c.usable());
    CHECK(c.name == "tmux 3.4" && c.multiplexer == "tmux");
    CHECK(c.cell_w == 16 && c.cell_h == 32);
    CHECK(c.cursor_after == CursorAfterImage::Stays);
    CHECK(!c.behaviour.known);                                                  // not a terminal we have looked at
    CHECK(c.summary().find("multiplexer is in between: tmux") != std::string::npos);
    CHECK(c.summary().find("not known for this terminal") != std::string::npos);
  }
  // ---- a rich xterm-like terminal ----
  {
    const std::string r = E + "[1;1R" + E + "[1;3R" + E + "P>|XTerm(370)" + E + "\\" + E + "[>41;370;0c" + E + "[?1;0;1024S" + E + "[?2;0;1000;800S" +
                          E + "[8;24;80t" + E + "[6;16;8t" + E + "[?2026;1$y" + E + "[?80;2$y" + E + "[?8452;2$y" + E + "[?64;1;2;4;6;9;15;22;28c";
    Capabilities c = Probe::parse(r);
    CHECK(c.answered && c.sixel && c.usable());
    CHECK(c.name == "XTerm(370)" && c.multiplexer.empty());
    CHECK(c.colour_registers == 1024 && c.max_width == 1000 && c.max_height == 800);
    CHECK(c.cursor_after == CursorAfterImage::Right && c.cursor_row_delta == 0 && c.cursor_col_delta == 2);
    CHECK(c.supports_sync_output() && c.sixel_display_mode == 2 && c.sixel_cursor_mode == 2);
    CHECK(!c.behaviour.known);                                                  // named, so no guess is made
    // The guess for Windows Terminal needs the exact answer, no name, no multiplexer.
    const std::string da = E + "[>0;10;1c" + E + "[?61;4c";
    CHECK(Probe::parse(da).behaviour.known);
    CHECK(!Probe::parse(E + "P>|SomeTerm 1" + E + "\\" + da).behaviour.known);
    CHECK(!Probe::parse(E + "P>|tmux 3.4" + E + "\\" + da).behaviour.known);
    CHECK(!Probe::parse(E + "[>0;10;2c" + E + "[?61;4c").behaviour.known);
    CHECK(!Probe::parse(E + "[>0;10;1c" + E + "[?61c").behaviour.known);         // and it must draw Sixel
    CHECK(c.summary().find("Largest picture: 1000x800") != std::string::npos);
  }
  // ---- Not Sixel; and no answer at all ----
  {
    Capabilities c = Probe::parse(E + "[>1;10;0c" + E + "[6;16;8t" + E + "[?62;1;6c");
    CHECK(c.answered && !c.sixel && !c.usable() && c.cell_w == 8);
    CHECK(c.summary().find("Sixel pictures: no") != std::string::npos);
    Capabilities none = Probe::parse("");
    CHECK(!none.answered && !none.sixel && !none.usable());
    CHECK(none.summary() == "The terminal did not answer.\n");
    // Some answers but not the last one: it did not finish answering.
    Capabilities part = Probe::parse(E + "[6;16;8t" + E + "[?2;0;1000;800S");
    CHECK(!part.answered && !part.usable());
  }
  // ---- Sixel shown only by the picture questions ----
  {
    Capabilities c = Probe::parse(E + "[?1;0;256S" + E + "[6;20;10t" + E + "[?62c");
    CHECK(c.answered && c.sixel && c.colour_registers == 256);
    // A failed question (status 3) says nothing.
    Capabilities f = Probe::parse(E + "[?1;3;256S" + E + "[?2;3;1000;800S" + E + "[6;20;10t" + E + "[?62c");
    CHECK(f.answered && !f.sixel && f.colour_registers == 0 && f.max_width == 0);
  }
  // ---- The size of a cell worked out from the window ----
  {
    Capabilities c = Probe::parse(E + "[8;30;100t" + E + "[4;600;1000t" + E + "[?62;4c");
    CHECK(c.cell_w == 10 && c.cell_h == 20 && c.usable());
    Capabilities z = Probe::parse(E + "[8;0;0t" + E + "[4;600;1000t" + E + "[?62;4c");   // zero cells: no division by zero
    CHECK(!z.usable() && z.cell_w == 0);
  }
  // ---- The multiplexer from its answer ----
  {
    CHECK(Probe::parse(E + "[>83;40003;0c" + E + "[?62;4c").multiplexer == "screen");
    CHECK(Probe::parse(E + "[>84;0;0c" + E + "[?62;4c").multiplexer == "tmux");
    CHECK(Probe::parse(E + "[>0;95;0c" + E + "[?62;4c").multiplexer.empty());
  }
  // ---- Answers in another order ----
  {
    std::vector<std::string> parts = {E + "[8;40;110t", E + "[4;800;1100t", E + "[6;20;10t", E + "[?2026;2$y", E + "[>0;10;1c",
                                      E + "[?1;0;256S", E + "[?2;0;1000;800S", E + "P>|foot 1.17" + E + "\\"};
    Capabilities want = Probe::parse(E + "[1;1R" + E + "[1;1R" + [&] { std::string s; for (auto& p : parts) s += p; return s; }() + E + "[?62;4c");
    std::mt19937 rng(1);
    for (int i = 0; i < 40; i++) {
      std::shuffle(parts.begin(), parts.end(), rng);
      std::string s = E + "[1;1R" + E + "[1;1R";
      for (auto& p : parts) s += p;
      Capabilities got = Probe::parse(s + E + "[?62;4c");
      CHECK(got.sixel == want.sixel && got.cell_w == want.cell_w && got.cols == want.cols && got.colour_registers == want.colour_registers &&
            got.max_width == want.max_width && got.name == want.name && got.sync_output_mode == want.sync_output_mode && got.da2 == want.da2);
    }
  }
  // ---- Where the cursor goes after the 60 pixel tall picture ----
  {
    auto after = [&](const std::string& cell, int row2, int col2) {
      return Probe::parse(E + "[1;1R" + E + "[" + std::to_string(row2) + ";" + std::to_string(col2) + "R" + cell + E + "[?62;4c").cursor_after;
    };
    const std::string c20 = E + "[6;20;10t";                       // a cell 20 pixels tall: the picture is 3 rows
    CHECK(after(c20, 1, 1) == CursorAfterImage::Stays);
    CHECK(after(c20, 1, 2) == CursorAfterImage::Right);
    CHECK(after(c20, 4, 1) == CursorAfterImage::Below);
    CHECK(after(c20, 4, 7) == CursorAfterImage::Below);
    CHECK(after(c20, 3, 1) == CursorAfterImage::OnLastRow);
    CHECK(after(c20, 2, 1) == CursorAfterImage::Unknown);          // half way: not a pattern we know
    CHECK(after(c20, 5, 1) == CursorAfterImage::Unknown);
    const std::string c30 = E + "[6;30;10t";                       // 30 pixels: 2 rows
    CHECK(after(c30, 3, 1) == CursorAfterImage::Below);
    CHECK(after(c30, 2, 1) == CursorAfterImage::OnLastRow);
    const std::string c25 = E + "[6;25;10t";                       // 60 / 25 is 2.4: the picture covers 3 rows
    CHECK(after(c25, 4, 1) == CursorAfterImage::Below);
    CHECK(after(c25, 3, 1) == CursorAfterImage::OnLastRow);
    const std::string c100 = E + "[6;100;10t";                     // a cell taller than the picture: 1 row
    CHECK(after(c100, 2, 1) == CursorAfterImage::Below);
    CHECK(after(c100, 1, 1) == CursorAfterImage::Stays);
    CHECK(after("", 2, 1) == CursorAfterImage::Below);             // cell size not known: any later row counts as below
    CHECK(after("", 3, 1) == CursorAfterImage::Below);
    CHECK(Probe::parse(E + "[1;1R" + E + "[3;1R" + E + "[6;20;10t" + E + "[?62;4c").summary().find("last row") != std::string::npos);
    CHECK(Probe::parse(E + "[1;1R" + E + "[4;1R" + E + "[6;20;10t" + E + "[?62;4c").summary().find("row below it") != std::string::npos);
  }
  // ---- Odd answers ----
  {
    // A name ended with BEL; control characters and a very long name are tamed.
    CHECK(Probe::parse(E + "P>|WezTerm 2024" + "\x07" + E + "[?62;4c").name == "WezTerm 2024");
    CHECK(Probe::parse(E + "P>|a\x01" "b\x7f" "c" + E + "\\" + E + "[?62;4c").name == "abc");
    CHECK(Probe::parse(E + "P>|" + std::string(1000, 'x') + E + "\\" + E + "[?62;4c").name.size() == 200);
    // Mode answers out of range are ignored; huge numbers do not overflow.
    CHECK(Probe::parse(E + "[?2026;7$y" + E + "[?62c").sync_output_mode == -1);
    Capabilities big = Probe::parse(E + "[8;99999999999999999999;99999999999999999999t" + E + "[?62;99999999999999999999c");
    CHECK(big.rows <= 1000000 && big.cols <= 1000000 && big.da1.size() == 2 && big.da1[1] <= 1000000);
    // Colon or semicolon both separate numbers; a reply with a stray control byte is skipped.
    CHECK(Probe::parse(E + "[6:20:10t" + E + "[?62;4c").cell_w == 10);
    CHECK(Probe::parse(E + "[6;20\n;10t" + E + "[?62;4c").cell_w == 0);
    // Position answers that are not a pair are ignored.
    CHECK(Probe::parse(E + "[5R" + E + "[?62;4c").cursor_after == CursorAfterImage::Unknown);
    // The cursor went backwards: not a pattern we know.
    CHECK(Probe::parse(E + "[3;5R" + E + "[1;1R" + E + "[?62;4c").cursor_after == CursorAfterImage::Unknown);
    // Only one position came back.
    CHECK(Probe::parse(E + "[1;1R" + E + "[?62;4c").cursor_after == CursorAfterImage::Unknown);
  }
  // ---- Garbage in: nothing crashes, nothing is believed too much ----
  {
    std::mt19937 rng(99);
    const std::string alphabet = "\x1b[]?>;:0123456789$RScyPt|\\\x07 abcXY-";
    for (int i = 0; i < 4000; i++) {
      std::string s;
      for (int j = 0, n = static_cast<int>(rng() % 60); j < n; j++) s += alphabet[rng() % alphabet.size()];
      Capabilities c = Probe::parse(s);
      CHECK(c.cols >= 0 && c.cols <= 1000000 && c.cell_w >= 0 && c.cell_w <= 1000000);
      (void)c.summary();
      (void)Probe::complete(s);
    }
    std::string raw;
    for (int j = 0; j < 5000; j++) raw += static_cast<char>(rng() % 256);
    (void)Probe::parse(raw);
    (void)Probe::parse(E + E + E + "[" + E + "[[[" + E + "P>|");
    (void)Probe::parse(std::string(100000, '\x1b'));
    (void)Probe::parse(E + "[" + std::string(100000, '9'));
  }

  if (failures == 0) printf("vtpen probe: all tests passed\n");
  return failures ? 1 : 0;
}

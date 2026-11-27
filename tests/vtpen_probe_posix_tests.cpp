// Tests for vtpen_probe_posix.h, against a pretend terminal that runs on a thread:
// first over pipes, then over a real pseudo-terminal. Build and run: ./m test

#include <pty.h>
#include <sys/wait.h>

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

#include "vtpen_probe_posix.h"

using namespace vtpen;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                      \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static const std::string E = "\x1b";
static const std::string WT_LIKE = E + "[1;1R" + E + "[4;1R" + E + "[>0;10;1c" + E + "[8;40;110t" + E + "[4;800;1100t" + E + "[6;20;10t" +
                                   E + "[?61;4;6c";

// Reads what the program writes until the last question (ESC [ c) has arrived.
static std::string read_questions(int fd, int timeout_ms = 3000) {
  std::string got;
  const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < end) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 50) <= 0) continue;
    char b[512];
    const ssize_t n = read(fd, b, sizeof b);
    if (n <= 0) break;
    got.append(b, static_cast<size_t>(n));
    if (got.size() >= 3 && got.compare(got.size() - 3, 3, "\x1b[c") == 0) break;
  }
  return got;
}

static void put(int fd, const std::string& s) {
  for (size_t d = 0; d < s.size();) {
    const ssize_t n = write(fd, s.data() + d, s.size() - d);
    if (n <= 0) return;
    d += static_cast<size_t>(n);
  }
}

// Runs the probe against a pretend terminal on pipes; `behave` gets the two pipe ends it plays on.
// `behave` may close the pipe it writes to by returning true.
static Capabilities probe_over_pipes(const std::function<bool(int from_prog, int to_prog, const std::string& asked)>& behave, int timeout_ms,
                                     std::string* asked_out = nullptr, bool cursor_test = false, long long* probe_ms = nullptr) {
  int to_prog[2], from_prog[2];
  if (pipe(to_prog) != 0 || pipe(from_prog) != 0) { printf("pipe failed\n"); exit(1); }
  std::string asked;
  std::atomic<bool> closed_by_fake{false};
  std::thread fake([&] {
    asked = read_questions(from_prog[0]);
    if (behave(from_prog[0], to_prog[1], asked)) { close(to_prog[1]); closed_by_fake = true; }
  });
  const auto t0 = Clock::now();
  Capabilities c = probe_terminal(to_prog[0], from_prog[1], timeout_ms, cursor_test);
  if (probe_ms) *probe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  close(to_prog[0]);        // a pretend terminal still writing to us now gets an error instead of waiting for ever
  fake.join();
  if (asked_out) *asked_out = asked;
  if (!closed_by_fake) close(to_prog[1]);
  close(from_prog[0]); close(from_prog[1]);
  return c;
}

int main() {
  signal(SIGPIPE, SIG_IGN);          // a pretend terminal writing to a closed pipe should get an error, not a signal
  // ---- Over pipes ----
  {
    std::string asked;
    Capabilities c = probe_over_pipes([](int, int to, const std::string&) { put(to, WT_LIKE); return false; }, 2000, &asked);
    CHECK(asked == Probe::questions());                         // it sent exactly the questions, without the cursor test
    CHECK(asked.find("1049") == std::string::npos);
    CHECK(c.answered && c.sixel && c.usable() && c.cell_w == 10);
    CHECK(c.cursor_after == CursorAfterImage::Below);
  }
  {
    std::string asked;
    probe_over_pipes([](int, int to, const std::string&) { put(to, WT_LIKE); return false; }, 2000, &asked, true);
    CHECK(asked == Probe::questions(true));                    // with the cursor test when asked for
    CHECK(asked.find("1049") != std::string::npos);
  }
  {  // Called with only the files and the time, it does not run the cursor test.
    int to_prog[2], from_prog[2];
    CHECK(pipe(to_prog) == 0 && pipe(from_prog) == 0);
    std::string asked;
    std::thread fake([&] { asked = read_questions(from_prog[0]); put(to_prog[1], WT_LIKE); });
    Capabilities c = probe_terminal(to_prog[0], from_prog[1], 2000);
    fake.join();
    CHECK(c.answered && asked == Probe::questions(false) && asked.find("1049") == std::string::npos);
    close(to_prog[0]); close(to_prog[1]); close(from_prog[0]); close(from_prog[1]);
  }
  {  // The answer arrives in small pieces with gaps in between.
    Capabilities c = probe_over_pipes([](int, int to, const std::string&) {
      for (size_t i = 0; i < WT_LIKE.size(); i += 7) {
        put(to, WT_LIKE.substr(i, 7));
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
      }
      return false;
    }, 3000);
    CHECK(c.answered && c.usable() && c.cell_w == 10 && c.cols == 110);
  }
  {  // Keys typed in the meantime come first and are ignored.
    Capabilities c = probe_over_pipes([](int, int to, const std::string&) { put(to, "hello\r" + E + "[A" + WT_LIKE); return false; }, 2000);
    CHECK(c.answered && c.usable());
  }
  {  // Nobody answers: it gives up after the time it was given, and says so.
    long long ms = 0;
    Capabilities c = probe_over_pipes([](int, int, const std::string&) { std::this_thread::sleep_for(std::chrono::milliseconds(600)); return false; }, 150,
                                      nullptr, false, &ms);
    CHECK(!c.answered && !c.sixel && !c.usable());
    CHECK(ms >= 140 && ms < 590);                                // waited about 150 ms, not for the thread
  }
  {  // It answers late but in time.
    Capabilities c = probe_over_pipes([](int, int to, const std::string&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      put(to, WT_LIKE);
      return false;
    }, 2000);
    CHECK(c.answered && c.usable());
  }
  {  // The input closes without an answer: it returns at once.
    long long ms = 0;
    Capabilities c = probe_over_pipes([](int, int, const std::string&) { return true; }, 5000, nullptr, false, &ms);
    CHECK(!c.answered && ms < 2000);
  }
  {  // Only part of the answers: not complete, and it does not wait forever.
    long long ms = 0;
    Capabilities c = probe_over_pipes([](int, int to, const std::string&) { put(to, E + "[6;20;10t"); return false; }, 200, nullptr, false, &ms);
    CHECK(!c.answered && !c.usable() && c.cell_w == 10 && ms < 1500);
  }
  {  // A flood of junk and no end: it stops at its limit.
    Capabilities c = probe_over_pipes([](int, int to, const std::string&) {
      std::string junk(8192, 'x');
      for (int i = 0; i < 20; i++) put(to, junk);
      return false;
    }, 3000);
    CHECK(!c.answered);
  }
  {  // A flood of junk is cut off at its limit, long before the time is up.
    long long ms = 0;
    Capabilities c = probe_over_pipes([](int, int to, const std::string&) { put(to, std::string(200000, 'x')); return false; }, 3000,
                                      nullptr, false, &ms);
    CHECK(!c.answered && ms < 1500);
  }
  {  // Junk that keeps trickling in does not keep it waiting past its time.
    long long ms = 0;
    Capabilities c = probe_over_pipes([](int, int to, const std::string&) {
      for (int i = 0; i < 75; i++) {
        put(to, "x");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      return false;
    }, 200, nullptr, false, &ms);
    CHECK(!c.answered && ms >= 150 && ms < 900);
  }
  {  // Time zero or negative: no waiting, and no crash.
    Capabilities c = probe_over_pipes([](int, int, const std::string&) { return false; }, 0);
    CHECK(!c.answered);
    Capabilities d = probe_over_pipes([](int, int, const std::string&) { return false; }, -50);
    CHECK(!d.answered);
  }
  {  // The output is closed: it cannot ask, and says so without hanging.
    int to_prog[2], from_prog[2];
    CHECK(pipe(to_prog) == 0 && pipe(from_prog) == 0);
    close(from_prog[0]);                                         // nobody reads what we write
    const auto t0 = Clock::now();
    Capabilities c = probe_terminal(to_prog[0], from_prog[1], 5000);
    CHECK(!c.answered && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 2000);
    close(to_prog[0]); close(to_prog[1]); close(from_prog[1]);
  }

  // ---- Over a real pseudo-terminal: raw mode while asking, put back afterwards ----
  {
    int master = -1, slave = -1;
    CHECK(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
    termios before{};
    tcgetattr(slave, &before);
    CHECK((before.c_lflag & ICANON) && (before.c_lflag & ECHO));   // a normal, line-by-line terminal
    std::atomic<bool> was_raw{false};
    std::string asked;
    std::thread fake([&] {
      asked = read_questions(master);
      termios now{};
      tcgetattr(slave, &now);
      was_raw = !(now.c_lflag & ICANON) && !(now.c_lflag & ECHO);
      put(master, WT_LIKE);
    });
    Capabilities c = probe_terminal(slave, slave, 3000, true);
    fake.join();
    termios after{};
    tcgetattr(slave, &after);
    CHECK(c.answered && c.usable() && c.cell_h == 20);
    CHECK(asked == Probe::questions(true));
    CHECK(was_raw);                                                // raw while it waited
    CHECK(after.c_lflag == before.c_lflag && after.c_iflag == before.c_iflag && after.c_oflag == before.c_oflag &&
          after.c_cflag == before.c_cflag && memcmp(after.c_cc, before.c_cc, sizeof before.c_cc) == 0);   // and put back
    close(master);
    close(slave);
  }

  if (failures == 0) printf("vtpen probe (posix): all tests passed\n");
  return failures ? 1 : 0;
}

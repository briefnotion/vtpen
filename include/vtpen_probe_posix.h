// vtpen probe, the easy way on Unix: asks the terminal and reads the answer.
//
//   vtpen::Capabilities c = vtpen::probe_terminal();   // uses stdin and stdout
//   if (c.usable()) { ... }
//
// It puts the terminal in raw mode while it waits (and puts it back), writes the
// questions from vtpen_probe.h and reads until the last answer comes, the time is
// up or the input closes. Keys typed during that moment are read along with the
// answers and dropped. Needs <poll.h>, <termios.h> and <unistd.h>, so Unix only.

#pragma once

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <string>

#include "vtpen_probe.h"

namespace vtpen {

inline Capabilities probe_terminal(int in_fd = 0, int out_fd = 1, int timeout_ms = 1000, bool cursor_test = false) {
  termios saved{};
  bool raw = false;
  if (isatty(in_fd) && tcgetattr(in_fd, &saved) == 0) {
    termios t = saved;
    cfmakeraw(&t);
    raw = tcsetattr(in_fd, TCSANOW, &t) == 0;
  }

  std::string reply;
  const std::string q = Probe::questions(cursor_test);
  bool sent = true;
  for (size_t done = 0; done < q.size();) {
    const ssize_t n = write(out_fd, q.data() + done, q.size() - done);
    if (n > 0) done += static_cast<size_t>(n);
    else if (n < 0 && errno == EINTR) continue;
    else { sent = false; break; }
  }

  if (sent) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
    constexpr size_t LIMIT = 64 * 1024;          // never keep more than this
    while (reply.size() < LIMIT && !Probe::complete(reply)) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) break;
      pollfd p{in_fd, POLLIN, 0};
      const int r = poll(&p, 1, static_cast<int>(left));
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      char buf[1024];
      const ssize_t n = read(in_fd, buf, sizeof buf);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;                         // closed, or an error
      reply.append(buf, static_cast<size_t>(n));
    }
  }

  if (raw) tcsetattr(in_fd, TCSANOW, &saved);
  return Probe::parse(reply);
}

}  // namespace vtpen

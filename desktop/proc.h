// Tiny portable "run a program, capture its output" helper (no console window on Windows).
#pragma once
#include <string>
#include <vector>

namespace proc {

// Runs argv[0] (found via PATH if it has no directory part) with the given
// arguments; stdout+stderr are captured in `out`. Returns the exit code, -1 if
// the program could not be started, -2 on timeout (the process is killed).
// It does NOT wait for the pipe to reach EOF: `adb devices` may leave the adb
// server daemon running with the pipe handle inherited, which would block a
// read-until-EOF forever.
int run(const std::vector<std::string> &argv, int timeout_ms, std::string &out);

} // namespace proc

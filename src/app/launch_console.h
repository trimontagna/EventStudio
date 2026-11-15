#pragma once

// The console window of a double-clicked EventStudio.bat: hidden while EventStudio runs, closed when it ends.
//
// EventStudio is a windowed program, but EventStudio.bat runs in a console window (it sets up the OpenEB
// environment, then waits for EventStudio). EventStudio attaches to that console only when no one can type in it:
// the processes on it must be the batch file's cmd (cmd /c) and helpers it started (powershell -Command, e.g.
// vcvars64's telemetry). A terminal EventStudio.bat was run from (cmd, PowerShell, bash...) is left as it is.
// With Windows Terminal hosting the console, the window hidden and closed is the Terminal window owning it
// (otherwise "closeOnExit": "never" would keep it open). `--console` keeps the console as it is.

namespace evs::app::launch_console {

void hide();  // at start (does nothing outside Windows, or when the console is not EventStudio's to hide)
void close(); // at the end: leave the console, close a Windows Terminal window (conhost closes with the batch)

} // namespace evs::app::launch_console

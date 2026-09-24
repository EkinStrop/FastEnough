# Native regression tests

Run from PowerShell on Windows with Visual Studio's C++ desktop workload and CMake tools installed:

```powershell
.\tests\run-tests.ps1
```

Use `-Configuration Debug` for a Debug run. `-VisualStudioPath` and `-CMakePath` override automatic tool discovery. Build output stays under the ignored `x64/tests` directory.

The tests compile the production discovery parser, snapshot filter, process runner, transfer coverage tracker, activity logger, local copy helper, and pane drop target. A controllable child process exercises simultaneous stdout/stderr, file streaming, nonzero exit status, silent hangs, cancellation, descendant cleanup, and shared daemon survival. No phone or installed ADB is needed.

Activity tests cover log rotation, concurrent writes, Unicode paths, disk errors, copy cancellation, and source preservation. The pane tests simulate ImGui mouse input to check drops in both directions across headers, rows, empty space, and footers, including rejected targets and single delivery. Fetch the ImGui dependency with `setup.ps1` before building the tests.

These tests do not validate Android helper behavior, physical transport failover, Dokan callbacks, or the rendered Windows interface. Those checks are tracked in `docs/implementation-plan.md`.

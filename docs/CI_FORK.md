# Fork CI gates

Pull requests into `codex/cmos-local-integration` automatically run two jobs:
the Windows `Agent Debug SDL2` build with native agent tests, and a Linux SDL2
build with DOSBox-X's built-in tests. Feature-branch pushes do not start another
copy of the same checks.

The upstream platform, installer, and older toolchain workflows remain available
through `workflow_dispatch` for targeted compatibility checks. They are not the
routine merge gate for this Windows/Linux research fork. Run one explicitly when
a change affects that platform or its packaging.

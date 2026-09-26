# Fork CI gates

Pull requests into `ai-re-agent` automatically run two jobs:
the Windows `Agent Debug SDL2` build with native agent tests, and a Linux SDL2
build with DOSBox-X's built-in tests. Feature-branch pushes do not start another
copy of the same checks.

The upstream platform, installer, and older toolchain workflows remain available
through `workflow_dispatch` for targeted compatibility checks. SDL1, AppImage,
distribution packaging, macOS, legacy Windows, and alternate compiler coverage
are manual. They are not the routine merge gate for this Windows/Linux research
fork. Run one explicitly when a change affects that platform or its packaging.

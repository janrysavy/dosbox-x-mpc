# Keep the MINGW64 libslirp build dependency

MSYS2 commit [5f571aceb58e7cbaf503f1a96718810fe29b10f7](https://github.com/msys2/MINGW-packages/commit/5f571aceb58e7cbaf503f1a96718810fe29b10f7)
removed MINGW64 from the libslirp recipe on 2026-09-17. The retained MinGW64 CI
jobs fail during setup with `target not found: mingw-w64-x86_64-libslirp`.
Observed in CMOS PR #1 run35727488864/job106744601267; this is a dependency
failure before compilation, not a CMOS regression or transient download error.

The fork already builds libslirp from source for MINGW32. The new shared script
uses the same makepkg-mingw approach for MINGW64, pins the recipe commit above,
verifies its checkout identity, changes only the selected architecture, and
lets makepkg verify the recipe's source archive SHA-256. Dependencies are
resolved normally by makepkg. It refuses a wrong MSYSTEM and stale build directory.
Three MINGW64 jobs use the script; their builds/tests and the existing UCRT64
job remain enabled. A focused workflow also compiles and runs a program against
the installed libslirp using pkg-config.

Local Windows MSYS2 bash syntax validation passes. The local MSYS environment
refuses the script before mutation as expected (exit 1). A Windows-format
GITHUB_WORKSPACE with an existing build directory also refuses with exit 1.
Full package build and linking
require the fresh GitHub Actions environment and are not yet claimed successful.

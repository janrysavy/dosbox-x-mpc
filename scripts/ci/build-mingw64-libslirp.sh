#!/usr/bin/env bash
# MSYS2 removed the MINGW64 binary on 2026-09-17. Keep this target via source.
set -euo pipefail
[[ "${MSYSTEM:-}" == MINGW64 ]] || { echo 'Expected MSYSTEM=MINGW64' >&2; exit 1; }
recipe_commit=5f571aceb58e7cbaf503f1a96718810fe29b10f7
work_dir="${GITHUB_WORKSPACE:-$PWD}/_ci-libslirp"
# Refuse stale output; every CI checkout gets a fresh directory.
mkdir "$work_dir"
git -C "$work_dir" init
git -C "$work_dir" remote add origin https://github.com/msys2/MINGW-packages.git
git -C "$work_dir" fetch --depth=1 origin "$recipe_commit"
git -C "$work_dir" checkout --detach FETCH_HEAD
[[ "$(git -C "$work_dir" rev-parse HEAD)" == "$recipe_commit" ]]
cd "$work_dir/mingw-w64-libslirp"
sed -i "s/^mingw_arch=.*/mingw_arch=('mingw64')/" PKGBUILD
MINGW_ARCH=mingw64 makepkg-mingw -sCLf --noconfirm
pacman --noconfirm -U mingw-w64-x86_64-libslirp-*-any.pkg.tar.zst
pkg-config --modversion slirp

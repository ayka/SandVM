#!/usr/bin/env bash
# Builds build/sandvm.exe with the portable llvm-mingw toolchain in .tools/llvm-mingw.
# (CMakeLists.txt is the canonical build for MSVC; this script needs no CMake or admin rights.)
# OUT=<path> writes the executable elsewhere, e.g. while build/sandvm.exe is running.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-$ROOT/.tools/llvm-mingw/bin/clang++}"
OUT="${OUT:-build/sandvm.exe}"
[[ -x "$CXX" || -x "$CXX.exe" ]] || { echo "compiler not found: $CXX" >&2; exit 2; }

mkdir -p "$ROOT/$(dirname "$OUT")"
SOURCES=$(cd "$ROOT" && find src -name '*.cpp' | sort)
(cd "$ROOT" && "$CXX" -std=c++20 -O2 -g -Wall -Wextra -Werror=return-type \
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
  -Isrc $SOURCES -o "$OUT" -static -lshell32 -luser32 -lgdi32 -lws2_32 -liphlpapi -lcomdlg32)
echo "built $OUT"

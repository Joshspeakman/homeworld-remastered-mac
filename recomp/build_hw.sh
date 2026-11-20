#!/bin/bash
# build_hw.sh <HomeworldRemastered.app> [outdir] : recompile the Steam Mac edition's HW2HD_Exe for the host CPU.
# Everything generated is derived from the user's executable and stays local (work/ is git-ignored).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="${1:?usage: build_hw.sh <HomeworldRemastered.app> [outdir]}"; OUT="${2:-$HERE/work/hw2hd}"
EXE="$APP/Contents/MacOS/HW2HD_Exe"
[ -f "$EXE" ] || { echo "no $EXE"; exit 1; }
PY="$HERE/.venv/bin/python"
[ -x "$PY" ] || { python3 -m venv "$HERE/.venv" && "$HERE/.venv/bin/pip" install -q capstone==5.0.7 unicorn==2.1.4; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
python3 "$HERE/tools/compiler_compat.py" "${CC:-clang}" "$OUT/compiler-flags.txt"
if [[ "${HW_SKIP_LIFT:-}" != 1 ]]; then
  echo "== analysis"; "$PY" "$HERE/tools/analyze.py" "$EXE" "$OUT/analysis.json" > "$OUT/analysis.log"
  sed -n '/^functions /p; /^jump tables /p' "$OUT/analysis.log"
  echo "== lifting"; "$PY" "$HERE/tools/lift.py" "$EXE" "$OUT/analysis.json" "$OUT" --game hw > "$OUT/lift.log"
  grep -E '^lifted|^internal' "$OUT/lift.log"
  echo "== compiling generated code"
  rm -rf "$OUT/obj"; mkdir -p "$OUT/obj"
  printf '%s\n' "$OUT"/lift_*.c "$OUT/fntab.c" | xargs -n 1 -P "${HW_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}" "$HERE/tools/cc1.sh" "$OUT"
fi
echo "== runtime"
"$HERE/tools/build_hw_runtime.sh" "$OUT"

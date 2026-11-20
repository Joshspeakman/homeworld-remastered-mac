#!/bin/bash
# build_hw_runtime.sh <outdir> : compile the Mac runtime (runtime/rt.c + runtime/mac) and link the hw2hd program
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; OUT="$1"; mkdir -p "$OUT/obj/rt"
python3 "$HERE/tools/gen_gl.py" "$OUT/gl_gen.c"
SRC=("$HERE"/runtime/mac/*.c); shopt -s nullglob; OBJC=("$HERE"/runtime/mac/*.m); shopt -u nullglob
python3 "$HERE/tools/gen_mac_imptab.py" "$OUT/mac_imptab.c" "${SRC[@]}" ${OBJC[@]+"${OBJC[@]}"} "$OUT/gl_gen.c"
DEPS="${DEPS:-/opt/homebrew}"   # the libraries: Homebrew's, or built for a release's macOS (tools/build_deps.sh, setup/make_release.sh)
INC=(-I "$HERE/runtime" -I "$HERE/runtime/mac" -I "$OUT" -I "$DEPS/include")
pids=()
for f in "$HERE"/third_party/core-math/*.c; do
  clang -c -O2 -ffp-contract=off -w -o "$OUT/obj/rt/cm_$(basename "$f" .c).o" "$f" & pids+=($!)
done
for f in "$HERE"/runtime/rt.c "$HERE"/runtime/mac/*.c "$OUT/mac_imptab.c" "$OUT/macho_tab.c" "$OUT/override_tab.c" "$OUT/gl_gen.c"; do
  clang -c -O1 -ffp-contract=off -g -Wall -Wno-unused-function -Wno-unused-variable -fno-strict-aliasing "${INC[@]}" -o "$OUT/obj/rt/$(basename "$f" .c).o" "$f" & pids+=($!)
done
for f in ${OBJC[@]+"${OBJC[@]}"}; do
  clang -c -O1 -fobjc-arc -g -Wall -Wno-unused-function "${INC[@]}" -o "$OUT/obj/rt/$(basename "$f" .m).om.o" "$f" & pids+=($!)
done
for f in "$HERE"/runtime/mac/mgl/*.m; do       # the Metal GL backend
  clang -c -O2 -fobjc-arc -g -Wall -Wno-unused-function "${INC[@]}" -o "$OUT/obj/rt/mgl_$(basename "$f" .m).o" "$f" & pids+=($!)
done
for f in "$HERE"/runtime/mac/mgl/*.cpp; do
  clang++ -std=c++17 -c -O2 -g "${INC[@]}" -o "$OUT/obj/rt/mgl_$(basename "$f" .cpp).o" "$f" & pids+=($!)
done
fail=0; for p in "${pids[@]}"; do wait "$p" || fail=1; done; [ $fail = 0 ] || exit 1
clang++ -o "$OUT/hw2hd" "$OUT"/obj/*.o "$OUT"/obj/rt/*.o -framework CoreFoundation -framework Foundation -framework AppKit \
  -framework Metal -framework QuartzCore -framework OpenGL -framework ImageIO -framework CoreServices -framework AudioToolbox -framework CoreAudio -framework IOKit -framework GameController -liconv -L"$DEPS/lib" -lSDL2 -lglslang -lglslang-default-resource-limits -lSPIRV-Tools-opt -lSPIRV-Tools -lspirv-cross-msl -lspirv-cross-glsl -lspirv-cross-core
echo "== $OUT/hw2hd"

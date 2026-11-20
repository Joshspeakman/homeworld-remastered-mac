#!/bin/bash
# build_deps.sh [prefix]: the libraries the Mac runtime links (tools/build_hw_runtime.sh), built from their release
# sources for the macOS the downloaded app supports (MACOSX_DEPLOYMENT_TARGET, default 14.0; Homebrew's bottles target
# the macOS of the Mac that installed them): SDL3 and sdl2-compat (zlib licence), SPIRV-Headers, SPIRV-Tools and
# SPIRV-Cross (Apache 2.0), glslang (BSD/Apache). The same versions as Homebrew's at the time of writing. Each tarball
# is kept in ~/ProjectBuilds/_toolkit/src and checked against its pinned SHA-256;
# the result goes to the prefix (default ~/ProjectBuilds/_toolkit/hw-deps-<target>): lib/ (SDL2/SDL3, glslang and
# SPIRV-Tools as dylibs, SPIRV-Cross static) and include/. Needs cmake and ninja (development only).
set -euo pipefail
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
P="${1:-$HOME/ProjectBuilds/_toolkit/hw-deps-$MACOSX_DEPLOYMENT_TARGET}"; SRC="$HOME/ProjectBuilds/_toolkit/src"; mkdir -p "$SRC"
[ -f "$P/.done" ] && { echo "$P"; exit 0; }
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fetch() {   # fetch <name> <url> <sha256 or ->: the tarball, checked; unpacked into $W/<name>
  local f="$SRC/$1.tar.gz"
  [ -f "$f" ] || { curl -fsSL -o "$f.part" "$2" && mv "$f.part" "$f"; }
  local got; got="$(shasum -a 256 "$f" | cut -d' ' -f1)"
  if [ "$3" != - ] && [ "$got" != "$3" ]; then echo "build_deps: $1's tarball isn't the pinned one ($got)"; rm -f "$f"; exit 1; fi
  [ "$3" != - ] || echo "build_deps: $1 sha256 $got (pin it)"
  mkdir -p "$W/$1"; tar -xzf "$f" -C "$W/$1" --strip-components 1
}
CM=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_PREFIX_PATH="$P" -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" -DCMAKE_INSTALL_NAME_DIR="@rpath" -DCMAKE_INSTALL_RPATH="@loader_path")
build() { cmake -S "$W/$1" -B "$W/$1/build" "${CM[@]}" "${@:2}" > "$W/$1.log" 2>&1 && cmake --build "$W/$1/build" >> "$W/$1.log" 2>&1 &&
          cmake --install "$W/$1/build" >> "$W/$1.log" 2>&1 || { tail -30 "$W/$1.log"; exit 1; }; echo "built $1"; }
SDK=vulkan-sdk-1.4.363.0
fetch SDL3 https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
build SDL3 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
fetch sdl2-compat https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz ec68abde77e2e459c8abc4f9587b8976b5d2a657b2bfda2fb3a079b0f8924588
build sdl2-compat -DSDL2COMPAT_TESTS=OFF
fetch SPIRV-Headers https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/$SDK.tar.gz 5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9
build SPIRV-Headers
fetch SPIRV-Tools https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/$SDK.tar.gz e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688
build SPIRV-Tools -DSPIRV-Headers_SOURCE_DIR="$W/SPIRV-Headers" -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DBUILD_SHARED_LIBS=ON -DSPIRV_TOOLS_BUILD_STATIC=OFF
fetch glslang https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz 9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
build glslang -DBUILD_SHARED_LIBS=ON -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_OPT=ON
fetch SPIRV-Cross https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/$SDK.tar.gz f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721
build SPIRV-Cross -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF
touch "$P/.done"; echo "$P"

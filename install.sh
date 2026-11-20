#!/bin/bash
# install.sh [--steam-app <HomeworldRemastered.app>] [--nightly] [--app-dir DIR] [--data-dir DIR] [--in-place]
#
# Builds Homeworld Remastered natively for Apple Silicon from your own Steam copy (the Mac edition: Steam app
# 244160, HomeworldRemastered.app) and installs "Homeworld Remastered.app" (with --nightly: "Homeworld Remastered
# Nightly.app") in ~/Applications.
#
# The game's files are imported into the data folder (default ~/Games/HomeworldNative/game-files: an APFS clone, so
# it takes no extra space) and the app reads them from there; with --in-place it reads the Steam folder instead.
# Profiles, saves, settings and the game's log go to ~/Games/HomeworldNative/userdata. Needs the Xcode command line
# tools, python3 and Homebrew's sdl2-compat, glslang and spirv-cross (brew install sdl2-compat glslang spirv-cross).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
STEAM_APP="$HOME/Library/Application Support/Steam/steamapps/common/Homeworld/HomeworldRemastered.app"
APPS="$HOME/Applications"; DATA="$HOME/Games/HomeworldNative"; NIGHTLY=0; INPLACE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --steam-app) STEAM_APP="$2"; shift 2;; --app-dir) APPS="$2"; shift 2;; --data-dir) DATA="$2"; shift 2;;
    --nightly) NIGHTLY=1; shift;; --in-place) INPLACE=1; shift;;
    *) echo "unknown option $1"; exit 1;;
  esac
done
if [ ! -f "$STEAM_APP/Contents/MacOS/HW2HD_Exe" ] && [ -f "$DATA/game-files/HomeworldRemastered.app/Contents/MacOS/HW2HD_Exe" ]; then
  STEAM_APP="$DATA/game-files/HomeworldRemastered.app"            # already imported: Steam's copy is not needed any more
fi
[ -f "$STEAM_APP/Contents/MacOS/HW2HD_Exe" ] || { echo "no Homeworld Remastered at $STEAM_APP (use --steam-app)"; exit 1; }
for lib in /opt/homebrew/lib/libSDL2-2.0.0.dylib /opt/homebrew/lib/libSDL3.dylib /opt/homebrew/lib/libglslang.dylib /opt/homebrew/lib/libspirv-cross-msl.a; do
  [ -f "$lib" ] || { echo "missing $lib: brew install sdl2-compat glslang spirv-cross"; exit 1; }
done
mkdir -p "$DATA" "$APPS"

GAME="$STEAM_APP"
if [ $INPLACE = 0 ]; then
  GAME="$DATA/game-files/HomeworldRemastered.app"
  if [ ! -f "$GAME/Contents/MacOS/HW2HD_Exe" ]; then
    echo "== importing the game into $DATA/game-files"
    mkdir -p "$DATA/game-files"
    cp -cR "$STEAM_APP" "$DATA/game-files/" 2>/dev/null || cp -R "$STEAM_APP" "$DATA/game-files/"
  fi
fi

NAME="Homeworld Remastered"; ID=com.joshspeakman.homeworld-remastered; BRANCH=stable
if [ $NIGHTLY = 1 ]; then NAME="Homeworld Remastered Nightly"; ID=$ID.nightly; BRANCH=nightly; fi
OUT="$HERE/build/$BRANCH"
echo "== recompiling (a few minutes)"
"$HERE/recomp/build_hw.sh" "$GAME" "$OUT"

echo "== building $NAME.app"
STAGE="$OUT/$NAME.app"; rm -rf "$STAGE"
mkdir -p "$STAGE/Contents/MacOS" "$STAGE/Contents/Frameworks" "$STAGE/Contents/Resources"
clang -O2 -fobjc-arc -framework Cocoa -o "$STAGE/Contents/MacOS/Homeworld" "$HERE/recomp/host/hw_launcher.m"
cp "$OUT/hw2hd" "$STAGE/Contents/MacOS/hw2hd"
# the Homebrew libraries the game links (SDL2 via sdl2-compat + SDL3, glslang and SPIRV-Tools for the Metal
# renderer's shader translation) go into the app, with every reference rewritten, so the app stands alone
FW="$STAGE/Contents/Frameworks"
bundle() {                                            # bundle <binary>: copy its /opt/homebrew deps, rewrite references
  local bin="$1" dep base
  for dep in $(otool -L "$bin" | awk 'NR>1 && $1 ~ /^\/opt\/homebrew\// {print $1}'); do
    base=$(basename "$dep")
    if [ ! -f "$FW/$base" ]; then cp "$dep" "$FW/$base"; chmod u+w "$FW/$base"; install_name_tool -id "@rpath/$base" "$FW/$base" 2>/dev/null; bundle "$FW/$base"; fi
    install_name_tool -change "$dep" "@executable_path/../Frameworks/$base" "$bin" 2>/dev/null
  done
}
bundle "$STAGE/Contents/MacOS/hw2hd"
cp /opt/homebrew/lib/libSDL3.dylib "$FW/libSDL3.dylib"; chmod u+w "$FW/libSDL3.dylib"   # sdl2-compat loads it by name next to itself
[ -f "$GAME/Contents/Resources/App.icns" ] && cp "$GAME/Contents/Resources/App.icns" "$STAGE/Contents/Resources/Homeworld.icns"
# the launch window's game cards (tools/hw_art.py: a screenshot of each game from the Steam store page) in the data
# folder's art/, and a copy in the app; logos and background come from the game's own Resources at run time
if python3 -I "$HERE/recomp/tools/hw_art.py" "$DATA/art" >/dev/null 2>&1; then cp -R "$DATA/art" "$STAGE/Contents/Resources/art"
else echo "   (no screenshots for the launch window: the cards use the game's own backgrounds)"; fi
# updates from the launch window: the commit this app was built from and the checkout it was installed from; the
# updater rebuilds the app from the newest commit of its branch (game files, profiles and saves kept)
U_BRANCH=main; [ $NIGHTLY = 1 ] && U_BRANCH=nightly
U_REPO="$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"; U_REPO="${U_REPO%/.git}"
U_BUILT="$(git -C "$HERE" rev-parse HEAD 2>/dev/null || true)"
if [ -n "$U_REPO" ] && [ -n "$U_BUILT" ] && git -C "$U_REPO" remote get-url origin >/dev/null 2>&1; then
  NF=; [ $NIGHTLY = 1 ] && NF=--nightly
  cat > "$STAGE/Contents/MacOS/update" <<UPDATE
#!/bin/bash
# $NAME updater (generated by install.sh): "update --check" prints the newest commit of $U_BRANCH on GitHub;
# "update" fetches it into a worktree of the checkout this app was installed from and rebuilds the app from it
set -euo pipefail
REPO=$(printf '%q' "$U_REPO"); BRANCH=$U_BRANCH; W="\$REPO/build/update-$U_BRANCH"
export GIT_TERMINAL_PROMPT=0 PATH="/opt/homebrew/bin:/usr/local/bin:\$PATH"
# a private repository: git signs in through gh with the repository owner's account (gh's active one may differ)
OWNER="\$(git -C "\$REPO" remote get-url origin | sed -E 's#.*github.com[:/]([^/]+)/.*#\1#')"
[ -n "\${GH_TOKEN:-}" ] || GH_TOKEN="\$(gh auth token --user "\$OWNER" 2>/dev/null || true)"
[ -z "\$GH_TOKEN" ] || export GH_TOKEN
if [ "\${1:-}" = --check ]; then                          # GitHub's newest, unless this app already has it or newer
  R="\$(git -C "\$REPO" ls-remote origin "refs/heads/\$BRANCH" | cut -f1)"
  if [ -n "\$R" ] && git -C "\$REPO" merge-base --is-ancestor "\$R" $U_BUILT 2>/dev/null; then echo $U_BUILT; else echo "\$R"; fi
  exit 0
fi
echo "== fetching \$BRANCH from GitHub"
git -C "\$REPO" fetch -q origin "\$BRANCH"
git -C "\$REPO" worktree prune
if [ -e "\$W/.git" ]; then git -C "\$W" checkout -q -f --detach "origin/\$BRANCH"
else mkdir -p "\$REPO/build"; git -C "\$REPO" worktree add -q -f --detach "\$W" "origin/\$BRANCH"; fi
echo "== \$BRANCH: \$(git -C "\$W" log -1 --format='%h %s')"
exec "\$W/install.sh" $NF --steam-app $(printf '%q' "$GAME") --data-dir $(printf '%q' "$DATA") --app-dir $(printf '%q' "$APPS")
UPDATE
  chmod +x "$STAGE/Contents/MacOS/update"
fi
cat > "$STAGE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>$ID</string>
  <key>CFBundleExecutable</key><string>Homeworld</string>
  <key>CFBundleIconFile</key><string>Homeworld</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo dev)</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
  <key>GCSupportsGameMode</key><true/>
  <key>NSHighResolutionCapable</key><true/>
  <key>HWGamePath</key><string>$GAME</string>
  <key>HWDataPath</key><string>$DATA</string>
  <key>HWBranch</key><string>$U_BRANCH</string>
  <key>HWBuilt</key><string>$U_BUILT</string>
</dict></plist>
PLIST
codesign --force --deep -s - "$STAGE" >/dev/null 2>&1 || true
rm -rf "$APPS/$NAME.app"; mv "$STAGE" "$APPS/"
echo "== installed $APPS/$NAME.app (game files: $GAME; profiles and saves: $DATA/userdata)"

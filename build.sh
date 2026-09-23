#!/bin/sh
# Build the native PoP2 executable from recompiled sources, plus the launcher.
set -e
cd "$(dirname "$0")"
LLVM="/c/Program Files/LLVM/bin"
CC="$LLVM/clang.exe"
mkdir -p obj
OPT=${OPT:--O1}
if [ ! -f src/gen/dispatch.c ]; then
  echo "src/gen is empty - run recomp/recomp.py first (see README)" >&2; exit 1
fi
for f in src/gen/*.c src/runtime.c src/sound.c src/opl.c src/video.c; do
  o=obj/$(basename "$f" .c).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ src/cpu.h -nt "$o" ]; then
    "$CC" $OPT -w -c "$f" -o "$o" &
  fi
done
wait
# resources: version info, manifest and - when res/make_art.py has been run - icons and banner
ART=""
[ -f res/pop2.ico ] && [ -f res/launcher.ico ] && [ -f res/banner.bmp ] && ART="/dHAVE_ART"
for n in pop2 launcher; do
  if [ ! -f res/$n.res ] || [ -n "$(find res -newer res/$n.res -type f ! -name '*.res' ! -name '*.png')" ]; then
    (cd res && MSYS_NO_PATHCONV=1 "$LLVM/llvm-rc.exe" $ART /fo $n.res $n.rc)
  fi
done
MSYS_NO_PATHCONV=1 "$CC" $OPT obj/*.o res/pop2.res -o pop2.exe -luser32 -lgdi32 -lwinmm -lopengl32 -Wl,/SUBSYSTEM:WINDOWS -Wl,/RELEASE
if [ ! -f "PoP2 Launcher.exe" ] || [ src/launcher.c -nt "PoP2 Launcher.exe" ] || [ res/launcher.res -nt "PoP2 Launcher.exe" ]; then
  MSYS_NO_PATHCONV=1 "$CC" -O2 -w src/launcher.c res/launcher.res -o "PoP2 Launcher.exe" -luser32 -lgdi32 -lshell32 -lole32 -luuid -lcomctl32 -lwinmm -Wl,/SUBSYSTEM:WINDOWS -Wl,/RELEASE
fi
echo built pop2.exe

#!/bin/sh
# Debug build: same sources with SPCHECK (stack/frame checks, memory watchpoints) -> pop2_dbg.exe
set -e
cd "$(dirname "$0")"
LLVM="/c/Program Files/LLVM/bin"
CC="$LLVM/clang.exe"
mkdir -p obj_dbg
for f in src/gen/*.c src/runtime.c src/sound.c src/opl.c src/video.c; do
  o=obj_dbg/$(basename "$f" .c).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ src/cpu.h -nt "$o" ]; then
    "$CC" -O1 -DSPCHECK -w -c "$f" -o "$o" &
  fi
done
wait
"$CC" -O1 obj_dbg/*.o res/pop2.res -o pop2_dbg.exe -luser32 -lgdi32 -lwinmm -lopengl32 -Wl,/SUBSYSTEM:WINDOWS
echo built pop2_dbg.exe

#!/bin/sh
# Run the game; if it hits code that wasn't recompiled, add the entry point and rebuild.
cd "$(dirname "$0")"
touch recomp/extra_entries.txt
for i in $(seq 1 ${1:-10}); do
  rm -f missing_entries.txt
  timeout ${RUNTIME:-12} ./pop2.exe $ARGS
  if [ -s missing_entries.txt ]; then
    cat missing_entries.txt | tee -a recomp/extra_entries.txt
    sort -u recomp/extra_entries.txt -o recomp/extra_entries.txt
    (cd recomp && python recomp.py extra_entries.txt | tail -1)
    ./build.sh | tail -1
  else
    echo "no missing entries"; break
  fi
done
tail -15 pop2native.log

#!/bin/sh
# Coverage: for each level, jump to its end (Alt+N, cheat mode), let the cutscene run into the
# next level, and add any code entry points that were not recompiled yet.
cd "$(dirname "$0")"
for L in ${LEVELS:-1 2 3 4 5 6 7 8 9 10 11 12 13 14}; do
  for try in 1 2 3 4 5 6 7 8; do
    K="2000:39,3500:39,5000:39,6000:38:1500,6300:31:200"
    for t in $(seq 20000 6000 ${RUNTIME:-90}000); do K="$K,$t:39,$((t+1500)):1c"; done   # space + enter (symbol screen)
    rm -f missing_entries.txt
    POP2_KEYS="$K" timeout ${RUNTIME:-90} ./pop2.exe MAKINIT LEVEL$L
    if [ -s missing_entries.txt ]; then
      echo "level $L: $(cat missing_entries.txt | tr '\n' ' ')"
      cat missing_entries.txt >> recomp/extra_entries.txt
      sort -u recomp/extra_entries.txt -o recomp/extra_entries.txt
      (cd recomp && python recomp.py extra_entries.txt | tail -1)
      ./build.sh | tail -1
    else
      echo "level $L try $try: ok ($(grep -o '[A-Z]*\.DAT' pop2native.log | uniq | tr '\n' ' ' | tail -c 200))"
      break
    fi
  done
done

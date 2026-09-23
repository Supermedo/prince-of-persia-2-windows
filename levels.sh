#!/bin/sh
# Coverage pass: start each level (MAKINIT LEVELn), mash movement keys, collect missing entry points.
cd "$(dirname "$0")"
for L in ${LEVELS:-1 2 3 4 5 6 7 8 9 10 11 12 13 14}; do
  for try in 1 2 3 4 5 6; do
    K=$(python -c "
import random
random.seed($L*100+$try)
k=[]
t=2000
while t<8000: k.append('%d:39'%t); t+=1500
keys=['14d','14b','148','150','2a','39','14d','14b']
while t<${RUNTIME:-40}000-2000:
    s=random.choice(keys); h=random.randint(100,1500); k.append('%d:%s:%d'%(t,s,h)); t+=random.randint(150,900)
print(','.join(k))")
    rm -f missing_entries.txt
    POP2_KEYS="$K" timeout ${RUNTIME:-40} ./pop2.exe MAKINIT LEVEL$L
    if [ -s missing_entries.txt ]; then
      echo "level $L: $(cat missing_entries.txt)"
      cat missing_entries.txt >> recomp/extra_entries.txt
      sort -u recomp/extra_entries.txt -o recomp/extra_entries.txt
      (cd recomp && python recomp.py extra_entries.txt | tail -1)
      ./build.sh | tail -1
    else
      echo "level $L try $try: ok ($(grep -io '[A-Z0-9_]*\.DAT' pop2native.log | sort -u | tr '\n' ' '))"
      break
    fi
  done
done

#!/bin/bash
OUT=downloads_compare.csv
echo "file;size_kb;old_ms;new_ms;speedup;verify" > $OUT
awk 'BEGIN{printf "files=0 sum_old=0 sum_new=0\n"}' > /dev/null
SUMOLD=0; SUMNEW=0; N=0
for f in C:/Users/My_Home/Downloads/*.png; do
  w=$(cygpath -w "$f")
  sz=$(( $(stat -c%s "$f") / 1024 ))
  old=$(./pngbench_old.exe "$w" 15 2 2>/dev/null | grep "Decode  median" | grep -oE "[0-9]+\.[0-9]+")
  ver=$(./pngbench.exe "$w" 15 2 --verify 2>/dev/null | grep -E "\[verify\] (PASS|FAIL)" | head -1 | grep -oE "PASS|FAIL")
  new=$(./pngbench.exe "$w" 15 2 2>/dev/null | grep "Decode  median" | grep -oE "[0-9]+\.[0-9]+")
  if [ -n "$old" ] && [ -n "$new" ]; then
    sp=$(awk -v o="$old" -v n="$new" 'BEGIN{printf "%.2f", (n>0)?o/n:0}')
    echo "$(basename "$f");$sz;$old;$new;$sp;$ver" >> $OUT
    SUMOLD=$(awk -v a=$SUMOLD -v b=$old 'BEGIN{printf "%.3f", a+b}')
    SUMNEW=$(awk -v a=$SUMNEW -v b=$new 'BEGIN{printf "%.3f", a+b}')
    N=$((N+1))
  else
    echo "$(basename "$f");$sz;ERR;ERR;;" >> $OUT
  fi
done
awk -v o=$SUMOLD -v n=$SUMNEW -v c=$N 'BEGIN{printf "files=%d sum_old=%.1fms sum_new=%.1fms overall_speedup=%.2fx\n", c, o, n, (n>0)?o/n:0}'

#!/bin/bash
printf "%-42s %9s %9s %9s %7s %s\n" "FILE" "SIZE" "OLD(ms)" "NEW(ms)" "SPEEDUP" "VERIFY"
for f in \
 "C:/Users/My_Home/Downloads/erasebg-transformed (18).png" \
 "C:/Users/My_Home/Downloads/erasebg-transformed (7).png" \
 "C:/Users/My_Home/Downloads/AI_Generated_Image_2025-11-13.png" \
 "C:/Users/My_Home/Downloads/download.png" \
 "C:/Users/My_Home/Downloads/ChatGPT Image Jul 23, 2026, 10_15_44 PM.png" \
 "C:/Users/My_Home/Downloads/ChatGPT Image May 3, 2026, 11_32_49 AM.png" \
 "C:/Users/My_Home/Downloads/erasebg-transformed (11).png" \
 "C:/Users/My_Home/Downloads/16bcaf07-0caf-413f-a060-5839b6ea1901.png" \
 "C:/Users/My_Home/Desktop/projects/jpegview/benchmarks/large_png/BlackMarble_2016_1200m_africa_s_labeled.png"; do
  [ -f "$f" ] || continue
  w=$(cygpath -w "$f")
  sz_kb=$(( $(stat -c%s "$f") / 1024 ))
  old=$(./pngbench_old.exe "$w" 30 3 2>/dev/null | grep "Decode  median" | grep -oE "[0-9]+\.[0-9]+")
  out=$(./pngbench.exe "$w" 30 3 --verify 2>/dev/null)
  new=$(echo "$out" | grep "Decode  median" | grep -oE "[0-9]+\.[0-9]+")
  ver=$(echo "$out" | grep -oE "\[verify\] (PASS|FAIL)" | head -1 | cut -d' ' -f2)
  if [ -n "$old" ] && [ -n "$new" ]; then
    sp=$(awk -v o=$old -v n=$new 'BEGIN{printf "%.2fx", o/n}')
    printf "%-42s %7dKB %9s %9s %7s %s\n" "$(basename "$f")" "$sz_kb" "$old" "$new" "$sp" "${ver:-n/a}"
  else
    printf "%-42s %7dKB %9s %9s %7s %s\n" "$(basename "$f")" "$sz_kb" "-" "-" "-" "decode-failed-both"
  fi
done

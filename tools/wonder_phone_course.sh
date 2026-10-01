#!/bin/bash
# Scripted course 1-1 run on a phone (the Mac's private/wonder/course.sh, on Android): the same timed inputs through
# NX_INPUT_SCRIPT, frames every 600, memory every 20 s from `dumpsys meminfo` (the app's total, its Graphics part
# - GPU memory, which the runtime's own Pss guard does not see - and the phone's available RAM), and a 10 s CPU
# profile of the game threads in the course (profile.txt). Results in private/wonder/phone_<name>/.
# usage: tools/wonder_phone_course.sh <serial> <name> [NX_X=Y ...]   (extra config.txt lines)
S=$1; N=$2; shift 2
cd "$(dirname "$0")/.."
PKG=io.github.belmantegu.wonderrecomp
D="/storage/emulated/0/Super Mario Bros. Wonder"
OUT=private/wonder/phone_$N
rm -rf "$OUT"; mkdir -p "$OUT/frames"
SC="25:A:0.5;40:A:1.0;60:A:1.0;95:RIGHT:0.3;105:A:1.0;118:A:1.0;150:A:3.0;160:A:0.1;160.5:A:0.1;161:A:0.1;161.5:A:0.1;162:A:0.1;170:A:0.05;175:A:0.3;176:A:0.3;177:A:0.3;"
for t in $(seq 185 3 380); do SC="$SC$t:A:0.3;"; done; SC="${SC}185:RIGHT:195;"
# REPLAY=<file>: a script made by tools/wonder_replay_script.py from a recorded play, instead of the blind one
[ -n "$REPLAY" ] && SC=$(cat "$REPLAY")
{
    echo "# scripted course run ($N)"
    echo "NX_HANDHELD=1"
    echo "NX_MEM_LIMIT_GB=5"
    echo "NX_DUMP_FRAMES=600"
    echo "NX_INPUT_SCRIPT=$SC"
    for x in "$@"; do echo "$x"; done
} > "$OUT/config.txt"
adb -s "$S" push "$OUT/config.txt" "$D/config.txt" > /dev/null
adb -s "$S" shell rm -rf "'$D/frames'" "'$D/profile.txt'"
adb -s "$S" shell mkdir -p "'$D/frames'"
adb -s "$S" shell am force-stop "$PKG"
adb -s "$S" logcat -c
adb -s "$S" shell am start -n "$PKG/android.app.NativeActivity" > /dev/null
echo "t_s total_pss_kb graphics_kb native_kb mem_available_kb" > "$OUT/memory.txt"
for t in $(seq 0 20 300); do
    sleep 20
    M=$(adb -s "$S" shell dumpsys meminfo "$PKG" 2>/dev/null)
    TOTAL=$(echo "$M" | grep -m1 "TOTAL PSS:" | awk '{print $3}')
    [ -z "$TOTAL" ] && TOTAL=$(echo "$M" | grep -m1 "^ *TOTAL " | awk '{print $2}')
    GFX=$(echo "$M" | grep -m1 "Graphics:" | awk '{print $2}')
    NAT=$(echo "$M" | grep -m1 "Native Heap:" | awk '{print $3}')
    AVAIL=$(adb -s "$S" shell grep MemAvailable /proc/meminfo | awk '{print $2}')
    echo "$((t + 20)) $TOTAL $GFX $NAT $AVAIL" >> "$OUT/memory.txt"
    [ "$t" = 200 ] && adb -s "$S" shell "echo 10 > '$D/profile'"
done
adb -s "$S" logcat -d -v time 'wonder:I' '*:S' > "$OUT/logcat.txt"
adb -s "$S" shell pidof "$PKG" > /dev/null && echo "running at the end" || echo "NOT running at the end"
adb -s "$S" pull "$D/nx.log" "$OUT/nx.log" > /dev/null
adb -s "$S" pull "$D/profile.txt" "$OUT/profile.txt" > /dev/null 2>&1
adb -s "$S" pull "$D/frames/." "$OUT/frames/" > /dev/null 2>&1
adb -s "$S" shell am force-stop "$PKG"
grep "per frame over" "$OUT/nx.log" | awk '{s+=$6;n++; if($6<30)c++} END{if(n) print n" intervals, mean "s/n" fps, "c" under 30"}'
cat "$OUT/memory.txt"

#!/bin/bash
# usage: tools/hang.sh [seconds]  -- run the game, interrupt after N seconds, print the named guest call stack
cd "$(dirname "$0")/.."
secs=${1:-5}
gdb -q -batch -ex "set debuginfod enabled off" -ex "run" -ex "bt 40" --args ./build/mss ../repo/orig/RMBE01/sys/main.dol ../game > build/hang.log 2>&1 &
sleep "$secs"; pkill -INT -x mss; wait
grep -v "^#" build/hang.log | grep -v "debuginfod\|libthread_db\|^\[\|Enable\|Debuginfod\|To make" | tail -15
grep "^#" build/hang.log | grep -o "f_[0-9A-F]\{8\}\|in [a-z_]* (" | while read -r f; do
  f=${f#in }; a=${f#f_}; echo "  $f $(grep "^$a " build/gen/names.txt | cut -d' ' -f2)"; done

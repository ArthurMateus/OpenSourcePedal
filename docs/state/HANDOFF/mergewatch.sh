#!/bin/bash
# usage: mergewatch.sh <pr> ; merges after all six required checks SUCCESS on the current head, stops on failure
pr=$1
req=("windows-msvc (Release, /WX)" "windows-msvc-debug (RT checker)" "pluginval (v1.0.4, strictness 9)" "guard" "clang-format" "halfband-check")
for i in $(seq 1 150); do
  j=$(gh pr view $pr --json state,title,statusCheckRollup)
  st=$(echo "$j" | python -c "import sys,json; print(json.load(sys.stdin)['state'])")
  [ "$st" != "OPEN" ] && { echo "PR $pr is $st"; exit 3; }
  r=$(echo "$j" | python -c "
import sys,json
d=json.load(sys.stdin); req=sys.argv[1:]
got={(c.get('name') or c.get('context')):(c.get('conclusion') or c.get('state') or c.get('status')) for c in d['statusCheckRollup']}
bad=[k for k,v in got.items() if v in ('FAILURE','CANCELLED','TIMED_OUT','ERROR','ACTION_REQUIRED')]
if bad: print('BAD',got); sys.exit()
print('OK' if all(got.get(k)=='SUCCESS' for k in req) else 'WAIT', got)" "${req[@]}")
  case "$r" in BAD*) echo "FAILED: $r"; exit 1;; OK*) echo "GREEN: $r"; t=$(echo "$j" | python -c "import sys,json; print(json.load(sys.stdin)['title'])"); gh pr merge $pr --squash --delete-branch --subject "$t (#$pr)"; exit $?;; esac
  sleep 60
done
echo "TIMEOUT: $r"; exit 2

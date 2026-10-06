#!/bin/bash
# Runs the installed kyty_run.sh from a terminal with the environment the launcher gives a game:
# every KYTY_*/TRACY_* variable of this shell is dropped, then u59-preset.json (next to the
# emulator) is applied, then the KEY=VALUE arguments are applied on top of it.
#
# The launcher reads the preset itself, but it also clears the shell's KYTY_* variables, so a
# diagnostic switch cannot reach a launcher run; a plain ./kyty_run.sh keeps the switch but skips
# the preset (no program cache, no pipeline prefetch, the slow default renderer paths).
#
# Usage: tools/run-u59.sh [KYTY_X=value ...] 2>&1 | tee run.log
#   KYTY_RUN_DIR selects the install directory (default: _Build/linux-clang/install).
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
dir=${KYTY_RUN_DIR:-$repo/_Build/linux-clang/install}
preset=$dir/u59-preset.json
[[ -x $dir/kyty_run.sh ]] || { echo "no kyty_run.sh in $dir (start the game once from the launcher)" >&2; exit 1; }
[[ -f $preset ]] || { echo "no u59-preset.json in $dir" >&2; exit 1; }

for name in $(compgen -e); do
	case $name in KYTY_* | TRACY_*) unset "$name" ;; esac
done
while IFS='=' read -r name value; do
	export "$name=$value"
done < <(python3 -c '
import json, sys
for key, value in json.load(open(sys.argv[1])).items():
    if not (key.startswith("KYTY_") or key.startswith("TRACY_")) or not isinstance(value, str):
        sys.exit(f"invalid preset entry {key!r}")
    print(f"{key}={value}")
' "$preset")
for pair in "$@"; do
	[[ $pair == KYTY_*=* || $pair == TRACY_*=* ]] || { echo "not a KYTY_/TRACY_ assignment: $pair" >&2; exit 1; }
	export "${pair?}"
done

echo "run-u59: $(env | grep -c '^KYTY_') KYTY_ variables; build $(strings "$dir/kyty_emulator" | grep -m1 -E '^[0-9a-f]{8}$' || echo unknown)" >&2
cd "$dir"
exec ./kyty_run.sh

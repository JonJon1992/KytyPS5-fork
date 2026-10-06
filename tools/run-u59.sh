#!/bin/bash
# Runs the installed kyty_run.sh from a terminal with the environment the launcher gives a game:
# every KYTY_*/TRACY_* variable of this shell is dropped, then u59-preset.json (next to the
# emulator) is applied, then the KEY=VALUE arguments are applied on top of it.
#
# The launcher reads the preset itself, but it also clears the shell's KYTY_* variables, so a
# diagnostic switch cannot reach a launcher run; a plain ./kyty_run.sh keeps the switch but skips
# the preset (no program cache, no pipeline prefetch, the slow default renderer paths).
#
# Usage: tools/run-u59.sh [--game <eboot.bin>] [KYTY_X=value ...] 2>&1 | tee run.log
#   The emulator arguments come from kyty_run.sh, which the launcher rewrites for the last game it
#   started; --game replaces the game so the run does not depend on that.
#   KYTY_RUN_DIR selects the install directory (default: _Build/linux-clang/install).
set -euo pipefail

game=
if [[ ${1:-} == --game ]]; then
	game=${2:?--game needs a path}
	[[ -f $game ]] || { echo "no such game file: $game" >&2; exit 1; }
	shift 2
fi

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

# The emulator command line of kyty_run.sh (its first quoted line), with --game replaced.
mapfile -t command < <(python3 -c '
import shlex, sys
line = next(l for l in open(sys.argv[1]) if l.startswith("\x27"))
args = shlex.split(line)
if sys.argv[2]:
    if "--game" not in args:
        sys.exit("kyty_run.sh has no --game argument")
    args[args.index("--game") + 1] = sys.argv[2]
print("\n".join(args))
' "$dir/kyty_run.sh" "$game")
game_index=-1
for index in "${!command[@]}"; do
	[[ ${command[$index]} == --game ]] && game_index=$((index + 1))
done
# KYTY_BUILD_LABEL (kytyGitVersion.h.in), the line a fatal error prints as "Source build <hash>".
# grep -m1 closes the pipe early; without pipefail its SIGPIPE to strings is not a failure.
build=$(set +o pipefail; strings "$dir/kyty_emulator" | grep -m1 '^Source build ' || true)
echo "run-u59: $(env | grep -c '^KYTY_') KYTY_ variables; ${build:-build unknown}; game ${command[$game_index]:-?}" >&2
cd "$dir"
exec "${command[@]}"

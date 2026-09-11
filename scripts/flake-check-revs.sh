#!/usr/bin/env bash
# scripts/flake-check-revs.sh — verify flake.nix's vendored-source pins
# match the git submodules they shadow.
#
# Why this exists: the flake does not use `?submodules=1`.  It pins each
# vendored dependency as its own flake input (tre-src, lime-src) and copies
# the tree in during postPatch.  So there are TWO sources of truth for every
# vendored rev, and nothing compared them.
#
# That is not a cosmetic split.  patches/tre-progress-hook.patch is generated
# against a specific TRE tree; when vendor/tre moved to upstream master and
# flake.nix kept pointing at the old v0.9.0 rev, the patch no longer applied
# and `nix build .#pg18` died in patchPhase -- while `make` from a git
# checkout (which uses the real submodule) still built and passed 41/41
# tests.  v3.2.3 shipped that way: green on every test I ran, unbuildable
# for anyone consuming the flake.
#
# Exit 0 if every pin matches, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

fail=0

# submodule path -> flake input name
declare -A INPUTS=(
    [vendor/tre]=tre-src
    [vendor/lime]=lime-src
)

for path in "${!INPUTS[@]}"; do
    input=${INPUTS[$path]}

    # Submodule rev as recorded in the index (not the checked-out HEAD:
    # what a fresh `clone --recurse-submodules` would get).
    sub_rev=$(git ls-tree HEAD "$path" | awk '{print $3}')
    if [[ -z $sub_rev ]]; then
        echo "FAIL: $path is not a submodule in HEAD" >&2
        fail=1
        continue
    fi

    # The 40-hex rev appearing in that input's url line in flake.nix.
    flake_rev=$(awk -v inp="$input" '
        $0 ~ "^[[:space:]]*" inp "[[:space:]]*=" { in_blk = 1 }
        in_blk && /url[[:space:]]*=/ {
            if (match($0, /[0-9a-f]{40}/)) {
                print substr($0, RSTART, RLENGTH); exit
            }
        }
        in_blk && /};/ { in_blk = 0 }
    ' flake.nix)

    if [[ -z $flake_rev ]]; then
        echo "FAIL: no 40-hex rev found for input '$input' in flake.nix" >&2
        fail=1
        continue
    fi

    if [[ $sub_rev != "$flake_rev" ]]; then
        echo "FAIL: $path and flake.nix input '$input' disagree:" >&2
        echo "        submodule:  $sub_rev" >&2
        echo "        flake.nix:  $flake_rev" >&2
        echo "      Update the input's url rev to match, then run" >&2
        echo "      'nix flake update $input' to refresh flake.lock." >&2
        fail=1
    else
        echo "ok   $path == flake input '$input' ($sub_rev)"
    fi
done

# flake.nix must not hard-code a version string that can drift from the
# control file (it silently stayed at 3.0.2 for three releases, so `nix
# build` produced a derivation named pg_tre-3.0.2 for 3.2.3).
if grep -qE '^\s*version\s*=\s*"[0-9]' flake.nix; then
    echo "FAIL: flake.nix hard-codes a version literal; derive it from" >&2
    echo "      pg_tre.control instead (it drifted for three releases)." >&2
    fail=1
else
    echo "ok   flake.nix derives version from pg_tre.control"
fi

if [[ $fail -eq 0 ]]; then
    echo "All flake/submodule revs agree."
fi
exit $fail

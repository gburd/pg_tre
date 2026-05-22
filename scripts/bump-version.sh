#!/usr/bin/env bash
#
# scripts/bump-version.sh OLD NEW
#
# Bumps the pg_tre version from OLD to NEW. Run from the repo
# root with a clean working tree. Auto-detects mode from the
# version-string syntax:
#
#   OLD=A.B.C       NEW=A.B.D-dev   →  dev bump (post-release)
#   OLD=X.Y.Z-dev   NEW=X.Y.Z       →  release (same triple)
#
# Updates SQL files, the Makefile DATA list, the control file,
# include/pg_tre/pg_tre.h, META.json, STATUS.md, CHANGELOG.md
# header line, debian/changelog, and packaging/pg_tre.spec.
#
# Does NOT author the upgrade SQL body, edit the upgrade-tests
# matrix, run the test suite, tag, or push — those are listed in
# the "next steps" output.
#
# Inspired by pg_textsearch's bump-version.sh.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 OLD NEW" >&2
    exit 2
fi

OLD=$1
NEW=$2

# --- mode detection ------------------------------------------------

is_release() {  # X.Y.Z (no -dev suffix)
    [[ $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}
is_dev() {      # X.Y.Z-dev
    [[ $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+-dev$ ]]
}

if is_release "$OLD" && is_dev "$NEW"; then
    MODE=dev
elif is_dev "$OLD" && is_release "$NEW" && [[ ${OLD%-dev} == "$NEW" ]]; then
    MODE=release
else
    echo "Error: invalid OLD/NEW combination." >&2
    echo "  Dev bump: OLD=A.B.C, NEW=X.Y.Z-dev" >&2
    echo "  Release:  OLD=X.Y.Z-dev, NEW=X.Y.Z (same triple)" >&2
    exit 2
fi

# --- safety checks -------------------------------------------------

if [[ ! -f pg_tre.control ]]; then
    echo "Error: run from the repo root." >&2
    exit 2
fi

if ! git diff-index --quiet HEAD; then
    echo "Error: working tree has uncommitted changes. Commit or stash first." >&2
    git status --porcelain >&2
    exit 2
fi

# Untracked files in vendored submodules are tolerated; they're
# build artifacts ignored at the top level via .gitignore.  We only
# care about uncommitted modifications to the index.
untracked_top=$(git status --porcelain | grep -E '^\?\?' | grep -vE '^\?\? (vendor/|_/)' || true)
if [[ -n $untracked_top ]]; then
    echo "Error: untracked files at the repo root:" >&2
    echo "$untracked_top" | sed 's/^/  /' >&2
    echo "Add to .gitignore or stage before bumping." >&2
    exit 2
fi

SRC_MAIN="sql/pg_tre--$OLD.sql"
DST_MAIN="sql/pg_tre--$NEW.sql"

if [[ ! -f $SRC_MAIN ]]; then
    echo "Error: $SRC_MAIN does not exist." >&2
    exit 2
fi
if [[ -f $DST_MAIN ]]; then
    echo "Error: $DST_MAIN already exists." >&2
    exit 2
fi

# --- mode-specific actions -----------------------------------------

if [[ $MODE == dev ]]; then
    NEW_UPGRADE="sql/pg_tre--$OLD--$NEW.sql"
    if [[ -f $NEW_UPGRADE ]]; then
        echo "Error: $NEW_UPGRADE already exists." >&2
        exit 2
    fi

    # Rename the base SQL file. Body substitution happens below.
    git mv "$SRC_MAIN" "$DST_MAIN"

    # Create the upgrade-script stub. Authors append catalog DDL
    # as work lands; release-time audit verifies completeness.
    cat > "$NEW_UPGRADE" <<EOF
-- pg_tre $OLD -> $NEW upgrade.
--
-- This stub is filled in as catalog-changing features land in
-- the dev cycle.  At release time, run the audit from
-- RELEASING.md to confirm every new CREATE FUNCTION /
-- OPERATOR / OPERATOR CLASS / TYPE / CAST / ALTER OPERATOR
-- FAMILY in sql/pg_tre--$NEW.sql has a matching statement
-- here, and every removed/renamed object has a matching
-- DROP / ALTER.
EOF
    git add "$NEW_UPGRADE"

    # Makefile: rename the base-file DATA entry, then append the
    # new upgrade entry. Use a targeted regex (`pg_tre--OLD.sql`)
    # rather than a broad `OLD.sql` to avoid corrupting legacy
    # upgrade-chain entries like `pg_tre--A--OLD.sql`.
    # Capture OLD/NEW into env so the perl one-liner can read them
    # cleanly — embedding shell vars in a perl script that itself
    # contains both single and double quotes is fragile and was
    # producing nonsense output (entries like
    # `pg_tre--1.2.0-dev--1.2.0-dev.sql`) when the order of
    # substitutions interacted with later blanket replacements.
    OLD_ENV=$OLD NEW_ENV=$NEW perl -i -pe '
        BEGIN {
            $old = $ENV{OLD_ENV};
            $new = $ENV{NEW_ENV};
            $added = 0;
        }
        # Rename the base install entry: pg_tre--OLD.sql -> pg_tre--NEW.sql.
        # Use a tight pattern so legacy chain entries
        # `pg_tre--A--OLD.sql` are not touched.
        s|pg_tre--\Q$old\E\.sql|pg_tre--$new.sql|g;
        # Append the new upgrade entry once, immediately after the
        # first DATA-list line.  Preserve the original line termination:
        # if the original DATA line had no trailing backslash (the entry
        # list was on a single line), end our inserted entry without one
        # too, otherwise we extend DATA into the next Makefile line
        # (e.g. `DATA_built =`) and break the install.
        if (!$added && m{^DATA\s*=\s*sql/pg_tre--}) {
            chomp(my $line = $_);
            my $had_continuation = $line =~ s|\s*\\\s*$||;
            my $upgrade = "sql/pg_tre--$old--$new.sql";
            if ($line !~ /\Q$upgrade\E/) {
                if ($had_continuation) {
                    $_ = "$line \\\n       $upgrade \\\n";
                } else {
                    $_ = "$line \\\n       $upgrade\n";
                }
            } else {
                $_ = "$line" . ($had_continuation ? " \\\n" : "\n");
            }
            $added = 1;
        }
    ' Makefile

elif [[ $MODE == release ]]; then
    # Find and rename the upgrade file `sql/pg_tre--PREV--OLD.sql`.
    shopt -s nullglob
    upgrades=( sql/pg_tre--*--"$OLD".sql )
    shopt -u nullglob
    if [[ ${#upgrades[@]} -ne 1 ]]; then
        echo "Error: expected exactly one sql/pg_tre--*--$OLD.sql," \
             "found ${#upgrades[@]}." >&2
        exit 2
    fi
    SRC_UPGRADE=${upgrades[0]}
    DST_UPGRADE=${SRC_UPGRADE//$OLD.sql/$NEW.sql}

    # Extract PREV (last released version) for downstream context.
    PREV=${SRC_UPGRADE#sql/pg_tre--}
    PREV=${PREV%%--*}

    git mv "$SRC_MAIN" "$DST_MAIN"
    git mv "$SRC_UPGRADE" "$DST_UPGRADE"

    # Makefile: update both DATA entries (main install + the
    # current upgrade file). Safe to use the broader `--OLD.sql`
    # regex here because in release mode OLD=X.Y.Z-dev only
    # appears in the current cycle's entries, never in legacy
    # upgrade-chain filenames.
    perl -i -pe "s|--\Q$OLD\E\.sql|--$NEW.sql|g" Makefile
fi

# Body substitutions for the renamed SQL files.
perl -i -pe "s/\Q$OLD\E/$NEW/g" "$DST_MAIN"
[[ $MODE == release ]] && perl -i -pe "s/\Q$OLD\E/$NEW/g" "$DST_UPGRADE"

# --- common text substitutions -------------------------------------

# Files where every literal occurrence of OLD becomes NEW.
# Makefile is intentionally NOT in this list. Its DATA list and
# PG_TRE_VERSION line have already been updated above with
# targeted regexes; a blanket s/OLD/NEW/g would also rewrite the
# inner version of legacy chain entries like
# `pg_tre--1.1.0--1.1.1.sql` -> `pg_tre--1.1.0--1.2.0-dev.sql`,
# which corrupts the upgrade chain.
common_files=(
    pg_tre.control
    META.json
    include/pg_tre/pg_tre.h
    debian/changelog
    packaging/pg_tre.spec
    scripts/release-check.sh
)

for f in "${common_files[@]}"; do
    if [[ -f $f ]]; then
        perl -i -pe "s/\Q$OLD\E/$NEW/g" "$f"
    fi
done

# Update the Makefile PG_TRE_VERSION line specifically. (The
# DATA-list rewriting above already handled SQL filenames.)
if grep -q '^PG_TRE_VERSION' Makefile; then
    perl -i -pe "s/^(PG_TRE_VERSION\s*=\s*)\Q$OLD\E\b/\${1}$NEW/" Makefile
fi

# STATUS.md and CHANGELOG.md need contextual handling: the bump
# script updates the version markers but leaves the prose alone.
# Authors fill in the release notes themselves.
if [[ -f STATUS.md ]]; then
    perl -i -pe "s/Released:\s*\*\*\Q$OLD\E\*\*/Released: **$NEW**/g" STATUS.md
fi

# --- straggler check -----------------------------------------------

# Find any remaining references to OLD outside known-safe
# locations. If anything legitimate shows up here, add it to
# common_files above.
stragglers=$(git grep --fixed-strings "$OLD" -- \
    ':!scripts/bump-version.sh' \
    ':!test/expected/' \
    ':!sql/pg_tre--*--*.sql' \
    ':!RELEASING.md' \
    ':!CHANGELOG.md' \
    ':!.github/workflows/upgrade-tests.yml' \
    ':!doc/' \
    | cut -d: -f1 | sort -u \
    || true)

# Filter out the Makefile DATA list, which legitimately retains
# OLD on the chain-entry line `sql/pg_tre--PREV--OLD.sql`.
if [[ -n $stragglers ]]; then
    real_stragglers=""
    for f in $stragglers; do
        if [[ $f == Makefile ]]; then
            # Lines mentioning OLD as a chain-entry version on
            # either side (`pg_tre--PREV--OLD.sql` or
            # `pg_tre--OLD--NEXT.sql`) are legitimate; only flag
            # lines that mention OLD outside that context.
            non_chain=$(grep --fixed-strings "$OLD" "$f" | \
                grep -vE "pg_tre--[^ ]*--$OLD\.sql|pg_tre--$OLD--[^ ]*\.sql" \
                || true)
            [[ -n $non_chain ]] && real_stragglers+="$f"$'\n'
        else
            real_stragglers+="$f"$'\n'
        fi
    done
    stragglers=$(printf '%s' "$real_stragglers" | sed '/^$/d')
fi

if [[ -n $stragglers ]]; then
    echo "Warning: literal '$OLD' still appears in:" >&2
    echo "$stragglers" | sed 's/^/  /' >&2
    echo "Review and update by hand if these need bumping." >&2
fi

# --- next steps ----------------------------------------------------

echo
echo "Version bump $OLD → $NEW complete."
echo

if [[ $MODE == release ]]; then
    cat <<EOF
Next steps (release):
  1. Edit CHANGELOG.md: add a [$NEW] section above [$PREV] with
     release notes.  Group entries under ### Added / ### Changed /
     ### Fixed / ### Removed / ### Security as appropriate.
  2. Add $NEW to the old_version matrix in
     .github/workflows/upgrade-tests.yml so future releases test
     upgrade compatibility from this version.
  3. Audit sql/pg_tre--$PREV--$NEW.sql against the diff:
       diff sql/pg_tre--$PREV.sql sql/pg_tre--$NEW.sql
     Every new CREATE FUNCTION / OPERATOR / OPERATOR CLASS /
     TYPE / CAST in the main file must have a matching statement
     in the upgrade file. See RELEASING.md.
  4. Run scripts/release-check.sh.
  5. Open a PR titled "Release pg_tre $NEW" and merge.
  6. After merge:
       git tag -a v$NEW -m "pg_tre $NEW"
       git push origin v$NEW
EOF
else
    cat <<EOF
Next steps (dev bump):
  1. Review the generated stub $NEW_UPGRADE.
  2. Open a PR titled "chore: bump version to $NEW" and merge.
  3. As features land in the cycle, append catalog DDL to
     sql/pg_tre--$OLD--$NEW.sql so that release-time audit
     finds nothing missing.
EOF
fi

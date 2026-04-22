#!/usr/bin/env bash
#
# Enforce the PostgreSQL extension versioning contract on every PR/push.
#
# The extension version (default_version in cloudsync.control, and the
# cloudsync--<ver>.sql filename) is MAJOR.MINOR only — it's derived from the
# first two components of CLOUDSYNC_VERSION in src/cloudsync.h.  The full
# semver of the binary is reported by the cloudsync_version() SQL function.
#
# Contract:
#   - PATCH bumps (e.g. 1.0.16 -> 1.0.17) keep EXTVERSION the same ('1.0').
#     Binary-only release; no SQL surface changes allowed; no user action
#     needed after swapping the .so.
#   - MINOR / MAJOR bumps (e.g. 1.0.x -> 1.1.0 or 1.x -> 2.0) move EXTVERSION.
#     Must ship a matching cloudsync--<prev>--<new>.sql upgrade script so
#     existing deployments can ALTER EXTENSION cloudsync UPDATE.
#
# This script runs in one of two modes depending on whether EXTVERSION moved
# since the most recent ancestor semver tag:
#
#   (a) EXTVERSION unchanged (patch release):
#       diff the current cloudsync.sql.in against the previous tag's install
#       script. If they differ, fail: SQL surface changed without a MINOR
#       bump, which would silently break users whose pg_extension.extversion
#       stays at the old value.
#
#   (b) EXTVERSION changed (minor/major release):
#       require src/postgresql/migrations/cloudsync--<prev>--<new>.sql.
#
# Exit codes:
#   0 - contract satisfied (no bump, or bump with migration present)
#   1 - contract violated (SQL drift without bump, or missing migration)
#   2 - misconfigured environment (no git, missing header, unresolvable tag)

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel 2>/dev/null || true)
if [ -z "$repo_root" ]; then
    echo "Error: not inside a git working tree; cannot determine previous release tag." >&2
    exit 2
fi
cd "$repo_root"

header="src/cloudsync.h"
sql_template="src/postgresql/cloudsync.sql.in"

[ -f "$header" ]       || { echo "Error: $header not found." >&2; exit 2; }
[ -f "$sql_template" ] || { echo "Error: $sql_template not found." >&2; exit 2; }

# Read full semver from the header and derive MAJOR.MINOR.
current_full=$(sed -n 's/^#define CLOUDSYNC_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$header")
if [ -z "$current_full" ]; then
    echo "Error: could not read CLOUDSYNC_VERSION from $header." >&2
    exit 2
fi
current_ext=$(printf '%s\n' "$current_full" | cut -d. -f1-2)
if [ -z "$current_ext" ]; then
    echo "Error: could not derive MAJOR.MINOR from CLOUDSYNC_VERSION '$current_full'." >&2
    exit 2
fi

# Find the latest ancestor semver tag.
prev_tag=$(git describe --tags --abbrev=0 --match '[0-9]*.[0-9]*.[0-9]*' 2>/dev/null || true)
if [ -z "$prev_tag" ]; then
    echo "No prior semver tag reachable from HEAD; skipping migration check."
    echo "(This is expected on an initial release or a shallow clone without tags.)"
    exit 0
fi

# Resolve what EXTVERSION the previous release shipped with.
#   Pre-new-scheme tags: tracked cloudsync.control held default_version.
#   New-scheme tags: control is generated; read CLOUDSYNC_VERSION from header
#                    at that tag and truncate.
prev_full=$(git show "${prev_tag}:${header}" 2>/dev/null \
            | sed -n 's/^#define CLOUDSYNC_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' || true)

prev_ext=$(git show "${prev_tag}:docker/postgresql/cloudsync.control" 2>/dev/null \
           | sed -n "s/^default_version = '\\([^']*\\)'.*/\\1/p" \
           | head -1 || true)

if [ -z "$prev_ext" ] && [ -n "$prev_full" ]; then
    prev_ext=$(printf '%s\n' "$prev_full" | cut -d. -f1-2)
fi

if [ -z "$prev_ext" ]; then
    echo "Error: could not determine EXTVERSION at tag ${prev_tag}." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Mode (a): EXTVERSION unchanged — verify SQL surface didn't drift.
# ---------------------------------------------------------------------------
if [ "$prev_ext" = "$current_ext" ]; then
    # Produce a normalized view of the previous release's install script.
    if git cat-file -e "${prev_tag}:${sql_template}" 2>/dev/null; then
        # New-scheme tag: substitute @EXTVERSION@ with that tag's EXTVERSION.
        prev_sql=$(git show "${prev_tag}:${sql_template}" \
                   | sed "s/@EXTVERSION@/${prev_ext}/g")
    else
        # Old-scheme tag: the literal cloudsync--<ext>.sql was tracked.
        prev_install_path="src/postgresql/cloudsync--${prev_ext}.sql"
        if ! git cat-file -e "${prev_tag}:${prev_install_path}" 2>/dev/null; then
            echo "Error: could not find previous install script at ${prev_tag}:${prev_install_path}." >&2
            exit 2
        fi
        prev_sql=$(git show "${prev_tag}:${prev_install_path}")
    fi

    # Current install script, rendered (substitute @EXTVERSION@ -> current_ext).
    curr_sql=$(sed "s/@EXTVERSION@/${current_ext}/g" "$sql_template")

    if [ "$prev_sql" = "$curr_sql" ]; then
        echo "OK: patch-only release candidate."
        echo "    EXTVERSION unchanged at '${current_ext}' since ${prev_tag}."
        echo "    Binary semver: ${prev_full:-unknown} -> ${current_full}."
        echo "    SQL surface identical; no migration script required."
        exit 0
    fi

    cat >&2 <<EOF
ERROR: Semver contract violated — SQL surface drift without a MINOR bump.

  Previous release tag       : ${prev_tag}
  EXTVERSION (prev == current): ${current_ext}
  Binary semver              : ${prev_full:-unknown} -> ${current_full}

The install script (src/postgresql/cloudsync.sql.in) differs from what the
previous release shipped, but EXTVERSION is still '${current_ext}'. Existing
deployments have pg_extension.extversion = '${current_ext}' and will NOT run
any upgrade script when they swap in the new .so, so the new SQL bindings
won't be applied to their catalog. This silently breaks users.

Pick one:

  1. Bump MINOR in src/cloudsync.h (e.g. 1.0.x -> 1.1.0), and add
     src/postgresql/migrations/cloudsync--${current_ext}--<new_ext>.sql
     with the DDL deltas (CREATE OR REPLACE FUNCTION ..., etc.). This is the
     correct choice for any intentional SQL-surface change.

  2. Revert the SQL-level change in cloudsync.sql.in if it was accidental
     (e.g. a refactor that went further than intended).

Diff (previous -> current, normalized):

EOF
    # Show a readable diff; fall back to a terse message if diff is unavailable.
    if command -v diff >/dev/null 2>&1; then
        diff -u <(printf '%s\n' "$prev_sql") <(printf '%s\n' "$curr_sql") >&2 || true
    else
        echo "(install 'diff' to see line-level changes)" >&2
    fi
    exit 1
fi

# ---------------------------------------------------------------------------
# Mode (b): EXTVERSION changed — require a migration file.
# ---------------------------------------------------------------------------
expected="src/postgresql/migrations/cloudsync--${prev_ext}--${current_ext}.sql"
if [ ! -f "$expected" ]; then
    cat >&2 <<EOF
ERROR: Missing PostgreSQL extension upgrade script.

  Previous release tag : ${prev_tag}
  Previous EXTVERSION  : ${prev_ext}
  Current EXTVERSION   : ${current_ext}  (from CLOUDSYNC_VERSION '${current_full}')

  Expected file        : ${expected}

Users on extension version '${prev_ext}' cannot run

    ALTER EXTENSION cloudsync UPDATE;

to reach '${current_ext}' without this file. Shipping without it forces users
into the destructive DROP EXTENSION ... CASCADE; CREATE EXTENSION ...
workaround, or leaves them in a split-brain state where cloudsync.so is the
new version but pg_extension.extversion is still '${prev_ext}'.

Create the file at the path above. If there are no SQL-level changes between
the versions (rare for a MINOR bump), a header comment is enough; otherwise
add the DDL deltas. See src/postgresql/migrations/README.md for the rules.
EOF
    exit 1
fi

echo "OK: ${expected} exists (upgrade path ${prev_ext} -> ${current_ext} covered)."
echo "    Binary semver: ${prev_full:-unknown} -> ${current_full}."

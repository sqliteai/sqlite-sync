#!/usr/bin/env bash
#
# Verify that the PostgreSQL extension has a valid upgrade script for the
# current CLOUDSYNC_VERSION in src/cloudsync.h.
#
# A missing cloudsync--<prev>--<curr>.sql means existing deployments cannot run
#     ALTER EXTENSION cloudsync UPDATE;
# to reach the new version. Worse, if a user replaces cloudsync.so without a
# valid upgrade path, they end up in a split-brain state where the .so on disk
# is new but pg_extension.extversion is stale and the SQL bindings in pg_proc
# may not match the ABI of the new binary. So this check is release-blocking.
#
# "Previous version" is derived from the most recent semver git tag reachable
# from HEAD:
#   - For pre-1.0.17 tags the extension version is frozen at '1.0' because the
#     static control file shipped default_version = '1.0' across every release.
#   - For 1.0.17 and later, the control file is generated from cloudsync.h at
#     build time, so we read CLOUDSYNC_VERSION from the tagged source tree.
#
# Exit codes:
#   0 - migration present (or no version bump since last tag; nothing to check)
#   1 - migration missing / required file not found
#   2 - misconfigured environment (no git, no header, unresolvable tag, etc.)

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel 2>/dev/null || true)
if [ -z "$repo_root" ]; then
    echo "Error: not inside a git working tree; cannot determine previous release tag." >&2
    exit 2
fi
cd "$repo_root"

header="src/cloudsync.h"
if [ ! -f "$header" ]; then
    echo "Error: $header not found." >&2
    exit 2
fi

current_version=$(sed -n 's/^#define CLOUDSYNC_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$header")
if [ -z "$current_version" ]; then
    echo "Error: could not read CLOUDSYNC_VERSION from $header." >&2
    exit 2
fi

prev_tag=$(git describe --tags --abbrev=0 --match '[0-9]*.[0-9]*.[0-9]*' 2>/dev/null || true)
if [ -z "$prev_tag" ]; then
    echo "No prior semver tag reachable from HEAD; skipping migration check."
    echo "(This is expected on an initial release or a shallow clone without tags.)"
    exit 0
fi

# Determine what pg_extension.extversion contains for deployments of prev_tag.
# Pre-new-scheme tags have a tracked control file with a literal default_version.
prev_version=$(git show "${prev_tag}:docker/postgresql/cloudsync.control" 2>/dev/null \
               | sed -n "s/^default_version = '\\([^']*\\)'.*/\\1/p" \
               | head -1 || true)

# New-scheme tags generate the control file at build time; fall back to
# CLOUDSYNC_VERSION in the header at that tag.
if [ -z "$prev_version" ]; then
    prev_version=$(git show "${prev_tag}:${header}" 2>/dev/null \
                   | sed -n 's/^#define CLOUDSYNC_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' || true)
fi

if [ -z "$prev_version" ]; then
    echo "Error: could not determine the extension version shipped at tag ${prev_tag}." >&2
    exit 2
fi

if [ "$prev_version" = "$current_version" ]; then
    echo "CLOUDSYNC_VERSION unchanged since ${prev_tag} (${prev_version})."
    echo "No migration script required."
    exit 0
fi

expected="src/postgresql/migrations/cloudsync--${prev_version}--${current_version}.sql"
if [ ! -f "$expected" ]; then
    cat >&2 <<EOF
ERROR: Missing PostgreSQL extension upgrade script.

  Previous release tag : ${prev_tag}
  Previous extversion  : ${prev_version}
  Current CLOUDSYNC_VERSION in ${header}: ${current_version}

  Expected file        : ${expected}

Users of the previous release cannot run:

    ALTER EXTENSION cloudsync UPDATE;

to reach ${current_version} without this file. Shipping without it forces
users into a destructive DROP EXTENSION ... CASCADE; CREATE EXTENSION ...
workaround, or leaves them in a split-brain state where cloudsync.so is
the new version but pg_extension.extversion is still ${prev_version}.

Create the file at the path above. If the release has no SQL-level changes,
a header comment is enough; otherwise add the DDL deltas (CREATE OR REPLACE
FUNCTION ..., DROP FUNCTION IF EXISTS ..., etc.). See
src/postgresql/migrations/README.md for the full rules.
EOF
    exit 1
fi

echo "OK: ${expected} exists (upgrade path ${prev_version} -> ${current_version} covered)."

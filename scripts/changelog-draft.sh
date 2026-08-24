#!/bin/sh
# changelog-draft.sh — print a draft list of commit subjects since the last
# tag (or an explicit --since ref), as raw material for a .changes entry.
#
# This is NOT meant to be piped directly into mcp-server-zypp.changes.
# Commit-message granularity almost never matches changelog granularity —
# use this output as a starting point, then hand-edit down to user-relevant
# bullets before running `osc vc` to actually commit the entry.
#
# Usage:
#   scripts/changelog-draft.sh                  # since the last reachable tag
#   scripts/changelog-draft.sh --since <ref>     # since an explicit ref
#   scripts/changelog-draft.sh --since <ref> --until <ref>

set -eu

since=""
until_ref="HEAD"

while [ $# -gt 0 ]; do
    case "$1" in
        --since)
            since="$2"
            shift 2
            ;;
        --until)
            until_ref="$2"
            shift 2
            ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [ -z "$since" ]; then
    if since=$(git describe --tags --abbrev=0 "$until_ref" 2>/dev/null); then
        :
    else
        echo "# No tags found — showing full history up to $until_ref." >&2
        since=""
    fi
fi

if [ -n "$since" ]; then
    range="$since..$until_ref"
    echo "# Draft changelog: commits in $range" >&2
else
    range="$until_ref"
    echo "# Draft changelog: full history up to $until_ref (no prior tag)" >&2
fi

echo "# This is RAW material, not a finished entry — edit before 'osc vc'." >&2
echo >&2

git log --no-merges --reverse --pretty=format:'- %s' "$range"
echo

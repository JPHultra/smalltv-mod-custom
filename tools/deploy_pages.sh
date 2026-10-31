#!/usr/bin/env bash
# Publish editor/ to the gh-pages branch without GitHub Actions.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REMOTE=${REMOTE:-origin}
BRANCH=${BRANCH:-gh-pages}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cp -a "$ROOT/editor/." "$TMP/"
touch "$TMP/.nojekyll"

git -C "$TMP" init -q
git -C "$TMP" checkout -q -b "$BRANCH"
git -C "$TMP" add -A
NAME=$(git -C "$ROOT" config user.name || true)
EMAIL=$(git -C "$ROOT" config user.email || true)
git -C "$TMP" -c "user.name=${NAME:-SmallTV}" -c "user.email=${EMAIL:-smalltv@localhost}" \
  commit -q -m "Deploy editor from $(git -C "$ROOT" rev-parse --short HEAD)"
git -C "$TMP" push --force "$(git -C "$ROOT" remote get-url "$REMOTE")" "HEAD:$BRANCH"

if command -v gh >/dev/null 2>&1; then
  REPO=$(cd "$ROOT" && gh repo view --json nameWithOwner --jq .nameWithOwner)
  if gh api -X PUT "repos/$REPO/pages" \
      -f build_type=legacy \
      -f "source[branch]=$BRANCH" \
      -f "source[path]=/" >/dev/null 2>&1; then
    URL=$(gh api "repos/$REPO/pages" --jq .html_url)
    echo "Published editor to $BRANCH: $URL"
  else
    echo "Pushed $BRANCH. Set Settings -> Pages -> Deploy from a branch -> $BRANCH / (root)." >&2
  fi
else
  echo "Pushed $BRANCH. Set Settings -> Pages -> Deploy from a branch -> $BRANCH / (root)."
fi

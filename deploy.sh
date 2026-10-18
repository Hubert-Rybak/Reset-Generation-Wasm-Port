#!/usr/bin/env bash
# Build the site and publish dist/ to the gh-pages branch, which GitHub Pages
# serves at https://<owner>.github.io/<repo>/.
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
rev=$(git rev-parse --short HEAD)
work=$(mktemp -d)
trap 'git worktree remove --force "$work" >/dev/null 2>&1 || true' EXIT

if git fetch -q origin gh-pages 2>/dev/null; then
  git worktree add -q --detach "$work" FETCH_HEAD
else                                   # first deployment: start an empty branch
  git worktree add -q --detach "$work"
  git -C "$work" checkout -q --orphan gh-pages
  git -C "$work" rm -rqf .
fi
find "$work" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -a dist/. "$work"/
git -C "$work" add -A
if git -C "$work" diff --cached --quiet; then
  echo "[*] gh-pages already up to date"
  exit 0
fi
git -C "$work" commit -q -m "Deploy $rev"
git -C "$work" push origin HEAD:gh-pages
echo "[*] deployed $rev to gh-pages"

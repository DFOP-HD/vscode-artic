#!/bin/bash
# Cuts a release: bumps the version, tags it and pushes. The Release workflow
# (.github/workflows/release.yml) then builds the Linux and Windows server binaries,
# packages a single VSIX containing both, and attaches it to the GitHub release.
#
# To build a VSIX locally instead, use ./package.sh.

set -euo pipefail

cd "$(dirname "$0")"

git pull --recurse-submodules

if git status --porcelain | grep .; then
    echo "Uncommitted changes found in the repository. Commit or stash them first."
    exit 1
fi

# The artic fork is a submodule we commit to, so a dirty worktree there is easy to miss.
if git -C ../artic status --porcelain | grep .; then
    echo "Uncommitted changes found in the artic submodule."
    exit 1
fi

echo "Incrementing version..."
VERSION_TYPE="${1:-patch}"
npm version "$VERSION_TYPE" --no-git-tag-version

NEW_VERSION=$(node -p "require('./package.json').version")
TAG="v$NEW_VERSION"

git add package.json package-lock.json
git commit -m "Release $TAG"
git tag "$TAG"
git push
git push origin "$TAG"

echo ""
echo "Pushed $TAG. Watch the release build at:"
echo "  https://github.com/DFOP-HD/vscode-artic/actions/workflows/release.yml"

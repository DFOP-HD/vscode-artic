#!/bin/bash
set -e

git pull

# Check for uncommitted changes in root and artic-lsp
if git status --porcelain | grep .; then
    echo "Uncommitted changes found in root repository."
    exit 1
fi

if git -C artic-lsp status --porcelain | grep .; then
    echo "Uncommitted changes found in artic-lsp repository."
    exit 1
fi

# Increment version number in package.json (patch version)
echo "Incrementing version..."
npm version patch --no-git-tag-version

# Compile and package the extension
echo "Compiling and packaging extension..."
./package_extension.sh

# Get new version
NEW_VERSION=$(node -p "require('./package.json').version")
TAG="v$NEW_VERSION"
VSIX_FILE=$(ls *.vsix | tail -n1)

# Commit version bump
git add package.json package-lock.json \
&& git commit -m "Release $TAG" \
&& git tag "$TAG" \
&& git push \
&& git push --tags

# Create GitHub release and upload package
echo "Creating GitHub release..."
gh release create "$TAG" "$VSIX_FILE" --title "Release $TAG" --notes "Automated release of version $TAG"

echo "Release $TAG published with $VSIX_FILE"
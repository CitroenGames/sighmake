#!/usr/bin/env bash
set -eu

# Tag the current HEAD as a sighmake release and push the tag, which triggers
# CI to build all platforms and publish the GitHub release.
#
# Usage:
#   ./release.sh 0.3.0
#   ./release.sh v0.3.0
#   ./release.sh 0.3.0 --yes      (skip confirmation prompt)

cd "$(dirname "$(realpath "$0")")"

usage() {
    cat <<'EOF'
Usage: release.sh <version> [--yes]

  <version>   Semver-style version, with or without leading "v" (e.g. 0.3.0 or v0.3.0).
  --yes, -y   Skip the confirmation prompt.

Tags HEAD as v<version> and pushes the tag, triggering CI to publish a GitHub release.
EOF
}

case "${1:-}" in
    ""|-h|--help|/?) usage; exit 1 ;;
esac

ARG="$1"
ASSUME_YES=0
case "${2:-}" in
    --yes|-y) ASSUME_YES=1 ;;
esac

# Strip optional leading "v"
VERSION="${ARG#v}"
VERSION="${VERSION#V}"
TAG="v$VERSION"

# Validate version: digits + dots, optional -suffix (e.g. 0.3.0 or 1.2.3-rc1)
if ! printf '%s' "$VERSION" | grep -Eq '^[0-9][0-9.]*[0-9a-zA-Z.-]*$'; then
    echo "Error: \"$VERSION\" does not look like a version (expected e.g. 0.3.0)." >&2
    exit 1
fi

# Must be in a git repo
if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "Error: not inside a git repository." >&2
    exit 1
fi

# Working tree clean?
if [ -n "$(git status --porcelain)" ]; then
    echo "Error: working tree has uncommitted changes. Commit or stash first." >&2
    git status --short
    exit 1
fi

# Tag must not already exist locally
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null 2>&1; then
    echo "Error: tag $TAG already exists locally. Delete it first if you really mean it:" >&2
    echo "    git tag -d $TAG" >&2
    exit 1
fi

# Tag must not already exist on origin
if git ls-remote --exit-code --tags origin "refs/tags/$TAG" >/dev/null 2>&1; then
    echo "Error: tag $TAG already exists on origin." >&2
    exit 1
fi

SHORT="$(git rev-parse --short HEAD)"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"

echo
echo "About to create release $TAG"
echo "  commit:  $SHORT"
echo "  branch:  $BRANCH"
echo "  pushing: origin $TAG"
echo

if [ "$BRANCH" != "master" ]; then
    echo "Warning: you are not on master (currently on $BRANCH)."
    echo
fi

if [ "$ASSUME_YES" = "0" ]; then
    printf "Proceed? [y/N] "
    read -r CONFIRM
    case "$CONFIRM" in
        y|Y) ;;
        *) echo "Aborted."; exit 1 ;;
    esac
fi

echo
echo "Creating tag $TAG..."
git tag -a "$TAG" -m "Release $TAG"

echo "Pushing tag to origin..."
if ! git push origin "$TAG"; then
    echo "Error: git push failed. Cleaning up local tag." >&2
    git tag -d "$TAG" >/dev/null 2>&1 || true
    exit 1
fi

echo
echo "Done. CI will now build and publish the $TAG release."
echo "Watch progress: https://github.com/CitroenGames/sighmake/actions"

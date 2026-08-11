#!/usr/bin/env bash
# Bumps this project's own PATCH version by 1, commits, tags, and pushes.
#
# Source of truth is CMakeLists.txt's own `project(logger VERSION X.Y.Z ...)` -
# this script reads it, increments Z, rewrites it in CMakeLists.txt and in the
# matching `GIT_TAG vX.Y.Z` example in README.md's "integrating into another
# CMake project" section, commits both, tags the commit vX.Y.Z, and pushes the
# current branch plus the new tag to origin.
#
# Refuses to run at all if the working tree is not clean - a bump commit must
# contain nothing but the version change, never whatever else happens to be
# sitting in the working tree at the time.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

cmake_file="CMakeLists.txt"
readme_file="README.md"

if [[ -n "$(git status --porcelain)" ]]; then
  echo "bump_git_tag: working tree is not clean - commit or stash everything else first." >&2
  git status --short >&2
  exit 1
fi

current_version="$(grep -oP 'project\(logger VERSION \K[0-9]+\.[0-9]+\.[0-9]+' "$cmake_file")"
if [[ -z "$current_version" ]]; then
  echo "bump_git_tag: could not find 'project(logger VERSION X.Y.Z ...)' in $cmake_file." >&2
  exit 1
fi

major="${current_version%%.*}"
rest="${current_version#*.}"
minor="${rest%%.*}"
patch="${rest#*.}"
new_patch=$((patch + 1))
new_version="${major}.${minor}.${new_patch}"
new_tag="v${new_version}"

if git rev-parse -q --verify "refs/tags/${new_tag}" >/dev/null; then
  echo "bump_git_tag: tag ${new_tag} already exists." >&2
  exit 1
fi

echo "bump_git_tag: ${current_version} -> ${new_version} (${new_tag})"

sed -i "s/project(logger VERSION ${current_version} /project(logger VERSION ${new_version} /" "$cmake_file"
sed -i "s/GIT_TAG v${current_version}/GIT_TAG ${new_tag}/" "$readme_file"

git add "$cmake_file" "$readme_file"
git commit -m "Bump version to ${new_version}"
git tag "$new_tag"

git push origin HEAD
git push origin "$new_tag"

echo "bump_git_tag: pushed commit and tag ${new_tag} to origin."

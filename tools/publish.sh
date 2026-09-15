#!/bin/zsh
# Publish the firmware directory to the public releases repository and tag a
# release there (FirmwareSpec.md §16, §19.11). Run from the private monorepo
# with everything committed:
#   firmware/tools/publish.sh            pushes the firmware tree (no release)
#   firmware/tools/publish.sh v1.2.0     pushes it and tags v1.2.0 there; GitHub Actions builds the release
# Edit firmware/RELEASE_NOTES.md before tagging: it becomes the release body and the manifest's notes.
set -e
PUBLIC=git@github.com:alex-nugent/SoundBoard.git
cd "$(dirname "$0")/../.."
if [[ -n "$(git status --porcelain firmware)" ]]; then echo "commit the firmware changes first"; exit 1; fi
git remote get-url public >/dev/null 2>&1 || git remote add public "$PUBLIC"
echo "pushing firmware/ to $PUBLIC ..."
git subtree push --prefix=firmware public main
if [[ -n "$1" ]]; then
  case "$1" in v*) ;; *) echo "a version looks like v1.2.0"; exit 1;; esac
  SHA=$(git subtree split --prefix=firmware)
  git tag -f "$1" "$SHA"
  git push -f public "refs/tags/$1"          # re-tagging a version after a fix moves the tag (a release already built for it is deleted on GitHub first)
  git tag -d "$1" >/dev/null
  echo "tagged $1 on the public repository; the release builds at https://github.com/alex-nugent/SoundBoard/actions"
fi

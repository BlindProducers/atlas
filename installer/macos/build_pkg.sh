#!/bin/sh
# Builds the macOS installer, Atlas-<version>-macOS.pkg, from a finished
# Release build of Atlas_VST3 and Atlas_AU. It installs Atlas.vst3 and
# Atlas.component for all users in /Library/Audio/Plug-Ins, and the Factory
# banks and every library's folders (installer/stage_library.cmake) in the
# Atlas folder of the user logged in (scripts/library/postinstall). The
# installer's Customize button can leave any of the three out.
#
#   installer/macos/build_pkg.sh [build directory] [output directory]
#
# The build directory defaults to build and the output directory to dist.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=${1:-$root/build}
out=${2:-$root/dist}
artefacts=$build/Atlas_artefacts/Release
version=$(sed -n 's/^project(Atlas VERSION \([^ ]*\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || { echo "No project version found in CMakeLists.txt" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/packages"

# component <id> <folder> <install location>: a package of what <folder> holds.
component() {
  ditto --noextattr --noqtn "$here/scripts/$1" "$work/scripts-$1"
  # pkgbuild marks bundles relocatable by default: Installer would then update
  # any bundle with the same ID it finds elsewhere on the disk (a build folder,
  # or the other plug-in format, which shares the ID) instead of installing to
  # the install location.
  pkgbuild --analyze --root "$2" "$work/$1.plist" > /dev/null
  i=0
  while plutil -extract "$i" xml1 -o /dev/null "$work/$1.plist" 2> /dev/null; do
    plutil -replace "$i.BundleIsRelocatable" -bool NO "$work/$1.plist"
    i=$((i + 1))
  done
  pkgbuild --root "$2" --component-plist "$work/$1.plist" \
    --identifier "plugins.alessio.atlas.$1" --version "$version" \
    --install-location "$3" --scripts "$work/scripts-$1" \
    "$work/packages/Atlas-$1.pkg"
}

# plugin <id> <bundle> <install location>
plugin() {
  mkdir "$work/$1"
  # Extended attributes (provenance, quarantine) would otherwise go into the
  # package as ._ files. The signature does not use them.
  ditto --noextattr --noqtn "$2" "$work/$1/$(basename "$2")"
  codesign --verify --deep --strict "$work/$1/$(basename "$2")"
  component "$1" "$work/$1" "$3"
}

plugin vst3 "$artefacts/VST3/Atlas.vst3" /Library/Audio/Plug-Ins/VST3
plugin au "$artefacts/AU/Atlas.component" /Library/Audio/Plug-Ins/Components
# The library goes to a folder of its own first: a package installs only to
# fixed places, and the postinstall script copies it into the user's
# Documents and removes it.
cmake -DDESTINATION="$work/library" -P "$root/installer/stage_library.cmake"
xattr -cr "$work/library"
component library "$work/library" "/Library/Application Support/Blind Producers/Atlas Library"

mkdir -p "$out"
sed "s/@VERSION@/$version/g" "$here/distribution.xml" > "$work/distribution.xml"
productbuild --distribution "$work/distribution.xml" --package-path "$work/packages" \
  "$out/Atlas-$version-macOS.pkg"

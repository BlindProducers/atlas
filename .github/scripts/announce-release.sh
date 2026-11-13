#!/usr/bin/env bash
# Tells a Discord channel about an Atlas version. The publish job of
# .github/workflows/release.yml runs it once per version: ANNOUNCE is "new"
# when the version's release was just created, "promoted" when a pre-release
# became the release. TAG is the release's tag (v2.0.0.3), TITLE its title,
# ATLAS_OFFICIAL true for a release and false for a pre-release, and dist/
# holds the files the release got.
#
# The message goes to the webhook in DISCORD_RELEASES_WEBHOOK, a repository
# secret holding the address Discord gives for one channel (Server Settings >
# Integrations > Webhooks). Without it the message is printed instead, which
# is also how to read it by hand, from the repository root:
#
#   TAG=v2.0.0.3 TITLE="Atlas 2.0.0.3" ANNOUNCE=new ATLAS_OFFICIAL=false .github/scripts/announce-release.sh
#
# The links lead to the release's page on the Atlas website, which lists what
# GitHub publishes, and to the addresses there that redirect to an installer.
set -euo pipefail

: "${TAG:?}" "${TITLE:?}" "${ANNOUNCE:?}"
page="https://atlas.blindproducers.org/download/${TAG#v}"
# Discord refuses a message of more than 2000 characters.
limit=1900

if [ "$ANNOUNCE" = promoted ]; then
  headline="${TITLE} is now the official release"
elif [ "${ATLAS_OFFICIAL:-false}" = true ]; then
  headline="${TITLE} is out"
else
  headline="${TITLE} is out (pre-release, for testing)"
fi
message="**${headline}**
Download page: ${page}/"
# An installer link only for an installer this release has.
if compgen -G "dist/*-macOS.pkg" > /dev/null; then
  message+=$'\n'"macOS installer: ${page}/macos/"
fi
if compgen -G "dist/*-Windows.exe" > /dev/null; then
  message+=$'\n'"Windows installer: ${page}/windows/"
fi

# What changed, as far as it fits: whole lines of the release's notes.
notes="docs/releases/${TAG}.md"
if [ -f "$notes" ]; then
  message+=$'\n'
  while IFS= read -r line || [ -n "$line" ]; do
    if [ $(( ${#message} + ${#line} + 1 )) -gt "$limit" ]; then
      message+=$'\n'"The rest is on the download page."
      break
    fi
    message+=$'\n'"$line"
  done < "$notes"
fi

if [ -z "${DISCORD_RELEASES_WEBHOOK:-}" ]; then
  printf '%s\n' "$message"
  echo "DISCORD_RELEASES_WEBHOOK is not set: nothing was posted." >&2
  exit 0
fi

# flags 4 leaves out the link previews, and nothing in the notes pings anyone.
jq -n --arg content "$message" '{content: $content, flags: 4, allowed_mentions: {parse: []}}' |
  curl --fail-with-body --silent --show-error --max-time 30 \
    -H "Content-Type: application/json" --data-binary @- "$DISCORD_RELEASES_WEBHOOK"

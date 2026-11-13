#!/usr/bin/env bash
# Tells a Discord channel about an Atlas version. The publish job of
# .github/workflows/release.yml runs it once per version, and the workflow's
# "announce" input runs it by hand for any release. TAG is the release's tag
# (v2.0.0.3). ANNOUNCE is "promoted" when a pre-release has just become the
# release; anything else announces the version as out. The title, whether it
# is a pre-release and which installers it has come from the release on
# GitHub, read with gh.
#
# The message goes to the webhook in DISCORD_RELEASES_WEBHOOK, a repository
# secret holding the address Discord gives for one channel (Server Settings >
# Integrations > Webhooks). Without it the message is printed instead, which
# is also how to read it by hand, from the repository root:
#
#   TAG=v2.0.0.3 .github/scripts/announce-release.sh
#
# The installer links are addresses on the Atlas website that redirect to the
# release's files.
set -euo pipefail

: "${TAG:?}"
page="https://atlas.blindproducers.org/download/${TAG#v}"
# Discord refuses a message of more than 2000 characters.
limit=1900

release=$(gh release view "$TAG" --json name,isPrerelease,assets)
title=$(jq -r .name <<< "$release")
if [ "${ANNOUNCE:-new}" = promoted ]; then
  headline="${title} is now the official release"
elif [ "$(jq -r .isPrerelease <<< "$release")" = true ]; then
  headline="${title} is out (pre-release, for testing)"
else
  headline="${title} is out"
fi
message="**${headline}**"

# A link for each installer the release has, told by the end of its file name.
has_file() {
  jq -e --arg end "$1" 'any(.assets[]; .name | endswith($end))' <<< "$release" > /dev/null
}
if has_file "-macOS.pkg"; then
  message+=$'\n'"[Download macOS installer](${page}/macos/)"
fi
if has_file "-Windows.exe"; then
  message+=$'\n'"[Download Windows installer](${page}/windows/)"
fi

# What changed, as far as it fits: whole lines of the release's notes.
notes="docs/releases/${TAG}.md"
if [ -f "$notes" ]; then
  message+=$'\n'
  while IFS= read -r line || [ -n "$line" ]; do
    if [ $(( ${#message} + ${#line} + 1 )) -gt "$limit" ]; then
      message+=$'\n'"[The rest is on the download page](${page}/)"
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
echo "Posted ${TAG} to Discord."

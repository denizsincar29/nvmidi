#!/usr/bin/env bash
#
# nvgt_pin.sh - fetch the nvgt build a host job should test against.
#
# The nvgt that a job downloads and the nvgt whose headers the plugin was
# compiled against have to be the same build. Nothing in the repo enforces
# that today: src/nvgt_plugin.h is a vendored copy and nvgt_plugin.cpp's
# version() check only compares the API number. So this script does two
# things at once: it downloads the rolling prerelease, and it refuses if the
# vendored header is not byte-identical to the header at the tag it just
# fetched. A drift turns into a red job instead of a plugin that loads in a
# CI runner and nowhere else.
#
# Usage:
#   scripts/nvgt_pin.sh fetch   <dir>   download and unpack nvgt into <dir>
#   scripts/nvgt_pin.sh verify  <dir>   check <dir>'s header against src/
#   scripts/nvgt_pin.sh version <dir>   print the version this dir carries
#
# The tarball is ~190 MB. It lands in $TMPDIR (workspace/scratch/) and is
# deleted as soon as it is unpacked - keep the unpacked tree, not the archive.

set -euo pipefail

REPO=samtupy/nvgt
TAG=latest
BASE="https://github.com/$REPO/releases/download/$TAG"

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
header="$here/src/nvgt_plugin.h"

usage() {
	echo "usage: $0 fetch|verify|version <dir>" >&2
	exit 2
}

[ $# -eq 2 ] || usage
action=$1
dir=$2
mkdir -p "$dir"
dir=$(CDPATH= cd -- "$dir" && pwd)

tarball_name() {
	# The asset name carries the version (nvgt_0.90.0_dev.tar.gz), so it can
	# only be learned from the release itself. That is what the API is for;
	# the download URL is not a redirect to the asset and will not reveal it.
	local owner repo
	owner=${REPO%%/*}
	repo=${REPO##*/}
	curl -fsSL "https://api.github.com/repos/$owner/$repo/releases/tags/$TAG" |
		sed -n 's/.*"browser_download_url": *"[^"]*\/\([^"\/]*\.tar\.gz\)".*/\1/p' |
		head -1
}

do_fetch() {
	local name archive
	name=$(tarball_name)
	if [ -z "$name" ]; then
		echo "nvgt: no .tar.gz asset on $REPO@$TAG" >&2
		exit 1
	fi
	archive="$dir/$name"
	echo "nvgt: asset $name"
	curl -fL --retry 3 -o "$archive" "$BASE/$name"
	tar -xzf "$archive" -C "$dir"
	rm -f "$archive"
	# The tarball unpacks into a version-named directory; flatten it so the
	# rest of the job does not have to know the version number.
	local inner
	inner=$(find "$dir" -maxdepth 1 -type d -name 'nvgt*' | head -1)
	if [ -n "$inner" ] && [ "$inner" != "$dir" ]; then
		mv "$inner"/* "$dir"/ 2>/dev/null || true
		rmdir "$inner" 2>/dev/null || true
	fi
	echo "nvgt: unpacked into $dir"
}

do_version() {
	# "nvgt 0.90.0" out of the tarball's own name, if nothing better exists.
	find "$dir" -maxdepth 1 -name 'nvgt*' -type f | sed -n 's/.*nvgt[_-]\([0-9][^.]*\.[^.]*\.[^.]*\).*/\1/p' | head -1
}

do_verify() {
	local tmp want got
	tmp=$(mktemp "${TMPDIR:-/tmp}/nvgt_plugin.XXXXXX")
	trap 'rm -f "$tmp"' RETURN
	# Hash the file, not a captured string: $( ) strips the trailing newline,
	# which would make every comparison fail on that one byte alone.
	curl -fsSL "https://raw.githubusercontent.com/$REPO/$TAG/src/nvgt_plugin.h" -o "$tmp"
	want=$(sha256sum "$tmp" | cut -d' ' -f1)
	got=$(sha256sum "$header" | cut -d' ' -f1)
	if [ "$want" != "$got" ]; then
		echo "nvgt: vendored header does not match $REPO@$TAG" >&2
		echo "  vendored $header -> $got" >&2
		echo "  upstream $REPO@$TAG -> $want" >&2
		exit 1
	fi
	echo "nvgt: header at $REPO@$TAG matches the vendored copy"
}

case "$action" in
	fetch)   do_fetch ;;
	verify)  do_verify ;;
	version) do_version ;;
	*)       usage ;;
esac

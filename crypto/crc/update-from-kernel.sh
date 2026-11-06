#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copy the CRC32C code of the kernel's lib/crc/ into crypto/crc/, unchanged.
#
# usage: crypto/crc/update-from-kernel.sh [<linux source tree> | <git tag>]
#
# The default is the source tree in /usr/src/linux. For a git tag like v7.4,
# only the needed files are fetched from the cgit web interface at $LINUX_GIT,
# by default the stable tree on git.kernel.org.
#
# The copied assembly is built with the stand-in headers in crypto/crc/include/.
# The code that picks an implementation at run time is crypto/crc32c.c, written
# after the kernel's glue in lib/crc/<arch>/crc32.h. The file SOURCE records
# the kernel version and the sha256 of the copied files and of that glue.
#
# Exit status 2: the glue has changed, so crypto/crc32c.c may need the same
# change, or a copied file includes something that is neither copied nor has a
# stand-in.

set -euo pipefail

# Copied unchanged, the same path below crypto/crc/ as below lib/crc/
files=(
	arm64/crc32-core.S
	x86/crc32c-3way.S
	x86/crc-pclmul-template.S
	x86/crc32-pclmul.S
	x86/crc-pclmul-consts.h
)

# Not copied, crypto/crc32c.c follows them
glue=(
	arm64/crc32.h
	x86/crc32.h
	x86/crc-pclmul-template.h
)

die() {
	echo "ERROR: $*" >&2
	exit 1
}

sha256() {
	sha256sum "$1" | cut -d ' ' -f 1
}

usage() {
	echo "usage: $0 [<linux source tree> | <git tag>]"
	echo
	echo "Copy the CRC32C code of the kernel's lib/crc/ from a source tree, by"
	echo "default /usr/src/linux, or for a tag like v7.4 from git.kernel.org."
}

case "${1:-}" in
-h|--help)
	usage
	exit 0
	;;
esac
if [ "$#" -gt 1 ]; then
	usage >&2
	exit 1
fi

dest=$(cd "$(dirname "$0")" && pwd)
src=${1:-/usr/src/linux}

ref=
if [ ! -d "$src" ]; then
	case "$src" in
	*/*) die "$src: no such directory" ;;
	esac
	ref=$src
	url=${LINUX_GIT:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}
	tmp=$(mktemp -d)
	trap 'rm -rf -- "$tmp"' EXIT
	src=$tmp/linux
	echo "Fetching the files of $ref from $url"
	for f in Makefile "${files[@]/#/lib/crc/}" "${glue[@]/#/lib/crc/}"; do
		mkdir -p "$src/$(dirname "$f")"
		curl -fsS -o "$src/$f" "$url/plain/$f?h=$ref" ||
			die "cannot fetch $f of $ref"
	done
elif [ "$(git -C "$src" rev-parse --show-toplevel 2>/dev/null)" = "$(cd "$src" && pwd -P)" ]; then
	ref=$(git -C "$src" describe --tags --exact-match HEAD 2>/dev/null ||
	      git -C "$src" rev-parse --verify -q HEAD || true)
fi

[ -f "$src/Makefile" ] || die "$src is not a kernel source tree"
compgen -G "$src/lib/crc/*/*.S" >/dev/null ||
	die "no assembly in $src/lib/crc/, it needs the full kernel source, not only the headers"
for f in "${files[@]}" "${glue[@]}"; do
	[ -f "$src/lib/crc/$f" ] || die "$src/lib/crc/$f not found"
done

version=$(awk '
	/^VERSION =/ { v = $3 }
	/^PATCHLEVEL =/ { p = $3 }
	/^SUBLEVEL =/ { s = $3 }
	/^EXTRAVERSION =/ { e = $3 }
	END { if (v != "") printf("%s.%s.%s%s", v, p, s, e) }' "$src/Makefile")
[ -n "$version" ] || die "no kernel version in $src/Makefile"

# The glue as recorded by the previous update
declare -A old
if [ -f "$dest/SOURCE" ]; then
	while read -r kind sum path; do
		if [ "$kind" = glue ]; then
			old[$path]=$sum
		fi
	done < "$dest/SOURCE"
fi

for f in "${files[@]}"; do
	mkdir -p "$dest/$(dirname "$f")"
	cp -- "$src/lib/crc/$f" "$dest/$f"
done

{
	echo "# Written by update-from-kernel.sh, do not edit."
	echo "# copied: files of the kernel's lib/crc/, unchanged in crypto/crc/"
	echo "# glue: files of the kernel's lib/crc/ that crypto/crc32c.c follows"
	echo "linux $version${ref:+ $ref}"
	for f in "${files[@]}"; do
		echo "copied $(sha256 "$src/lib/crc/$f") $f"
	done
	for f in "${glue[@]}"; do
		echo "glue $(sha256 "$src/lib/crc/$f") $f"
	done
} > "$dest/SOURCE"

echo "Copied lib/crc/ of linux $version${ref:+ ($ref)}"

status=0
for f in "${glue[@]}"; do
	if [ -n "${old[$f]:-}" ] && [ "${old[$f]}" != "$(sha256 "$src/lib/crc/$f")" ]; then
		echo "CHANGED: lib/crc/$f, check whether crypto/crc32c.c needs the same change"
		status=2
	fi
done

# Every included file must be copied too or have a stand-in
for f in "${files[@]}"; do
	case "$f" in *.S) ;; *) continue ;; esac
	while read -r inc; do
		[ -f "$dest/include/$inc" ] && continue
		echo "MISSING: $f includes <$inc>, add a stand-in to crypto/crc/include/"
		status=2
	done < <(sed -n 's/^#include[[:space:]]*<\([^>]*\)>.*/\1/p' "$dest/$f")
	while read -r inc; do
		[ -f "$dest/$(dirname "$f")/$inc" ] && continue
		echo "MISSING: $f includes \"$inc\", add it to the copied files"
		status=2
	done < <(sed -n 's/^#include[[:space:]]*"\([^"]*\)".*/\1/p' "$dest/$f")
done

git -C "$dest" status --short -- . || true

exit "$status"

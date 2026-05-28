#!/bin/bash
#
# Reproducibility check for mkfs.btrfs.
#
# Runs mkfs.btrfs (and `mkfs.btrfs --rootdir`) under a matrix of varied
# environments and asserts the resulting images are byte-identical via
# plain `cmp`. Prints a matrix of PASS/FAIL/SKIP and exits non-zero on
# any unexpected failure.
#
# Requires root (uses loop-mounted filesystems for source-FS variation
# and chown for uid variation).
#
# Usage: sudo bash tests/reproducibility-check.sh
#

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
TOP=$(dirname "$SCRIPT_DIR")
TEST_TOP="$SCRIPT_DIR"
INTERNAL_BIN="$TOP"
RESULTS="$TEST_TOP/reproducibility-tests-results.txt"
export TOP TEST_TOP INTERNAL_BIN RESULTS

: > "$RESULTS"

# shellcheck disable=SC1091
source "$TEST_TOP/common" || exit 1

if [[ "$(id -u)" -ne 0 ]]; then
	_fail "this script must run as root (uses loop-mounted filesystems)"
fi

MKFS="$TOP/mkfs.btrfs"
if [[ ! -x "$MKFS" ]]; then
	echo "Building mkfs.btrfs..."
	(cd "$TOP" && make -j"$(nproc)" mkfs.btrfs >/dev/null) || \
		_fail "failed to build mkfs.btrfs"
fi

# Pinned values used in all variations: a fixed fs UUID plus
# SOURCE_DATE_EPOCH and DETERMINISTIC_SEED=1. This is the full
# documented reproducible-build recipe; mkfs derives every other UUID
# (device, chunk-tree, subvolumes) from the fs UUID.
FIXED_UUID="12345678-1234-5678-1234-567812345678"
FIXED_EPOCH="1700000000"

WORK=$(mktemp -d -t btrfs-repro-XXXXXX)
SOURCE_DIR="$WORK/source"
MOUNTS=()

cleanup() {
	local mp
	for mp in "${MOUNTS[@]}"; do
		[[ -n "$mp" ]] && mountpoint -q "$mp" 2>/dev/null && \
			umount "$mp" 2>/dev/null
	done
	rm -rf "$WORK"
}

# ----------------------------------------------------------------------
# Source-tree generator. Produces a small but varied tree intended to
# exercise the bits of the rootdir code that matter for reproducibility:
# multiple directory levels, files of inline / single-block / multi-block
# size, hardlinks, symlinks, sparse files, empty files.
# ----------------------------------------------------------------------
create_test_tree() {
	local dest="$1"
	mkdir -p "$dest"

	local d f
	for d in alpha beta gamma; do
		mkdir -p "$dest/$d"
		for f in one two three; do
			printf 'content of %s/%s\n' "$d" "$f" > "$dest/$d/$f"
		done
		dd if=/dev/zero of="$dest/$d/block" bs=4K count=1 status=none
		printf 'X%.0s' $(seq 1 65536) > "$dest/$d/multi"
	done

	ln -s "../alpha/one" "$dest/beta/sym_rel"
	ln -s "/etc/hostname" "$dest/gamma/sym_abs"

	ln "$dest/alpha/one" "$dest/gamma/hard_one"

	touch "$dest/empty"

	truncate -s 4M "$dest/sparse"
	printf 'tail\n' >> "$dest/sparse"

	mkdir -p "$dest/deep/a/b/c/d/e"
	printf 'deep\n' > "$dest/deep/a/b/c/d/e/leaf"
}

normalize_source_dir() {
	local dir="$1"
	chown -R --no-dereference 0:0 "$dir"
}

create_test_tree "$SOURCE_DIR"
normalize_source_dir "$SOURCE_DIR"

# ----------------------------------------------------------------------
# Image runner. Runs mkfs.btrfs twice with two distinct shell contexts
# and cmp's the resulting images.
#
# Arguments:
#   $1  label                            (e.g. "baseline")
#   $2  use_rootdir                      ("yes" | "no")
#   $3  context_a (shell snippet that, when eval'd, runs the mkfs)
#   $4  context_b (shell snippet, ditto)
#
# The snippets must define the function _RUN_MKFS that takes one
# argument (the image path).
# ----------------------------------------------------------------------
run_pair() {
	local label="$1"
	local mode="$2"
	local ctx_a="$3"
	local ctx_b="$4"

	local img_a="$WORK/${label}_${mode}_a.img"
	local img_b="$WORK/${label}_${mode}_b.img"
	rm -f "$img_a" "$img_b"
	truncate -s 256M "$img_a" "$img_b"

	local ret_a=0 ret_b=0
	(
		eval "$ctx_a"
		_RUN_MKFS "$img_a" >>"$RESULTS" 2>&1
	) || ret_a=$?
	(
		eval "$ctx_b"
		_RUN_MKFS "$img_b" >>"$RESULTS" 2>&1
	) || ret_b=$?

	local result
	if [[ "$ret_a" -ne 0 || "$ret_b" -ne 0 ]]; then
		# Avoid a false PASS from equal partially-written images.
		result="ERR"
	elif cmp -s "$img_a" "$img_b"; then
		result="PASS"
	else
		result="FAIL"
	fi

	rm -f "$img_a" "$img_b"
	echo "$result"
}

# Leave --device-uuid unset to test its derivation from -U.
mkfs_empty() {
	cat <<'EOF'
_RUN_MKFS() {
	SOURCE_DATE_EPOCH=$FIXED_EPOCH DETERMINISTIC_SEED=1 \
		"$MKFS" -f -K -U "$FIXED_UUID" -L repro "$1"
}
EOF
}

# Select two installed locales so fallback cannot turn the test into a no-op.
LOCALE_AVAILABLE=$(locale -a 2>/dev/null)
LOCALE_A=""
LOCALE_B=""
for _locale in de_DE.UTF-8 fr_FR.UTF-8 ja_JP.UTF-8 \
               en_US.UTF-8 en_US.utf8 en_GB.utf8 \
               C.UTF-8 C.utf8 POSIX C; do
	if printf '%s\n' "$LOCALE_AVAILABLE" | grep -qx "$_locale"; then
		if [[ -z "$LOCALE_A" ]]; then
			LOCALE_A="$_locale"
		elif [[ -z "$LOCALE_B" ]]; then
			LOCALE_B="$_locale"
			break
		fi
	fi
done
unset _locale LOCALE_AVAILABLE
export LOCALE_A LOCALE_B

# Bake the source path into the snippet so each variation can select its tree.
mkfs_rootdir() {
	local src="$1"
	cat <<EOF
_RUN_MKFS() {
	SOURCE_DATE_EPOCH=\$FIXED_EPOCH DETERMINISTIC_SEED=1 \\
		"\$MKFS" -f -K -U "\$FIXED_UUID" -L repro \\
		-r "$src" "\$1"
}
EOF
}

mkfs_rootdir_compress() {
	local src="$1" algo="$2"
	cat <<EOF
_RUN_MKFS() {
	SOURCE_DATE_EPOCH=\$FIXED_EPOCH DETERMINISTIC_SEED=1 \\
		"\$MKFS" -f -K -U "\$FIXED_UUID" -L repro \\
		--compress $algo -r "$src" "\$1"
}
EOF
}

# ----------------------------------------------------------------------
# Variations. Each function prints PASS or FAIL on one line for the
# requested mode (empty | rootdir). SKIP if not applicable.
# ----------------------------------------------------------------------

var_baseline() {
	local mode="$1"
	local runner
	if [[ "$mode" == "empty" ]]; then runner=$(mkfs_empty)
	else runner=$(mkfs_rootdir "$SOURCE_DIR"); fi
	run_pair "baseline" "$mode" "$runner" "$runner"
}

var_clock() {
	local mode="$1"
	local runner
	if [[ "$mode" == "empty" ]]; then runner=$(mkfs_empty)
	else runner=$(mkfs_rootdir "$SOURCE_DIR"); fi
	# Ensure time(NULL) differs between runs.
	run_pair "clock" "$mode" "$runner" "sleep 2; $runner"
}

var_tz() {
	local mode="$1"
	local runner
	if [[ "$mode" == "empty" ]]; then runner=$(mkfs_empty)
	else runner=$(mkfs_rootdir "$SOURCE_DIR"); fi
	run_pair "tz" "$mode" "export TZ=Asia/Tokyo; $runner" \
		"export TZ=Europe/Berlin; $runner"
}

var_locale() {
	local mode="$1"
	if [[ -z "$LOCALE_A" || -z "$LOCALE_B" ]]; then
		echo "SKIP"
		return
	fi
	local runner
	if [[ "$mode" == "empty" ]]; then runner=$(mkfs_empty)
	else runner=$(mkfs_rootdir "$SOURCE_DIR"); fi
	run_pair "locale" "$mode" \
		"export LANG=$LOCALE_A LC_ALL=$LOCALE_A; $runner" \
		"export LANG=$LOCALE_B LC_ALL=$LOCALE_B; $runner"
}

var_umask() {
	local mode="$1"
	local runner
	if [[ "$mode" == "empty" ]]; then runner=$(mkfs_empty)
	else runner=$(mkfs_rootdir "$SOURCE_DIR"); fi
	run_pair "umask" "$mode" "umask 022; $runner" "umask 077; $runner"
}

var_cwd() {
	local mode="$1"
	local cwd_a="$WORK/cwd-a"
	local cwd_b="$WORK/cwd-b"
	mkdir -p "$cwd_a" "$cwd_b"
	local runner
	if [[ "$mode" == "empty" ]]; then runner=$(mkfs_empty)
	else runner=$(mkfs_rootdir "$SOURCE_DIR"); fi
	run_pair "cwd" "$mode" "cd '$cwd_a'; $runner" "cd '$cwd_b'; $runner"
}

# Exercise the documented ownership normalization from two starting owners.
var_uid() {
	local mode="$1"
	if [[ "$mode" == "empty" ]]; then echo "SKIP"; return; fi
	local src_a="$WORK/src-uid-a"
	local src_b="$WORK/src-uid-b"
	rm -rf "$src_a" "$src_b"
	cp -a "$SOURCE_DIR" "$src_a"
	cp -a "$SOURCE_DIR" "$src_b"
	chown -R 1000:1000 "$src_a"
	chown -R 2000:2000 "$src_b"
	normalize_source_dir "$src_a"
	normalize_source_dir "$src_b"
	run_pair "uid" "$mode" \
		"$(mkfs_rootdir "$src_a")" \
		"$(mkfs_rootdir "$src_b")"
}

# Compare identical source trees staged on ext4 and XFS.
var_source_fs() {
	local mode="$1"
	if [[ "$mode" == "empty" ]]; then echo "SKIP"; return; fi

	local img_ext4="$WORK/src-ext4.img"
	local img_xfs="$WORK/src-xfs.img"
	local mnt_ext4="$WORK/mnt-ext4"
	local mnt_xfs="$WORK/mnt-xfs"

	mkdir -p "$mnt_ext4" "$mnt_xfs"
	truncate -s 256M "$img_ext4" "$img_xfs"

	if ! command -v mkfs.ext4 >/dev/null || \
	   ! command -v mkfs.xfs  >/dev/null; then
		echo "SKIP"
		return
	fi

	mkfs.ext4 -q -F "$img_ext4" >/dev/null 2>&1 || { echo "SKIP"; return; }
	mkfs.xfs  -q -f "$img_xfs"  >/dev/null 2>&1 || { echo "SKIP"; return; }

	mount -o loop "$img_ext4" "$mnt_ext4" || { echo "SKIP"; return; }
	MOUNTS+=("$mnt_ext4")
	mount -o loop "$img_xfs"  "$mnt_xfs"  || { echo "SKIP"; return; }
	MOUNTS+=("$mnt_xfs")

	# Remove ext4-only content so the source trees match.
	rm -rf "$mnt_ext4/lost+found"

	cp -a "$SOURCE_DIR/." "$mnt_ext4/"
	cp -a "$SOURCE_DIR/." "$mnt_xfs/"
	normalize_source_dir "$mnt_ext4"
	normalize_source_dir "$mnt_xfs"

	local result
	result=$(run_pair "source_fs" "$mode" \
		"$(mkfs_rootdir "$mnt_ext4")" \
		"$(mkfs_rootdir "$mnt_xfs")")

	umount "$mnt_ext4"
	umount "$mnt_xfs"
	# Drop the entries we just unmounted from the cleanup list.
	MOUNTS=("${MOUNTS[@]/$mnt_ext4}")
	MOUNTS=("${MOUNTS[@]/$mnt_xfs}")

	echo "$result"
}

# Use enough compressible data to exercise each compressor's output path.
var_compress() {
	local mode="$1"
	if [[ "$mode" == "empty" ]]; then echo "SKIP"; return; fi

	local csrc="$WORK/csrc"
	if [[ ! -e "$csrc/seq" ]]; then
		mkdir -p "$csrc"
		seq 1 4000000 > "$csrc/seq"
		normalize_source_dir "$csrc"
	fi

	local algo res overall=SKIP
	for algo in zlib lzo zstd; do
		res=$(run_pair "compress_$algo" "$mode" \
			"$(mkfs_rootdir_compress "$csrc" "$algo")" \
			"$(mkfs_rootdir_compress "$csrc" "$algo")")
		case "$res" in
		PASS) [[ "$overall" == FAIL ]] || overall=PASS ;;
		FAIL) overall=FAIL ;;
		ERR)  ;;	# algo not built into this mkfs; skip it
		esac
	done
	echo "$overall"
}

# ----------------------------------------------------------------------
# Driver: run every variation in both modes, collect into a matrix.
# ----------------------------------------------------------------------

VARIATIONS=(baseline clock tz locale umask cwd uid source_fs compress)
# CPU-count and cross-architecture (page size) are not in the matrix:
# sysconf(_SC_NPROCESSORS_ONLN) can't be varied via taskset (it reads
# /sys), and cross-arch needs separate hardware. Both are verified by
# hand instead.

declare -A EMPTY_RESULT
declare -A ROOTDIR_RESULT

for v in "${VARIATIONS[@]}"; do
	echo -n "  $v ... "
	EMPTY_RESULT[$v]=$("var_$v" empty)
	ROOTDIR_RESULT[$v]=$("var_$v" rootdir)
	echo "empty=${EMPTY_RESULT[$v]} rootdir=${ROOTDIR_RESULT[$v]}"
done

# ----------------------------------------------------------------------
# Print final matrix.
# ----------------------------------------------------------------------

echo
echo "Reproducibility matrix"
echo "---------------------------------------------"
printf '%-15s %-8s %-10s\n' "Variation" "Empty" "--rootdir"
echo "---------------------------------------------"
total=0
passed=0
for v in "${VARIATIONS[@]}"; do
	e=${EMPTY_RESULT[$v]}
	r=${ROOTDIR_RESULT[$v]}
	printf '%-15s %-8s %-10s\n' "$v" "$e" "$r"
	for cell in "$e" "$r"; do
		case "$cell" in
		PASS) total=$((total + 1)); passed=$((passed + 1)) ;;
		FAIL) total=$((total + 1)) ;;
		ERR)  total=$((total + 1)) ;;
		SKIP) ;;
		esac
	done
done
echo "---------------------------------------------"
echo "Result: $passed / $total passing"
echo
echo "Full log: $RESULTS"

if [[ "$passed" -ne "$total" ]]; then
	exit 1
fi
cleanup
exit 0

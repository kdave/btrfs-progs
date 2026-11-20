#!/bin/bash
#
# End-to-end benchmark of btrfs-progs commands, see --help.

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
TOP=$(dirname "$SCRIPT_DIR")

runs=5
data_mib=1024
source_dir=
send_opts=
compress=
target_dev=
work_base=/dev/shm
cpus=

usage() {
	cat <<EOF
Usage: [BTRFS=PATH] [MKFS_BTRFS=PATH] $0 [options]

Time btrfs-progs commands end to end on the same data, in phases:

  dump     btrfs receive --dump of a send stream of the data
  check    btrfs check --check-data-csum of an image of the data
  receive  btrfs receive of the stream onto a new filesystem, with the sync
  mkfs     mkfs.btrfs --rootdir of the data to an image file

The executables are given by the BTRFS and MKFS_BTRFS environment variables.
The default for BTRFS is the one in PATH, for MKFS_BTRFS the one next to BTRFS.

The data is generated unless -d is given. The work files are in the -w
directory, by default /dev/shm, which needs about four times the data size.
The results are checked once, with fssum and the kernel. It prints each run
and the medians in MiB/s: of the stream for dump and receive, of the data for
check and mkfs.

Options:
  -n RUNS     number of rounds (default: $runs)
  -S MIB      size of the generated data in MiB (default: $data_mib)
  -d DIR      use the files of DIR instead of generating data
  -o OPTS     options for btrfs send, e.g. "--proto 2 --compressed-data"
  -c ALG      mount the source filesystem with compress=ALG
  -t DEV      device for the filesystems that are mounted, required,
              ALL DATA ON IT IS LOST
  -w DIR      create the work directory in DIR (default: $work_base)
  -C CPUS     run the timed commands pinned to CPUS (taskset -c)
  -h, --help  this help

Needs root.
EOF
}

die() {
	echo "ERROR: $*" >&2
	exit 1
}

[[ "$1" == --help ]] && { usage; exit 0; }
while getopts "n:S:d:o:c:t:w:C:h" opt; do
	case "$opt" in
	n) runs=$OPTARG ;;
	S) data_mib=$OPTARG ;;
	d) source_dir=$OPTARG ;;
	o) send_opts=$OPTARG ;;
	c) compress=$OPTARG ;;
	t) target_dev=$OPTARG ;;
	w) work_base=$OPTARG ;;
	C) cpus=$OPTARG ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 1 ;;
	esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || die "no arguments, set BTRFS to the btrfs executable to time"

[[ "$runs" =~ ^[1-9][0-9]*$ ]] || die "-n needs a positive number: $runs"
if ! [[ "$data_mib" =~ ^[0-9]+$ ]] || (( data_mib < 16 )); then
	die "-S needs a size of at least 16 MiB: $data_mib"
fi
[[ -z "$source_dir" || -d "$source_dir" ]] || die "not a directory: $source_dir"

(( EUID == 0 )) || die "needs root"
[[ -n "$target_dev" ]] || die "give -t DEV for the filesystems that are mounted"

if [[ -z "$BTRFS" ]]; then
	BTRFS=$(command -v btrfs) || die "no btrfs in PATH, set BTRFS"
fi
if [[ -z "$MKFS_BTRFS" ]]; then
	MKFS_BTRFS=$(dirname "$BTRFS")/mkfs.btrfs
fi
if [[ ! -f "$BTRFS" || ! -x "$BTRFS" ]]; then
	die "BTRFS is not an executable: $BTRFS"
fi
if [[ ! -f "$MKFS_BTRFS" || ! -x "$MKFS_BTRFS" ]]; then
	die "MKFS_BTRFS is not an executable: $MKFS_BTRFS"
fi
btrfs=$(readlink -f "$BTRFS")
mkfs=$(readlink -f "$MKFS_BTRFS")

[[ -b "$target_dev" ]] || die "not a block device: $target_dev"
findmnt -rno TARGET -S "$target_dev" >/dev/null && die "$target_dev is mounted"
holders=(/sys/class/block/"$(basename "$(readlink -f "$target_dev")")"/holders/*)
[[ -e "${holders[0]}" ]] && die "$target_dev is in use by ${holders[*]##*/}"

pin=()
if [[ -n "$cpus" ]]; then
	command -v taskset >/dev/null || die "-C needs taskset (util-linux)"
	pin=(taskset -c "$cpus")
fi

fssum=$TOP/fssum
[[ -x "$fssum" ]] || fssum=
[[ -n "$fssum" ]] || echo "No $TOP/fssum (make fssum): the data will not be compared"

work=
src=
dst=
cleanup() {
	local m

	for m in "$src" "$dst"; do
		[[ -n "$m" ]] && mountpoint -q "$m" && umount "$m"
	done
	[[ -n "$work" ]] && rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

work=$(mktemp -d "$work_base/benchmarks.XXXXXX") || die "cannot create a work directory in $work_base"
log=$work/log
raw=$work/raw
: > "$raw"
src=$work/src
dst=$work/dst
mkdir "$src" "$dst"

# Set t_real to the wall-clock time of the command, in seconds
TIMEFORMAT='%3R'
timed() {
	local rc

	{ time "$@" >/dev/null 2>>"$log"; } 2> "$work/time"
	rc=$?
	read -r t_real < "$work/time"
	return "$rc"
}

run() {
	echo "### $*" >> "$log"
	"$@" >> "$log" 2>&1 || die "failed: $* (see the end of the log below)$(echo; tail -n 20 "$log")"
}

avail_mib() {
	df -Pm "$1" | awk 'NR == 2 { print $4 }'
}

# Half of the data in large files, random or base64 text that compresses,
# half in small files of 2 KiB to 1 MiB
generate() {
	local dir=$1
	local mib=$2
	local large=$((mib / 2))
	local text=$((large / 2))
	local random=$((large - text))
	local quarter=$(((mib - large) / 4))
	local i size

	mkdir "$dir/large"
	for ((i = 0; random > 0; i++)); do
		size=$((random < 64 ? random : 64))
		head -c "${size}M" /dev/urandom > "$dir/large/$i.bin"
		random=$((random - size))
	done
	for ((i = 0; text > 0; i++)); do
		size=$((text < 64 ? text : 64))
		base64 /dev/urandom | head -c "${size}M" > "$dir/large/$i.txt"
		text=$((text - size))
	done
	for size in 2 16 128 1024; do
		mkdir "$dir/$size"
		head -c "${quarter}M" /dev/urandom | split -a 6 -d -b "${size}K" - "$dir/$size/f"
	done
}

mount_dev() {
	run mount ${2:+-o "$2"} "$target_dev" "$1"
}

# The drives under block device $1, e.g. of a partition or dm device
drive() {
	lsblk -snPo TYPE,MODEL "$1" 2>/dev/null | awk -F'"' '
	$2 == "disk" {
		gsub(/\\x22/, "\"", $4)
		printf "%s%s", n++ ? "; " : "", $4
	}'
}

where() {
	local src fstype d=

	read -r src fstype < <(findmnt -no SOURCE,FSTYPE -T "$1")
	src=${src%%[*}
	[[ -b "$src" ]] && d=$(drive "$src")
	echo "$fstype${d:+ on $d}"
}

echo "btrfs: $btrfs"
echo "Version: $("$btrfs" --version | head -n 1)"
if [[ -n "$mkfs" ]]; then
	echo "mkfs.btrfs: $mkfs"
	echo "Version: $("$mkfs" --version 2>&1 | head -n 1)"
fi
echo "Kernel: $(uname -r)"
echo "CPU: $(lscpu 2>/dev/null | sed -n 's/^Model name: *//p' | head -n 1)"
echo "Work directory: $work ($(where "$work"))"
echo "Device: $target_dev (wiped before every use)"
echo "Model: $(drive "$target_dev")"

# About the data size each for the data, stream and the two images
[[ -n "$source_dir" ]] && data_mib=$(du -sm "$source_dir" | cut -f1)
need=$((data_mib * 3))
[[ -z "$source_dir" ]] && need=$((need + data_mib))
(( $(avail_mib "$work") > need )) ||
	die "$work_base has less than the $need MiB free that the data and images need"

manifest=
if [[ -n "$source_dir" ]]; then
	data=$(readlink -f "$source_dir")
	echo "Data: $data"
else
	data=$work/data
	mkdir "$data"
	echo "Data: generated"
	generate "$data" "$data_mib" 2>>"$log" || die "generating the data failed"
fi
data_bytes=$(du -sb "$data" | cut -f1)
echo "Data: $(awk -v b="$data_bytes" 'BEGIN { printf "%.1f", b / 1048576 }') MiB in $(find "$data" -type f | wc -l) files"
# Compared: data, mode, owner and mtime; not atime, ctime and xattrs,
# e.g. security labels that differ between tmpfs and btrfs
if [[ -n "$fssum" ]]; then
	run "$fssum" -A -T -f -w "$work/manifest" "$data"
	manifest=$work/manifest
fi

run "$mkfs" -q -f "$target_dev"
mount_dev "$src" ${compress:+compress=$compress}
run "$btrfs" subvolume create "$src/data"
run cp -a "$data/." "$src/data/"
run "$btrfs" subvolume snapshot -r "$src/data" "$src/snap"
run sync -f "$src"
stream=$work/stream
# shellcheck disable=SC2086
run "$btrfs" send $send_opts -f "$stream" "$src/snap"
run umount "$src"

stream_bytes=$(stat -c %s "$stream")
stream_mib=$(awk -v b="$stream_bytes" 'BEGIN { printf "%.1f", b / 1048576 }')
commands=$("$btrfs" receive --dump -f "$stream" 2>>"$log" | wc -l)
# receive does not fail on every error, e.g. not on a stream it cannot open
(( commands > 0 )) || die "the stream has no commands$(echo; tail -n 5 "$log")"
echo "Stream: $stream_mib MiB, about $commands commands"

check_img=$work/check.img
: > "$check_img"
run "$mkfs" -q -f --csum crc32c --rootdir "$data" "$check_img"

# The first run decides the format for all: two decimals below 1,000 MiB/s
decimals=
mibs_fmt='
function mibs(x,    s, d, r) {
	s = sprintf(dec ? "%.2f" : "%.0f", x)
	d = ""
	if (dec) {
		d = substr(s, length(s) - 2)
		s = substr(s, 1, length(s) - 3)
	}
	r = ""
	while (length(s) > 3) {
		r = "," substr(s, length(s) - 2) r
		s = substr(s, 1, length(s) - 3)
	}
	return s r d
}'

# Phase $1, round $2, $3 bytes in t_real seconds
record() {
	local m

	m=$(awk -v b="$3" -v t="$t_real" 'BEGIN { printf "%.3f", (t > 0 ? b / 1048576 / t : 0) }')
	echo "$1 $2 $m" >> "$raw"
	if [[ -z "$decimals" ]]; then
		decimals=$(awk -v m="$m" 'BEGIN { print (m < 999.995 ? 1 : 0) }')
	fi
	m=$(awk -v m="$m" -v dec="$decimals" "$mibs_fmt"' BEGIN { print mibs(m) }')
	printf '%-8s %5s %10s\n' "$1" "$2" "$m"
}

# The data is only received once it is written, so time the sync too
receive_once() {
	local r=$1
	local real

	run "$mkfs" -q -f "$target_dev"
	mount_dev "$dst"
	sync

	timed "${pin[@]}" "$btrfs" receive -f "$stream" "$dst" ||
		die "receive failed$(echo; tail -n 20 "$log")"
	real=$t_real
	timed sync -f "$dst" || die "sync failed"
	t_real=$(awk -v a="$real" -v b="$t_real" 'BEGIN { printf "%.3f", a + b }')
	record receive "$r" "$stream_bytes"

	# receive creates the subvolume under the name it was sent with
	if (( r == 1 )) && [[ -n "$manifest" ]]; then
		run "$fssum" -r "$manifest" "$dst/snap"
	fi
	run umount "$dst"
}

dump_once() {
	local r=$1

	timed "${pin[@]}" "$btrfs" receive --dump -f "$stream" ||
		die "receive --dump failed"
	record dump "$r" "$stream_bytes"
}

# Mounting the image makes the kernel verify the checksums mkfs wrote
mkfs_once() {
	local r=$1
	local img=$work/mkfs.img

	rm -f "$img"
	: > "$img"
	timed "${pin[@]}" "$mkfs" -q -f --csum crc32c --rootdir "$data" "$img" ||
		die "mkfs.btrfs --rootdir failed$(echo; tail -n 20 "$log")"
	record mkfs "$r" "$data_bytes"

	if (( r == 1 )); then
		run "$btrfs" check --check-data-csum "$img"
		if [[ -n "$manifest" ]]; then
			run "$mkfs" -q -f --csum crc32c --rootdir "$data" "$target_dev"
			mount_dev "$dst" ro
			run "$fssum" -r "$manifest" "$dst"
			run umount "$dst"
		fi
	fi
	rm -f "$img"
}

check_once() {
	local r=$1

	timed "${pin[@]}" "$btrfs" check --check-data-csum "$check_img" ||
		die "check --check-data-csum failed$(echo; tail -n 20 "$log")"
	record check "$r" "$data_bytes"
}

echo
printf '%-8s %5s %10s\n' phase round MiB/s
for ((r = 1; r <= runs; r++)); do
	dump_once "$r"
	check_once "$r"
	receive_once "$r"
	mkfs_once "$r"
done

echo
echo "Median of $runs rounds:"
echo
awk -v dec="$decimals" "$mibs_fmt"'
function median(p,    n, i, j, t, a) {
	n = cnt[p]
	for (i = 1; i <= n; i++)
		a[i] = val[p, i]
	for (i = 2; i <= n; i++)
		for (j = i; j > 1 && a[j - 1] > a[j]; j--) {
			t = a[j]; a[j] = a[j - 1]; a[j - 1] = t
		}
	return n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
}
{
	val[$1, ++cnt[$1]] = $3
}
END {
	np = split("dump check receive mkfs", phase, " ")
	w = length("MiB/s")
	for (p = 1; p <= np; p++) {
		v[p] = mibs(median(phase[p]))
		if (length(v[p]) > w)
			w = length(v[p])
	}
	dashes = sprintf("%*s", w + 1, "")
	gsub(/ /, "-", dashes)
	printf "| %-7s | %*s |\n", "Phase", w, "MiB/s"
	printf "|---------|%s:|\n", dashes
	for (p = 1; p <= np; p++)
		printf "| %-7s | %*s |\n", phase[p], w, v[p]
}' "$raw"

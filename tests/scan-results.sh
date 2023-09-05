#!/bin/sh
# Look for some frequent error message templates in test logs
#
# Usage: $0 [test-log.txt]

scan_log() {
	local file="$1"

	echo "Scanning $file"
	last=
	while read line; do
		case "$line" in
			===\ START\ TEST*) last="$line" ;;
			*Assertion*failed*) echo "ASSERTION FAILED: $last" ;;
			*runtime\ error*) echo "RUNTIME ERROR (sanitizer): $last" ;;
			*AddressSanitizer*heap-use-after-free*) echo "RUNTIME ERROR (use after free): $last" ;;
			*LeakSanitizer:*leak*) echo "SANITIZER REPORT: memory leak: $last" ;;
			*Warning:\ assertion*failed*) echo "ASSERTION WARNING: $last" ;;
			*command\ not\ found*) echo "COMMAND NOT FOUND: $last" ;;
			*extent\ buffer\ leak*) echo "EXTENT BUFFER LEAK: $last" ;;
			*) : ;;
		esac
	done < "$file"
}

# Scan only the given file
if [ -n "$1" ]; then
	scan_log "$1"
	exit
fi

# Scan all existing test logs
for file in *.txt; do
	scan_log "$file"
done

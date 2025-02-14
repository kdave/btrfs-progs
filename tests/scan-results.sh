#!/bin/bash
# Look for some frequent error message templates in test logs
#
# Usage: $0 [test-log.txt]

ret=0

scan_log() {
	local file="$1"

	echo "Scanning $file"
	last=
	while read line; do
		case "$line" in
			===\ START\ TEST*)	last="$line" ;;
			*Assertion*failed*)	ret=1; echo "ASSERTION FAILED: $last" ;;
			*runtime\ error*)	ret=1; echo "RUNTIME ERROR (sanitizer): $last" ;;
			*AddressSanitizer*heap-use-after-free*) ret=1; echo "RUNTIME ERROR (use after free): $last" ;;
			*LeakSanitizer:*leak*)	ret=1; echo "SANITIZER REPORT: memory leak: $last" ;;
			*Warning:\ assertion*failed*) ret=1; echo "ASSERTION WARNING: $last" ;;
			*command\ not\ found*)	ret=1; echo "COMMAND NOT FOUND: $last" ;;
			*extent\ buffer\ leak*)	ret=1; echo "EXTENT BUFFER LEAK: $last" ;;
			*) : ;;
		esac
	done < "$file"
}

# Scan only the given file
if [ -n "$1" ]; then
	scan_log "$1"
	exit "$ret"
fi

# Scan all existing test logs
for file in *.txt; do
	scan_log "$file"
done

exit "$ret"

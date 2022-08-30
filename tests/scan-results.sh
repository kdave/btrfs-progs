#!/bin/sh

# look for some error messages in all test logs

for i in *.txt; do
	echo "Scanning $i"
	last=
	while read line; do
		case "$line" in
			===\ Entering*) last="$line" ;;
			*Assertion*failed*) echo "ASSERTION FAILED: $last" ;;
			*runtime\ error*) echo "RUNTIME ERROR (sanitizer): $last" ;;
			*AddressSanitizer*heap-use-after-free*) echo "RUNTIME ERROR (use after free): $last" ;;
			*LeakSanitizer:*leak*) echo "SANITIZER REPORT: memory leak: $last" ;;
			*Warning:\ assertion*failed*) echo "ASSERTION WARNING: $last" ;;
			*command\ not\ found*) echo "COMMAND NOT FOUND: $last" ;;
			*extent\ buffer\ leak*) echo "EXTENT BUFFER LEAK: $last" ;;
			*) : ;;
		esac
	done < "$i"
done

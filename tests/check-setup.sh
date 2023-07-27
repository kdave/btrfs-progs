#!/bin/bash
#
# Check that the system setup and configuration are sufficient for all tests to run

for dir in *-tests; do
	missing=
	echo "Checking prerequisities for: $dir"
	for prog in $(find "$dir" -name 'test.sh' -exec grep check_global_prereq '{}' \; | sort -u); do
		if [ "$prog" = check_global_prereq ]; then
			continue
		fi
		if type -p "$prog" &> /dev/null; then
			echo "Check $prog: OK"
		else
			echo "Check $prog: MISSING"
			missing+=" $prog"
		fi
	done

	if ! [ -z "$missing" ]; then
		echo "MISSING: $missing"
	fi
done

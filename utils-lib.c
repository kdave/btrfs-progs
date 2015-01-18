#include "kerncompat.h"
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>

#if BTRFS_FLAT_INCLUDES
#else
#endif /* BTRFS_FLAT_INCLUDES */

/*
 * This function should be only used when parsing command arg, it won't return
 * error to its caller and rather exit directly just like usage().
 */
u64 arg_strtou64(const char *str)
{
	u64 value;
	char *ptr_parse_end = NULL;

	value = strtoull(str, &ptr_parse_end, 0);
	if (ptr_parse_end && *ptr_parse_end != '\0') {
		fprintf(stderr, "ERROR: %s is not a valid numeric value.\n",
			str);
		exit(1);
	}

	/*
	 * if we pass a negative number to strtoull, it will return an
	 * unexpected number to us, so let's do the check ourselves.
	 */
	if (str[0] == '-') {
		fprintf(stderr, "ERROR: %s: negative value is invalid.\n",
			str);
		exit(1);
	}
	if (value == ULLONG_MAX) {
		fprintf(stderr, "ERROR: %s is too large.\n", str);
		exit(1);
	}
	return value;
}

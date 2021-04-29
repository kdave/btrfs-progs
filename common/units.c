/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "common/units.h"

/*
 * Note: this function uses a static per-thread buffer. Do not call this
 * function more than 10 times within one argument list!
 */
const char *pretty_size_mode(u64 size, unsigned mode)
{
	static __thread int ps_index = 0;
	static __thread char ps_array[10][32];
	char *ret;

	ret = ps_array[ps_index];
	ps_index++;
	ps_index %= 10;
	(void)pretty_size_snprintf(size, ret, 32, mode);

	return ret;
}

static const char* unit_suffix_binary[] =
	{ "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
static const char* unit_suffix_decimal[] =
	{ "B", "kB", "MB", "GB", "TB", "PB", "EB"};

int pretty_size_snprintf(u64 size, char *str, size_t str_size, unsigned unit_mode)
{
	int num_divs;
	float fraction;
	u64 base = 0;
	int mult = 0;
	const char** suffix = NULL;
	u64 last_size;
	int negative;

	if (str_size == 0)
		return 0;

	negative = !!(unit_mode & UNITS_NEGATIVE);
	unit_mode &= ~UNITS_NEGATIVE;

	if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_RAW) {
		if (negative)
			snprintf(str, str_size, "%lld", size);
		else
			snprintf(str, str_size, "%llu", size);
		return 0;
	}

	if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_BINARY) {
		base = 1024;
		mult = 1024;
		suffix = unit_suffix_binary;
	} else if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_DECIMAL) {
		base = 1000;
		mult = 1000;
		suffix = unit_suffix_decimal;
	}

	/* Unknown mode */
	if (!base) {
		fprintf(stderr, "INTERNAL ERROR: unknown unit base, mode %u\n",
				unit_mode);
		assert(0);
		return -1;
	}

	num_divs = 0;
	last_size = size;
	switch (unit_mode & UNITS_MODE_MASK) {
	case UNITS_TBYTES:
		base *= mult;
		num_divs++;
		/* fallthrough */
	case UNITS_GBYTES:
		base *= mult;
		num_divs++;
		/* fallthrough */
	case UNITS_MBYTES:
		base *= mult;
		num_divs++;
		/* fallthrough */
	case UNITS_KBYTES:
		num_divs++;
		break;
	case UNITS_BYTES:
		base = 1;
		num_divs = 0;
		break;
	default:
		if (negative) {
			s64 ssize = (s64)size;
			s64 last_ssize = ssize;

			while ((ssize < 0 ? -ssize : ssize) >= mult) {
				last_ssize = ssize;
				ssize /= mult;
				num_divs++;
			}
			last_size = (u64)last_ssize;
		} else {
			while (size >= mult) {
				last_size = size;
				size /= mult;
				num_divs++;
			}
		}
		/*
		 * If the value is smaller than base, we didn't do any
		 * division, in that case, base should be 1, not original
		 * base, or the unit will be wrong
		 */
		if (num_divs == 0)
			base = 1;
	}

	if (num_divs >= ARRAY_SIZE(unit_suffix_binary)) {
		str[0] = '\0';
		printf("INTERNAL ERROR: unsupported unit suffix, index %d\n",
				num_divs);
		assert(0);
		return -1;
	}

	if (negative) {
		fraction = (float)(s64)last_size / base;
	} else {
		fraction = (float)last_size / base;
	}

	return snprintf(str, str_size, "%.2f%s", fraction, suffix[num_divs]);
}

void units_set_mode(unsigned *units, unsigned mode)
{
	unsigned base = *units & UNITS_MODE_MASK;

	*units = base | mode;
}

void units_set_base(unsigned *units, unsigned base)
{
	unsigned mode = *units & ~UNITS_MODE_MASK;

	*units = base | mode;
}

unsigned int get_unit_mode_from_arg(int *argc, char *argv[], int df_mode)
{
	unsigned int unit_mode = UNITS_DEFAULT;
	int arg_i;
	int arg_end;

	for (arg_i = 0; arg_i < *argc; arg_i++) {
		if (!strcmp(argv[arg_i], "--"))
			break;

		if (!strcmp(argv[arg_i], "--raw")) {
			unit_mode = UNITS_RAW;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--human-readable")) {
			unit_mode = UNITS_HUMAN_BINARY;
			argv[arg_i] = NULL;
			continue;
		}

		if (!strcmp(argv[arg_i], "--iec")) {
			units_set_mode(&unit_mode, UNITS_BINARY);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--si")) {
			units_set_mode(&unit_mode, UNITS_DECIMAL);
			argv[arg_i] = NULL;
			continue;
		}

		if (!strcmp(argv[arg_i], "--kbytes")) {
			units_set_base(&unit_mode, UNITS_KBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--mbytes")) {
			units_set_base(&unit_mode, UNITS_MBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--gbytes")) {
			units_set_base(&unit_mode, UNITS_GBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--tbytes")) {
			units_set_base(&unit_mode, UNITS_TBYTES);
			argv[arg_i] = NULL;
			continue;
		}

		if (!df_mode)
			continue;

		if (!strcmp(argv[arg_i], "-b")) {
			unit_mode = UNITS_RAW;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-h")) {
			unit_mode = UNITS_HUMAN_BINARY;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-H")) {
			unit_mode = UNITS_HUMAN_DECIMAL;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-k")) {
			units_set_base(&unit_mode, UNITS_KBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-m")) {
			units_set_base(&unit_mode, UNITS_MBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-g")) {
			units_set_base(&unit_mode, UNITS_GBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-t")) {
			units_set_base(&unit_mode, UNITS_TBYTES);
			argv[arg_i] = NULL;
			continue;
		}
	}

	for (arg_i = 0, arg_end = 0; arg_i < *argc; arg_i++) {
		if (!argv[arg_i])
			continue;
		argv[arg_end] = argv[arg_i];
		arg_end++;
	}

	*argc = arg_end;

	return unit_mode;
}


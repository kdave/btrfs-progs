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

/*
 * Test json output formatter
 *
 * Usage:
 *
 * $ ./json-formatter-test
 * 123
 *
 * Without arguments returns the number of tests.
 *
 * $ ./json-formatter-test 17
 * ...
 *
 * Run the given test, print formatted json for further processing or validation.
 */

#include <stdio.h>
#include <stdlib.h>
#include "common/utils.h"
#include "common/format-output.h"
#include "cmds/commands.h"

/* Default empty output */
void test_simple_empty()
{
	static const struct rowspec rows[] = {
		ROWSPEC_END
	};
	struct format_ctx fctx;

	fmt_start(&fctx, rows, 32, 0);
	fmt_end(&fctx);
}

/* Single object with a few members */
void test1()
{
	static const struct rowspec rows1[] = {
		{ .key = "device", .fmt = "%s", .out_text = "device", .out_json = "device" },
		{ .key = "devid", .fmt = "%llu", .out_text = "devid", .out_json = "devid" },
		ROWSPEC_END
	};
	struct format_ctx fctx;

	fmt_start(&fctx, rows1, 32, 0);
	fmt_print_start_group(&fctx, "device-info", JSON_TYPE_MAP);
	fmt_print(&fctx, "device", "/dev/sda");
	fmt_print(&fctx, "devid", 1);
	fmt_print_end_group(&fctx, NULL);
	fmt_end(&fctx);
}

int main(int argc, char **argv)
{
	int testno;
	static void (*tests[])() = {
		test_simple_empty,
		test1,
	};

	btrfs_config_init();
	bconf.output_format = CMD_FORMAT_JSON;

	/* Without arguments, print the number of tests available */
	if (argc == 1) {
		printf("%zu\n", ARRAY_SIZE(tests));
		return 0;
	}
	testno = atoi(argv[1]);
	testno--;

	if (testno < 0 || testno >= ARRAY_SIZE(tests)) {
		fprintf(stderr, "ERROR: test number %d is out of range (max %zu)\n",
				testno + 1, ARRAY_SIZE(tests));
		return 1;
	}
	tests[testno]();

	return 0;
}

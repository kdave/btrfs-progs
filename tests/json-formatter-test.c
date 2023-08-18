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
#include <uuid/uuid.h>
#include "common/utils.h"
#include "common/format-output.h"
#include "cmds/commands.h"

/* Default empty output */
static void test1_simple_empty()
{
	static const struct rowspec rows[] = {
		ROWSPEC_END
	};
	struct format_ctx fctx;

	fmt_start(&fctx, rows, 32, 0);
	fmt_end(&fctx);
}

/* Single object with a few members */
static void test2()
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

/* Escaped strings */
static void test3_escape()
{
	static const struct rowspec rows1[] = {
		{ .key = "devid", .fmt = "%llu", .out_text = "devid", .out_json = "devid" },
		{ .key = "path1", .fmt = "str", .out_text = "path1", .out_json = "path1" },
		{ .key = "path2", .fmt = "str", .out_text = "path2", .out_json = "path2" },
		ROWSPEC_END
	};
	struct format_ctx fctx;
	char control_chars[] = { [0] = '.', [0x20] = 0 };
	int i;

	for (i = 1; i < 0x20; i++)
		control_chars[i] = i;

	fmt_start(&fctx, rows1, 32, 0);
	fmt_print_start_group(&fctx, "device-info", JSON_TYPE_MAP);
	fmt_print(&fctx, "devid", 1);
	fmt_print(&fctx, "path1", "/fun\ny/p\th/\b/\\/\f\"quo\rte\"");
	fmt_print(&fctx, "path2", control_chars);
	fmt_print_end_group(&fctx, NULL);
	fmt_end(&fctx);
}

static void test4_unquoted_bool()
{
	static const struct rowspec rows1[] = {
		{ .key = "readonly", .fmt = "bool", .out_text = "readonly", .out_json = "readonly" },
		ROWSPEC_END
	};
	struct format_ctx fctx;

	fmt_start(&fctx, rows1, 32, 0);
	fmt_print_start_group(&fctx, "flags1", JSON_TYPE_MAP);
	fmt_print(&fctx, "readonly", 0);
	fmt_print_end_group(&fctx, NULL);
	fmt_print_start_group(&fctx, "flags2", JSON_TYPE_MAP);
	fmt_print(&fctx, "readonly", 1);
	fmt_print_end_group(&fctx, NULL);
	fmt_print_start_group(&fctx, "flags3", JSON_TYPE_MAP);
	fmt_print(&fctx, "readonly", false);
	fmt_print_end_group(&fctx, NULL);
	fmt_print_start_group(&fctx, "flags4", JSON_TYPE_MAP);
	fmt_print(&fctx, "readonly", true);
	fmt_print_end_group(&fctx, NULL);
	fmt_end(&fctx);
}

static void test5_uuid()
{
	static const struct rowspec rows1[] = {
		{ .key = "randomuuid", .fmt = "uuid", .out_text = "randomuuid", .out_json = "randomuuid" },
		{ .key = "nulluuid", .fmt = "uuid", .out_text = "nulluuid", .out_json = "nulluuid" },
		ROWSPEC_END
	};
	struct format_ctx fctx;
	uuid_t randomuuid, nulluuid;

	uuid_generate(randomuuid);
	uuid_clear(nulluuid);

	fmt_start(&fctx, rows1, 32, 0);
	fmt_print(&fctx, "randomuuid", randomuuid);
	fmt_print(&fctx, "nulluuid", nulluuid);
	fmt_end(&fctx);
}

int main(int argc, char **argv)
{
	int testno;
	static void (*tests[])() = {
		NULL,
		test1_simple_empty,
		test2,
		test3_escape,
		test4_unquoted_bool,
		test5_uuid,
	};
	const int testmax = ARRAY_SIZE(tests) - 1;

	btrfs_config_init();
	bconf.output_format = CMD_FORMAT_JSON;

	/* Without arguments, print the number of tests available */
	if (argc == 1) {
		printf("%d\n", testmax);
		return 0;
	}
	testno = atoi(argv[1]);
	if (testno < 1 || testno > ARRAY_SIZE(tests)) {
		fprintf(stderr, "ERROR: test number %d is out of range (min 1, max %d)\n",
				testno, testmax);
		return 1;
	}
	tests[testno]();

	return 0;
}

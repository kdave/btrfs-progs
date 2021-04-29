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

#include "kerncompat.h"
#include <stdio.h>
#include <uuid/uuid.h>
#include "common/defs.h"
#include "common/format-output.h"
#include "common/utils.h"
#include "common/units.h"
#include "cmds/commands.h"

static void print_uuid(const u8 *uuid)
{
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];

	if (uuid_is_null(uuid)) {
		putchar('-');
	} else {
		uuid_unparse(uuid, uuidparse);
		printf("%s", uuidparse);
	}
}

static void fmt_indent1(int indent)
{
	while (indent--)
		putchar(' ');
}

static void fmt_indent2(int indent)
{
	while (indent--) {
		putchar(' ');
		putchar(' ');
	}
}

static void fmt_error(struct format_ctx *fctx)
{
	printf("INTERNAL ERROR: formatting json: depth=%d\n", fctx->depth);
	exit(1);
}

static void fmt_inc_depth(struct format_ctx *fctx)
{
	if (fctx->depth >= JSON_NESTING_LIMIT - 1) {
		printf("INTERNAL ERROR: nesting too deep, limit %d\n",
				JSON_NESTING_LIMIT);
		exit(1);
	}
	fctx->depth++;
}

static void fmt_dec_depth(struct format_ctx *fctx)
{
	if (fctx->depth < 1) {
		printf("INTERNAL ERROR: nesting below first level\n");
		exit(1);
	}
	fctx->depth--;
}

static void fmt_separator(struct format_ctx *fctx)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		/* Check current depth */
		if (fctx->memb[fctx->depth] == 0) {
			/* First member, only indent */
			putchar('\n');
			fmt_indent2(fctx->depth);
			fctx->memb[fctx->depth] = 1;
		} else if (fctx->memb[fctx->depth] == 1) {
			/* Something has been printed already */
			printf(",\n");
			fmt_indent2(fctx->depth);
			fctx->memb[fctx->depth] = 2;
		} else {
			/* N-th member */
			printf(",\n");
			fmt_indent2(fctx->depth);
		}
	}
}

void fmt_start(struct format_ctx *fctx, const struct rowspec *spec, int width,
		int indent)
{
	memset(fctx, 0, sizeof(*fctx));
	fctx->width = width;
	fctx->indent = indent;
	fctx->rowspec = spec;
	fctx->depth = 1;

	if (bconf.output_format & CMD_FORMAT_JSON) {
		putchar('{');
		/* The top level is a map and is the first one */
		fctx->jtype[fctx->depth] = JSON_TYPE_MAP;
		fctx->memb[fctx->depth] = 0;
		fmt_print_start_group(fctx, "__header", JSON_TYPE_MAP);
		fmt_separator(fctx);
		printf("\"version\": \"1\"");
		fctx->memb[fctx->depth] = 1;
		fmt_print_end_group(fctx, "__header");
	}
}

void fmt_end(struct format_ctx *fctx)
{
	if (fctx->depth != 1)
		fprintf(stderr, "WARNING: wrong nesting\n");

	/* Close, no continuation to print */
	if (bconf.output_format & CMD_FORMAT_JSON) {
		fmt_dec_depth(fctx);
		fmt_separator(fctx);
		printf("}\n");
	}
}

void fmt_start_list_value(struct format_ctx *fctx)
{
	if (bconf.output_format == CMD_FORMAT_TEXT) {
		fmt_indent1(fctx->indent);
	} else if (bconf.output_format == CMD_FORMAT_JSON) {
		fmt_separator(fctx);
		fmt_indent2(fctx->depth);
		putchar('"');
	}
}

void fmt_end_list_value(struct format_ctx *fctx)
{
	if (bconf.output_format == CMD_FORMAT_TEXT)
		putchar('\n');
	else if (bconf.output_format == CMD_FORMAT_JSON)
		putchar('"');
}

void fmt_start_value(struct format_ctx *fctx, const struct rowspec *row)
{
	if (bconf.output_format == CMD_FORMAT_TEXT) {
		if (strcmp(row->fmt, "list") == 0)
			putchar('\n');
		else if (strcmp(row->fmt, "map") == 0)
			putchar('\n');
	} else if (bconf.output_format == CMD_FORMAT_JSON) {
		if (strcmp(row->fmt, "list") == 0) {
		} else if (strcmp(row->fmt, "map") == 0) {
		} else {
			putchar('"');
		}
	}
}

/*
 * Newline depends on format type:
 * - json does delayed continuation "," in case there's a following object
 * - plain text always ends with a newline
 */
void fmt_end_value(struct format_ctx *fctx, const struct rowspec *row)
{
	if (bconf.output_format == CMD_FORMAT_TEXT)
		putchar('\n');
	if (bconf.output_format == CMD_FORMAT_JSON) {
		if (strcmp(row->fmt, "list") == 0) {
		} else if (strcmp(row->fmt, "map") == 0) {
		} else {
			putchar('"');
		}
	}
}

void fmt_print_start_group(struct format_ctx *fctx, const char *name,
		enum json_type jtype)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		fmt_separator(fctx);
		fmt_inc_depth(fctx);
		fctx->jtype[fctx->depth] = jtype;
		fctx->memb[fctx->depth] = 0;
		if (name)
			printf("\"%s\": ", name);
		if (jtype == JSON_TYPE_MAP)
			putchar('{');
		else if (jtype == JSON_TYPE_ARRAY)
			putchar('[');
		else
			fmt_error(fctx);
	}
}

void fmt_print_end_group(struct format_ctx *fctx, const char *name)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		/* Whatever was on previous line won't continue with "," */
		const enum json_type jtype = fctx->jtype[fctx->depth];

		fmt_dec_depth(fctx);
		putchar('\n');
		fmt_indent2(fctx->depth);
		if (jtype == JSON_TYPE_MAP)
			putchar('}');
		else if (jtype == JSON_TYPE_ARRAY)
			putchar(']');
		else
			fmt_error(fctx);
	}
}

/* Use rowspec to print according to currently set output format */
void fmt_print(struct format_ctx *fctx, const char* key, ...)
{
	va_list args;
	const struct rowspec *row;
	bool found = false;

	va_start(args, key);
	row = &fctx->rowspec[0];

	while (row->key) {
		if (strcmp(key, row->key) == 0) {
			found = true;
			break;
		}
		row++;
	}
	if (!found) {
		printf("INTERNAL ERROR: unknown key: %s\n", key);
		exit(1);
	}

	if (bconf.output_format == CMD_FORMAT_TEXT) {
		const bool print_colon = row->out_text[0];
		int len;

		/* Print indented key name */
		fmt_indent1(fctx->indent);
		len = strlen(row->out_text);

		printf("%s", row->out_text);
		if (print_colon) {
			putchar(':');
			len++;
		}
		/* Align start for the value */
		fmt_indent1(fctx->width - len);
	} else if (bconf.output_format == CMD_FORMAT_JSON) {
		if (strcmp(row->fmt, "list") == 0) {
			fmt_print_start_group(fctx, row->out_json,
					JSON_TYPE_ARRAY);
		} else if (strcmp(row->fmt, "map") == 0) {
			fmt_print_start_group(fctx, row->out_json,
					JSON_TYPE_MAP);
		} else {
			/* Simple key/values */
			fmt_separator(fctx);
			printf("\"%s\": ", row->out_json);
		}
	}

	fmt_start_value(fctx, row);

	if (row->fmt[0] == '%') {
		vprintf(row->fmt, args);
	} else if (strcmp(row->fmt, "uuid") == 0) {
		const u8 *uuid = va_arg(args, const u8*);

		print_uuid(uuid);
	} else if (strcmp(row->fmt, "time-long") == 0) {
		const time_t ts = va_arg(args, time_t);

		if (ts) {
			char tstr[256];
			struct tm tm;

			localtime_r(&ts, &tm);
			strftime(tstr, 256, "%Y-%m-%d %X %z", &tm);
			printf("%s", tstr);
		} else {
			putchar('-');
		}
	} else if (strcmp(row->fmt, "list") == 0) {
	} else if (strcmp(row->fmt, "map") == 0) {
	} else if (strcmp(row->fmt, "qgroupid") == 0) {
		const u64 level = va_arg(args, u64);
		const u64 id = va_arg(args, u64);

		printf("%llu/%llu", level, id);
	} else if (strcmp(row->fmt, "size-or-none") == 0) {
		const u64 size = va_arg(args, u64);
		const unsigned int unit_mode = va_arg(args, unsigned int);

		if (size)
			printf("%s", pretty_size_mode(size, unit_mode));
		else
			putchar('-');
	} else if (strcmp(row->fmt, "size") == 0) {
		const u64 size = va_arg(args, u64);
		const unsigned int unit_mode = va_arg(args, unsigned int);

		printf("%s", pretty_size_mode(size, unit_mode));
	} else {
		printf("INTERNAL ERROR: unknown format %s\n", row->fmt);
	}

	fmt_end_value(fctx, row);
	va_end(args);
}

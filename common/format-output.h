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

#ifndef __BTRFS_FORMAT_OUTPUT_H__
#define __BTRFS_FORMAT_OUTPUT_H__

struct rowspec {
	/* Identifier for the row */
	const char *key;
	/*
	 * Format to print:
	 * - starting with %: must be a valid printf spec
	 *   (values: va_args)
	 * - uuid: format UUID as text
	 *   (value: u8 *uuid)
	 * - list: print list opening bracket [
	 *   (values printed separately)
	 * - map:  start a new group, opens {
	 *   (values printed separately)
	 * - size: pretty print size according to
	 *   (values: u64 size, u32 unit_type)
	 * - size-or-none: pretty print non-zero values, "-" otherwise
	 *   (values: same as "size")
	 * - qgroupid: print qgroup from separate level and id
	 *   (values: u64 level, u64 id)
	 */
	const char *fmt;
	/* String to print in format:text */
	const char *out_text;
	/* String to print in format:json, is quoted */
	const char *out_json;
};

#define	ROWSPEC_END	{ .key = NULL },
#define JSON_NESTING_LIMIT	16

/*
 * Nested types
 */
enum json_type {
	JSON_TYPE_INVALID,
	JSON_TYPE_MAP,
	JSON_TYPE_ARRAY,
};

struct format_ctx {
	/* Preferred width of the first column with key (format: text) */
	int width;
	/* Initial indentation before the first column (format: text) */
	int indent;
	/* Nesting of groups like lists or maps (format: json) */
	int depth;

	/* Array of named output fileds as defined by the command */
	const struct rowspec *rowspec;

	char jtype[JSON_NESTING_LIMIT];
	enum json_type memb[JSON_NESTING_LIMIT];
};

void fmt_start(struct format_ctx *fctx, const struct rowspec *spec, int width,
		int indent);
void fmt_end(struct format_ctx *fctx);

void fmt_print(struct format_ctx *fctx, const char* key, ...);

void fmt_start_list_value(struct format_ctx *fctx);
void fmt_end_list_value(struct format_ctx *fctx);

void fmt_start_value(struct format_ctx *fctx, const struct rowspec *row);
void fmt_end_value(struct format_ctx *fctx, const struct rowspec *row);

void fmt_print_start_group(struct format_ctx *fctx, const char *name,
		enum json_type jtype);
void fmt_print_end_group(struct format_ctx *fctx, const char *name);

#endif

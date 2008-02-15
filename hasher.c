/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kerncompat.h"
#include "hash.h"

int main() {
	u64 result;
	int ret;
	char line[255];
	char *p;
	while(1) {
		p = fgets(line, 255, stdin);
		if (!p)
			break;
		if (strlen(line) == 0)
			continue;
		if (line[strlen(line)-1] == '\n')
			line[strlen(line)-1] = '\0';
		result = btrfs_name_hash(line, strlen(line));
		printf("hash returns %llu\n", (unsigned long long)result);
	}
	return 0;
}

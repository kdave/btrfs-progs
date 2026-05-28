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
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include "common/messages.h"
#include "common/parse-utils.h"
#include "common/reproducible.h"

time_t reproducible_now(void)
{
	const char *sde = getenv("SOURCE_DATE_EPOCH");
	u64 val;

	if (!sde || !*sde)
		return time(NULL);

	if (parse_u64(sde, &val) < 0 || val > (u64)LONG_MAX) {
		error("invalid SOURCE_DATE_EPOCH: '%s' is not a non-negative integer fitting in time_t",
		      sde);
		exit(1);
	}
	return (time_t)val;
}

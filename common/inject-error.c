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
 * Simple error injection framework. Enabled at build time by -DINJECT.
 */

#include "inject-error.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef INJECT

static bool inject_verbose = true;

static bool cookie_enabled(unsigned long cookie) {
	const char *inj = getenv("INJECT");
	unsigned long envcookie;

	if (inj == NULL || inj[0] == 0)
		return false;

	envcookie = strtoul(inj, NULL, 0);
	if (envcookie == cookie)
		return true;
	return false;
}

#define inject_error(cookie)	__inject_error((cookie), __FILE__, __LINE__)

bool __inject_error(unsigned long cookie, const char *file, int line) {
	if (cookie == 0x0) {
		fprintf(stderr, "Error injection testing cookie 0x0\n");
		return true;
	}

	if (cookie_enabled(cookie)) {
		if (inject_verbose)
			fprintf(stderr, "Error injection: cookie 0x%lx in %s:%d\n",
				cookie, file, line);
		return true;
	}

	return false;
}

#else

#define inject_error(cookie)		(false)

#endif

#ifdef DEMO
#include <unistd.h>
/* gcc -o inject-error inject-error.c -DDEMO */
int work(int x) {
	sleep(1);
	printf("x=%d\n", x);
	if (x == 3 && inject_error(0x03)) {
		printf("error injected\n");
		return -1;
	}
	return 0;
}

int main() {
	int ret = 0;
	int x = 1;

	printf("Injection: INJECT=%s\n", getenv("INJECT"));
	while (ret == 0) {
		ret = work(x);
		x++;
	}
	return 0;
}
#endif

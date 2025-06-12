
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

#include "include/config.h"

#ifdef __ANDROID__

/* Workaround for `pthread_cancel()` in Android, using `pthread_kill()` instead,
 * as Android NDK does not support `pthread_cancel()`.
 */

#include <string.h>
#include <signal.h>
#include "android-compat/pthread.h"

int pthread_setcanceltype(int type, int *oldtype) { return 0; }
int pthread_setcancelstate(int state, int *oldstate) { return 0; }
int pthread_cancel(pthread_t thread_id) {
	int status;
	if ((status = btrfs_set_thread_exit_handler()) == 0) {
		status = pthread_kill(thread_id, SIGUSR1);
	}
	return status;
}

void btrfs_thread_exit_handler(int sig) {
	pthread_exit(0);
}

int btrfs_set_thread_exit_handler() {
	int rc;
	struct sigaction actions;

	memset(&actions, 0, sizeof(actions));
	sigemptyset(&actions.sa_mask);
	actions.sa_flags = 0;
	actions.sa_handler = btrfs_thread_exit_handler;

	rc = sigaction(SIGUSR1, &actions, NULL);
	return rc;
}

#endif

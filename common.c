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

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

int open_file_or_dir(const char *fname)
{
	int ret;
	struct stat st;
	DIR *dirstream;
	int fd;

	ret = stat(fname, &st);
	if (ret < 0) {
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		dirstream = opendir(fname);
		if (!dirstream) {
			return -2;
		}
		fd = dirfd(dirstream);
	} else {
		fd = open(fname, O_RDWR);
	}
	if (fd < 0) {
		return -3;
	}
	return fd;
}

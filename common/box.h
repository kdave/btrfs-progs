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

#ifndef __BTRFS_BOX_H__
#define __BTRFS_BOX_H__

/*
 * For tools that can co-exist in a single binary and their main() gets
 * switched by the file name.
 */
#ifdef ENABLE_BOX
#define BOX_MAIN(name)		name##_main
#define DECLARE_BOX_MAIN(name)	int name##_main(int argc, char **argv)

/*
 * Declarations of the built-in tools, pairing with actual definitions of the
 * respective main function
 */
DECLARE_BOX_MAIN(mkfs);
DECLARE_BOX_MAIN(image);
DECLARE_BOX_MAIN(convert);
DECLARE_BOX_MAIN(btrfstune);

#else
#define BOX_MAIN(standalone)	main
#endif

#endif

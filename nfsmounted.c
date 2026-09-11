/*
 * nfsmounted.c -- determine if a pathname has been NFS mounted
 * Copyright (C) 1993 Rick Sladkey <jrs@world.std.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Library Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#if defined(HAVE_UNISTD_H) || defined(STDC_HEADERS)
#include <unistd.h>
#endif
#include <stdio.h>
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#ifdef _POSIX_VERSION
#include <limits.h>			/* for PATH_MAX */
#else
#include <sys/param.h>			/* for MAXPATHLEN */
#endif
#include <errno.h>
#ifndef STDC_HEADERS
extern int errno;
#endif

#include <sys/stat.h>			/* for S_IFLNK */

#ifndef PATH_MAX
#ifdef _POSIX_VERSION
#define PATH_MAX _POSIX_PATH_MAX
#else
#ifdef MAXPATHLEN
#define PATH_MAX MAXPATHLEN
#else
#define PATH_MAX 1024
#endif
#endif
#endif

#include <sys/sysmacros.h>

#ifdef __linux__
#include <sys/vfs.h>			/* for statfs() */
#ifndef NFS_SUPER_MAGIC
#define NFS_SUPER_MAGIC	0x6969
#endif
#endif

/*
 * Linux gives every filesystem without a block device a major-0 device
 * number -- tmpfs, btrfs, ZFS, overlayfs and FUSE as well as NFS -- so
 * major(st_dev) == 0 alone flagged all of those as NFS mounts. Use it
 * only as a quick "not NFS" test and ask the kernel for the type.
 */
int
nfsmounted(const char *path, struct stat *sbp)
{
#ifdef __linux__
	struct statfs	sfs;

	if (major(sbp->st_dev) != 0)
		return 0;
	if (statfs(path, &sfs) < 0)
		return 0;
	return sfs.f_type == NFS_SUPER_MAGIC;
#endif
	return 0;
}

/* Defines the libntfs-3g public headers expect from config.h. */
#ifndef NH_CONFIG_H
#define NH_CONFIG_H
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_LIMITS_H 1
#define HAVE_FCNTL_H 1
#define HAVE_UNISTD_H 1
#define HAVE_TIME_H 1
#define HAVE_SETXATTR 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_UTIMENSAT 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/security.h>
#include <ntfs-3g/reparse.h>
#include <ntfs-3g/mft.h>
#include <ntfs-3g/runlist.h>
#include <ntfs-3g/bitmap.h>
#include <ntfs-3g/cache.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/misc.h>
#endif

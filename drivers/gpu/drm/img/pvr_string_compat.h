/* SPDX-License-Identifier: GPL-2.0 OR MIT */
#ifndef PVR_STRING_COMPAT_H
#define PVR_STRING_COMPAT_H
#include <linux/version.h>
#include <linux/string.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0)
/* Preserve the DDK's source-length return contract; strscpy() differs on
 * truncation. Source must be a NUL-terminated string, as with old strlcpy(). */
static inline size_t pvr_strlcpy(char *dst, const char *src, size_t size)
{
	size_t length = strlen(src);
	if (size) {
		size_t copied = length < size - 1 ? length : size - 1;
		memcpy(dst, src, copied);
		dst[copied] = '\0';
	}
	return length;
}
#define strlcpy pvr_strlcpy
#endif
#endif

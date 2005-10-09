/*	$Csoft: ucslcpy.c,v 1.3 2003/08/31 11:58:09 vedge Exp $	*/

/*
 * Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND TODD C. MILLER DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL TODD C. MILLER BE LIABLE
 * FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <engine/engine.h>

#include <sys/types.h>
#include <string.h>

#include "unicode.h"

/*
 * Copy src to string dst of size bytes. At most (bytes-1)/sizeof(Uint32)
 * Unicode characters will be copied.
 *
 * Always NUL terminates (unless bytes == 0).
 * Returns AG_UCS4Len(src)*sizeof(Uint32); if retval >= bytes, truncation
 * occurred.
 */
size_t
AG_UCS4Copy(Uint32 *dst, const Uint32 *src, size_t bytes)
{
	Uint32 *d = dst;
	const Uint32 *s = src;
	size_t n = bytes / sizeof(Uint32);

	/* Copy as many characters as will fit. */
	if (n != 0 && --n != 0) {
		do {
			if ((*d++ = *s++) == 0) {
				break;
			}
		} while (--n != 0);
	}

	/* Not enough room in dst, add NUL and traverse rest of src. */
	if (n == 0) {
		if (bytes != 0) {
			*d = '\0';			 /* NUL-terminate dst */
		}
		while (*s++)
			;
	}

	return ((s - src - 1) * sizeof(Uint32));      /* Does not include NUL */
}


/*
 * Copyright (c) 2011 Hypertriton, Inc. <http://hypertriton.com/>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Audio file output driver.
 */

#include <config/have_sndfile.h>
#ifdef HAVE_SNDFILE

#include <core/core.h>

#include "au_init.h"
#include "au_dev_out.h"
#include <sndfile.h>

typedef struct au_dev_out_wav {
	struct au_dev_out _inherit;
	SNDFILE  *file;
	SF_INFO   info;
	AG_Thread th;
} AU_DevOutFile;

static void
Init(void *obj)
{
	AU_DevOut *dev = obj;
	AU_DevOutFile *df = obj;

	dev->flags |= AU_DEV_OUT_THREADED;
	df->file = NULL;
	memset(&df->info, 0, sizeof(df->info));
}

static void *
AU_DevFileThread(void *obj)
{
	AU_DevOut *dev = obj;
	AU_DevOutFile *df = obj;
	struct timespec ts;
	long rv;

	ts.tv_sec = 1;
	ts.tv_nsec = 0;

	for (;;) {
		AG_MutexLock(&dev->lock);
		if (dev->flags & AU_DEV_OUT_CLOSING) {
			dev->flags &= ~(AU_DEV_OUT_CLOSING);
			AG_MutexUnlock(&dev->lock);
			return (NULL);
		}
		if (AG_CondTimedWait(&dev->rdRdy, &dev->lock, &ts) == 0) {
#if 0
			int i;
			for (i = 0; i < dev->bufSize*dev->ch; i++) {
				fprintf(stderr, "%.02f ", dev->buf[i]);
			}
			fprintf(stderr, "\n");
#endif
			rv = sf_writef_float(df->file, dev->buf, dev->bufSize);
			if (rv < dev->bufSize) {
				dev->flags |= AU_DEV_OUT_ERROR;
			}
			dev->bufSize = 0;
		}
		AG_CondBroadcast(&dev->wrRdy);
		AG_MutexUnlock(&dev->lock);
	}
	return (NULL);
}

static int
Open(void *obj, const char *path, int rate, int ch)
{
	AU_DevOut *dev = obj;
	AU_DevOutFile *df = obj;
	const char *c;

	if (df->file != NULL) {
		AG_SetError("Audio dump to file already in progress");
		return (-1);
	}
	memset(&df->info, 0, sizeof(df->info));
	df->info.samplerate = rate;
	df->info.channels = ch;
	dev->rate = rate;
	dev->ch = ch;

	if ((c = strrchr(path, '.')) != NULL &&		/* XXX */
	    strcasecmp(c, ".ogg") == 0) {
		df->info.format = SF_FORMAT_OGG|SF_FORMAT_VORBIS;
	} else {
		df->info.format = SF_FORMAT_WAV|SF_FORMAT_FLOAT;
	}

	if ((df->file = sf_open(path, SFM_WRITE, &df->info)) == NULL) {
		AG_SetError("%s(%d): %s", path, rate, sf_strerror(NULL));
		return (-1);
	}
	if (AG_ThreadCreate(&df->th, AU_DevFileThread, df) != 0) {
		sf_write_sync(df->file);
		sf_close(df->file);
		df->file = NULL;
		return (-1);
	}
	return (0);
}

static void
Close(void *obj)
{
	AU_DevOutFile *df = obj;

	if (df->file != NULL) {
		sf_write_sync(df->file);
		sf_close(df->file);
		df->file = NULL;
	}
}

const AU_DevOutClass auDevOut_file = {
	"file",
	sizeof(AU_DevOutFile),
	Init,
	NULL,	/* Destroy */
	Open,
	Close
};

#endif /* HAVE_SNDFILE */

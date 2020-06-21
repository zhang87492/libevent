/*
 * Copyright (c) 2002, 2003 Niels Provos <provos@citi.umich.edu>
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
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "event.h"

struct evbuffer *
evbuffer_new(void)
{
	struct evbuffer *buffer;
	
	buffer = calloc(1, sizeof(struct evbuffer));

	return (buffer);
}

void
evbuffer_free(struct evbuffer *buffer)
{
	if (buffer->orig_buffer != NULL)
		free(buffer->orig_buffer);
	free(buffer);
}

/* 
 * This is a destructive add.  The data from one buffer moves into
 * the other buffer.
 */

#define SWAP(x,y) do { \
	(x)->buffer = (y)->buffer; \
	(x)->orig_buffer = (y)->orig_buffer; \
	(x)->misalign = (y)->misalign; \
	(x)->totallen = (y)->totallen; \
	(x)->off = (y)->off; \
} while (0)

int
evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf)
{
	/*
	两个buffer相加。
	*/
	int res;

	/* Short cut for better performance */
	if (outbuf->off == 0) {
		struct evbuffer tmp;
		size_t oldoff = inbuf->off;

		/* Swap them directly */
		SWAP(&tmp, outbuf);
		SWAP(outbuf, inbuf);
		SWAP(inbuf, &tmp);

		/* 
		 * Optimization comes with a price; we need to notify the
		 * buffer if necessary of the changes. oldoff is the amount
		 * of data that we tranfered from inbuf to outbuf
		 * oldoff是数据的数量。从inbuf到outbuf的长度。
		 * 第一个长度是原本的，第二个长度是变化后的。
		 */
		if (inbuf->off != oldoff && inbuf->cb != NULL)
			(*inbuf->cb)(inbuf, oldoff, inbuf->off, inbuf->cbarg);
		if (oldoff && outbuf->cb != NULL)
			(*outbuf->cb)(outbuf, 0, oldoff, outbuf->cbarg);
		
		return (0);
	}

	res = evbuffer_add(outbuf, inbuf->buffer, inbuf->off);
	/* 添加成功，把inbuf里的内容清空掉。*/
	if (res == 0)
		evbuffer_drain(inbuf, inbuf->off);

	return (res);
}

int
evbuffer_add_printf(struct evbuffer *buf, char *fmt, ...)
{
	int res = -1;
	char *msg;
#ifdef WIN32
	static char buffer[4096];
#endif
	va_list ap;

	va_start(ap, fmt);

#ifndef WIN32
	if (vasprintf(&msg, fmt, ap) == -1)
		goto end;
#else
	_vsnprintf(buffer, sizeof(buffer) - 1, fmt, ap);
	buffer[sizeof(buffer)-1] = '\0';
	msg = buffer;
#endif
	
	res = strlen(msg);
	if (evbuffer_add(buf, msg, res) == -1)
		res = -1;
#ifndef WIN32
	free(msg);

end:
#endif
	va_end(ap);

	return (res);
}

/* Reads data from an event buffer and drains the bytes read */

int
evbuffer_remove(struct evbuffer *buf, void *data, size_t datlen)
{
	int nread = datlen;
	if (nread >= buf->off)
		nread = buf->off;

	memcpy(data, buf->buffer, nread);
	evbuffer_drain(buf, nread);
	
	return (nread);
}

/* Adds data to an event buffer.数据对齐 */

static __inline void
evbuffer_align(struct evbuffer *buf)
{
	memmove(buf->orig_buffer, buf->buffer, buf->off);
	buf->buffer = buf->orig_buffer;
	buf->misalign = 0;
}

int
evbuffer_add(struct evbuffer *buf, void *data, size_t datlen)
{
	//要被添加的buf。这次需要的空间大小值。要加上原本已经使用的空间
	size_t need = buf->misalign + buf->off + datlen;
	size_t oldoff = buf->off;
	/* 需要进行空间扩展 */
	if (buf->totallen < need) {
		/* 已用空间大于要使用的空间，则进行对齐。 */
		if (buf->misalign >= datlen) {
			evbuffer_align(buf);
		} else {
			void *newbuf;
			size_t length = buf->totallen;

			if (length < 256)
				length = 256;
			while (length < need)
				length <<= 1;

			if (buf->orig_buffer != buf->buffer)
				evbuffer_align(buf);
			if ((newbuf = realloc(buf->buffer, length)) == NULL)
				return (-1);

			buf->orig_buffer = buf->buffer = newbuf;
			buf->totallen = length;
		}
	}

	memcpy(buf->buffer + buf->off, data, datlen);
	buf->off += datlen;

	if (datlen && buf->cb != NULL)
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

	return (0);
}

void
evbuffer_drain(struct evbuffer *buf, size_t len)
{
	size_t oldoff = buf->off;

	if (len >= buf->off) {
		buf->off = 0;
		buf->buffer = buf->orig_buffer;
		buf->misalign = 0;
		goto done;
	}

	buf->buffer += len;
	buf->misalign += len;

	buf->off -= len;

 done:
	/* Tell someone about changes in this buffer */
	if (buf->off != oldoff && buf->cb != NULL)
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

}

int
evbuffer_read(struct evbuffer *buffer, int fd, int howmuch)
{
	/*
	从文件读取，读取指定howmuch数量。
	最多读4096个，4k
	*/
	u_char inbuf[4096];
	int n;
#ifdef WIN32
	DWORD dwBytesRead;
#endif
	
	if (howmuch < 0 || howmuch > sizeof(inbuf))
		howmuch = sizeof(inbuf);

#ifndef WIN32
	n = read(fd, inbuf, howmuch);
	if (n == -1)
		return (-1);
	if (n == 0)
		return (0);
#else
	n = ReadFile((HANDLE)fd, inbuf, howmuch, &dwBytesRead, NULL);
	if (n == 0)
		return (-1);
	if (dwBytesRead == 0)
		return (0);
	n = dwBytesRead;
#endif

	evbuffer_add(buffer, inbuf, n);

	return (n);
}

int
evbuffer_write(struct evbuffer *buffer, int fd)
{
	int n;
#ifdef WIN32
	DWORD dwBytesWritten;
#endif

#ifndef WIN32
	n = write(fd, buffer->buffer, buffer->off);
	if (n == -1)
		return (-1);
	if (n == 0)
		return (0);
#else
	n = WriteFile((HANDLE)fd, buffer->buffer, buffer->off, &dwBytesWritten, NULL);
	if (n == 0)
		return (-1);
	if (dwBytesWritten == 0)
		return (0);
	n = dwBytesWritten;
#endif
	evbuffer_drain(buffer, n);

	return (n);
}

u_char *
evbuffer_find(struct evbuffer *buffer, u_char *what, size_t len)
{
	size_t remain = buffer->off;
	u_char *search = buffer->buffer;
	u_char *p;
	/*
	在参数 search 所指向的字符串的前 n 个字节中搜索第一次出现字符 *what（一个无符号字符）的位置。
	*/
	while ((p = memchr(search, *what, remain)) != NULL && remain >= len) {
		if (memcmp(p, what, len) == 0)
			return (p);

		search = p + 1;
		remain = buffer->off - (size_t)(search - buffer->buffer);
	}

	return (NULL);
}

void evbuffer_setcb(struct evbuffer *buffer,
    void (*cb)(struct evbuffer *, size_t, size_t, void *),
    void *cbarg)
{
	buffer->cb = cb;
	buffer->cbarg = cbarg;
}

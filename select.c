/*	$OpenBSD: select.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>

#ifdef USE_LOG
#include "log.h"
#else
#define LOG_DBG(x)
#define log_error(x)	perror(x)
#endif

#include "event.h"
#include "evsignal.h"

extern struct event_list eventqueue;

#ifndef howmany
#define        howmany(x, y)   (((x)+((y)-1))/(y))
#endif

extern volatile sig_atomic_t evsignal_caught;

struct selectop {
	/* 最大的文件句柄。*/
	int event_fds;		/* Highest fd in fd set */
	int event_fdsz;		// 存储信号集需要的字节大小。
	fd_set *event_readset;/* 读信号事件集合*/
	fd_set *event_writeset;/* 写信号事件集合*/
	sigset_t evsigmask;// linux 信号集。用于检测是否有信号发送过来。
} sop;

void *select_init	(void);
int select_add		(void *, struct event *);
int select_del		(void *, struct event *);
int select_recalc	(void *, int);
int select_dispatch	(void *, struct timeval *);

const struct eventop selectops = {
	"select",
	select_init,
	select_add,
	select_del,
	select_recalc,
	select_dispatch
};

void *
select_init(void)
{
	/* Disable kqueue when this environment variable is set */
	if (getenv("EVENT_NOSELECT"))
		return (NULL);

	memset(&sop, 0, sizeof(sop));

	evsignal_init(&sop.evsigmask);

	return (&sop);
}

/*
 * Called with the highest fd that we know about.  If it is 0, completely
 * recalculate everything.
 */

int
select_recalc(void *arg, int max)
{
	struct selectop *sop = arg;
	fd_set *readset, *writeset;
	struct event *ev;
	int fdsz;
	/*
	设置event_fds为最大值
	*/
	if (sop->event_fds < max)
		sop->event_fds = max;
	/*
	如果event_fds不存在，则遍历队列，取最大值。
	*/
	if (!sop->event_fds) {
		TAILQ_FOREACH(ev, &eventqueue, ev_next)
			if (ev->ev_fd > sop->event_fds)
				sop->event_fds = ev->ev_fd;
	}
	/*
	计算存储event_fds所需要的位数。
	*/
	fdsz = howmany(sop->event_fds + 1, NFDBITS) * sizeof(fd_mask);
	if (fdsz > sop->event_fdsz) {
		if ((readset = realloc(sop->event_readset, fdsz)) == NULL) {
			log_error("malloc");
			return (-1);
		}

		if ((writeset = realloc(sop->event_writeset, fdsz)) == NULL) {
			log_error("malloc");
			free(readset);
			return (-1);
		}

		memset((char *)readset + sop->event_fdsz, 0,
		    fdsz - sop->event_fdsz);
		memset((char *)writeset + sop->event_fdsz, 0,
		    fdsz - sop->event_fdsz);

		sop->event_readset = readset;
		sop->event_writeset = writeset;
		sop->event_fdsz = fdsz;
	}

	return (evsignal_recalc(&sop->evsigmask));
}

/*
下发任务，发起一次，select。等待信号发送。
*/
int
select_dispatch(void *arg, struct timeval *tv)	
{
	int maxfd, res;
	struct event *ev, *next;
	struct selectop *sop = arg;

	memset(sop->event_readset, 0, sop->event_fdsz);
	memset(sop->event_writeset, 0, sop->event_fdsz);
	/*
	再初始化，信号集。
	*/
	TAILQ_FOREACH(ev, &eventqueue, ev_next) {
		if (ev->ev_events & EV_WRITE)
			FD_SET(ev->ev_fd, sop->event_writeset);
		if (ev->ev_events & EV_READ)
			FD_SET(ev->ev_fd, sop->event_readset);
	}
	/*
	先注册信号。
	*/
	if (evsignal_deliver(&sop->evsigmask) == -1)
		return (-1);
	/*
	调用select函数，等待事件发送。
	tv如果有值，则超时自动返回。
	*/
	res = select(sop->event_fds + 1, sop->event_readset, 
	    sop->event_writeset, NULL, tv);
	/*
	select函数完后。需再重新注册信号事件。
	如果select中有事件发生，需要再重新注册一遍事件。或者select中，是事件触发select结束
	会导致程序未处理接下来的信号事件
	*/
	if (evsignal_recalc(&sop->evsigmask) == -1)
		return (-1);

	if (res == -1) {
		if (errno != EINTR) {
			log_error("select");
			return (-1);
		}
		/*
		处理信号。
		*/
		evsignal_process();
		return (0);
	} else if (evsignal_caught)
		evsignal_process();

	LOG_DBG((LOG_MISC, 80, "%s: select reports %d", __func__, res));
	/*
	监听读事件，或者写事件，是否有事件发生。
	*/
	maxfd = 0;
	for (ev = TAILQ_FIRST(&eventqueue); ev != NULL; ev = next) {
		next = TAILQ_NEXT(ev, ev_next);

		res = 0;
		if (FD_ISSET(ev->ev_fd, sop->event_readset))
			res |= EV_READ;
		if (FD_ISSET(ev->ev_fd, sop->event_writeset))
			res |= EV_WRITE;
		res &= ev->ev_events;

		if (res) {
			if (!(ev->ev_events & EV_PERSIST))
				event_del(ev);
			event_active(ev, res, 1);
		} else if (ev->ev_fd > maxfd)
			maxfd = ev->ev_fd;
	}

	sop->event_fds = maxfd;

	return (0);
}

/*
添加事件。
如果只是select的信号事件，就增加到信号事件集里。
如果不是select的信号事件。就增加最大的文件fd
这样，自然就添加到了select的监听事件集里。
*/
int
select_add(void *arg, struct event *ev)
{
	struct selectop *sop = arg;

	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_add(&sop->evsigmask, ev));

	/* 
	 * Keep track of the highest fd, so that we can calculate the size
	 * of the fd_sets for select(2)
	 */
	if (sop->event_fds < ev->ev_fd)
		sop->event_fds = ev->ev_fd;

	return (0);
}

/*
 * Nothing to be done here.
 文件句柄，扩大了就没有再缩小了。
 删除信号事件。
 */
int
select_del(void *arg, struct event *ev)
{
	struct selectop *sop = arg;

	if (!(ev->ev_events & EV_SIGNAL))
		return (0);

	return (evsignal_del(&sop->evsigmask, ev));
}

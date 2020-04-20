/*	$OpenBSD: poll.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Niels Provos.
 * 4. The name of the author may not be used to endorse or promote products
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
#include <poll.h>//poll的头文件。
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

extern volatile sig_atomic_t evsignal_caught;

/*
struct pollfd {  
 int fd;        //文件描述符  
 short events;  //要求查询的事件掩码  
 short revents; //返回的事件掩码  
};  
int poll(struct pollfd *ufds, unsigned int nfds, int timeout);  
*/
struct pollop {
	int event_count;		/* Highest number alloc */
	struct pollfd *event_set; /* poll的存储结构。 */
	struct event **event_back;
	sigset_t evsigmask; //每个类都需要注册信号的监管结构。
} pop;
//再看看这个博客
//https://blog.csdn.net/zhuxiaoping54532/article/details/51701549
void *poll_init	(void);
int poll_add		(void *, struct event *);
int poll_del		(void *, struct event *);
int poll_recalc	(void *, int);
int poll_dispatch	(void *, struct timeval *);

struct eventop pollops = {
	"poll",
	poll_init,
	poll_add,
	poll_del,
	poll_recalc,
	poll_dispatch
};

void *
poll_init(void)
{
	/* Disable kqueue when this environment variable is set */
	if (getenv("EVENT_NOPOLL"))
		return (NULL);
	/*基本上都是这样初始化结构体的。 */
	memset(&pop, 0, sizeof(pop));

	evsignal_init(&pop.evsigmask);

	return (&pop);
}

/*
 * Called with the highest fd that we know about.  If it is 0, completely
 * recalculate everything.
 */
/*
结构不一样，所以不需要重新初始化句柄。
*/
int
poll_recalc(void *arg, int max)
{
	struct pollop *pop = arg;
	
	return (evsignal_recalc(&pop->evsigmask));
}

int
poll_dispatch(void *arg, struct timeval *tv)
{
	int res, i, count, sec, nfds;
	struct event *ev;
	struct pollop *pop = arg;

	count = pop->event_count;
	nfds = 0;
	/*
	设置循环，读取event的事件队列。设置要监听的句柄。
	设置监听的信号。
	*/
	TAILQ_FOREACH(ev, &eventqueue, ev_next) {
		if (nfds + 1 >= count) {
			if (count < 32)
				count = 32;
			else
				count *= 2;

			/* We need more file descriptors */
			pop->event_set = realloc(pop->event_set,
			    count * sizeof(struct pollfd));
			if (pop->event_set == NULL) {
				log_error("realloc");
				return (-1);
			}
			pop->event_back = realloc(pop->event_back,
			    count * sizeof(struct event *));
			if (pop->event_back == NULL) {
				log_error("realloc");
				return (-1);
			}
			pop->event_count = count;
		}
		if (ev->ev_events & EV_WRITE) {
			struct pollfd *pfd = &pop->event_set[nfds];
			pfd->fd = ev->ev_fd;
			pfd->events = POLLOUT;
			pfd->revents = 0;

			pop->event_back[nfds] = ev;

			nfds++;
		}
		if (ev->ev_events & EV_READ) {
			struct pollfd *pfd = &pop->event_set[nfds];

			pfd->fd = ev->ev_fd;
			pfd->events = POLLIN;
			pfd->revents = 0;

			pop->event_back[nfds] = ev;

			nfds++;
		}
	}

	if (evsignal_deliver(&pop->evsigmask) == -1)
		return (-1);

	sec = tv->tv_sec * 1000 + tv->tv_usec / 1000;
	res = poll(pop->event_set, nfds, sec);
	/*返回值:
	>0：数组fds中准备好读、写或出错状态的那些socket描述符的总数量；
    ==0：数组fds中没有任何socket描述符准备好读、写，或出错；此时poll超时，超时时间是timeout毫秒；
	换句话说，如果所检测的 socket描述符上没有任何事件发生的话，那么poll()函数会阻塞timeout所指定的毫秒时间长度之后返回，
		timeout==0，那么 poll() 函数立即返回而不阻塞，
		timeout==INFTIM，那么poll() 函数会一直阻塞下去，直到所检测的socket描述符上的感兴趣的事件发生时才返回，
			如果感兴趣的事件永远不发生，那么poll()就会永远阻塞下去；
	-1：  poll函数调用失败，同时会自动设置全局变量errno；
			如果待检测的socket描述符为负值，则对这个描述符的检测就会被忽略，
			也就是不会对成员变量events进行检测，在events上注册的事件也会被忽略，
			poll()函数返回的时候，会把成员变量revents设置为0，表示没有事件发生；
	*/
	if (evsignal_recalc(&pop->evsigmask) == -1)
		return (-1);

	if (res == -1) {
		if (errno != EINTR) {
			log_error("poll");
			return (-1);
		}

		evsignal_process();
		return (0);
	} else if (evsignal_caught)
		evsignal_process();

	LOG_DBG((LOG_MISC, 80, "%s: poll reports %d", __func__, res));

	if (res == 0)
		return (0);

	for (i = 0; i < nfds; i++) {
		res = 0;
		if (pop->event_set[i].revents & POLLIN)
			res = EV_READ;
		else if (pop->event_set[i].revents & POLLOUT)
			res = EV_WRITE;
		if (res == 0)
			continue;

		ev = pop->event_back[i];
		res &= ev->ev_events;

		if (res) {
			if (!(ev->ev_events & EV_PERSIST))
				event_del(ev);
			event_active(ev, res, 1);
		}	
	}

	return (0);
}

int
poll_add(void *arg, struct event *ev)
{
	struct pollop *pop = arg;

	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_add(&pop->evsigmask, ev));

	return (0);
}

/*
 * Nothing to be done here.
 */

int
poll_del(void *arg, struct event *ev)
{
	struct pollop *pop = arg;

	if (!(ev->ev_events & EV_SIGNAL))
		return (0);

	return (evsignal_del(&pop->evsigmask, ev));
}

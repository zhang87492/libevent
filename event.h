/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
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
#ifndef _EVENT_H_
#define _EVENT_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WIN32
#include <windows.h>
#endif

#define EVLIST_TIMEOUT	0x01
#define EVLIST_INSERTED	0x02 // 读写事件
#define EVLIST_SIGNAL	0x04
#define EVLIST_ACTIVE	0x08
#define EVLIST_INTERNAL	0x10 //内部事件
#define EVLIST_INIT	0x80

/* EVLIST_X_ Private space: 0x1000-0xf000 */
#define EVLIST_ALL	(0xf000 | 0x9f)

#define EV_TIMEOUT	0x01
#define EV_READ		0x02
#define EV_WRITE	0x04
#define EV_SIGNAL	0x08
#define EV_PERSIST	0x10	/* Persistant event */

/* Fix so that ppl dont have to run with <sys/queue.h> */
#ifndef TAILQ_ENTRY
#define _EVENT_DEFINED_TQENTRY
#define TAILQ_ENTRY(type)						\
struct {								\
	struct type *tqe_next;	/* next element */			\
	struct type **tqe_prev;	/* address of previous next element */	\
}
#endif /* !TAILQ_ENTRY */
#ifndef RB_ENTRY
#define _EVENT_DEFINED_RBENTRY
#define RB_ENTRY(type)							\
struct {								\
	struct type *rbe_left;		/* left element */		\
	struct type *rbe_right;		/* right element */		\
	struct type *rbe_parent;	/* parent element */		\
	int rbe_color;			/* node color */		\
}
#endif /* !RB_ENTRY */

struct event {
	/*
	定义的队列元素，通过这些结构体串了起来。
	ev_flags标志它现在是在哪个队列中。即下面三个TAILQ可用
	*/
	TAILQ_ENTRY (event) ev_next;
	TAILQ_ENTRY (event) ev_active_next;//一种相当巧妙的设计？感觉有点意识？比之其他代码好像有点意识的感觉
	TAILQ_ENTRY (event) ev_signal_next;
	RB_ENTRY (event) ev_timeout_node;

#ifdef WIN32
	HANDLE ev_fd;
	OVERLAPPED overlap;
#else
	int ev_fd;
#endif
	short ev_events; //标志是什么事件，写事件还是读事件
	short ev_ncalls;
	/* 该值指向ev_ncalls的地址。这样设计感觉有点奇怪啊。 */
	short *ev_pncalls;	/* Allows deletes in callback */

	struct timeval ev_timeout;//时间的处理时间。根据传进来的超时时间，在内部重新计算一遍。

	void (*ev_callback)(int, short, void *arg);
	void *ev_arg;
	/* 标志事件结果。写事件还是读事件，超时事件*/
	//标志着是什么事件发生，信号还是超时等等。
	int ev_res;		/* result passed to event callback */
	int ev_flags; //标志是哪个队列，活动队列，信号队列
};

/* 别致的写法*/
#define EVENT_SIGNAL(ev)	(int)ev->ev_fd
#define EVENT_FD(ev)		(int)ev->ev_fd

#ifdef _EVENT_DEFINED_TQENTRY
#undef TAILQ_ENTRY
#undef _EVENT_DEFINED_TQENTRY
#else
/*
定义event_list为双向列表
为什么双向队列的尾指针要是二维指针。
因为它指向了前一个元素的next指针。前一个元素的next指针是一维指针
*/
TAILQ_HEAD (event_list, event);
#endif /* _EVENT_DEFINED_TQENTRY */
#ifdef _EVENT_DEFINED_RBENTRY
#undef RB_ENTRY
#undef _EVENT_DEFINED_RBENTRY
#endif /* _EVENT_DEFINED_RBENTRY */

struct eventop {
	char *name;
	void *(*init)(void);// void * 是返回值
	int (*add)(void *, struct event *);
	int (*del)(void *, struct event *);
	int (*recalc)(void *, int);
	int (*dispatch)(void *, struct timeval *);
};

#define TIMEOUT_DEFAULT	{5, 0}

void event_init(void);
int event_dispatch(void);

#define EVLOOP_ONCE	0x01
#define EVLOOP_NONBLOCK	0x02
int event_loop(int);
int event_loopexit(struct timeval *);	/* Causes the loop to exit */

int timeout_next(struct timeval *);
void timeout_correct(struct timeval *);
void timeout_process(void);

#define evtimer_add(ev, tv)		event_add(ev, tv)
#define evtimer_set(ev, cb, arg)	event_set(ev, -1, 0, cb, arg)
#define evtimer_del(ev)			event_del(ev)
#define evtimer_pending(ev, tv)		event_pending(ev, EV_TIMEOUT, tv)
#define evtimer_initialized(ev)		((ev)->ev_flags & EVLIST_INIT)

#define timeout_add(ev, tv)		event_add(ev, tv)
#define timeout_set(ev, cb, arg)	event_set(ev, -1, 0, cb, arg)
#define timeout_del(ev)			event_del(ev)
#define timeout_pending(ev, tv)		event_pending(ev, EV_TIMEOUT, tv)
#define timeout_initialized(ev)		((ev)->ev_flags & EVLIST_INIT)

#define signal_add(ev, tv)		event_add(ev, tv)
#define signal_set(ev, x, cb, arg)	\
	event_set(ev, x, EV_SIGNAL|EV_PERSIST, cb, arg)
#define signal_del(ev)			event_del(ev)
#define signal_pending(ev, tv)		event_pending(ev, EV_SIGNAL, tv)
#define signal_initialized(ev)		((ev)->ev_flags & EVLIST_INIT)

void event_set(struct event *, int, short, void (*)(int, short, void *), void *);
int event_once(int, short, void (*)(int, short, void *), void *, struct timeval *);

int event_add(struct event *, struct timeval *);
int event_del(struct event *);
void event_active(struct event *, int, short);

int event_pending(struct event *, short, struct timeval *);

#ifdef WIN32
#define event_initialized(ev)		((ev)->ev_flags & EVLIST_INIT && (ev)->ev_fd != INVALID_HANDLE_VALUE)
#else
#define event_initialized(ev)		((ev)->ev_flags & EVLIST_INIT)
#endif

/* 
 * These functions deal with buffering input and output 
 orig_buffer            buffer
 |< -------misalign------->|<-------off------->|-------- |  
 |< --------------- totallen --------------------------->|
 buffer会根据使用情况持续往前跳跃。初始时buffer等于orig_buffer
*/
struct evbuffer {
	u_char *buffer;
	u_char *orig_buffer;//新增保留原始数据

	size_t misalign;//这个是已经使用过了的长度.
	size_t totallen;//这个是总长度
	size_t off;//这个是当前已使用buffer的长度
	/* 注册回调函数，当buffer变化时。*/
	void (*cb)(struct evbuffer *, size_t, size_t, void *);
	void *cbarg;
};

/* Just for error reporting - use other constants otherwise */
#define EVBUFFER_READ		0x01
#define EVBUFFER_WRITE		0x02
#define EVBUFFER_EOF		0x10
#define EVBUFFER_ERROR		0x20
#define EVBUFFER_TIMEOUT	0x40

struct bufferevent;
typedef void (*evbuffercb)(struct bufferevent *, void *);
typedef void (*everrorcb)(struct bufferevent *, short what, void *);

struct event_watermark {
	size_t low;
	size_t high;
};

/*
读写buff的事件
*/
struct bufferevent {
	struct event ev_read;//这边怎么不是用*号，为什么不用*号。为什么声明为成员变量，非指针
	struct event ev_write;

	struct evbuffer *input;
	struct evbuffer *output;

	struct event_watermark wm_read;
	struct event_watermark wm_write;

	evbuffercb readcb;
	evbuffercb writecb;
	everrorcb errorcb;
	void *cbarg;

	int timeout_read;	/* in seconds */
	int timeout_write;	/* in seconds 写成功的超时时间，还是写成功用了多少时间？*/

	short enabled;	/* events that are currently enabled 标志现在是什么事件可用（EV_READ。这几个事件）*/
};

struct bufferevent *bufferevent_new(int fd,
    evbuffercb readcb, evbuffercb writecb, everrorcb errorcb, void *cbarg);
void bufferevent_free(struct bufferevent *bufev);
int bufferevent_write(struct bufferevent *bufev, void *data, size_t size);
int bufferevent_write_buffer(struct bufferevent *bufev, struct evbuffer *buf);
size_t bufferevent_read(struct bufferevent *bufev, void *data, size_t size);
int bufferevent_enable(struct bufferevent *bufev, short event);
int bufferevent_disable(struct bufferevent *bufev, short event);
void bufferevent_settimeout(struct bufferevent *bufev,
    int timeout_read, int timeout_write);

#define EVBUFFER_LENGTH(x)	(x)->off
#define EVBUFFER_DATA(x)	(x)->buffer
#define EVBUFFER_INPUT(x)	(x)->input
#define EVBUFFER_OUTPUT(x)	(x)->output

struct evbuffer *evbuffer_new(void);
void evbuffer_free(struct evbuffer *);
int evbuffer_add(struct evbuffer *, void *, size_t);
int evbuffer_remove(struct evbuffer *, void *, size_t);
int evbuffer_add_buffer(struct evbuffer *, struct evbuffer *);
int evbuffer_add_printf(struct evbuffer *, char *fmt, ...);
// 清空evbuffer里的mis_align的内容。即是对齐。
void evbuffer_drain(struct evbuffer *, size_t);
int evbuffer_write(struct evbuffer *, int);
int evbuffer_read(struct evbuffer *, int, int);
// 在evbuffer中查找字符串
u_char *evbuffer_find(struct evbuffer *, u_char *, size_t);
void evbuffer_setcb(struct evbuffer *, void (*)(struct evbuffer *, size_t, size_t, void *), void *);

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_H_ */

/*
 * The driver's tasklets: what an interrupt handler leaves for later. Linux runs them on the
 * soft interrupt of the processor that asked for them, before it goes back to what it was
 * doing; there is no soft interrupt here, and the driver's code never runs in an interrupt at
 * all (lx_base.c: "nothing here is called from an interrupt"), so what is left for later is
 * left for a thread of its own, which this is.
 *
 * One queue, and one thread that takes from it and calls what it finds. A tasklet that is
 * asked for twice before it has run runs once: that is Linux's promise, and the driver's
 * interrupt handlers are written against it. tasklet_kill() waits for one that is running
 * and takes one that is only waiting out of the queue: the driver calls it as it stops.
 */
#include <sys/queue.h>

#include <linux/interrupt.h>
#include <linux/spinlock.h>

#include <lx_os.h>
#include "lx.h"

/* 0: it is neither waiting nor running; 1: waiting in the queue; 2: running on the thread */
#define	TASKLET_IDLE		0
#define	TASKLET_WAITING		1
#define	TASKLET_RUNNING		2

static struct {
	TAILQ_HEAD(, tasklet_struct) waiting;
	int thread_made;
} tasklets;

static const char work;			/* what the thread sleeps on until there is some */

static void
tasklets_run(void *argument)
{
	struct tasklet_struct *t;

	for (;;) {
		lx_os_lock();
		while (TAILQ_EMPTY(&tasklets.waiting))
			lx_os_sleep(&work, 0);
		t = TAILQ_FIRST(&tasklets.waiting);
		TAILQ_REMOVE(&tasklets.waiting, t, entry);
		t->tasklet_state = TASKLET_RUNNING;
		lx_os_unlock();

		if (t->use_callback)
			t->callback(t);
		else
			t->func(t->data);

		lx_os_lock();
		t->tasklet_state = TASKLET_IDLE;
		lx_os_wakeup(t);		/* whoever waits in tasklet_kill() */
		lx_os_unlock();
	}
}

/* The thread is made the first time a tasklet is asked for: there is no start to call. */
static void
tasklets_start(void)
{
	if (__sync_bool_compare_and_swap(&tasklets.thread_made, 0, 1)) {
		TAILQ_INIT(&tasklets.waiting);
		if (lx_os_thread("wifi-tasklet", tasklets_run, NULL) != 0)
			tasklets.thread_made = 0;
	}
}

void
tasklet_setup(struct tasklet_struct *t, tasklet_callback_t *callback)
{
	tasklets_start();

	t->callback = callback;
	t->use_callback = true;
	t->func = NULL;
	t->data = 0;
	t->tasklet_state = TASKLET_IDLE;
	atomic_set(&t->count, 0);
}

void
tasklet_schedule(struct tasklet_struct *t)
{
	tasklets_start();

	lx_os_lock();
	if (t->tasklet_state == TASKLET_IDLE) {
		t->tasklet_state = TASKLET_WAITING;
		TAILQ_INSERT_TAIL(&tasklets.waiting, t, entry);
		lx_os_wakeup(&work);
	}
	lx_os_unlock();
}

void
tasklet_kill(struct tasklet_struct *t)
{
	lx_os_lock();
	if (t->tasklet_state == TASKLET_WAITING) {
		/* it has not begun: it never will */
		TAILQ_REMOVE(&tasklets.waiting, t, entry);
		t->tasklet_state = TASKLET_IDLE;
	} else {
		while (t->tasklet_state == TASKLET_RUNNING)
			lx_os_sleep(t, 0);
	}
	lx_os_unlock();
}

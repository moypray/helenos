/*
 * Copyright (c) 2009 Jakub Jermar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
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

/** @addtogroup libc
 * @{
 */
/** @file
 */

#include <fibril_synch.h>
#include <fibril.h>
#include <async.h>
#include <adt/list.h>
#include <futex.h>
#include <sys/time.h>
#include <errno.h>
#include <assert.h>
#include <stacktrace.h>
#include <stdlib.h>
#include <stdio.h>
#include <io/kio.h>
#include <mem.h>
#include <context.h>

#include "private/async.h"
#include "private/fibril.h"

static fibril_local bool deadlocked = false;

typedef struct {
	link_t link;
	fibril_event_t event;
	fibril_mutex_t *mutex;
	fid_t fid;
} awaiter_t;

#define AWAITER_INIT { .fid = fibril_get_id() }

static void print_deadlock(fibril_owner_info_t *oi)
{
	// FIXME: Print to stderr.

	fibril_t *f = (fibril_t *) fibril_get_id();

	if (deadlocked) {
		kio_printf("Deadlock detected while printing deadlock. Aborting.\n");
		abort();
	}
	deadlocked = true;

	printf("Deadlock detected.\n");
	stacktrace_print();

	printf("Fibril %p waits for primitive %p.\n", f, oi);

	while (oi && oi->owned_by) {
		printf("Primitive %p is owned by fibril %p.\n",
		    oi, oi->owned_by);
		if (oi->owned_by == f)
			break;
		stacktrace_print_fp_pc(
		    context_get_fp(&oi->owned_by->ctx),
		    context_get_pc(&oi->owned_by->ctx));
		printf("Fibril %p waits for primitive %p.\n",
		    oi->owned_by, oi->owned_by->waits_for);
		oi = oi->owned_by->waits_for;
	}
}


static void check_fibril_for_deadlock(fibril_owner_info_t *oi, fibril_t *fib)
{
	futex_assert_is_locked(&async_futex);

	while (oi && oi->owned_by) {
		if (oi->owned_by == fib) {
			futex_unlock(&async_futex);
			print_deadlock(oi);
			abort();
		}
		oi = oi->owned_by->waits_for;
	}
}

static void check_for_deadlock(fibril_owner_info_t *oi)
{
	check_fibril_for_deadlock(oi, fibril_self());
}

void fibril_mutex_initialize(fibril_mutex_t *fm)
{
	fm->oi.owned_by = NULL;
	fm->counter = 1;
	list_initialize(&fm->waiters);
}

void fibril_mutex_lock(fibril_mutex_t *fm)
{
	fibril_t *f = (fibril_t *) fibril_get_id();

	futex_lock(&async_futex);

	if (fm->counter-- > 0) {
		fm->oi.owned_by = f;
		futex_unlock(&async_futex);
		return;
	}

	awaiter_t wdata = AWAITER_INIT;
	list_append(&wdata.link, &fm->waiters);
	check_for_deadlock(&fm->oi);
	f->waits_for = &fm->oi;

	futex_unlock(&async_futex);

	fibril_wait_for(&wdata.event);
}

bool fibril_mutex_trylock(fibril_mutex_t *fm)
{
	bool locked = false;

	futex_lock(&async_futex);
	if (fm->counter > 0) {
		fm->counter--;
		fm->oi.owned_by = (fibril_t *) fibril_get_id();
		locked = true;
	}
	futex_unlock(&async_futex);

	return locked;
}

static void _fibril_mutex_unlock_unsafe(fibril_mutex_t *fm)
{
	assert(fm->oi.owned_by == (fibril_t *) fibril_get_id());

	if (fm->counter++ < 0) {
		awaiter_t *wdp = list_pop(&fm->waiters, awaiter_t, link);
		assert(wdp);

		fibril_t *f = (fibril_t *) wdp->fid;
		fm->oi.owned_by = f;
		f->waits_for = NULL;

		fibril_notify(&wdp->event);
	} else {
		fm->oi.owned_by = NULL;
	}
}

void fibril_mutex_unlock(fibril_mutex_t *fm)
{
	futex_lock(&async_futex);
	_fibril_mutex_unlock_unsafe(fm);
	futex_unlock(&async_futex);
}

bool fibril_mutex_is_locked(fibril_mutex_t *fm)
{
	futex_lock(&async_futex);
	bool locked = (fm->oi.owned_by == (fibril_t *) fibril_get_id());
	futex_unlock(&async_futex);
	return locked;
}

void fibril_rwlock_initialize(fibril_rwlock_t *frw)
{
	frw->oi.owned_by = NULL;
	frw->writers = 0;
	frw->readers = 0;
	list_initialize(&frw->waiters);
}

void fibril_rwlock_read_lock(fibril_rwlock_t *frw)
{
	fibril_t *f = (fibril_t *) fibril_get_id();

	futex_lock(&async_futex);

	if (!frw->writers) {
		/* Consider the first reader the owner. */
		if (frw->readers++ == 0)
			frw->oi.owned_by = f;
		futex_unlock(&async_futex);
		return;
	}

	f->is_writer = false;

	awaiter_t wdata = AWAITER_INIT;
	list_append(&wdata.link, &frw->waiters);
	check_for_deadlock(&frw->oi);
	f->waits_for = &frw->oi;

	futex_unlock(&async_futex);

	fibril_wait_for(&wdata.event);
}

void fibril_rwlock_write_lock(fibril_rwlock_t *frw)
{
	fibril_t *f = (fibril_t *) fibril_get_id();

	futex_lock(&async_futex);

	if (!frw->writers && !frw->readers) {
		frw->oi.owned_by = f;
		frw->writers++;
		futex_unlock(&async_futex);
		return;
	}

	f->is_writer = true;

	awaiter_t wdata = AWAITER_INIT;
	list_append(&wdata.link, &frw->waiters);
	check_for_deadlock(&frw->oi);
	f->waits_for = &frw->oi;

	futex_unlock(&async_futex);

	fibril_wait_for(&wdata.event);
}

static void _fibril_rwlock_common_unlock(fibril_rwlock_t *frw)
{
	if (frw->readers) {
		if (--frw->readers) {
			if (frw->oi.owned_by == (fibril_t *) fibril_get_id()) {
				/*
				 * If this reader fibril was considered the
				 * owner of this rwlock, clear the ownership
				 * information even if there are still more
				 * readers.
				 *
				 * This is the limitation of the detection
				 * mechanism rooted in the fact that tracking
				 * all readers would require dynamically
				 * allocated memory for keeping linkage info.
				 */
				frw->oi.owned_by = NULL;
			}

			return;
		}
	} else {
		frw->writers--;
	}

	assert(!frw->readers && !frw->writers);

	frw->oi.owned_by = NULL;

	while (!list_empty(&frw->waiters)) {
		link_t *tmp = list_first(&frw->waiters);
		awaiter_t *wdp;
		fibril_t *f;

		wdp = list_get_instance(tmp, awaiter_t, link);
		f = (fibril_t *) wdp->fid;

		if (f->is_writer) {
			if (frw->readers)
				break;
			frw->writers++;
		} else {
			frw->readers++;
		}

		f->waits_for = NULL;
		list_remove(&wdp->link);
		frw->oi.owned_by = f;
		fibril_notify(&wdp->event);

		if (frw->writers)
			break;
	}
}

void fibril_rwlock_read_unlock(fibril_rwlock_t *frw)
{
	futex_lock(&async_futex);
	assert(frw->readers > 0);
	_fibril_rwlock_common_unlock(frw);
	futex_unlock(&async_futex);
}

void fibril_rwlock_write_unlock(fibril_rwlock_t *frw)
{
	futex_lock(&async_futex);
	assert(frw->writers == 1);
	assert(frw->oi.owned_by == fibril_self());
	_fibril_rwlock_common_unlock(frw);
	futex_unlock(&async_futex);
}

bool fibril_rwlock_is_read_locked(fibril_rwlock_t *frw)
{
	futex_lock(&async_futex);
	bool locked = (frw->readers > 0);
	futex_unlock(&async_futex);
	return locked;
}

bool fibril_rwlock_is_write_locked(fibril_rwlock_t *frw)
{
	futex_lock(&async_futex);
	assert(frw->writers <= 1);
	bool locked = (frw->writers > 0) && (frw->oi.owned_by == fibril_self());
	futex_unlock(&async_futex);
	return locked;
}

bool fibril_rwlock_is_locked(fibril_rwlock_t *frw)
{
	return fibril_rwlock_is_read_locked(frw) ||
	    fibril_rwlock_is_write_locked(frw);
}

void fibril_condvar_initialize(fibril_condvar_t *fcv)
{
	list_initialize(&fcv->waiters);
}

/**
 * FIXME: If `timeout` is negative, the function returns ETIMEOUT immediately,
 *        and if `timeout` is 0, the wait never times out.
 *        This is not consistent with other similar APIs.
 */
errno_t
fibril_condvar_wait_timeout(fibril_condvar_t *fcv, fibril_mutex_t *fm,
    suseconds_t timeout)
{
	assert(fibril_mutex_is_locked(fm));

	if (timeout < 0)
		return ETIMEOUT;

	awaiter_t wdata = AWAITER_INIT;
	wdata.mutex = fm;

	struct timeval tv;
	struct timeval *expires = NULL;
	if (timeout) {
		getuptime(&tv);
		tv_add_diff(&tv, timeout);
		expires = &tv;
	}

	futex_lock(&async_futex);
	_fibril_mutex_unlock_unsafe(fm);
	list_append(&wdata.link, &fcv->waiters);
	futex_unlock(&async_futex);

	(void) fibril_wait_timeout(&wdata.event, expires);

	futex_lock(&async_futex);
	bool timed_out = link_in_use(&wdata.link);
	list_remove(&wdata.link);
	futex_unlock(&async_futex);

	fibril_mutex_lock(fm);

	return timed_out ? ETIMEOUT : EOK;
}

void fibril_condvar_wait(fibril_condvar_t *fcv, fibril_mutex_t *fm)
{
	(void) fibril_condvar_wait_timeout(fcv, fm, 0);
}

void fibril_condvar_signal(fibril_condvar_t *fcv)
{
	futex_lock(&async_futex);

	awaiter_t *w = list_pop(&fcv->waiters, awaiter_t, link);
	if (w != NULL)
		fibril_notify(&w->event);

	futex_unlock(&async_futex);
}

void fibril_condvar_broadcast(fibril_condvar_t *fcv)
{
	futex_lock(&async_futex);

	awaiter_t *w;
	while ((w = list_pop(&fcv->waiters, awaiter_t, link)))
		fibril_notify(&w->event);

	futex_unlock(&async_futex);
}

/** Timer fibril.
 *
 * @param arg	Timer
 */
static errno_t fibril_timer_func(void *arg)
{
	fibril_timer_t *timer = (fibril_timer_t *) arg;
	errno_t rc;

	fibril_mutex_lock(timer->lockp);

	while (timer->state != fts_cleanup) {
		switch (timer->state) {
		case fts_not_set:
		case fts_fired:
			fibril_condvar_wait(&timer->cv, timer->lockp);
			break;
		case fts_active:
			rc = fibril_condvar_wait_timeout(&timer->cv,
			    timer->lockp, timer->delay);
			if (rc == ETIMEOUT && timer->state == fts_active) {
				timer->state = fts_fired;
				timer->handler_fid = fibril_get_id();
				fibril_mutex_unlock(timer->lockp);
				timer->fun(timer->arg);
				fibril_mutex_lock(timer->lockp);
				timer->handler_fid = 0;
			}
			break;
		case fts_cleanup:
		case fts_clean:
			assert(false);
			break;
		}
	}

	/* Acknowledge timer fibril has finished cleanup. */
	timer->state = fts_clean;
	fibril_condvar_broadcast(&timer->cv);
	fibril_mutex_unlock(timer->lockp);

	return 0;
}

/** Create new timer.
 *
 * @return		New timer on success, @c NULL if out of memory.
 */
fibril_timer_t *fibril_timer_create(fibril_mutex_t *lock)
{
	fid_t fid;
	fibril_timer_t *timer;

	timer = calloc(1, sizeof(fibril_timer_t));
	if (timer == NULL)
		return NULL;

	fid = fibril_create(fibril_timer_func, (void *) timer);
	if (fid == 0) {
		free(timer);
		return NULL;
	}

	fibril_mutex_initialize(&timer->lock);
	fibril_condvar_initialize(&timer->cv);

	timer->fibril = fid;
	timer->state = fts_not_set;
	timer->lockp = (lock != NULL) ? lock : &timer->lock;

	fibril_add_ready(fid);
	return timer;
}

/** Destroy timer.
 *
 * @param timer		Timer, must not be active or accessed by other threads.
 */
void fibril_timer_destroy(fibril_timer_t *timer)
{
	fibril_mutex_lock(timer->lockp);
	assert(timer->state == fts_not_set || timer->state == fts_fired);

	/* Request timer fibril to terminate. */
	timer->state = fts_cleanup;
	fibril_condvar_broadcast(&timer->cv);

	/* Wait for timer fibril to terminate */
	while (timer->state != fts_clean)
		fibril_condvar_wait(&timer->cv, timer->lockp);
	fibril_mutex_unlock(timer->lockp);

	free(timer);
}

/** Set timer.
 *
 * Set timer to execute a callback function after the specified
 * interval.
 *
 * @param timer		Timer
 * @param delay		Delay in microseconds
 * @param fun		Callback function
 * @param arg		Argument for @a fun
 */
void fibril_timer_set(fibril_timer_t *timer, suseconds_t delay,
    fibril_timer_fun_t fun, void *arg)
{
	fibril_mutex_lock(timer->lockp);
	fibril_timer_set_locked(timer, delay, fun, arg);
	fibril_mutex_unlock(timer->lockp);
}

/** Set locked timer.
 *
 * Set timer to execute a callback function after the specified
 * interval. Must be called when the timer is locked.
 *
 * @param timer		Timer
 * @param delay		Delay in microseconds
 * @param fun		Callback function
 * @param arg		Argument for @a fun
 */
void fibril_timer_set_locked(fibril_timer_t *timer, suseconds_t delay,
    fibril_timer_fun_t fun, void *arg)
{
	assert(fibril_mutex_is_locked(timer->lockp));
	assert(timer->state == fts_not_set || timer->state == fts_fired);
	timer->state = fts_active;
	timer->delay = delay;
	timer->fun = fun;
	timer->arg = arg;
	fibril_condvar_broadcast(&timer->cv);
}

/** Clear timer.
 *
 * Clears (cancels) timer and returns last state of the timer.
 * This can be one of:
 *    - fts_not_set	If the timer has not been set or has been cleared
 *    - fts_active	Timer was set but did not fire
 *    - fts_fired	Timer fired
 *
 * @param timer		Timer
 * @return		Last timer state
 */
fibril_timer_state_t fibril_timer_clear(fibril_timer_t *timer)
{
	fibril_timer_state_t old_state;

	fibril_mutex_lock(timer->lockp);
	old_state = fibril_timer_clear_locked(timer);
	fibril_mutex_unlock(timer->lockp);

	return old_state;
}

/** Clear locked timer.
 *
 * Clears (cancels) timer and returns last state of the timer.
 * This can be one of:
 *    - fts_not_set	If the timer has not been set or has been cleared
 *    - fts_active	Timer was set but did not fire
 *    - fts_fired	Timer fired
 * Must be called when the timer is locked.
 *
 * @param timer		Timer
 * @return		Last timer state
 */
fibril_timer_state_t fibril_timer_clear_locked(fibril_timer_t *timer)
{
	fibril_timer_state_t old_state;

	assert(fibril_mutex_is_locked(timer->lockp));

	while (timer->handler_fid != 0) {
		if (timer->handler_fid == fibril_get_id()) {
			printf("Deadlock detected.\n");
			stacktrace_print();
			printf("Fibril %p is trying to clear timer %p from "
			    "inside its handler %p.\n",
			    fibril_get_id(), timer, timer->fun);
			abort();
		}

		fibril_condvar_wait(&timer->cv, timer->lockp);
	}

	old_state = timer->state;
	timer->state = fts_not_set;

	timer->delay = 0;
	timer->fun = NULL;
	timer->arg = NULL;
	fibril_condvar_broadcast(&timer->cv);

	return old_state;
}

/**
 * Initialize a semaphore with initial count set to the provided value.
 *
 * @param sem    Semaphore to initialize.
 * @param count  Initial count. Must not be negative.
 */
void fibril_semaphore_initialize(fibril_semaphore_t *sem, long count)
{
	/*
	 * Negative count denotes the length of waitlist,
	 * so it makes no sense as an initial value.
	 */
	assert(count >= 0);
	sem->count = count;
	list_initialize(&sem->waiters);
}

/**
 * Produce one token.
 * If there are fibrils waiting for tokens, this operation satisfies
 * exactly one waiting `fibril_semaphore_down()`.
 * This operation never blocks the fibril.
 *
 * @param sem  Semaphore to use.
 */
void fibril_semaphore_up(fibril_semaphore_t *sem)
{
	futex_lock(&async_futex);
	sem->count++;

	if (sem->count <= 0) {
		awaiter_t *w = list_pop(&sem->waiters, awaiter_t, link);
		assert(w);
		fibril_notify(&w->event);
	}

	futex_unlock(&async_futex);
}

/**
 * Consume one token.
 * If there are no available tokens (count <= 0), this operation blocks until
 * another fibril produces a token using `fibril_semaphore_up()`.
 *
 * @param sem  Semaphore to use.
 */
void fibril_semaphore_down(fibril_semaphore_t *sem)
{
	futex_lock(&async_futex);
	sem->count--;

	if (sem->count >= 0) {
		futex_unlock(&async_futex);
		return;
	}

	awaiter_t wdata = AWAITER_INIT;
	list_append(&wdata.link, &sem->waiters);

	futex_unlock(&async_futex);

	fibril_wait_for(&wdata.event);
}

/** @}
 */

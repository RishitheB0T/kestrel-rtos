/*
 * sync.c - semaphores, mutexes with priority inheritance, message queues.
 *
 * Kernel objects live in kernel RAM. Tasks only ever hold a *handle* (a
 * small integer), so a buggy task cannot corrupt a queue's internals: every
 * handle is checked here before use.
 *
 * Handle layout: (type << 8) | index, so passing a semaphore handle where a
 * mutex is expected is caught as K_EINVAL instead of silently misbehaving.
 */
#include <string.h>

#include "kernel.h"

#define K_MAX_SEMS       16
#define K_MAX_MUTEXES    16
#define K_MAX_QUEUES     8
#define K_QUEUE_POOL     1024u

#define HTYPE_SEM   1u
#define HTYPE_MUTEX 2u
#define HTYPE_QUEUE 3u

typedef struct {
    uint8_t used;
    uint16_t count, max;
    k_list_t waiters;
} k_sem_t;

typedef struct k_mutex {
    uint8_t used, flags, owner_died;
    k_tcb_t *owner;
    k_list_t waiters;
} k_mutex_t;

typedef struct {
    uint8_t used;
    uint16_t item_size, depth, head, count;
    uint8_t *buf;
    k_list_t send_waiters, recv_waiters;
} k_queue_t;

static k_sem_t sems[K_MAX_SEMS];
static k_mutex_t mutexes[K_MAX_MUTEXES];
static k_queue_t queues[K_MAX_QUEUES];
static uint8_t queue_pool[K_QUEUE_POOL] __attribute__((aligned(4)));
static uint32_t queue_pool_used;

void k_sync_init(void) {}

static void *handle_lookup(uint32_t h, uint32_t type, void *pool, uint32_t n, uint32_t sz)
{
    uint32_t idx = h & 0xFFu;
    if ((h >> 8) != type || idx >= n)
        return NULL;
    uint8_t *obj = (uint8_t *)pool + idx * sz;
    return *obj ? obj : NULL; /* first byte of every object is `used` */
}

#define SEM(h)   ((k_sem_t *)handle_lookup(h, HTYPE_SEM, sems, K_MAX_SEMS, sizeof(k_sem_t)))
#define MUTEX(h) ((k_mutex_t *)handle_lookup(h, HTYPE_MUTEX, mutexes, K_MAX_MUTEXES, sizeof(k_mutex_t)))
#define QUEUE(h) ((k_queue_t *)handle_lookup(h, HTYPE_QUEUE, queues, K_MAX_QUEUES, sizeof(k_queue_t)))

/* ======================================================================
 * Semaphores
 * ====================================================================== */

int32_t sys_sem_create(uint32_t initial, uint32_t max, uint32_t a2, uint32_t a3)
{
    (void)a2, (void)a3;
    if (max == 0 || max > 0xFFFFu || initial > max)
        return K_EINVAL;
    for (uint32_t i = 0; i < K_MAX_SEMS; i++) {
        k_sem_t *s = &sems[i];
        if (!s->used) {
            s->used = 1;
            s->count = (uint16_t)initial;
            s->max = (uint16_t)max;
            list_init(&s->waiters);
            return (int32_t)((HTYPE_SEM << 8) | i);
        }
    }
    return K_ENOMEM;
}

int32_t sys_sem_take(uint32_t h, uint32_t timeout, uint32_t a2, uint32_t a3)
{
    (void)a2, (void)a3;
    k_sem_t *s = SEM(h);
    if (!s)
        return K_EINVAL;
    if (s->count) {
        s->count--;
        return K_OK;
    }
    if (timeout == K_NO_WAIT)
        return K_EAGAIN;
    k_block_current(&s->waiters, timeout);
    return K_PENDING;
}

int32_t sys_sem_give(uint32_t h, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    k_sem_t *s = SEM(h);
    if (!s)
        return K_EINVAL;
    k_tcb_t *w = k_wq_first(&s->waiters);
    if (w) {
        k_wake(w, K_OK); /* hand the unit straight to the waiter */
        k_reschedule();
        return K_OK;
    }
    if (s->count >= s->max)
        return K_EAGAIN;
    s->count++;
    return K_OK;
}

/* ======================================================================
 * Mutexes with priority inheritance
 *
 * Priority inversion: a high-priority task H waits for a mutex held by a
 * low-priority task L, while a medium task M (which needs no mutex) keeps
 * preempting L. H is effectively stuck behind M. (This hung the Mars
 * Pathfinder lander in 1997.)
 *
 * Fix: while L holds a mutex that H wants, L runs at H's priority, so M can
 * no longer preempt it. Inheritance is transitive: if L is itself waiting
 * for a mutex held by K, K is boosted too.
 * ====================================================================== */

static uint8_t top_waiter_prio(const k_mutex_t *m)
{
    k_tcb_t *w = k_wq_first((k_list_t *)&m->waiters);
    return w ? w->prio : 0;
}

/* Effective priority = max(own base priority, top waiter of every
 * inheritance-enabled mutex the task holds). Returns 1 if it changed. */
static int recompute_prio(k_tcb_t *t)
{
    uint8_t p = t->base_prio;
    for (int i = 0; i < K_MAX_MUTEXES; i++) {
        k_mutex_t *m = &mutexes[i];
        if (m->used && m->owner == t && !(m->flags & K_MUTEX_NO_PI)) {
            uint8_t w = top_waiter_prio(m);
            if (w > p)
                p = w;
        }
    }
    if (p == t->prio)
        return 0;
    k_set_prio(t, p);
    return 1;
}

/* Walk the "owner is blocked on another mutex" chain, updating each owner.
 * The depth bound also protects against cycles. */
static void pi_update_chain(k_tcb_t *t)
{
    for (int depth = 0; t && depth < K_MAX_TASKS; depth++) {
        if (!recompute_prio(t))
            break;
        k_mutex_t *next = t->blocked_on;
        if (!next || (next->flags & K_MUTEX_NO_PI))
            break;
        t = next->owner;
    }
}

void k_mutex_waiter_gone(struct k_mutex *m)
{
    if (m->owner && !(m->flags & K_MUTEX_NO_PI))
        pi_update_chain(m->owner);
}

/* Give the mutex to its best waiter (or free it). */
static void mutex_handoff(k_mutex_t *m, int32_t result)
{
    k_tcb_t *next = k_wq_first(&m->waiters);
    m->owner = next;
    if (!next)
        return;
    k_wake(next, result);
    if (!(m->flags & K_MUTEX_NO_PI))
        recompute_prio(next); /* remaining waiters may outrank the new owner */
}

int32_t sys_mutex_create(uint32_t flags, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    for (uint32_t i = 0; i < K_MAX_MUTEXES; i++) {
        k_mutex_t *m = &mutexes[i];
        if (!m->used) {
            m->used = 1;
            m->flags = (uint8_t)(flags & K_MUTEX_NO_PI);
            m->owner_died = 0;
            m->owner = NULL;
            list_init(&m->waiters);
            return (int32_t)((HTYPE_MUTEX << 8) | i);
        }
    }
    return K_ENOMEM;
}

int32_t sys_mutex_lock(uint32_t h, uint32_t timeout, uint32_t a2, uint32_t a3)
{
    (void)a2, (void)a3;
    k_mutex_t *m = MUTEX(h);
    k_tcb_t *cur = k_current;
    if (!m)
        return K_EINVAL;

    if (!m->owner) {
        m->owner = cur;
        if (m->owner_died) { /* previous owner died holding it */
            m->owner_died = 0;
            return K_EOWNERDEAD;
        }
        return K_OK;
    }

    /* Deadlock detection: follow owner -> mutex it waits for -> owner ...
     * If the chain leads back to us, blocking would wait forever. */
    k_tcb_t *o = m->owner;
    for (int depth = 0; o && depth < K_MAX_TASKS; depth++) {
        if (o == cur)
            return K_EDEADLK;
        o = o->blocked_on ? o->blocked_on->owner : NULL;
    }

    if (timeout == K_NO_WAIT)
        return K_EAGAIN;

    cur->blocked_on = m;
    k_block_current(&m->waiters, timeout);
    if (!(m->flags & K_MUTEX_NO_PI))
        pi_update_chain(m->owner);
    k_reschedule();
    return K_PENDING;
}

int32_t sys_mutex_unlock(uint32_t h, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    k_mutex_t *m = MUTEX(h);
    k_tcb_t *cur = k_current;
    if (!m)
        return K_EINVAL;
    if (m->owner != cur)
        return K_EPERM;
    mutex_handoff(m, K_OK);
    recompute_prio(cur); /* drop any priority we inherited through m */
    k_reschedule();
    return K_OK;
}

/* Robust mutexes: if a task dies holding a mutex, the next waiter gets it
 * with K_EOWNERDEAD (so it knows the protected data may be inconsistent),
 * instead of everyone waiting forever. */
void k_mutex_owner_died(k_tcb_t *t)
{
    for (int i = 0; i < K_MAX_MUTEXES; i++) {
        k_mutex_t *m = &mutexes[i];
        if (m->used && m->owner == t) {
            if (k_wq_first(&m->waiters))
                mutex_handoff(m, K_EOWNERDEAD);
            else {
                m->owner = NULL;
                m->owner_died = 1;
            }
        }
    }
}

/* ======================================================================
 * Message queues: fixed-size items copied in and out by the kernel.
 * If a receiver is already waiting, a sent item is copied straight into
 * the receiver's buffer, skipping the ring entirely.
 * ====================================================================== */

int32_t sys_queue_create(uint32_t isz, uint32_t depth, uint32_t a2, uint32_t a3)
{
    (void)a2, (void)a3;
    if (isz == 0 || depth == 0 || isz > 256 || depth > 256)
        return K_EINVAL;
    uint32_t bytes = (isz * depth + 3u) & ~3u;
    if (queue_pool_used + bytes > K_QUEUE_POOL)
        return K_ENOMEM;
    for (uint32_t i = 0; i < K_MAX_QUEUES; i++) {
        k_queue_t *q = &queues[i];
        if (!q->used) {
            q->used = 1;
            q->item_size = (uint16_t)isz;
            q->depth = (uint16_t)depth;
            q->head = q->count = 0;
            q->buf = &queue_pool[queue_pool_used];
            queue_pool_used += bytes;
            list_init(&q->send_waiters);
            list_init(&q->recv_waiters);
            return (int32_t)((HTYPE_QUEUE << 8) | i);
        }
    }
    return K_ENOMEM;
}

static uint8_t *slot(k_queue_t *q, uint32_t i)
{
    return q->buf + ((q->head + i) % q->depth) * q->item_size;
}

int32_t sys_queue_send(uint32_t h, uint32_t item, uint32_t timeout, uint32_t a3)
{
    (void)a3;
    k_queue_t *q = QUEUE(h);
    if (!q)
        return K_EINVAL;
    if (!k_user_can_read(k_current, (const void *)item, q->item_size))
        return K_EFAULT;

    k_tcb_t *r = k_wq_first(&q->recv_waiters);
    if (r) {
        memcpy(r->wait_buf, (const void *)item, q->item_size);
        k_wake(r, K_OK);
        k_reschedule();
        return K_OK;
    }
    if (q->count < q->depth) {
        memcpy(slot(q, q->count), (const void *)item, q->item_size);
        q->count++;
        return K_OK;
    }
    if (timeout == K_NO_WAIT)
        return K_EAGAIN;
    k_current->wait_buf = (void *)item;
    k_block_current(&q->send_waiters, timeout);
    return K_PENDING;
}

int32_t sys_queue_recv(uint32_t h, uint32_t item, uint32_t timeout, uint32_t a3)
{
    (void)a3;
    k_queue_t *q = QUEUE(h);
    if (!q)
        return K_EINVAL;
    if (!k_user_can_write(k_current, (void *)item, q->item_size))
        return K_EFAULT;

    if (q->count) {
        memcpy((void *)item, slot(q, 0), q->item_size);
        q->head = (uint16_t)((q->head + 1u) % q->depth);
        q->count--;
        k_tcb_t *s = k_wq_first(&q->send_waiters);
        if (s) { /* a sender was waiting for space: take its item now */
            memcpy(slot(q, q->count), s->wait_buf, q->item_size);
            q->count++;
            k_wake(s, K_OK);
            k_reschedule();
        }
        return K_OK;
    }
    if (timeout == K_NO_WAIT)
        return K_EAGAIN;
    k_current->wait_buf = (void *)item;
    k_block_current(&q->recv_waiters, timeout);
    return K_PENDING;
}

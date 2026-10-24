/*
 * trace.c - records scheduler events with cycle timestamps.
 *
 * Capture is one-shot: k_trace_start() clears the buffer, events are
 * appended until it is full or k_trace_stop() is called, and user code
 * reads them back with k_trace_read(). tools/trace2perfetto.py turns the
 * dump into a timeline you can open at https://ui.perfetto.dev
 */
#include "kernel.h"

#define K_TRACE_EVENTS 768

static k_trace_ev_t buf[K_TRACE_EVENTS];
static uint32_t count;
static uint8_t enabled;

void k_trace(uint8_t ev, uint8_t tid, uint16_t arg)
{
    if (!enabled || count >= K_TRACE_EVENTS)
        return;
    k_trace_ev_t *e = &buf[count++];
    e->ts = k_now_cycles();
    e->ev = ev;
    e->tid = tid;
    e->arg = arg;
}

int32_t sys_trace_ctl(uint32_t on, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    if (on) {
        count = 0;
        enabled = 1;
        /* Start the timeline with whoever is running right now. */
        k_trace(K_TR_SWITCH, k_current->id, k_current->prio);
    } else {
        enabled = 0;
    }
    return (int32_t)count;
}

int32_t sys_trace_read(uint32_t first, uint32_t out, uint32_t n, uint32_t a3)
{
    (void)a3;
    if (first >= count)
        return 0;
    if (n > count - first)
        n = count - first;
    if (!k_user_can_write(k_current, (void *)out, n * sizeof(k_trace_ev_t)))
        return K_EFAULT;
    k_trace_ev_t *dst = (k_trace_ev_t *)out;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = buf[first + i];
    return (int32_t)n;
}

int32_t sys_trace_mark(uint32_t id, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    k_trace(K_TR_MARK, k_current->id, (uint16_t)id);
    return K_OK;
}

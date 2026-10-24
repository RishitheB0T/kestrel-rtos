/*
 * System call numbers. Shared by the user-side stubs (lib/syscalls.c) and
 * the kernel's dispatch table (kernel/syscall.c). The number is encoded in
 * the `svc #N` instruction itself.
 */
#ifndef KESTREL_SYSCALLS_H
#define KESTREL_SYSCALLS_H

enum {
    SYS_TASK_CREATE,
    SYS_YIELD,
    SYS_SLEEP,
    SYS_EXIT,
    SYS_TASK_INFO,
    SYS_TASK_SELF,
    SYS_TICKS,
    SYS_CYCLES,
    SYS_CPU_CYCLES,
    SYS_CPU_HZ,
    SYS_SEM_CREATE,
    SYS_SEM_TAKE,
    SYS_SEM_GIVE,
    SYS_MUTEX_CREATE,
    SYS_MUTEX_LOCK,
    SYS_MUTEX_UNLOCK,
    SYS_QUEUE_CREATE,
    SYS_QUEUE_SEND,
    SYS_QUEUE_RECV,
    SYS_WRITE,
    SYS_READ,
    SYS_TRACE_CTL,
    SYS_TRACE_READ,
    SYS_TRACE_MARK,
    SYS_REBOOT,
    SYS_HALT,
    SYS_COUNT
};

#endif

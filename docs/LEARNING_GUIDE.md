# Kestrel learning guide: from zero to explaining every line

This guide assumes you know Java and basic programming. It assumes **no** embedded,
OS-internals or C background. Every chapter has the same shape:

1. **The idea**: plain words, no jargon.
2. **Tiny example**: an analogy or a few lines of code.
3. **The proper names**: the terms interviewers use, each defined right away.
4. **In Kestrel**: where it lives in the code.
5. **Go deeper**: interview-level detail. Skip it on your first read.

Read chapters 1–5 first. They're the core. Each one builds on the one before.

---

## 0. A study plan that fits around your OA (30 Oct)

The online assessment tests C/C++, data structures, algorithms and OS basics. This project
teaches the OS part directly, so use it as OS revision instead of an extra task.

| Day | Do this | It also revises |
|---|---|---|
| 1 | Ch. 1–3 (big picture, boot, interrupts). Run `make run` and type commands. | how a program starts, memory layout |
| 2 | Ch. 4–5 (context switch, scheduler). Draw the stack frame on paper. | process vs thread, scheduling algorithms |
| 3 | Ch. 6–7 (system calls, memory protection) | user/kernel mode, segfaults |
| 4 | Ch. 8–9 (mutexes, priority inversion, deadlock, queues) | synchronization: very common OA topic |
| 5 | Ch. 10–11 (driver, ring buffer, tracing) | circular queues, producer/consumer |
| 6 | Ch. 12 (the neural-network part) | the "AI" item in the JD |
| 7 | Ch. 13–14 (testing, bug stories). Explain the whole project out loud in 2 minutes | |

After the OA, do the "make it yours" exercises in chapter 15. That's what makes the
project truly yours in an interview.

---

## 1. The big picture

**The idea.** A microcontroller is a tiny computer with one CPU core and no operating system.
Normally one program runs top to bottom forever. Kestrel adds a small operating system so
**several independent jobs** can share that one CPU. Each job thinks it has the CPU to itself.
The OS also **stops a buggy job from breaking the others**. On top of that, one of the jobs
runs a neural network that recognises handwritten digits.

**Tiny example (analogy).** One chef (the CPU) and many orders (jobs). A head waiter (the OS)
decides which order the chef cooks right now. They interrupt a slow order when an urgent one
arrives, and keep a note of exactly where the chef stopped on each order. If one order
catches fire, the head waiter throws that order out, and the kitchen keeps running.

**The proper names.**
- **Task**: one independent job. It's like a Java `Thread`.
- **Kernel**: the core of the OS, the "head waiter" code. Here, everything in `kernel/`.
- **RTOS** (real-time operating system): an OS designed to react within a predictable time.
  Used in cars, drones and GPU firmware.
- **Bare metal**: code running directly on hardware with nothing underneath it.
- **Cortex-M4**: the CPU design inside your STM32F4 chip, made by ARM. The "F" in M4F means it
  has a floating-point unit (**FPU**: hardware for `float` math).

**In Kestrel.** `kernel/` = the OS. `app/` = the jobs (shell, demos, tests). `nn/` = the
neural-network engine. `tools/` = Python programs that run on your PC.

> **Remember it:** *Kestrel = head waiter (kernel) + many orders (tasks) + one order that
> reads handwriting (the NN).*

---

## 2. How the chip starts: boot

**The idea.** When you power the chip, it doesn't know about `main()`. It reads two numbers
from a fixed place in memory: where its stack should start, and the address of the first
instruction to run. That first piece of code has to set up memory so normal C code can work,
then call `main()`.

**Tiny example.** Java does this for you: the JVM sets up the heap, loads classes and initialises
`static` fields, *then* calls `main`. On bare metal, *we* write that "JVM startup" part, in
about 30 lines.

**The proper names.**
- **Flash**: memory that keeps its contents when power is off. Holds the code. 0x08000000 on STM32.
- **RAM / SRAM**: fast memory that is wiped at power-off. Holds variables. 0x20000000.
- **Vector table**: a list of addresses at the start of flash. Entry 0 is the initial stack
  pointer, entry 1 is the reset handler, and the rest say "when interrupt X happens, run this
  function."
- **Reset handler**: the first function that runs.
- **.data / .bss**: global variables with an initial value (`int x = 5;`) and those without
  (`int y;`, which C guarantees is 0). The reset handler copies `.data` from flash to RAM and
  zeroes `.bss`.
- **Linker script**: a file telling the linker where each kind of thing goes in memory.

**In Kestrel.**
- `kernel/startup.c:36`: the vector table. `kernel/startup.c:62`: `Reset_Handler`, which copies
  `.data`, zeroes `.bss`, turns on the FPU, then calls `main()`.
- `linker/stm32f4.ld`: memory layout. A neat trick: *every object file from `kernel/` is put into
  kernel RAM by its file name*, so no kernel variable can end up where tasks could write it.
- `kernel/boot.c`: `main()` starts the UART, MPU and scheduler, creates the first task `init`,
  and starts the scheduler. `main` never runs again after that.

> **Remember it:** *Power on → read the vector table → Reset_Handler builds the C world → main
> builds the OS → the OS runs tasks forever.*

---

## 3. Interrupts: the doorbell

**The idea.** The CPU runs your code, but hardware sometimes needs attention *right now*: a byte
arrived on the serial port, a timer went off. The hardware rings a "doorbell". The CPU pauses
what it was doing, runs a short function for that doorbell, then continues exactly where it
stopped.

**Tiny example.** You're reading a book (your program). The doorbell rings (interrupt). You put a
bookmark in (save state), answer the door (run the handler), come back and continue from the
bookmark (restore state).

**The proper names.**
- **Interrupt / exception**: the doorbell. ARM calls all of them "exceptions": interrupts from
  peripherals, faults from bad memory access, and `svc` system calls.
- **ISR** (interrupt service routine) or **handler**: the function that runs for it.
- **NVIC**: the hardware that manages interrupt priorities. A higher-priority interrupt can
  interrupt a lower one.
- **SysTick**: a timer built into every Cortex-M. Kestrel sets it to ring 1000 times a second.
  Each ring is a **tick**, the OS's heartbeat for time.
- **Hardware stacking**: on every exception, the CPU *itself* pushes 8 registers (r0–r3, r12,
  lr, pc, xPSR) onto the stack. That's the automatic bookmark.

**In Kestrel.** `SysTick_Handler` (`kernel/sched.c:191`) wakes sleeping tasks and rotates time
slices. `USART2_IRQHandler` (`kernel/console.c:98`) moves bytes in and out of the serial port.

---

## 4. Tasks and the context switch (the heart of an OS)

**The idea.** To pause a task and later resume it, you must save **everything the CPU was
holding for it**: its registers (the CPU's tiny scratch variables) and where its stack was.
Then you load another task's saved values. That swap is the **context switch**.

**Tiny example.** Two students share one calculator. When A hands it to B, A first writes down
every number on the display and in memory. When A gets it back, A types those numbers back in.
A never notices the calculator was shared.

**The proper names.**
- **Registers**: 16 general-purpose 32-bit slots (r0–r15) inside the CPU, plus 32 float
  registers (s0–s31). `sp` (r13) is the **stack pointer**, `lr` (r14) the **link register**
  (return address), `pc` (r15) the **program counter** (the next instruction).
- **Stack**: each task's private scratch memory for local variables and saved registers.
  Each Kestrel task has its own stack.
- **Context**: all of the above for one task.
- **TCB** (task control block): the kernel's record for one task. Saved stack pointer, state,
  priority, name… (`k_tcb_t` in `kernel/kernel.h`).
- **PSP / MSP**: Cortex-M has *two* stack pointers. Tasks use the Process Stack Pointer, the
  kernel and interrupts use the Main Stack Pointer. That keeps kernel data off task stacks.
- **PendSV**: a special low-priority exception that Kestrel uses *only* for switching tasks.

**How the switch works (read with `kernel/context.S:26` open):**
1. Something decides a different task should run (a tick, a task blocking, a task waking).
   It sets the PendSV pending bit. Kestrel never switches in the middle of other kernel work.
2. When every other interrupt has finished, PendSV runs. The CPU has already pushed 8 registers
   onto the current task's stack (hardware stacking).
3. Our assembly pushes the *other* 8 registers (r4–r11), plus the float registers s16–s31
   **only if the task used the FPU**.
4. It calls `k_switch()` (C, `kernel/sched.c:224`), which saves the old stack pointer in the
   old TCB, picks the next task, reprograms memory protection for it, and returns its stack
   pointer.
5. The assembly pops the new task's r4–r11 (and s16–s31), sets the stack pointer, and returns.
   The CPU pops the remaining 8 registers, and the new task continues as if never paused.

A brand-new task has never been paused, so `k_task_create_internal` (`kernel/sched.c:263`)
writes a **fake saved context** onto its stack. Its "saved pc" is the task's entry function, so
the first switch "returns" into the start of the task.

> **Remember it:** *Half the bookmark is written by the CPU, half by PendSV. Switching tasks =
> swapping which stack pointer you restore from.*

**Go deeper.**
- *Why PendSV and not do it inside SysTick?* PendSV has the lowest priority, so a switch never
  happens in the middle of another interrupt, and several "please switch" requests collapse into one.
- *What is EXC_RETURN?* A magic value in `lr` during an exception. Its bits say which stack to
  return to and whether a float frame was pushed (bit 4). We save it per task, so tasks that
  never use floats never pay for saving 16 float registers.
- *Lazy FP stacking:* the CPU even delays saving s0–s15 until someone actually touches the FPU
  in the handler. That optimisation caused this project's best bug (chapter 14).

---

## 5. The scheduler: who runs next?

**The idea.** Each task has a priority number (0–31; bigger = more important). The rule:
**the most important task that is ready always runs.** If several equally important tasks
are ready, they take turns, a few milliseconds each.

**Tiny example (Java).** Like a `PriorityQueue<Task>` where you always run `peek()`. But a
`PriorityQueue` takes O(log n) per operation, and Kestrel picks the next task in O(1).

**How O(1) works.** Keep 32 lists, one per priority, plus one 32-bit number where bit *p* is 1
if list *p* has a ready task:

```c
ready_bitmap = 0b...0000_0100_0000_0101   // tasks ready at priorities 10, 2 and 0
prio = 31 - __builtin_clz(ready_bitmap);  // count leading zeros -> 10
next = first task in ready[10];
```

`CLZ` (count leading zeros) is **one CPU instruction**, so finding the highest priority costs
the same with 2 tasks or 200.

**The proper names.**
- **Preemptive**: the OS can take the CPU away from a task at any moment (vs. *cooperative*,
  where tasks must give it up voluntarily).
- **Ready / running / blocked**: a task is *running* (has the CPU), *ready* (wants it), or
  *blocked* (waiting for something: a timer, a lock, a message).
- **Round robin / time slice**: equal-priority tasks take turns, each for a fixed time
  (`K_SLICE_TICKS` = 5 ms).
- **Idle task**: priority 0, always ready. It runs `wfi` (Wait For Interrupt), which sleeps
  the CPU to save power until the next doorbell.

**In Kestrel.** `ready_highest()` (`kernel/sched.c:52`). Sleeping tasks wait on a list sorted by
wake-up time, so each tick only checks the front of that list. The `round_robin` test checks
that three equal tasks get roughly 1/3 of the CPU each.

**Go deeper.** Tick counters wrap around after 2³² ms (49 days), so times are compared with
`(int32_t)(a - b) < 0`, which stays correct across the wrap (`time_before` in `sched.c`).

---

## 6. Privilege and system calls

**The idea.** If any task could write anywhere, one bug could corrupt the whole system. So tasks
run in a **restricted mode** where they physically cannot touch kernel memory or hardware.
When a task needs the kernel (sleep, lock, print), it executes a special instruction that
**jumps into the kernel through one guarded door**.

**Tiny example.** A hotel guest can't walk into the kitchen. They call room service (the door),
and staff with kitchen access do it for them. Staff check the request makes sense: "deliver
to room 9999" gets refused.

**The proper names.**
- **Privileged / unprivileged**: the two CPU modes. The kernel is privileged; Kestrel's tasks are
  all unprivileged.
- **System call (syscall)**: the request through the door. On ARM, the instruction is `svc #N`
  ("supervisor call"), where N says which service.
- **Confused deputy**: a bug where the kernel (the "deputy" with more power) is tricked into
  misusing that power on a caller's behalf.

**In Kestrel.**
- User side: `lib/user.c`. `k_sleep(10)` puts 10 in register r0 and runs `svc #2`.
- Kernel side: `k_svc_dispatch` (`kernel/syscall.c:249`) reads N from the `svc` instruction itself,
  looks it up in a function table, and writes the result into the task's saved r0.
- **Never trust a pointer from a task.** If a task says "copy the message into address X", the
  kernel first checks X is memory *that task* could write (`k_user_can_write`, `syscall.c:33`).
  Without that check, a task could ask the kernel to copy kernel secrets out. That's the
  confused deputy. The test `syscall_pointer_checks` tries exactly this and expects `K_EFAULT`.
- **Blocking syscalls**: if `k_sem_take` must wait, the handler returns `K_PENDING`. The task is
  marked blocked and another runs. Whoever later wakes it writes the real result into its saved
  r0, so when it resumes, the `svc` "returns" that value.

> **Remember it:** *Tasks live in the guest rooms; `svc` is room service; the kernel checks every
> room number before delivering.*

---

## 7. Memory protection (MPU) and surviving crashes

**The idea.** A small piece of hardware checks **every memory access** against a list of "this
range may be read/written by tasks". If a task touches anything else, the CPU stops it
instantly and runs a fault handler. Kestrel's handler kills **only that task** and keeps
everything else running.

**Tiny example.** Keycards. Each employee's card opens their own office (their stack), the
shared kitchen (shared app data) and the lobby (the code). It doesn't open the server room
(kernel memory) or other people's offices. Each time the head waiter switches employees,
the card readers are reprogrammed for the new person.

**The proper names.**
- **MPU** (Memory Protection Unit): the keycard system. It has 8 **regions**, each a range plus
  rules (read-only? writable? executable?). On Cortex-M4 a region's size must be a power of
  two and its start address a multiple of that size.
- **MemManage fault**: "you touched memory your keycard doesn't open."
- **BusFault**: an access to something that isn't there or isn't allowed at the bus level
  (e.g. a task writing the SysTick registers).
- **UsageFault**: an illegal instruction, e.g. integer divide by zero (Kestrel turns that trap on).
- **Kernel panic**: when the *kernel itself* faults, nothing can be trusted, so print and stop.

**In Kestrel.**
- `k_mpu_init` (`kernel/mpu.c:52`) sets up: region 0 = flash (read-only, runnable), region 1 =
  shared app RAM. `k_mpu_load` (`mpu.c:82`) is called on **every context switch**: region 2 =
  this task's stack, region 3 = an optional "grant" (e.g. one GPIO port for the LED task).
- `k_fault_handler` (`kernel/fault.c:35`) looks at `EXC_RETURN`. If the fault came from a task,
  it records the reason and address, kills the task, and returns. PendSV then switches away,
  so the bad instruction never runs again.
- Try it: `crash stack`, `crash kernel`, `crash null`, `crash periph`, `crash ppb`, `crash div0`,
  `crash otherstack`. The shell survives each one.
- **Why stacks sit right above kernel RAM:** a stack grows *downwards*. If it overflows, it walks
  into kernel RAM or another task's stack, which its keycard doesn't open, so it faults
  immediately instead of silently corrupting data.
- **Grants as capabilities:** a task may give a child access to an I/O window only if it lies
  inside its *own* window. Authority can be passed down but never grown (`sys_task_create`,
  `syscall.c:57`).

**Go deeper.** A null-pointer read on Cortex-M normally *succeeds* (address 0 holds the vector
table!). The MPU makes it fault, just like a Java `NullPointerException`, except enforced by
hardware.

---

## 8. Locks, priority inversion and deadlock

**The idea.** When two tasks share data, they need a lock so only one changes it at a time.
Locks plus priorities create a famous trap: an important task waits for a lock held by an
unimportant task, and a medium task, which needs no lock at all, keeps the unimportant
one from finishing. The important task is stuck behind the medium one. The fix: **while the
unimportant task holds a lock the important one wants, temporarily give it the important one's
priority.**

**Tiny example.** An ambulance (High) waits behind a garbage truck (Low) in a one-lane tunnel
(the lock). Cars (Medium) keep cutting in front of the garbage truck, so the ambulance waits
for all the cars too. The fix: give the garbage truck a siren until it leaves the tunnel.

**The proper names.**
- **Mutex**: a lock with an owner. Like Java's `ReentrantLock`, or a `synchronized` block.
- **Semaphore**: a counter of available "tokens". `take` waits for a token, `give` returns one.
  Java has `java.util.concurrent.Semaphore`.
- **Priority inversion**: the trap above. It hung NASA's Mars Pathfinder lander in 1997. They
  fixed it remotely by turning on priority inheritance in VxWorks.
- **Priority inheritance (PI)**: the fix above.
- **Transitive PI**: if Low is itself waiting for a lock held by Lower, Lower gets boosted too.
- **Deadlock**: A holds lock 1 and waits for lock 2; B holds lock 2 and waits for lock 1.
  Both wait forever.
- **Robust mutex**: if the owner dies while holding the lock, the next waiter gets it with an
  "owner died" warning instead of waiting forever.

**In Kestrel.**
- `recompute_prio` (`kernel/sync.c:141`): effective priority = max(own priority, the top waiter
  of every PI mutex it holds). `pi_update_chain` (`sync.c:160`) walks owner → lock it waits for
  → owner, boosting each one.
- Deadlock detection in `sys_mutex_lock` (`sync.c:207`): before blocking, follow the chain of
  owners. If it leads back to the caller, return `K_EDEADLK` instead of hanging.
- Robust mutexes: `k_mutex_owner_died` (`sync.c:262`) runs when a task is killed.
- **See it:** shell command `pi` runs the experiment both ways. The README picture is a
  recording of it: **65 ms** wait without PI, **15 ms** with.

> **Remember it:** *Whoever holds the lock borrows the priority of whoever waits for it.*

---

## 9. Message queues

**The idea.** Instead of sharing variables, tasks can send each other messages. The kernel
copies the message into a waiting line; the receiver takes from the front. If the line is
full, the sender waits. If it's empty, the receiver waits.

**Tiny example (Java).** `ArrayBlockingQueue<Msg> q = new ArrayBlockingQueue<>(4); q.put(m);
q.take();` Same behaviour.

**In Kestrel.** `sys_queue_send` (`kernel/sync.c:313`). One optimisation: if a receiver is
*already waiting*, the kernel copies the message straight into the receiver's variable and
skips the queue. Tasks only ever hold a **handle** (a small number like `0x301`), never a pointer
to the queue, so they can't corrupt its internals.

---

## 10. The UART driver and ring buffers

**The idea.** Sending one character over the serial cable takes ~87 microseconds, which is
ages for a CPU. Instead of making tasks wait for each character, `k_write` drops characters
into a circular buffer and returns immediately. Each time the hardware is ready for the next
character, it raises an interrupt, and the handler feeds it one character from the buffer.

**Tiny example.** A mailbox. You drop letters in and leave; the postman empties it on their own
schedule. If the mailbox is full, you wait until the postman has taken some.

**The proper names.**
- **UART**: the serial port hardware (here USART2).
- **Ring buffer (circular buffer)**: an array used as a queue whose end wraps around to the
  start. With a power-of-two size, `index & (SIZE-1)` does the wrap cheaply.
- **Producer / consumer**: one side adds, the other removes, the classic OS pattern.
- **Driver**: the code that talks to a piece of hardware on behalf of everyone else.

**In Kestrel.** `k_console_put` (`kernel/console.c:70`) and `USART2_IRQHandler`
(`console.c:98`). If the buffer is full, the writing task is **blocked**, not spinning, until the
interrupt has freed half the buffer.

---

## 11. Seeing what the OS does: tracing and accounting

**The idea.** The kernel writes a timestamped note every time something interesting happens:
a switch, a block, a wake-up, a priority change. A Python script turns those notes into a
timeline picture.

**In Kestrel.**
- `kernel/trace.c` records events. `k_now_cycles()` (`sched.c:170`) builds a cycle-accurate clock
  from SysTick (tick count × ticks-per-ms + the timer's current value). It works on real boards
  *and* in QEMU.
- `tools/trace2perfetto.py` makes a Perfetto JSON timeline (the trace viewer Chrome and Android
  engineers use) and the SVG in the README.
- On each switch, the kernel adds the elapsed cycles to the outgoing task's CPU time. That's how
  `ps` shows CPU%. Stack high-water marks work by filling each new stack with `0xDEADBEEF` and
  later counting how much of that pattern got overwritten.

---

## 12. The neural network part

This is the "AI" part of the JD, and it's really a systems problem: how do you run a neural
network on a chip with 64 KB of RAM and no floating-point speed to spare?

### 12.1 What the network does (Level 1)

**The idea.** The network looks at an 8×8 picture of a handwritten digit (64 numbers, one per
pixel) and outputs 10 scores, one per digit 0–9. The highest score is its answer. Inside,
it's mostly **multiply-and-add**: "this pixel × this learned weight, add it up". Our network
does 84,224 of them per picture.

- **Convolution (conv)**: slide a small 3×3 window over the image; at each position, multiply
  the 9 pixels by 9 learned weights and add. It detects small patterns like edges.
- **ReLU**: replace negative numbers with 0.
- **Max-pool**: shrink the image by keeping the biggest value of each 2×2 block.
- **MAC** (multiply-accumulate): one `acc += a * b`.

### 12.2 Quantization: floats → 8-bit integers

**The idea.** Store every number as a small integer (−128…127) plus one shared "how big is one
step" value. 4× less memory, and integer math is much faster on small chips.

**Tiny example.** Measuring heights with a ruler that only has 256 marks. If the marks are 1 cm
apart starting at −128 cm, then mark 50 means 50 cm. You lose a little precision, but every
height now fits in one byte.

**The proper names.**
- `real = scale × (q − zero_point)`: **scale** is the step size, **zero point** is the integer
  that means real 0.
- **Calibration**: run the float network on sample data, see the smallest and largest value
  each layer produces, and choose scale and zero point to cover that range.
  (NVIDIA TensorRT's "INT8 calibration" does this too.)
- **Per-channel weights**: each output channel gets its own scale, so a channel with tiny
  weights doesn't lose all its precision to one with big weights.

**Result:** float accuracy 98.33 %, int8 accuracy 98.33 %, with the same answer on 100 % of
test images. Code: `tools/nn/quantize.py`.

### 12.3 Integer-only math (Level 2)

The CPU must never use floats at runtime, so every "× 0.0037" becomes "× an integer, then shift
right". `quantize_multiplier` (`quantize.py:33`) turns a real multiplier M into
`mult × 2^-shift`, and `nn_requant()` in `nn/nn.h` does it with one 64-bit multiply.

**Zero-point folding:** `Σ(x − z)·w = Σx·w − z·Σw`. The part `z·Σw` never changes, so we
compute it once on the PC and fold it into the bias. The chip's inner loop is then a plain
dot product.

### 12.4 Making it fast (Level 2)

Three versions of the same convolution, all producing **identical bits**:

1. **Naive** (`conv_naive`, `nn/nn_kernels.c:27`): six nested loops. Easy to read, 13.7
   instructions per MAC.
2. **SIMD** (`dot_q7_q15`, `nn_kernels.c:81`): **SIMD** = Single Instruction, Multiple Data.
   The Cortex-M4 has `SMLAD`, which does **two** multiply-adds in one instruction, on two
   16-bit numbers packed into one 32-bit register. First, **im2col** (`nn_kernels.c:61`) copies
   each 3×3×C window into one flat row, so a conv becomes plain dot products. 5.7 instructions/MAC.
3. **Tiled** (`conv_tiled`, `nn_kernels.c:115`): compute 2 pixels × 2 output channels together.
   Each value loaded from memory is used twice, so memory traffic halves. That's the same idea
   GPU matrix-multiply kernels use with shared memory. 3.3 instructions/MAC: **4.1× fewer than
   naive**.

> **Remember it:** *Fewer loads per multiply = faster. SIMD does 2 multiplies per instruction;
> tiling reuses each load twice.*

### 12.5 Memory planning (Level 2)

**The idea.** The output of layer 1 is only needed until layer 2 has read it. After that, its
memory can hold something else.

**Tiny example.** Meeting rooms: two meetings that never overlap in time can use the same room.

`tools/nn/memplan.py:30` places buffers biggest-first, each at the lowest address that doesn't
collide with a buffer alive at the same time. Result: **1,344 bytes instead of 3,056 (−56 %)**,
fixed at compile time, with no `malloc` on the chip. `tests/host/test_memplan.py` checks it on
2,000 random problems.

### 12.6 Ahead-of-time code generation

Instead of shipping a model file and an interpreter, `tools/nn/codegen.py` writes a C function
`model_run()` (`nn/model_gen.c`) that calls the kernels in order with every shape and address
baked in. That's how TensorRT "engines" and TVM's AOT mode work too.

The whole pipeline is one command: `python3 tools/nn/compile_model.py`.

---

## 13. How it's tested

- **QEMU**: a program that emulates an STM32F405 on your PC. `make test` boots Kestrel in it
  and runs 24 tests. GitHub runs this on every push (CI).
- **`-icount shift=0`**: QEMU counts instructions exactly (1 instruction = 1 ns of fake time),
  so test timings repeat run to run, and instruction counts can be measured exactly.
- **Bit-exact golden testing**: Python computes the exact expected integer outputs for all 359
  test images. The chip (and a PC build under AddressSanitizer) must match every byte.
- **Register torture test** (`app/regs_torture.S`): fills all 40 registers with known values in
  3 tasks that keep preempting each other, then checks every register survived. If the context
  switch forgot even one register, this fails.

---

## 14. The bug stories (tell these in interviews)

**1. The lazy FPU save that killed innocent tasks.** The CPU delays saving float registers
until it's sure they're needed. When the kernel killed a task that had used floats, that
delayed save was still pending. It fired later, aimed at the dead task's freed stack. The MPU
correctly blocked it, but the blame landed on whoever was running, so innocent tasks
(including `init`) were killed with "MPU blocked lazy FPU state save". The fix is one line in
`k_task_kill` (`kernel/sched.c:328`): cancel the pending save (`FPCCR.LSPACT = 0`). I confirmed
it by removing the line and watching `context_switch_integrity` fail.

**2. The "optimized" kernel that was 2× slower.** The SIMD kernel first ran *more* instructions
than the naive one. QEMU's instruction count made that visible. The cause: the compiler flag
`-ffreestanding` (needed for bare metal) also disables built-in `memcpy`, so a 4-byte load in
the innermost loop became a byte-by-byte function call. `__builtin_memcpy` fixed it. Reading
the generated assembly then showed 10 **register spills** (values evicted to the stack because
the CPU ran out of registers) per loop iteration. Restructuring the loop cut that, and the
speedup went from 3.67× to 4.1×.

---

## 15. Make it yours: exercises

Do at least two of these yourself, without AI help. In an interview you'll then have
first-hand stories, which is the strongest signal you can give.

1. **Run on your board**: `make BOARD=... flash`, run `test` and `bench`, put real cycle counts in
   the README table. *(Easy, essential.)*
2. **Speed up the clock**: configure the PLL to 84/100/168 MHz (set flash wait states first!).
   Measure how cycle counts change. Why do they? (Hint: flash wait states.) *(Medium.)*
3. **`k_task_kill(tid)` syscall**: who's allowed to kill whom? What about its mutexes? *(Medium.)*
4. **UART with DMA**: let the DMA controller send the whole buffer without an interrupt per byte. *(Medium.)*
5. **Tickless idle**: when all tasks sleep, stop the 1 ms tick and program one timer for the next
   wake-up. Measure the power saved. *(Hard.)*
6. **MNIST**: train on 28×28 MNIST with the same compiler. Does it fit in 64 KB of RAM? Use the
   memory planner's report to find out. *(Medium.)*
7. **Assembly micro-kernel**: write the tiled inner loop in assembly with zero spills. Beat 3.3
   instructions/MAC. *(Hard.)*

---

## Where to go deeper next

Pick **one** of these and go deep before moving on:

- the context switch, line by line (`kernel/context.S`)
- priority inheritance and its edge cases (`kernel/sync.c`)
- how the MPU regions are encoded (`kernel/mpu.c`)
- quantization math end to end (`tools/nn/quantize.py` → `nn/nn.h`)

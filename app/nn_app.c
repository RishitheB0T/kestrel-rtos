/*
 * nn_app.c - run the int8 CNN on the embedded test set.
 */
#include <string.h>

#include "app.h"
#include "board.h"
#include "model_gen.h"

static const char *const impl_names[NN_NUM_IMPLS] = {"naive C", "DSP SIMD", "SIMD+tiling"};

static int argmax(const int8_t *v, int n)
{
    int best = 0;
    for (int i = 1; i < n; i++)
        if (v[i] > v[best])
            best = i;
    return best;
}

/* Run every test image through every kernel variant. Timing uses this
 * task's own CPU time, so preemption by other tasks is not counted. */
void nn_bench(nn_bench_t *b)
{
    int8_t logits[MODEL_NUM_CLASSES];

    memset(b, 0, sizeof(*b));
    b->total = model_test_count;
    for (int impl = 0; impl < NN_NUM_IMPLS; impl++) {
        uint32_t t0 = k_cpu_cycles();
        for (int i = 0; i < model_test_count; i++) {
            model_run(model_test_images[i], logits, (nn_impl_t)impl);
            if (memcmp(logits, model_test_logits[i], sizeof(logits)) != 0)
                b->mismatches[impl]++;
            if (impl == NN_TILED && argmax(logits, MODEL_NUM_CLASSES) == model_test_labels[i])
                b->correct++;
        }
        b->cycles_per_inference[impl] = (k_cpu_cycles() - t0) / (uint32_t)model_test_count;
    }
}

void nn_print_bench(const nn_bench_t *b)
{
    uint32_t base = b->cycles_per_inference[NN_NAIVE];
    k_printf("int8 CNN, %u MACs/inference, %d test images, arena %u bytes\n",
             (unsigned)MODEL_MACS, b->total, (unsigned)MODEL_ARENA_BYTES);
    k_printf("  %-12s %12s %10s %9s %8s %s\n", "kernel", "cycles/inf", "us/inf", "MAC/cyc",
             "speedup", "bit-exact");
    for (int i = 0; i < NN_NUM_IMPLS; i++) {
        uint32_t c = b->cycles_per_inference[i];
        uint32_t mac_per_cyc_x100 = (uint32_t)((uint64_t)MODEL_MACS * 100u / (c ? c : 1));
        uint32_t speed_x100 = (uint32_t)((uint64_t)base * 100u / (c ? c : 1));
        k_printf("  %-12s %12u %10u %6u.%02u %5u.%02ux %s\n", impl_names[i], (unsigned)c,
                 (unsigned)(c / (CPU_HZ / 1000000u)), (unsigned)(mac_per_cyc_x100 / 100),
                 (unsigned)(mac_per_cyc_x100 % 100), (unsigned)(speed_x100 / 100),
                 (unsigned)(speed_x100 % 100), b->mismatches[i] ? "NO" : "yes");
    }
    k_printf("  accuracy: %d/%d = %d.%02d%%\n", b->correct, b->total,
             b->correct * 100 / b->total, (b->correct * 10000 / b->total) % 100);
#ifdef BOARD_QEMU
    k_printf("  note: QEMU is not cycle-accurate; run on a real board for true cycle counts.\n");
#endif
}

/* Print a few test digits as ASCII art with the network's answer. */
void nn_demo(int count)
{
    static const char shades[] = " .:-=+*#%@";
    int8_t logits[MODEL_NUM_CLASSES];

    for (int i = 0; i < count && i < model_test_count; i++) {
        const int8_t *img = model_test_images[i];
        model_run(img, logits, NN_TILED);
        for (int y = 0; y < 8; y++) {
            char row[20];
            for (int x = 0; x < 8; x++) {
                int v = (img[y * 8 + x] + 128) * 9 / 255; /* back to 0..9 */
                row[2 * x] = row[2 * x + 1] = shades[v];
            }
            row[16] = '\0';
            if (y == 3)
                k_printf("  |%s|   predicted %d, label %d %s\n", row,
                         argmax(logits, MODEL_NUM_CLASSES), model_test_labels[i],
                         argmax(logits, MODEL_NUM_CLASSES) == model_test_labels[i] ? "" : "<-- wrong");
            else
                k_printf("  |%s|\n", row);
        }
        k_printf("\n");
    }
}

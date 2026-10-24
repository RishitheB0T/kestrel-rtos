/*
 * Host test: run the firmware's NN kernels natively (x86/ARM64 PC) on every
 * test image and require bit-exact agreement with the Python integer
 * reference. The DSP instructions are replaced by the portable C models in
 * nn/nn_simd.h, so this also checks the SIMD lane bookkeeping.
 */
#include <stdio.h>
#include <string.h>

#include "model_gen.h"

static const char *const impl_names[NN_NUM_IMPLS] = {"naive", "simd", "tiled"};

int main(void)
{
    int failures = 0;

    for (int impl = 0; impl < NN_NUM_IMPLS; impl++) {
        int mismatches = 0, correct = 0;
        for (int i = 0; i < model_test_count; i++) {
            int8_t logits[MODEL_NUM_CLASSES];
            model_run(model_test_images[i], logits, (nn_impl_t)impl);
            if (memcmp(logits, model_test_logits[i], sizeof(logits)) != 0)
                mismatches++;
            int best = 0;
            for (int c = 1; c < MODEL_NUM_CLASSES; c++)
                if (logits[c] > logits[best])
                    best = c;
            correct += best == model_test_labels[i];
        }
        printf("%-6s %d/%d images bit-exact, accuracy %d/%d = %.2f%%\n", impl_names[impl],
               model_test_count - mismatches, model_test_count, correct, model_test_count,
               100.0 * correct / model_test_count);
        failures += mismatches;
    }

    if (failures) {
        printf("FAIL: %d mismatching outputs\n", failures);
        return 1;
    }
    printf("PASS: all kernels match the Python reference bit for bit\n");
    return 0;
}

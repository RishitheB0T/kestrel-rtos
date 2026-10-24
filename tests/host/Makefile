# Native build of the NN kernels + generated model, for testing on a PC.
CC ?= cc
ROOT := ../..
CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -I$(ROOT)/nn -fsanitize=address,undefined
SRCS := test_nn.c $(ROOT)/nn/nn_kernels.c $(ROOT)/nn/model_gen.c $(ROOT)/nn/testset_gen.c

.PHONY: run clean

run: test_nn
	./test_nn
	python3 test_memplan.py

test_nn: $(SRCS) $(ROOT)/nn/nn.h $(ROOT)/nn/nn_simd.h $(ROOT)/nn/model_gen.h
	$(CC) $(CFLAGS) $(SRCS) -o $@

clean:
	rm -f test_nn

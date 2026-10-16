# Headless PC test build (MSYS2 UCRT64 gcc). Generated code comes from tools/gen_c.py:
#   make -C pc GEN=/e/MKDD/gen -j8
GEN ?= ../../MKDD/gen
OUT ?= build
CC = gcc
OPT ?= -O2
CFLAGS = -std=gnu11 $(OPT) -g -ffp-contract=off -fwrapv -fno-strict-aliasing -I../runtime -I$(GEN) -Wall -Wno-unused-function
GENCFLAGS = -std=gnu11 $(OPT) -ffp-contract=off -fwrapv -fno-strict-aliasing -I../runtime -I$(GEN) -w

RT = core ps plat machine sched disc hw dsp misc memcard thp diag hle gx
PARTS = $(sort $(basename $(notdir $(wildcard $(GEN)/part_*.c))))
OBJS = $(addprefix $(OUT)/rt_,$(addsuffix .o,$(RT))) $(addprefix $(OUT)/gen_,$(addsuffix .o,$(PARTS))) \
       $(OUT)/gen_table.o $(OUT)/gen_mem.o $(OUT)/main_pc.o

$(OUT)/mkdd_pc.exe: $(OBJS)
	$(CC) -o $@ $^ -lm -lpthread

$(OUT)/rt_%.o: ../runtime/%.c ../runtime/*.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/gen_%.o: $(GEN)/%.c ../runtime/gc_core.h | $(OUT)
	$(CC) $(GENCFLAGS) -c $< -o $@
$(OUT)/gen_mem.o: $(GEN)/mem.S | $(OUT)
	cd $(GEN) && $(CC) -c mem.S -o $(abspath $@)
$(OUT)/main_pc.o: main_pc.c ../runtime/*.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT):
	mkdir -p $(OUT)
.PHONY: clean
clean:
	rm -rf $(OUT)

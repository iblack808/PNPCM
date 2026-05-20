CC ?= gcc
CFLAGS ?= -O2 -static

OUTDIR ?= ../bin/x86/linux
OUT = $(OUTDIR)/oracle_gpu_fs_test

.PHONY: all clean

all: $(OUT)

$(OUT): oracle_gpu_fs_test.c
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(OUT)

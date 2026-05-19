CC ?= gcc
CFLAGS ?= -O2 -static

OUTDIR ?= ../bin/x86/linux
SMOKE_OUT = $(OUTDIR)/oracle_gpu_fs_test
DECODE_OUT = $(OUTDIR)/oracle_gpu_decode_attention_test
OUTS = $(SMOKE_OUT) $(DECODE_OUT)

.PHONY: all clean

all: $(OUTS)

$(SMOKE_OUT): oracle_gpu_fs_test.c
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(DECODE_OUT): oracle_gpu_decode_attention_test.c
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(OUTS)

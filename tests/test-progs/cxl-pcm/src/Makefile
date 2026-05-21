CC ?= gcc
CFLAGS ?= -O2 -static

OUTDIR ?= ../bin/x86/linux
OUTS = $(OUTDIR)/cxl_pcm_mem_test

.PHONY: all clean

all: $(OUTS)

$(OUTDIR)/cxl_pcm_mem_test: cxl_pcm_mem_test.c
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(OUTS)

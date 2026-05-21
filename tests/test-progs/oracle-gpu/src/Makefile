CC ?= gcc
CFLAGS ?= -O2 -static
CPPFLAGS += -I../../../../src

OUTDIR ?= ../bin/x86/linux
OUTS = \
	$(OUTDIR)/oracle_gpu_fs_test \
	$(OUTDIR)/oracle_gpu_generic_test \
	$(OUTDIR)/oracle_gpu_cxl_pcm_test

.PHONY: all clean

all: $(OUTS)

$(OUTDIR)/oracle_gpu_fs_test: oracle_gpu_fs_test.c
	mkdir -p $(OUTDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(OUTDIR)/oracle_gpu_generic_test: oracle_gpu_generic_test.c
	mkdir -p $(OUTDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(OUTDIR)/oracle_gpu_cxl_pcm_test: oracle_gpu_cxl_pcm_test.c
	mkdir -p $(OUTDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

clean:
	rm -f $(OUTS)

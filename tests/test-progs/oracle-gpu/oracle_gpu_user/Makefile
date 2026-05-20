CC ?= gcc
AR ?= ar
CFLAGS ?= -O2 -fPIC
CPPFLAGS += -I. -I../../../../src
LDFLAGS ?=
TEST_LDFLAGS ?= -static

OUTDIR ?= ../bin/x86/linux
LIBDIR ?= ../lib/x86/linux
PREFIX ?= /usr/local
INCLUDEDIR ?= $(PREFIX)/include/oraclegpu
INSTALL_LIBDIR ?= $(PREFIX)/lib
INSTALL_BINDIR ?= $(PREFIX)/bin

RUNTIME_OBJ = oracle_gpu_runtime.o
TEST_OBJ = oracle_gpu_runtime_test.o
STATIC_LIB = $(LIBDIR)/liboraclegpu.a
SHARED_LIB = $(LIBDIR)/liboraclegpu.so
TEST_BIN = $(OUTDIR)/oracle_gpu_runtime_test

.PHONY: all clean install

all: $(STATIC_LIB) $(SHARED_LIB) $(TEST_BIN)

$(RUNTIME_OBJ): oracle_gpu_runtime.c oracle_gpu_runtime.h oracle_gpu_abi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ oracle_gpu_runtime.c

$(TEST_OBJ): oracle_gpu_runtime_test.c oracle_gpu_runtime.h oracle_gpu_abi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ oracle_gpu_runtime_test.c

$(STATIC_LIB): $(RUNTIME_OBJ)
	mkdir -p $(LIBDIR)
	$(AR) rcs $@ $(RUNTIME_OBJ)

$(SHARED_LIB): $(RUNTIME_OBJ)
	mkdir -p $(LIBDIR)
	$(CC) -shared $(LDFLAGS) -o $@ $(RUNTIME_OBJ)

$(TEST_BIN): $(TEST_OBJ) $(STATIC_LIB)
	mkdir -p $(OUTDIR)
	$(CC) $(TEST_LDFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJ) $(STATIC_LIB)

install: $(STATIC_LIB) $(SHARED_LIB) $(TEST_BIN)
	install -d $(DESTDIR)$(INCLUDEDIR) $(DESTDIR)$(INSTALL_LIBDIR) $(DESTDIR)$(INSTALL_BINDIR)
	install -m 0644 oracle_gpu_abi.h oracle_gpu_runtime.h $(DESTDIR)$(INCLUDEDIR)/
	install -m 0644 $(STATIC_LIB) $(SHARED_LIB) $(DESTDIR)$(INSTALL_LIBDIR)/
	install -m 0755 $(TEST_BIN) $(DESTDIR)$(INSTALL_BINDIR)/

clean:
	rm -f $(RUNTIME_OBJ) $(TEST_OBJ) $(STATIC_LIB) $(SHARED_LIB) $(TEST_BIN)

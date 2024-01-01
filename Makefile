ifndef RISCV
$(error RISCV is unset)
else
$(info Running with RISCV=$(RISCV))
endif

PREFIX ?= $RISCV/
SRC_DIR := src
SRCS= $(SRC_DIR)/sifive_uart.cc $(SRC_DIR)/iceblk.cc
UTIL_OBJS := iomem.o cutils.o 

CFLAGS=-O2 -Wall -g -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -MMD
CFLAGS+=-D_GNU_SOURCE 

default: libvitioblockdevice.so

iomem.o : iomem.c iomem.h
	gcc $(CFLAGS) -c -o $@ $^

cutils.o : cutils.c cutils.h
	gcc $(CFLAGS) -c -o $@ $^

libvitioblockdevice.so : virtio.cc vitio.h $(UTIL_OBJS)
	g++ -L $(RISCV)/lib -Wl,-rpath,$(RISCV)/lib -shared -o $@ -std=c++17 -I $(RISCV)/include -isystem $(RISCV)/include/fdt -fPIC $< $(UTIL_OBJS)

libspikedevices.so: $(SRCS)
	g++ -L $(RISCV)/lib -Wl,-rpath,$(RISCV)/lib -shared -o $@ -std=c++17 -I $(RISCV)/include -isystem $(RISCV)/include/fdt -fPIC $^

.PHONY: install
install: libspikedevices.so libvitioblockdevice.so
	cp libspikedevices.so $(RISCV)/lib
	cp libvitioblockdevice.so $(RISCV)/lib

clean:
	rm -rf *.o *.so

ifndef RISCV
$(error RISCV is unset)
else
$(info Running with RISCV=$(RISCV))
endif

PREFIX ?= $RISCV/
SRC_DIR := src
SRCS= $(SRC_DIR)/sifive_uart.cc $(SRC_DIR)/iceblk.cc
UTIL_OBJS := $(SRC_DIR)/cutils.o $(SRC_DIR)/fs.o $(SRC_DIR)/fs_disk.o
DEVICE_DLIBS := libspikedevices.so  libvirtio9pdiskdevice.so libvirtioblockdevice.so 

VIRTIO_CFLAGS=-O2 -Wall -g -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -MMD
VIRTIO_CFLAGS+=-D_GNU_SOURCE -fPIC 

default: all

all: $(DEVICE_DLIBS)

$(SRC_DIR)/fs_disk.o : $(SRC_DIR)/fs_disk.c $(SRC_DIR)/list.h
	gcc $(VIRTIO_CFLAGS) -c -o $@ $<

$(UTIL_OBJS: %.o) : %.c %.h
	gcc $(VIRTIO_CFLAGS) -c -o $@ $^

virtio_base.o : $(SRC_DIR)/virtio.cc $(SRC_DIR)/virtio.h 
	g++ -L $(RISCV)/lib -c -o $@ -std=c++17 -I $(RISCV)/include -isystem $(RISCV)/include/fdt -fPIC $< 

libvirtio9pdiskdevice.so : $(SRC_DIR)/virtio-9p-disk.cc $(SRC_DIR)/virtio-9p-disk.h virtio_base.o $(UTIL_OBJS)
	g++ -L $(RISCV)/lib -Wl,-rpath,$(RISCV)/lib -shared -o $@ -std=c++17 -I $(RISCV)/include -isystem $(RISCV)/include/fdt -fPIC $< virtio_base.o $(UTIL_OBJS)

libvirtioblockdevice.so : $(SRC_DIR)/virtio-block.cc $(SRC_DIR)/virtio-block.h virtio_base.o $(UTIL_OBJS)
	g++ -L $(RISCV)/lib -Wl,-rpath,$(RISCV)/lib -shared -o $@ -std=c++17 -I $(RISCV)/include -isystem $(RISCV)/include/fdt -fPIC $< virtio_base.o $(UTIL_OBJS)

libspikedevices.so: $(SRCS)
	g++ -L $(RISCV)/lib -Wl,-rpath,$(RISCV)/lib -shared -o $@ -std=c++17 -I $(RISCV)/include -isystem $(RISCV)/include/fdt -fPIC $^

.PHONY: install
install: $(DEVICE_DLIBS)
	cp $^ $(RISCV)/lib

clean:
	rm -rf *.o *.so src/*.o src/*.d

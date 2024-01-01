/*
 * VIRTIO driver
 * 
 * Copyright (c) 2016 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef VIRTIO_H
#define VIRTIO_H

#include <sys/select.h>
#include <riscv/abstract_device.h>
#include <riscv/simif.h>
#include <riscv/abstract_interrupt_controller.h>
#include <riscv/mmu.h>
#include <riscv/processor.h>
#include <riscv/simif.h>
#include <riscv/sim.h>
#include <riscv/dts.h>
#include <fdt/libfdt.h>

#define VIRTIO_BASE_ADDR 0x40010000
#define VIRTIO_SIZE      0x1000
#define VIRTIO_IRQ       1

#define VIRTIO_PAGE_SIZE 4096

#if defined(EMSCRIPTEN)
#define VIRTIO_ADDR_BITS 32
#else
#define VIRTIO_ADDR_BITS 64
#endif

#if VIRTIO_ADDR_BITS == 64
typedef uint64_t virtio_phys_addr_t;
#else
typedef uint32_t virtio_phys_addr_t;
#endif

class IRQSpike {
  abstract_interrupt_controller_t* intctrl;
  uint32_t interrupt_id;
public:
  IRQSpike(abstract_interrupt_controller_t* intctrl_, uint32_t irq_num)
    :intctrl(intctrl_),interrupt_id(irq_num){}
  void set(int level) {intctrl->set_interrupt_level(interrupt_id, level);}
};

// Override default set_irq(IRQSignal*,int) static function
static inline void set_irq(IRQSpike* irq, int level) {
  irq->set(level);
}

typedef struct {
    /* MMIO only: */
    uint64_t addr;
    IRQSpike *irq;
} VIRTIOBusDef;

struct VIRTIODevice; 

#define VIRTIO_DEBUG_IO (1 << 0)
#define VIRTIO_DEBUG_9P (1 << 1)

void virtio_set_debug(VIRTIODevice *s, int debug_flags);

/* block device */

typedef void BlockDeviceCompletionFunc(VIRTIODevice *opaque, int ret);

struct BlockDevice ;
struct BlockDeviceFile;

typedef enum {
    BF_MODE_RO,
    BF_MODE_RW,
    BF_MODE_SNAPSHOT,
} BlockDeviceModeEnum;

typedef struct BlockDeviceFile {
    FILE *f;
    int64_t nb_sectors;
    BlockDeviceModeEnum mode;
    uint8_t **sector_table;
} BlockDeviceFile;


struct BlockDevice {
    int64_t (*get_sector_count)(BlockDevice *bs);
    int (*read_async)(BlockDevice *bs,
                      uint64_t sector_num, uint8_t *buf, int n,
                      BlockDeviceCompletionFunc *cb, VIRTIODevice *opaque);
    int (*write_async)(BlockDevice *bs,
                       uint64_t sector_num, const uint8_t *buf, int n,
                       BlockDeviceCompletionFunc *cb, VIRTIODevice *opaque);
    BlockDeviceFile *opaque;
};

VIRTIODevice *virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs);

class virtioblk_t : public abstract_device_t {
public:
  virtioblk_t(
      const simif_t* sim,
      abstract_interrupt_controller_t *intctrl,
      uint32_t interrupt_id,
      std::vector<std::string> sargs);
  ~virtioblk_t();
  bool load(reg_t addr, size_t len, uint8_t* bytes) override;
  bool store(reg_t addr, size_t len, const uint8_t* bytes) override;
private:
  const simif_t* sim;
  abstract_interrupt_controller_t *intctrl;
  uint32_t interrupt_id;

private:
  VIRTIODevice* blk_dev;
  IRQSpike* irq;
};


#endif // VIRTIO_H
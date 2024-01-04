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
#include "virtio.h"
#include "fs.h"
#include "list.h" 

#define VIRTIO_9P_FS_BASE 0x40011000
#define VIRTIO_9P_FS_IRQ       2

class virtio9p_t: public virtio_base_t {
public:
  virtio9p_t(
      const simif_t* sim,
      abstract_interrupt_controller_t *intctrl,
      uint32_t interrupt_id,
      std::vector<std::string> sargs);
  ~virtio9p_t();
private:
};
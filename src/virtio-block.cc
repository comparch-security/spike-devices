#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include "virtio-block.h"
#include "cutils.h"

int fdt_parse_virtioblk(
    const void *fdt,
    reg_t* blkdev_addr,
    uint32_t* blkdev_int_id,
    const char *compatible) {
  int nodeoffset, rc, len;
  const fdt32_t *reg_p;

  nodeoffset = fdt_node_offset_by_compatible(fdt, -1, compatible);
  if (nodeoffset < 0)
    return nodeoffset;

  rc = fdt_get_node_addr_size(fdt, nodeoffset, blkdev_addr, NULL, "reg");
  if (rc < 0 || !blkdev_addr)
    return -ENODEV;

  reg_p = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "interrupts", &len);
  if (blkdev_int_id) {
    if (reg_p)
      *blkdev_int_id = fdt32_to_cpu(*reg_p);
    else
      *blkdev_int_id = VIRTIO_IRQ;
  }

  return 0;
}

virtioblk_t::virtioblk_t(
      const simif_t* sim,
      abstract_interrupt_controller_t *intctrl,
      uint32_t interrupt_id,
      std::vector<std::string> sargs)
  : virtio_base_t(sim, intctrl, interrupt_id, sargs)
{
  std::map<std::string, std::string> argmap;

  for (auto arg : sargs) {
    size_t eq_idx = arg.find('=');
    if (eq_idx != std::string::npos) {
      argmap.insert(std::pair<std::string, std::string>(arg.substr(0, eq_idx), arg.substr(eq_idx+1)));
    }
  }

  std::string fname;
  BlockDeviceModeEnum block_device_mode = BF_MODE_RW;
  
  auto it = argmap.find("img");
  if (it == argmap.end()) {
    // invalid block device.
    printf("Virtio block device plugin INIT ERROR: `img` argument not specified.\n"
            "Please use spike option --device=virtioblk,img=file to use an exist block device file.\n");
    exit(1);
  }
  else {
    fname = it->second;
  }

    it = argmap.find("mode");
    if (it != argmap.end()) {
        if (it->second == "ro") {
            block_device_mode = BF_MODE_RO;
        }
        else if (it->second == "snapshot") {
            block_device_mode = BF_MODE_SNAPSHOT;
        }
        else {
            block_device_mode = BF_MODE_RW;
        }
    }


    int irq_num;
    VIRTIOBusDef vbus_s, *vbus = &vbus_s;
    BlockDevice* bs = block_device_init(fname.c_str(), block_device_mode); //initialization

    memset(vbus, 0, sizeof(*vbus));
    vbus->addr = VIRTIO_BASE_ADDR;
    irq_num = VIRTIO_IRQ;
    irq = new IRQSpike(intctrl, irq_num);

    // only one virtio block device
    //REQUIRE: register irq_num as plic_irq number
    // vbus->irq = &s->plci_irq[irq_num];
    vbus->irq = irq;

    virtio_dev = virtio_block_init(vbus, bs, sim);
    vbus->addr += VIRTIO_SIZE;


}

virtioblk_t::~virtioblk_t() {
    if (irq) delete irq;
}


std::string virtioblk_generate_dts(const sim_t* sim, const std::vector<std::string>& args) {
  std::stringstream s;
  s << std::hex 
    << "    virtioblk: virtio@" << VIRTIO_BASE_ADDR << " {\n"
    << "      compatible = \"virtio,mmio\";\n"
       "      interrupt-parent = <&PLIC>;\n"
       "      interrupts = <" << std::dec << VIRTIO_IRQ;
    reg_t virtioblkbs = VIRTIO_BASE_ADDR;
    reg_t virtioblksz = VIRTIO_SIZE;
  s << std::hex << ">;\n"
       "      reg = <0x" << (virtioblkbs >> 32) << " 0x" << (virtioblkbs & (uint32_t)-1) <<
                   " 0x" << (virtioblksz >> 32) << " 0x" << (virtioblksz & (uint32_t)-1) << ">;\n"
       "    };\n";
    return s.str();
}

virtioblk_t* virtioblk_parse_from_fdt(
  const void* fdt, const sim_t* sim, reg_t* base,
    std::vector<std::string> sargs)
{
  uint32_t blkdev_int_id;
  if (fdt_parse_virtioblk(fdt, base, &blkdev_int_id, "virtio,mmio") == 0) {
    abstract_interrupt_controller_t* intctrl = sim->get_intctrl();
    return new virtioblk_t(sim, intctrl, blkdev_int_id, sargs);
  } else {
    return nullptr;
  }
}

REGISTER_DEVICE(virtioblk, virtioblk_parse_from_fdt, virtioblk_generate_dts);
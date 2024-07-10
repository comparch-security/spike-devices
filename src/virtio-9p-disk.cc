#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include "virtio-9p-disk.h"
#include "cutils.h"


int fdt_parse_virtio9p(
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
      *blkdev_int_id = VIRTIO_9P_FS_IRQ;
  }

  return 0;
}

virtio9p_t::virtio9p_t(
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
  std::string mount_tag = "/dev/root";
  
  auto it = argmap.find("path");
  if (it == argmap.end()) {
    // invalid block device.
    printf("Virtio 9p disk fs device plugin INIT ERROR: `path` argument not specified.\n"
            "Please use spike option --device=virtio9p,path=/path/to/folder to use an exist host filesystem folder path.\n");
    exit(1);
  }
  else {
    fname = it->second;
  }

  it = argmap.find("tag");
  if (it != argmap.end()) {
    mount_tag = it->second;
  }
  else {
    printf("Virtio 9p disk fs device plugin INIT WARN: `tag` argument not specified. Use default %s\n", mount_tag.c_str());
  }
  
  int irq_num;
  VIRTIOBusDef vbus_s, *vbus = &vbus_s;
  FSDevice* fs = fs_disk_init(fname.c_str());
  if (!fs) {
    printf("Virtio 9p disk fs device plugin INIT ERROR: `path` %s must be a directory\n", fname.c_str());
    exit(1);
  }

  memset(vbus, 0, sizeof(*vbus));
  vbus->addr = VIRTIO_9P_FS_BASE;
  irq_num  = VIRTIO_9P_FS_IRQ;
  irq = new IRQSpike(intctrl, irq_num);
  vbus->irq = irq;

  virtio_dev = virtio_9p_init(vbus, fs, mount_tag.c_str(), sim);
  vbus->addr += VIRTIO_SIZE;

}

virtio9p_t::~virtio9p_t() {
    if (irq) delete irq;
}


std::string virtio9p_generate_dts(const sim_t* sim, const std::vector<std::string>& args) {
  std::stringstream s;
  s << std::hex 
    << "    virtio9p: virtio@" << VIRTIO_9P_FS_BASE << " {\n"
    << "      compatible = \"virtio,mmio\";\n"
       "      interrupt-parent = <&PLIC>;\n"
       "      interrupts = <" << std::dec << VIRTIO_9P_FS_IRQ;
    reg_t virtio9pbs = VIRTIO_9P_FS_BASE;
    reg_t virtio9psz = VIRTIO_SIZE;
  s << std::hex << ">;\n"
       "      reg = <0x" << (virtio9pbs >> 32) << " 0x" << (virtio9pbs & (uint32_t)-1) <<
                   " 0x" << (virtio9psz >> 32) << " 0x" << (virtio9psz & (uint32_t)-1) << ">;\n"
       "    };\n";
    return s.str();
}

virtio9p_t* virtio9p_parse_from_fdt(
  const void* fdt, const sim_t* sim, reg_t* base,
    std::vector<std::string> sargs)
{
  uint32_t blkdev_int_id;
  if (fdt_parse_virtio9p(fdt, base, &blkdev_int_id, "virtio,mmio") == 0) {
    abstract_interrupt_controller_t* intctrl = sim->get_intctrl();
    return new virtio9p_t(sim, intctrl, blkdev_int_id, sargs);
  } else {
    return nullptr;
  }
}

REGISTER_DEVICE(virtio9p, virtio9p_parse_from_fdt, virtio9p_generate_dts);
# Spike Devices

For spike device plugins.

## Dependencies
- `riscv-toolchains`
- If you are using this within Chipyard, the dependencies will be already installed in the `$RISCV` directory.

## Quick Start
```bash
# iceblk and sifive uart
make libspikedevices.so

# virtio block device
make 
```

## Usage
### virtio block device:

Kernel config requirements:
- CONFIG_VIRTIO_MMIO=y
- CONFIG_VIRTIO_BLK=y
- CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y (Optional)

DTS part:
```
  soc {
    ...
    virtioblk: virtio@40010000 {
      compatible = "virtio,mmio";
      interrupt-parent = <&PLIC>;
      interrupts = <1>;
      reg = <0x0 0x40010000 0x0 0x1000>;
    };
  };
```

Create an img file and format it, say `raw.img` with ext4 fs.
- NTFS/FAT/DOS fs require kernel configuration.
```bash
truncate -s 1G raw.img
mkfs.ext4 raw.img
```

Available img file access modes:
- rw : Read and Write, if mode option is ignored, set by default.
- ro : Read Only
- snapshot : Read and Write, but changes will not sync to img file.

```bash
make  # Generate libvirtioblockdevice.so
make install # Optional, shared library will be installed to $(RISCV)/lib
spike --device="virtioblk,img=raw.img" --extlib=/path/to/libvirtioblockdevice.so bbl
# We can also set the access mode 
spike --device="virtioblk,img=raw.img,mode=snapshot" --extlib=/path/to/libvirtioblockdevice.so bbl
```

Inside kernel shell:
```ash
mount -t devtmpfs none /dev
mount /dev/vda /mnt
# do sth...
umount /mnt
```
`poweroff` without unmounting `/mnt` is also ok, since kernel will automatically unmount it.
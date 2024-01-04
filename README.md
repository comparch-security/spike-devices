# Spike Devices

For spike device plugins.

Virtio MMIO devices ported from [TinyEMU](https://www.bellard.org/tinyemu/).

Supported virtio MMIO devices:
- Virtio MMIO block device
- Virtio 9P filesystem device (local disk filesystem only)

## Dependencies
- `riscv-toolchains`
- If you are using this within Chipyard, the dependencies will be already installed in the `$RISCV` directory.

## Quick Start
```bash
# iceblk and sifive uart
make libspikedevices.so

# virtio block device
make libvirtioblockdevice.so

# virtio 9p filesystem device
make libvirtio9pdiskdevice.so

# all of the above
make all
```

## Usage
### virtio block device:

##### Kernel Config Requirements
- CONFIG_VIRTIO_MMIO=y
- CONFIG_VIRTIO_BLK=y
- CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y (Optional)

#### DTS Part
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

#### Device Parameters

- img=*str* : Path to the image file that serves as block device. 
- mode=*str* : Optional. Image file access modes.


Available img file access modes:
- rw : Read and Write, if mode option is ignored, set by default.
- ro : Read Only
- snapshot : Read and Write, but changes will not sync to img file.

#### Example
Create an img file and format it, say `raw.img` with ext4 fs.
- NTFS/FAT/DOS fs require kernel configuration.
```bash
truncate -s 1G raw.img
mkfs.ext4 raw.img
```

*Note* : the spike option `--extlib` should be set before `--device`, or the device will not be found.

```bash
make  # Generate libvirtioblockdevice.so
make install # Optional, shared library will be installed to $(RISCV)/lib
spike --extlib=/path/to/libvirtioblockdevice.so --device="virtioblk,img=raw.img" bbl
# We can also set the access mode 
spike --extlib=/path/to/libvirtioblockdevice.so --device="virtioblk,img=raw.img,mode=snapshot" bbl
```

Inside kernel shell:
```ash
mount -t devtmpfs none /dev
mount /dev/vda /mnt
# do sth...
umount /mnt
```
`poweroff` without unmounting `/mnt` is also ok, since kernel will automatically unmount it.

### virtio 9P Filesystem device

Recommended kernel version : v6.1.y

#### Kernel Config Requirements
- CONFIG_VIRTIO_MMIO=y
- CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y (Optional)
- CONFIG_NETWORK_FILESYSTEMS=y
- CONFIG_NET_9P=y
- CONFIG_NET_9P_VIRTIO=y
- CONFIG_9P_FS=y
- CONFIG_9P_FS_POSIX_ACL=y
- CONFIG_NET_9P_DEBUG=y (Optional)

#### DTS Part
```
soc {
  ...
  virtio9p: virtio@40011000 {
    compatible = "virtio,mmio";
    interrupt-parent = <&PLIC>;
    interrupts = <2>;
    reg = <0x0 0x40011000 0x0 0x1000>;
  };
};
```

#### Device Parameters

- file=*str* : Path to the host shared folder.
- tag=*str* : Optinal. Mount tag the shared folder. Default is "/dev/root".

Guest OS will use mount tag to specify the device to mount.

#### Example

Choose a folder in host filesystem, say `/tmp`. User must have access rights to this folder.

Run spike :
```bash
make # Generate libvirtio9pdiskdevice.so
make install # Optional, shared library will be installed to $(RISCV)/lib
spike --extlib=/path/to/libvirtio9pdiskdevice.so --device="virtio9p,file=/tmp" bbl
# We can also set the mount tag 
spike --extlib=/path/to/libvirtio9pdiskdevice.so --device="virtio9p,file=/tmp,tag=hostshare" bbl
```

The mount tag can be found inside kernel: (Assume the vitio 9p device is probed as `virtio0`)


```ash
mount -t sysfs sysfs /sys # If sysfs is not mounted
cat /sys/bus/virtio/drivers/9pnet_virtio/virtio0/mount_tag
```


Inside kernel : (Assume the mount tag is set to `hostshare`)
```ash
# msize should be set in kernel v6.x
# msize should be a multiple of 4096, and no more than 57344.
# See bugs below.
mount -t 9p -o msize=8192 hostshare /mnt 
# do sth ...
# from /mnt, guest kernel can access the content in host filesystem folder /tmp
umount /mnt
```
`poweroff` without umounting `/mnt` is also ok, since kernel will automatically unmount it.

#### Bugs

1. According to the linux kernel updates on 9p net support since 6.4, the following 9P2000.L file open flags are omitted in `TLOPEN` request:
    * P9_DOTL_APPEND
    * P9_DOTL_NONBLOCK
    * P9_DOTL_DSYNC
    * P9_DOTL_FASYNC
    * P9_DOTL_DIRECT
    * P9_DOTL_LARGEFILE
    * P9_DOTL_DIRECTORY
    * P9_DOTL_NOFOLLOW
    * P9_DOTL_NOATIME
    * P9_DOTL_CLOEXEC
    * P9_DOTL_SYNC

    Therefore, when opening directories, the 9p server (spike-devices) should open directory based on stat info from host filesystem, instead of the open flags in `TLOPEN` request from guest kernel's 9p client.

    The kernel patch commit related:
46c30cb8f5393586c6ebc7b53a235c85bfac1de8

2. The message size `msize` in
`TVERSION` request should be set to a multiple of 4096 and no more than 57344(0xE000). Otherwise kernel will report a `WARN_ON_ONCE` inside function `virtqueue_add_split` during `TREADDIR` request generation.

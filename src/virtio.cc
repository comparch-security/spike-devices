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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include "virtio.h"

//#define DEBUG_VIRTIO


/* MMIO addresses - from the Linux kernel */
#define VIRTIO_MMIO_MAGIC_VALUE		0x000
#define VIRTIO_MMIO_VERSION		0x004
#define VIRTIO_MMIO_DEVICE_ID		0x008
#define VIRTIO_MMIO_VENDOR_ID		0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL	0x014
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL	0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE	0x028 /* version 1 only */
#define VIRTIO_MMIO_QUEUE_SEL		0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
#define VIRTIO_MMIO_QUEUE_NUM		0x038
#define VIRTIO_MMIO_QUEUE_ALIGN		0x03c /* version 1 only */
#define VIRTIO_MMIO_QUEUE_PFN		0x040 /* version 1 only */
#define VIRTIO_MMIO_QUEUE_READY		0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
#define VIRTIO_MMIO_STATUS		0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW	0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH	0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW	0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH	0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION	0x0fc
#define VIRTIO_MMIO_CONFIG		0x100
/* The following interface is not implemented yet,
 * which were added in VirtIO v1.2 */
#define VIRTIO_MMIO_SHM_SEL         0x0ac
#define VIRTIO_MMIO_SHM_LEN_LOW     0x0b0
#define VIRTIO_MMIO_SHM_LEN_HIGH    0x0b4
#define VIRTIO_MMIO_SHM_BASE_LOW    0x0b8
#define VIRTIO_MMIO_SHM_BASE_HIGH   0x0bc
#define VIRTIO_MMIO_QUEUE_RESET     0x0c0

#define MAX_QUEUE 8
#define MAX_CONFIG_SPACE_SIZE 256
#define MAX_QUEUE_NUM 16

typedef struct {
    uint32_t ready; /* 0 or 1 */
    uint32_t num;
    uint16_t last_avail_idx;
    virtio_phys_addr_t desc_addr;
    virtio_phys_addr_t avail_addr;
    virtio_phys_addr_t used_addr;
    BOOL manual_recv; /* if TRUE, the device_recv() callback is not called */
} QueueState;

#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2
#define VRING_DESC_F_INDIRECT	4

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags; /* VRING_DESC_F_x */
    uint16_t next;
} VIRTIODesc;


/* return < 0 to stop the notification (it must be manually restarted
   later), 0 if OK */
typedef int VIRTIODeviceRecvFunc(VIRTIODevice *s1, int queue_idx,
                                 int desc_idx, int read_size,
                                 int write_size);

/* return NULL if no RAM at this address. The mapping is valid for one page */
typedef uint8_t *VIRTIOGetRAMPtrFunc(VIRTIODevice *s, virtio_phys_addr_t paddr, BOOL is_rw);

struct VIRTIODevice {
    /* MMIO only */
    IRQSpike* irq;
    VIRTIOGetRAMPtrFunc *get_ram_ptr;
    int debug;

    uint32_t int_status;
    uint32_t status;
    uint32_t device_features_sel;
    uint32_t queue_sel; /* currently selected queue */
    QueueState queue[MAX_QUEUE];

    /* device specific */
    uint32_t device_id;
    uint32_t vendor_id;
    uint32_t device_features;
    VIRTIODeviceRecvFunc *device_recv;
    void (*config_write)(VIRTIODevice *s); /* called after the config
                                              is written */
    uint32_t config_space_size; /* in bytes, must be multiple of 4 */
    uint8_t config_space[MAX_CONFIG_SPACE_SIZE];
};

#define SECTOR_SIZE 512

static int64_t bf_get_sector_count(BlockDevice *bs)
{
    BlockDeviceFile *bf = bs->opaque;
    return bf->nb_sectors;
}

//#define DUMP_BLOCK_READ

static int bf_read_async(BlockDevice *bs,
                         uint64_t sector_num, uint8_t *buf, int n,
                         BlockDeviceCompletionFunc *cb, VIRTIODevice *opaque)
{
    BlockDeviceFile *bf = bs->opaque;
    //    printf("bf_read_async: sector_num=%" PRId64 " n=%d\n", sector_num, n);
#ifdef DUMP_BLOCK_READ
    {
        static FILE *f;
        if (!f)
            f = fopen("/tmp/read_sect.txt", "wb");
        fprintf(f, "%" PRId64 " %d\n", sector_num, n);
    }
#endif
    if (!bf->f)
        return -1;
    if (bf->mode == BF_MODE_SNAPSHOT) {
        int i;
        for(i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num]) {
                fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
                fread(buf, 1, SECTOR_SIZE, bf->f);
            } else {
                memcpy(buf, bf->sector_table[sector_num], SECTOR_SIZE);
            }
            sector_num++;
            buf += SECTOR_SIZE;
        }
    } else {
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fread(buf, 1, n * SECTOR_SIZE, bf->f);
    }
    /* synchronous read */
    return 0;
}

static int bf_write_async(BlockDevice *bs,
                          uint64_t sector_num, const uint8_t *buf, int n,
                          BlockDeviceCompletionFunc *cb, VIRTIODevice *opaque)
{
    BlockDeviceFile *bf = bs->opaque;
    int ret;

    switch(bf->mode) {
    case BF_MODE_RO:
        ret = -1; /* error */
        break;
    case BF_MODE_RW:
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fwrite(buf, 1, n * SECTOR_SIZE, bf->f);
        ret = 0;
        break;
    case BF_MODE_SNAPSHOT:
        {
            int i;
            if ((sector_num + n) > bf->nb_sectors)
                return -1;
            for(i = 0; i < n; i++) {
                if (!bf->sector_table[sector_num]) {
                    bf->sector_table[sector_num] = (uint8_t*)malloc(SECTOR_SIZE);
                }
                memcpy(bf->sector_table[sector_num], buf, SECTOR_SIZE);
                sector_num++;
                buf += SECTOR_SIZE;
            }
            ret = 0;
        }
        break;
    default:
        abort();
    }

    return ret;
}

static BlockDevice *block_device_init(const char *filename,
                                      BlockDeviceModeEnum mode)
{
    BlockDevice *bs;
    BlockDeviceFile *bf;
    int64_t file_size;
    FILE *f;
    const char *mode_str;

    if (mode == BF_MODE_RW) {
        mode_str = "r+b";
    } else {
        mode_str = "rb";
    }
    
    f = fopen(filename, mode_str);
    if (!f) {
        perror(filename);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    file_size = ftello(f);

    bs = (BlockDevice*)mallocz(sizeof(*bs));
    bf = (BlockDeviceFile*)mallocz(sizeof(*bf));

    bf->mode = mode;
    bf->nb_sectors = file_size / 512;
    bf->f = f;

    if (mode == BF_MODE_SNAPSHOT) {
        bf->sector_table = (uint8_t**)mallocz(sizeof(bf->sector_table[0]) *
                                   bf->nb_sectors);
    }
    
    bs->opaque = bf;
    bs->get_sector_count = bf_get_sector_count;
    bs->read_async = bf_read_async;
    bs->write_async = bf_write_async;
    return bs;
}

static uint32_t virtio_mmio_read(VIRTIODevice *opaque, uint32_t offset1, int size_log2);
static void virtio_mmio_write(VIRTIODevice *opaque, uint32_t offset,
                              uint32_t val, int size_log2);

static void virtio_reset(VIRTIODevice *s)
{
    int i;

    s->status = 0;
    s->queue_sel = 0;
    s->device_features_sel = 0;
    s->int_status = 0;
    for(i = 0; i < MAX_QUEUE; i++) {
        QueueState *qs = &s->queue[i];
        qs->ready = 0;
        qs->num = MAX_QUEUE_NUM;
        qs->desc_addr = 0;
        qs->avail_addr = 0;
        qs->used_addr = 0;
        qs->last_avail_idx = 0;
    }
}

static uint8_t *virtio_mmio_get_ram_ptr(VIRTIODevice *s, virtio_phys_addr_t paddr, BOOL is_rw)
{
    //TODO: implement method to get pointer in guest ram via sim_t;
    return nullptr;
}


static void virtio_init(VIRTIODevice *s, VIRTIOBusDef *bus,
                        uint32_t device_id, int config_space_size,
                        VIRTIODeviceRecvFunc *device_recv, const simif_t* sim)
{
    memset(s, 0, sizeof(*s));

    {
        s->sim = sim;
        /* MMIO case */
        s->irq = bus->irq;
        // TODO: Register virtio_mmio_read, virtio_mmio_write as read/write function.
        s->get_ram_ptr = virtio_mmio_get_ram_ptr;
    }

    s->device_id = device_id;
    s->vendor_id = 0xffff;
    s->config_space_size = config_space_size;
    s->device_recv = device_recv;
    virtio_reset(s);
}

static uint16_t virtio_read16(VIRTIODevice *s, virtio_phys_addr_t addr)
{
    uint8_t *ptr;
    if (addr & 1)
        return 0; /* unaligned access are not supported */
    ptr = s->get_ram_ptr(s, addr, FALSE);
    if (!ptr)
        return 0;
    return *(uint16_t *)ptr;
}

static void virtio_write16(VIRTIODevice *s, virtio_phys_addr_t addr,
                           uint16_t val)
{
    uint8_t *ptr;
    if (addr & 1)
        return; /* unaligned access are not supported */
    ptr = s->get_ram_ptr(s, addr, TRUE);
    if (!ptr)
        return;
    *(uint16_t *)ptr = val;
}

static void virtio_write32(VIRTIODevice *s, virtio_phys_addr_t addr,
                           uint32_t val)
{
    uint8_t *ptr;
    if (addr & 3)
        return; /* unaligned access are not supported */
    ptr = s->get_ram_ptr(s, addr, TRUE);
    if (!ptr)
        return;
    *(uint32_t *)ptr = val;
}

static int virtio_memcpy_from_ram(VIRTIODevice *s, uint8_t *buf,
                                  virtio_phys_addr_t addr, int count)
{
    uint8_t *ptr;
    int l;

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        ptr = s->get_ram_ptr(s, addr, FALSE);
        if (!ptr)
            return -1;
        memcpy(buf, ptr, l);
        addr += l;
        buf += l;
        count -= l;
    }
    return 0;
}

static int virtio_memcpy_to_ram(VIRTIODevice *s, virtio_phys_addr_t addr, 
                                const uint8_t *buf, int count)
{
    uint8_t *ptr;
    int l;

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        ptr = s->get_ram_ptr(s, addr, TRUE);
        if (!ptr)
            return -1;
        memcpy(ptr, buf, l);
        addr += l;
        buf += l;
        count -= l;
    }
    return 0;
}

static int get_desc(VIRTIODevice *s, VIRTIODesc *desc,  
                    int queue_idx, int desc_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    return virtio_memcpy_from_ram(s, (uint8_t *)desc, qs->desc_addr +
                                  desc_idx * sizeof(VIRTIODesc),
                                  sizeof(VIRTIODesc));
}

static int memcpy_to_from_queue(VIRTIODevice *s, uint8_t *buf,
                                int queue_idx, int desc_idx,
                                int offset, int count, BOOL to_queue)
{
    VIRTIODesc desc;
    int l, f_write_flag;

    if (count == 0)
        return 0;

    get_desc(s, &desc, queue_idx, desc_idx);

    if (to_queue) {
        f_write_flag = VRING_DESC_F_WRITE;
        /* find the first write descriptor */
        for(;;) {
            if ((desc.flags & VRING_DESC_F_WRITE) == f_write_flag)
                break;
            if (!(desc.flags & VRING_DESC_F_NEXT))
                return -1;
            desc_idx = desc.next;
            get_desc(s, &desc, queue_idx, desc_idx);
        }
    } else {
        f_write_flag = 0;
    }

    /* find the descriptor at offset */
    for(;;) {
        if ((desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
            return -1;
        if (offset < desc.len)
            break;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            return -1;
        desc_idx = desc.next;
        offset -= desc.len;
        get_desc(s, &desc, queue_idx, desc_idx);
    }

    for(;;) {
        l = min_int(count, desc.len - offset);
        if (to_queue)
            virtio_memcpy_to_ram(s, desc.addr + offset, buf, l);
        else
            virtio_memcpy_from_ram(s, buf, desc.addr + offset, l);
        count -= l;
        if (count == 0)
            break;
        offset += l;
        buf += l;
        if (offset == desc.len) {
            if (!(desc.flags & VRING_DESC_F_NEXT))
                return -1;
            desc_idx = desc.next;
            get_desc(s, &desc, queue_idx, desc_idx);
            if ((desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
                return -1;
            offset = 0;
        }
    }
    return 0;
}

static int memcpy_from_queue(VIRTIODevice *s, void *buf,
                             int queue_idx, int desc_idx,
                             int offset, int count)
{
    return memcpy_to_from_queue(s, (uint8_t*)buf, queue_idx, desc_idx, offset, count,
                                FALSE);
}

static int memcpy_to_queue(VIRTIODevice *s,
                           int queue_idx, int desc_idx,
                           int offset, const void *buf, int count)
{
    return memcpy_to_from_queue(s, (uint8_t *)buf, queue_idx, desc_idx, offset,
                                count, TRUE);
}

/* signal that the descriptor has been consumed */
static void virtio_consume_desc(VIRTIODevice *s,
                                int queue_idx, int desc_idx, int desc_len)
{
    QueueState *qs = &s->queue[queue_idx];
    virtio_phys_addr_t addr;
    uint32_t index;

    addr = qs->used_addr + 2;
    index = virtio_read16(s, addr);
    virtio_write16(s, addr, index + 1);

    addr = qs->used_addr + 4 + (index & (qs->num - 1)) * 8;
    virtio_write32(s, addr, desc_idx);
    virtio_write32(s, addr + 4, desc_len);

    s->int_status |= 1;
    set_irq(s->irq, 1);
}

static int get_desc_rw_size(VIRTIODevice *s, 
                             int *pread_size, int *pwrite_size,
                             int queue_idx, int desc_idx)
{
    VIRTIODesc desc;
    int read_size, write_size;

    read_size = 0;
    write_size = 0;
    get_desc(s, &desc, queue_idx, desc_idx);

    for(;;) {
        if (desc.flags & VRING_DESC_F_WRITE)
            break;
        read_size += desc.len;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            goto done;
        desc_idx = desc.next;
        get_desc(s, &desc, queue_idx, desc_idx);
    }
    
    for(;;) {
        if (!(desc.flags & VRING_DESC_F_WRITE))
            return -1;
        write_size += desc.len;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            break;
        desc_idx = desc.next;
        get_desc(s, &desc, queue_idx, desc_idx);
    }

 done:
    *pread_size = read_size;
    *pwrite_size = write_size;
    return 0;
}

/* XXX: test if the queue is ready ? */
static void queue_notify(VIRTIODevice *s, int queue_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    uint16_t avail_idx;
    int desc_idx, read_size, write_size;

    if (qs->manual_recv)
        return;

    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    while (qs->last_avail_idx != avail_idx) {
        desc_idx = virtio_read16(s, qs->avail_addr + 4 + 
                                 (qs->last_avail_idx & (qs->num - 1)) * 2);
        if (!get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx)) {
#ifdef DEBUG_VIRTIO
            if (s->debug & VIRTIO_DEBUG_IO) {
                printf("queue_notify: idx=%d read_size=%d write_size=%d\n",
                       queue_idx, read_size, write_size);
            }
#endif
            if (s->device_recv(s, queue_idx, desc_idx,
                               read_size, write_size) < 0)
                break;
        }
        qs->last_avail_idx++;
    }
}

static uint32_t virtio_config_read(VIRTIODevice *s, uint32_t offset,
                                   int size_log2)
{
    uint32_t val;
    switch(size_log2) {
    case 0:
        if (offset < s->config_space_size) {
            val = s->config_space[offset];
        } else {
            val = 0;
        }
        break;
    case 1:
        if (offset < (s->config_space_size - 1)) {
            val = get_le16(&s->config_space[offset]);
        } else {
            val = 0;
        }
        break;
    case 2:
        if (offset < (s->config_space_size - 3)) {
            val = get_le32(s->config_space + offset);
        } else {
            val = 0;
        }
        break;
    default:
        abort();
    }
    return val;
}

static void virtio_config_write(VIRTIODevice *s, uint32_t offset,
                                uint32_t val, int size_log2)
{
    switch(size_log2) {
    case 0:
        if (offset < s->config_space_size) {
            s->config_space[offset] = val;
            if (s->config_write)
                s->config_write(s);
        }
        break;
    case 1:
        if (offset < s->config_space_size - 1) {
            put_le16(s->config_space + offset, val);
            if (s->config_write)
                s->config_write(s);
        }
        break;
    case 2:
        if (offset < s->config_space_size - 3) {
            put_le32(s->config_space + offset, val);
            if (s->config_write)
                s->config_write(s);
        }
        break;
    }
}

static uint32_t virtio_mmio_read(VIRTIODevice *opaque, uint32_t offset, int size_log2)
{
    VIRTIODevice *s = (VIRTIODevice*)opaque;
    uint32_t val;

    if (offset >= VIRTIO_MMIO_CONFIG) {
        return virtio_config_read(s, offset - VIRTIO_MMIO_CONFIG, size_log2);
    }

    if (size_log2 == 2) {
        switch(offset) {
        case VIRTIO_MMIO_MAGIC_VALUE:
            val = 0x74726976;
            break;
        case VIRTIO_MMIO_VERSION:
            val = 2;
            break;
        case VIRTIO_MMIO_DEVICE_ID:
            val = s->device_id;
            break;
        case VIRTIO_MMIO_VENDOR_ID:
            val = s->vendor_id;
            break;
        case VIRTIO_MMIO_DEVICE_FEATURES:
            switch(s->device_features_sel) {
            case 0:
                val = s->device_features;
                break;
            case 1:
                val = 1; /* version 1 */
                break;
            default:
                val = 0;
                break;
            }
            break;
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            val = s->device_features_sel;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            val = s->queue_sel;
            break;
        case VIRTIO_MMIO_QUEUE_NUM_MAX:
            val = MAX_QUEUE_NUM;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            val = s->queue[s->queue_sel].num;
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            val = s->queue[s->queue_sel].desc_addr;
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            val = s->queue[s->queue_sel].avail_addr;
            break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            val = s->queue[s->queue_sel].used_addr;
            break;
#if VIRTIO_ADDR_BITS == 64
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            val = s->queue[s->queue_sel].desc_addr >> 32;
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            val = s->queue[s->queue_sel].avail_addr >> 32;
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
            val = s->queue[s->queue_sel].used_addr >> 32;
            break;
#endif
        case VIRTIO_MMIO_QUEUE_READY:
            val = s->queue[s->queue_sel].ready;
            break;
        case VIRTIO_MMIO_INTERRUPT_STATUS:
            val = s->int_status;
            break;
        case VIRTIO_MMIO_STATUS:
            val = s->status;
            break;
        case VIRTIO_MMIO_CONFIG_GENERATION:
            val = 0;
            break;
        default:
            val = 0;
            break;
        }
    } else {
        val = 0;
    }
#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_IO) {
        printf("virto_mmio_read: offset=0x%x val=0x%x size=%d\n", 
               offset, val, 1 << size_log2);
    }
#endif
    return val;
}

#if VIRTIO_ADDR_BITS == 64
static void set_low32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = (*paddr & ~(virtio_phys_addr_t)0xffffffff) | val;
}

static void set_high32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = (*paddr & 0xffffffff) | ((virtio_phys_addr_t)val << 32);
}
#else
static void set_low32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = val;
}
#endif

static void virtio_mmio_write(VIRTIODevice *opaque, uint32_t offset,
                              uint32_t val, int size_log2)
{
    VIRTIODevice *s = (VIRTIODevice*)opaque;
    
#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_IO) {
        printf("virto_mmio_write: offset=0x%x val=0x%x size=%d\n",
               offset, val, 1 << size_log2);
    }
#endif

    if (offset >= VIRTIO_MMIO_CONFIG) {
        virtio_config_write(s, offset - VIRTIO_MMIO_CONFIG, val, size_log2);
        return;
    }

    if (size_log2 == 2) {
        switch(offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            s->device_features_sel = val;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            if (val < MAX_QUEUE)
                s->queue_sel = val;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            if ((val & (val - 1)) == 0 && val > 0) {
                s->queue[s->queue_sel].num = val;
            }
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            set_low32(&s->queue[s->queue_sel].desc_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            set_low32(&s->queue[s->queue_sel].avail_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            set_low32(&s->queue[s->queue_sel].used_addr, val);
            break;
#if VIRTIO_ADDR_BITS == 64
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            set_high32(&s->queue[s->queue_sel].desc_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            set_high32(&s->queue[s->queue_sel].avail_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
            set_high32(&s->queue[s->queue_sel].used_addr, val);
            break;
#endif
        case VIRTIO_MMIO_STATUS:
            s->status = val;
            if (val == 0) {
                /* reset */
                set_irq(s->irq, 0);
                virtio_reset(s);
            }
            break;
        case VIRTIO_MMIO_QUEUE_READY:
            s->queue[s->queue_sel].ready = val & 1;
            break;
        case VIRTIO_MMIO_QUEUE_NOTIFY:
            if (val < MAX_QUEUE)
                queue_notify(s, val);
            break;
        case VIRTIO_MMIO_INTERRUPT_ACK:
            s->int_status &= ~val;
            if (s->int_status == 0) {
                set_irq(s->irq, 0);
            }
            break;
        }
    }
}

void virtio_set_debug(VIRTIODevice *s, int debug)
{
    s->debug = debug;
}

static void virtio_config_change_notify(VIRTIODevice *s)
{
    /* INT_CONFIG interrupt */
    s->int_status |= 2;
    set_irq(s->irq, 1);
}

/*********************************************************************/
/* block device */

typedef struct {
    uint32_t type;
    uint8_t *buf;
    int write_size;
    int queue_idx;
    int desc_idx;
} BlockRequest;

struct VIRTIOBlockDevice : public VIRTIODevice {
public:
    BlockDevice *bs;

    BOOL req_in_progress;
    BlockRequest req; /* request in progress */
} ;

typedef struct {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector_num;
} BlockRequestHeader;

#define VIRTIO_BLK_T_IN          0
#define VIRTIO_BLK_T_OUT         1
#define VIRTIO_BLK_T_FLUSH       4
#define VIRTIO_BLK_T_FLUSH_OUT   5

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

#define SECTOR_SIZE 512

static void virtio_block_req_end(VIRTIODevice *s, int ret)
{
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;
    int write_size;
    int queue_idx = s1->req.queue_idx;
    int desc_idx = s1->req.desc_idx;
    uint8_t *buf, buf1[1];

    switch(s1->req.type) {
    case VIRTIO_BLK_T_IN:
        write_size = s1->req.write_size;
        buf = s1->req.buf;
        if (ret < 0) {
            buf[write_size - 1] = VIRTIO_BLK_S_IOERR;
        } else {
            buf[write_size - 1] = VIRTIO_BLK_S_OK;
        }
        memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, write_size);
        free(buf);
        virtio_consume_desc(s, queue_idx, desc_idx, write_size);
        break;
    case VIRTIO_BLK_T_OUT:
        if (ret < 0)
            buf1[0] = VIRTIO_BLK_S_IOERR;
        else
            buf1[0] = VIRTIO_BLK_S_OK;
        memcpy_to_queue(s, queue_idx, desc_idx, 0, buf1, sizeof(buf1));
        virtio_consume_desc(s, queue_idx, desc_idx, 1);
        break;
    default:
        abort();
    }
}

static void virtio_block_req_cb(VIRTIODevice *opaque, int ret)
{
    VIRTIODevice *s = opaque;
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;

    virtio_block_req_end(s, ret);
    
    s1->req_in_progress = FALSE;

    /* handle next requests */
    queue_notify((VIRTIODevice *)s, s1->req.queue_idx);
}

/* XXX: handle async I/O */
static int virtio_block_recv_request(VIRTIODevice *s, int queue_idx,
                                     int desc_idx, int read_size,
                                     int write_size)
{
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;
    BlockDevice *bs = s1->bs;
    BlockRequestHeader h;
    uint8_t *buf;
    int len, ret;

    if (s1->req_in_progress)
        return -1;
    
    if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, sizeof(h)) < 0)
        return 0;
    s1->req.type = h.type;
    s1->req.queue_idx = queue_idx;
    s1->req.desc_idx = desc_idx;
    switch(h.type) {
    case VIRTIO_BLK_T_IN:
        s1->req.buf = (uint8_t*)malloc(write_size);
        s1->req.write_size = write_size;
        ret = bs->read_async(bs, h.sector_num, s1->req.buf, 
                             (write_size - 1) / SECTOR_SIZE,
                             virtio_block_req_cb, s);
        if (ret > 0) {
            /* asyncronous read */
            s1->req_in_progress = TRUE;
        } else {
            virtio_block_req_end(s, ret);
        }
        break;
    case VIRTIO_BLK_T_OUT:
        assert(write_size >= 1);
        len = read_size - sizeof(h);
        buf = (uint8_t*)malloc(len);
        memcpy_from_queue(s, buf, queue_idx, desc_idx, sizeof(h), len);
        ret = bs->write_async(bs, h.sector_num, buf, len / SECTOR_SIZE,
                              virtio_block_req_cb, s);
        free(buf);
        if (ret > 0) {
            /* asyncronous write */
            s1->req_in_progress = TRUE;
        } else {
            virtio_block_req_end(s, ret);
        }
        break;
    default:
        break;
    }
    return 0;
}

VIRTIODevice *virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs, const simif_t* sim)
{
    VIRTIOBlockDevice *s;
    uint64_t nb_sectors;

    s = (VIRTIOBlockDevice *)mallocz(sizeof(*s));
    virtio_init(s, bus,
                2, 8, virtio_block_recv_request, sim);
    s->bs = bs;
    
    nb_sectors = bs->get_sector_count(bs);
    put_le32(s->config_space, nb_sectors);
    put_le32(s->config_space + 4, nb_sectors >> 32);

    return (VIRTIODevice *)s;
}


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
  : sim(sim), intctrl(intctrl), interrupt_id(interrupt_id)
{
  std::map<std::string, std::string> argmap;

  for (auto arg : sargs) {
    size_t eq_idx = arg.find('=');
    if (eq_idx != std::string::npos) {
      argmap.insert(std::pair<std::string, std::string>(arg.substr(0, eq_idx), arg.substr(eq_idx+1)));
    }
  }


  auto it = argmap.find("img");
  if (it == argmap.end()) {
    // dummy block device.
  }
  else {
    std::string fname = it->second;

    VIRTIODevice* blk_dev;
    int irq_num, i, max_xlen, ram_flags;
    VIRTIOBusDef vbus_s, *vbus = &vbus_s;
    BlockDevice* bs = block_device_init(fname.c_str(), BF_MODE_RW); //initialization

    max_xlen=64;

    memset(vbus, 0, sizeof(*vbus));
    vbus->addr = VIRTIO_BASE_ADDR;
    irq_num = VIRTIO_IRQ;
    irq = new IRQSpike(intctrl, irq_num);

    // only one virtio block device
    //REQUIRE: register irq_num as plic_irq number
    // vbus->irq = &s->plci_irq[irq_num];
    vbus->irq = irq;

    blk_dev = virtio_block_init(vbus, bs, sim);
    vbus->addr += VIRTIO_SIZE;

  }

}

virtioblk_t::~virtioblk_t() {
    if (irq) delete irq;
}

std::string virtioblk_generate_dts(const sim_t* sim) {
  return std::string();
}

virtioblk_t* virtioblk_parse_from_fdt(
  const void* fdt, const sim_t* sim, reg_t* base,
    std::vector<std::string> sargs)
{
  uint32_t blkdev_int_id;
  if (fdt_parse_virtioblk(fdt, base, &blkdev_int_id, "ucbbar,blkdev") == 0) {
    abstract_interrupt_controller_t* intctrl = sim->get_intctrl();
    return new virtioblk_t(sim, intctrl, blkdev_int_id, sargs);
  } else {
    return nullptr;
  }
}

REGISTER_DEVICE(virtioblk, virtioblk_parse_from_fdt, virtioblk_generate_dts);
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
#include "cutils.h"
#include "fs.h"
#include "list.h"

// #define DEBUG_VIRTIO


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

// #define OPT_MEMCPY_RAM

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
    const simif_t* sim;
    /* MMIO only */
    IRQSpike* irq;
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
        // printf("bf_read_async successfully finished.\n");
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

BlockDevice *block_device_init(const char *filename,
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


static void virtio_init(VIRTIODevice *s, VIRTIOBusDef *bus,
                        uint32_t device_id, int config_space_size,
                        VIRTIODeviceRecvFunc *device_recv, const simif_t* sim)
{
    memset(s, 0, sizeof(*s));

    {
        s->sim = sim;
        /* MMIO case */
        s->irq = bus->irq;
    }

    s->device_id = device_id;
    s->vendor_id = 0xffff;
    s->config_space_size = config_space_size;
    s->device_recv = device_recv;
    virtio_reset(s);
}

static uint16_t virtio_read16(VIRTIODevice *s, virtio_phys_addr_t addr)
{
    mmu_t* simdram = s->sim->debug_mmu;
    return simdram->load<uint16_t>(addr);
}

static void virtio_write16(VIRTIODevice *s, virtio_phys_addr_t addr,
                           uint16_t val)
{
    mmu_t* simdram = s->sim->debug_mmu;
    simdram->store<uint16_t>(addr, val);
}

static void virtio_write32(VIRTIODevice *s, virtio_phys_addr_t addr,
                           uint32_t val)
{
    mmu_t* simdram = s->sim->debug_mmu;
    simdram->store<uint32_t>(addr, val);
}

static int memcpy_from_ram_intrapage(VIRTIODevice *s, uint8_t *buf,
                                  virtio_phys_addr_t addr, int count)
{
    mmu_t* simdram = s->sim->debug_mmu;
    int l = 0;
    while (count > 0) {
#ifdef OPT_MEMCPY_RAM
        if (addr & 1 || count < 2) {
            *(buf+l) = simdram->load<uint8_t>(addr);
            count -= 1;
            l += 1;
            addr += 1;
        }
        else if (addr & 2 || count < 4) {
            *(uint16_t*)(buf+l) = simdram->load<uint16_t>(addr);
            count -= 2;
            l += 2;
            addr += 2;
        }
        else if (addr & 4 || count < 8) {
            *(uint32_t*)(buf+l) = simdram->load<uint32_t>(addr);
            count -= 4;
            l += 4;
            addr += 4;
        }
        else {// addr is 8byte aligned
            *(uint64_t*)(buf+l) = simdram->load<uint64_t>(addr);
            count -= 8;
            l += 8;
            addr += 8;
        }
#else 
        buf[l] = simdram->load<uint8_t>(addr+l);
        l++;
        count--;
#endif
    }
    return l;
}

static int memcpy_to_ram_intrapage(VIRTIODevice *s, virtio_phys_addr_t addr,
                                   const uint8_t *buf, int count)
{
    mmu_t* simdram = s->sim->debug_mmu;
    int l = 0;
    while (count > 0) {
#ifdef OPT_MEMCPY_RAM
        if (addr & 1 || count < 2) {
            simdram->store<uint8_t>(addr, *(uint8_t*)(buf+l));
            count -= 1;
            l += 1;
            addr += 1;
        }
        else if (addr & 2 || count < 4) {
            simdram->store<uint16_t>(addr, *(uint16_t*)(buf+l));
            count -= 2;
            l += 2;
            addr += 2;
        }
        else if (addr & 4 || count < 8) {
            simdram->store<uint32_t>(addr, *(uint32_t*)(buf+l));
            count -= 4;
            l += 4;
            addr += 4;
        }
        else {// addr is 8byte aligned
            simdram->store<uint64_t>(addr, *(uint64_t*)(buf+l));
            count -= 8;
            l += 8;
            addr += 8;
        }
#else 
        simdram->store<uint8_t>(addr+l, buf[l]);
        l++;
        count--;
#endif
    }
    return l;
}

static int virtio_memcpy_from_ram(VIRTIODevice *s, uint8_t *buf,
                                  virtio_phys_addr_t addr, int count)
{
    uint8_t *ptr;
    int l;

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        memcpy_from_ram_intrapage(s, buf, addr, l);
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
        memcpy_to_ram_intrapage(s, addr, buf, l);
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

#ifdef DEBUG_VIRTIO
    if (to_queue) {
        printf("Reading from buf %p, len = %u, to queue qidx = %d, desc_idx = %d\n",
            buf, count, queue_idx, desc_idx);
    }
    else {
        printf("Reading from queue qidx = %d, desc_idx = %d, len = %u, to buf %p\n",
            queue_idx, desc_idx, count, buf);
    }
#endif
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
#ifdef DEBUG_VIRTIO
    printf("Reading successfully finished.\n");
#endif 
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
#ifdef DEBUG_VIRTIO
    printf("Consuming virtio desc qidx = %d, desc_idx = %d, desc_len = %d\n",
        queue_idx, desc_idx, desc_len);
#endif
    addr = qs->used_addr + 2;
    index = virtio_read16(s, addr);
    virtio_write16(s, addr, index + 1);
    addr = qs->used_addr + 4 + (index & (qs->num - 1)) * 8;
    virtio_write32(s, addr, desc_idx);
    virtio_write32(s, addr + 4, desc_len);
#ifdef DEBUG_VIRTIO
    printf("Consumed virtio desc qidx = %d, desc_idx = %d, desc_len = %d\n",
        queue_idx, desc_idx, desc_len);
#endif
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
            {
                printf("queue_notify: idx=%d read_size=%d write_size=%d\n",
                       queue_idx, read_size, write_size);
            }
#endif
            if (s->device_recv(s, queue_idx, desc_idx,
                               read_size, write_size) < 0)
                break;
        }
        qs->last_avail_idx++;
#ifdef DEBUG_VIRTIO
        printf("avail_idx = %d, last_avail_idx = %d.\n",
            avail_idx, qs->last_avail_idx);
#endif
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
#ifdef DEBUG_VIRTIO
    {
        printf("virto_config_read: offset=0x%x val=0x%x size=%d\n", 
               offset, val, 1 << size_log2);
    }
#endif
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
    {
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
    {
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
#ifdef DEBUG_VIRTIO
            {
        printf("queue_notify on qidx %d invoked by MMIO write begin.\n", val);
#endif
                queue_notify(s, val);
#ifdef DEBUG_VIRTIO
        printf("queue_notify on qidx %d invoked by MMIO write finished.\n", val);
            }
#endif
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
#ifdef DEBUG_VIRTIO
    printf("Entering req end func... ret = %d, req type =in?%d\n", ret, s1->req.type);
#endif 
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

#ifdef DEBUG_VIRTIO
        printf("Entering recv req function ... qidx = %d, desc_idx = %d, read_size = %d, write_size = %d\n",
            queue_idx, desc_idx, read_size, write_size);
#endif

    if (s1->req_in_progress)
#ifdef DEBUG_VIRTIO
    {
        printf("Request in progress, exit recv req function.\n");
        return -1;
    }
#else
        return -1;
#endif 
    if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, sizeof(h)) < 0)
        return 0;
    s1->req.type = h.type;
    s1->req.queue_idx = queue_idx;
    s1->req.desc_idx = desc_idx;
#ifdef DEBUG_VIRTIO
    printf("req in?=%d\n",h.type);
#endif
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
#ifdef DEBUG_VIRTIO
    printf("Exiting recv req function ... qidx = %d, desc_idx = %d, read_size = %d, write_size = %d\n",
        queue_idx, desc_idx, read_size, write_size);
#endif
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

/*********************************************************************/
/* 9p filesystem device */

typedef struct {
    struct list_head link;
    uint32_t fid;
    FSFile *fd;
} FIDDesc;

struct VIRTIO9PDevice : public VIRTIODevice {
    FSDevice *fs;
    int msize; /* maximum message size */
    struct list_head fid_list; /* list of FIDDesc */
    BOOL req_in_progress;
};

static FIDDesc *fid_find1(VIRTIO9PDevice *s, uint32_t fid)
{
    struct list_head *el;
    FIDDesc *f;

    list_for_each(el, &s->fid_list) {
        f = list_entry(el, FIDDesc, link);
        if (f->fid == fid)
            return f;
    }
    return NULL;
}

static FSFile *fid_find(VIRTIO9PDevice *s, uint32_t fid)
{
    FIDDesc *f;

    f = fid_find1(s, fid);
    if (!f)
        return NULL;
    return f->fd;
}

static void fid_delete(VIRTIO9PDevice *s, uint32_t fid)
{
    FIDDesc *f;

    f = fid_find1(s, fid);
    if (f) {
        s->fs->fs_delete(s->fs, f->fd);
        list_del(&f->link);
        free(f);
    }
}

static void fid_set(VIRTIO9PDevice *s, uint32_t fid, FSFile *fd)
{
    FIDDesc *f;

    f = fid_find1(s, fid);
    if (f) {
        s->fs->fs_delete(s->fs, f->fd);
        f->fd = fd;
    } else {
        f = (FIDDesc*)malloc(sizeof(*f));
        f->fid = fid;
        f->fd = fd;
        list_add(&f->link, &s->fid_list);
    }
}

#ifdef DEBUG_VIRTIO

typedef struct {
    uint8_t tag;
    const char *name;
} Virtio9POPName;

static const Virtio9POPName virtio_9p_op_names[] = {
    { 8, "statfs" },
    { 12, "lopen" },
    { 14, "lcreate" },
    { 16, "symlink" },
    { 18, "mknod" },
    { 22, "readlink" },
    { 24, "getattr" },
    { 26, "setattr" },
    { 30, "xattrwalk" },
    { 40, "readdir" },
    { 50, "fsync" },
    { 52, "lock" },
    { 54, "getlock" },
    { 70, "link" },
    { 72, "mkdir" },
    { 74, "renameat" },
    { 76, "unlinkat" },
    { 100, "version" },
    { 104, "attach" },
    { 108, "flush" },
    { 110, "walk" },
    { 116, "read" },
    { 118, "write" },
    { 120, "clunk" },
    { 0, NULL },
};

static const char *get_9p_op_name(int tag)
{
    const Virtio9POPName *p;
    for(p = virtio_9p_op_names; p->name != NULL; p++) {
        if (p->tag == tag)
            return p->name;
    }
    return NULL;
}

#endif /* DEBUG_VIRTIO */

static int marshall(VIRTIO9PDevice *s, 
                    uint8_t *buf1, int max_len, const char *fmt, ...)
{
    va_list ap;
    int c;
    uint32_t val;
    uint64_t val64;
    uint8_t *buf, *buf_end;

#ifdef DEBUG_VIRTIO
    
        printf(" ->");
#endif
    va_start(ap, fmt);
    buf = buf1;
    buf_end = buf1 + max_len;
    for(;;) {
        c = *fmt++;
        if (c == '\0')
            break;
        switch(c) {
        case 'b':
            assert(buf + 1 <= buf_end);
            val = va_arg(ap, int);
#ifdef DEBUG_VIRTIO
            
                printf(" b=%d", val);
#endif
            buf[0] = val;
            buf += 1;
            break;
        case 'h':
            assert(buf + 2 <= buf_end);
            val = va_arg(ap, int);
#ifdef DEBUG_VIRTIO
            
                printf(" h=%d", val);
#endif
            put_le16(buf, val);
            buf += 2;
            break;
        case 'w':
            assert(buf + 4 <= buf_end);
            val = va_arg(ap, int);
#ifdef DEBUG_VIRTIO
            
                printf(" w=%d", val);
#endif
            put_le32(buf, val);
            buf += 4;
            break;
        case 'd':
            assert(buf + 8 <= buf_end);
            val64 = va_arg(ap, uint64_t);
#ifdef DEBUG_VIRTIO
            
                printf(" d=%" PRId64, val64);
#endif
            put_le64(buf, val64);
            buf += 8;
            break;
        case 's':
            {
                char *str;
                int len;
                str = va_arg(ap, char *);
#ifdef DEBUG_VIRTIO
                
                    printf(" s=\"%s\"", str);
#endif
                len = strlen(str);
                assert(len <= 65535);
                assert(buf + 2 + len <= buf_end);
                put_le16(buf, len);
                buf += 2;
                memcpy(buf, str, len);
                buf += len;
            }
            break;
        case 'Q':
            {
                FSQID *qid;
                assert(buf + 13 <= buf_end);
                qid = va_arg(ap, FSQID *);
#ifdef DEBUG_VIRTIO
                
                    printf(" Q=%d:%d:%" PRIu64, qid->type, qid->version, qid->path);
#endif
                buf[0] = qid->type;
                put_le32(buf + 1, qid->version);
                put_le64(buf + 5, qid->path);
                buf += 13;
            }
            break;
        default:
            abort();
        }
    }
    va_end(ap);
    return buf - buf1;
}

/* return < 0 if error */
/* XXX: free allocated strings in case of error */
static int unmarshall(VIRTIO9PDevice *s, int queue_idx,
                      int desc_idx, int *poffset, const char *fmt, ...)
{
    VIRTIODevice *s1 = (VIRTIODevice *)s;
    va_list ap;
    int offset, c;
    uint8_t buf[16];

    offset = *poffset;
    va_start(ap, fmt);
    for(;;) {
        c = *fmt++;
        if (c == '\0')
            break;
        switch(c) {
        case 'b':
            {
                uint8_t *ptr;
                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 1))
                    return -1;
                ptr = va_arg(ap, uint8_t *);
                *ptr = buf[0];
                offset += 1;
#ifdef DEBUG_VIRTIO
                
                    printf(" b=%d", *ptr);
#endif
            }
            break;
        case 'h':
            {
                uint16_t *ptr;
                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 2))
                    return -1;
                ptr = va_arg(ap, uint16_t *);
                *ptr = get_le16(buf);
                offset += 2;
#ifdef DEBUG_VIRTIO
                
                    printf(" h=%d", *ptr);
#endif
            }
            break;
        case 'w':
            {
                uint32_t *ptr;
                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 4))
                    return -1;
                ptr = va_arg(ap, uint32_t *);
                *ptr = get_le32(buf);
                offset += 4;
#ifdef DEBUG_VIRTIO
                
                    printf(" w=%d", *ptr);
#endif
            }
            break;
        case 'd':
            {
                uint64_t *ptr;
                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 8))
                    return -1;
                ptr = va_arg(ap, uint64_t *);
                *ptr = get_le64(buf);
                offset += 8;
#ifdef DEBUG_VIRTIO
                
                    printf(" d=%" PRId64, *ptr);
#endif
            }
            break;
        case 's':
            {
                char *str, **ptr;
                int len;

                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 2))
                    return -1;
                len = get_le16(buf);
                offset += 2;
                str = (char*)malloc(len + 1);
                if (memcpy_from_queue(s1, str, queue_idx, desc_idx, offset, len))
                    return -1;
                str[len] = '\0';
                offset += len;
                ptr = va_arg(ap, char **);
                *ptr = str;
#ifdef DEBUG_VIRTIO
                
                    printf(" s=\"%s\"", *ptr);
#endif
            }
            break;
        default:
            abort();
        }
    }
    va_end(ap);
    *poffset = offset;
    return 0;
}

static void virtio_9p_send_reply(VIRTIO9PDevice *s, int queue_idx,
                                 int desc_idx, uint8_t id, uint16_t tag, 
                                 uint8_t *buf, int buf_len)
{
    uint8_t *buf1;
    int len;

#ifdef DEBUG_VIRTIO
     {
        if (id == 6)
            printf(" (error)");
        printf("\n");
    }
#endif
    len = buf_len + 7;
    buf1 = (uint8_t*)malloc(len);
    put_le32(buf1, len);
    buf1[4] = id + 1;
    put_le16(buf1 + 5, tag);
    memcpy(buf1 + 7, buf, buf_len);
    memcpy_to_queue((VIRTIODevice *)s, queue_idx, desc_idx, 0, buf1, len);
    virtio_consume_desc((VIRTIODevice *)s, queue_idx, desc_idx, len);
    free(buf1);
}

static void virtio_9p_send_error(VIRTIO9PDevice *s, int queue_idx,
                                 int desc_idx, uint16_t tag, uint32_t error)
{
    uint8_t buf[4];
    int buf_len;

    buf_len = marshall(s, buf, sizeof(buf), "w", -error);
    virtio_9p_send_reply(s, queue_idx, desc_idx, 6, tag, buf, buf_len);
}

typedef struct {
    VIRTIO9PDevice *dev;
    int queue_idx;
    int desc_idx;
    uint16_t tag;
} P9OpenInfo;

static void virtio_9p_open_reply(FSDevice *fs, FSQID *qid, int err,
                                 P9OpenInfo *oi)
{
    VIRTIO9PDevice *s = oi->dev;
    uint8_t buf[32];
    int buf_len;
    
    if (err < 0) {
        virtio_9p_send_error(s, oi->queue_idx, oi->desc_idx, oi->tag, err);
    } else {
        buf_len = marshall(s, buf, sizeof(buf),
                           "Qw", qid, s->msize - 24);
        virtio_9p_send_reply(s, oi->queue_idx, oi->desc_idx, 12, oi->tag,
                             buf, buf_len);
    }
    free(oi);
}

static void virtio_9p_open_cb(FSDevice *fs, FSQID *qid, int err,
                              void *opaque)
{
    P9OpenInfo *oi = (P9OpenInfo *)opaque;
    VIRTIO9PDevice *s = oi->dev;
    int queue_idx = oi->queue_idx;
    
    virtio_9p_open_reply(fs, qid, err, oi);

    s->req_in_progress = FALSE;

    /* handle next requests */
    queue_notify((VIRTIODevice *)s, queue_idx);
}

static int virtio_9p_recv_request(VIRTIODevice *s1, int queue_idx,
                                   int desc_idx, int read_size,
                                   int write_size)
{
    VIRTIO9PDevice *s = (VIRTIO9PDevice *)s1;
    int offset, header_len;
    uint8_t id;
    uint16_t tag;
    uint8_t buf[1024];
    int buf_len, err;
    FSDevice *fs = s->fs;

    if (queue_idx != 0)
        return 0;
    
    if (s->req_in_progress)
        return -1;
    
    offset = 0;
    header_len = 4 + 1 + 2;
    if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, header_len)) {
        tag = 0;
        goto protocol_error;
    }
    //size = get_le32(buf);
    id = buf[4];
    tag = get_le16(buf + 5);
    offset += header_len;
    
#ifdef DEBUG_VIRTIO
     {
        const char *name;
        name = get_9p_op_name(id);
        printf("9p: op=");
        if (name)
            printf("%s", name);
        else
            printf("%d", id);
    }
#endif
    /* Note: same subset as JOR1K */
    switch(id) {
    case 8: /* statfs */
        {
            FSStatFS st;

            fs->fs_statfs(fs, &st);
            buf_len = marshall(s, buf, sizeof(buf),
                               "wwddddddw", 
                               0,
                               st.f_bsize,
                               st.f_blocks,
                               st.f_bfree,
                               st.f_bavail,
                               st.f_files,
                               st.f_ffree,
                               0, /* id */
                               256 /* max filename length */
                               );
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 12: /* lopen */
        {
            uint32_t fid, flags;
            FSFile *f;
            FSQID qid;
            P9OpenInfo *oi;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "ww", &fid, &flags))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            oi = (P9OpenInfo *)malloc(sizeof(*oi));
            oi->dev = s;
            oi->queue_idx = queue_idx;
            oi->desc_idx = desc_idx;
            oi->tag = tag;
            err = fs->fs_open(fs, &qid, f, flags, virtio_9p_open_cb, oi);
            if (err <= 0) {
                virtio_9p_open_reply(fs, &qid, err, oi);
            } else {
                s->req_in_progress = TRUE;
            }
        }
        break;
    case 14: /* lcreate */
        {
            uint32_t fid, flags, mode, gid;
            char *name;
            FSFile *f;
            FSQID qid;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wswww", &fid, &name, &flags, &mode, &gid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->fs_create(fs, &qid, f, name, flags, mode, gid);
            }
            free(name);
            if (err) 
                goto error;
            buf_len = marshall(s, buf, sizeof(buf),
                               "Qw", &qid, s->msize - 24);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 16: /* symlink */
        {
            uint32_t fid, gid;
            char *name, *symgt;
            FSFile *f;
            FSQID qid;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wssw", &fid, &name, &symgt, &gid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->fs_symlink(fs, &qid, f, name, symgt, gid);
            }
            free(name);
            free(symgt);
            if (err)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf),
                               "Q", &qid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 18: /* mknod */
        {
            uint32_t fid, mode, major, minor, gid;
            char *name;
            FSFile *f;
            FSQID qid;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wswwww", &fid, &name, &mode, &major, &minor, &gid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->fs_mknod(fs, &qid, f, name, mode, major, minor, gid);
            }
            free(name);
            if (err)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf),
                               "Q", &qid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 22: /* readlink */
        {
            uint32_t fid;
            char buf1[1024];
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "w", &fid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->fs_readlink(fs, buf1, sizeof(buf1), f);
            }
            if (err)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf), "s", buf1);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 24: /* getattr */
        {
            uint32_t fid;
            uint64_t mask;
            FSFile *f;
            FSStat st;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wd", &fid, &mask))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            err = fs->fs_stat(fs, f, &st);
            if (err)
                goto error;

            buf_len = marshall(s, buf, sizeof(buf),
                               "dQwwwddddddddddddddd", 
                               mask, &st.qid,
                               st.st_mode, st.st_uid, st.st_gid,
                               st.st_nlink, st.st_rdev, st.st_size,
                               st.st_blksize, st.st_blocks,
                               st.st_atime_sec, (uint64_t)st.st_atime_nsec,
                               st.st_mtime_sec, (uint64_t)st.st_mtime_nsec,
                               st.st_ctime_sec, (uint64_t)st.st_ctime_nsec,
                               (uint64_t)0, (uint64_t)0,
                               (uint64_t)0, (uint64_t)0);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 26: /* setattr */
        {
            uint32_t fid, mask, mode, uid, gid;
            uint64_t size, atime_sec, atime_nsec, mtime_sec, mtime_nsec;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wwwwwddddd", &fid, &mask, &mode, &uid, &gid,
                           &size, &atime_sec, &atime_nsec, 
                           &mtime_sec, &mtime_nsec))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            err = fs->fs_setattr(fs, f, mask, mode, uid, gid, size, atime_sec,
                                 atime_nsec, mtime_sec, mtime_nsec);
            if (err)
                goto error;
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 30: /* xattrwalk */
        {
            /* not supported yet */
            err = -P9_ENOTSUP;
            goto error;
        }
        break;
    case 40: /* readdir */
        {
            uint32_t fid, count;
            uint64_t offs;
            uint8_t *buf;
            int n;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wdw", &fid, &offs, &count))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            buf = (uint8_t*)malloc(count + 4);
            n = fs->fs_readdir(fs, f, offs, buf + 4, count);
            if (n < 0) {
                err = n;
                goto error;
            }
            put_le32(buf, n);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, n + 4);
            free(buf);
        }
        break;
    case 50: /* fsync */
        {
            uint32_t fid;
            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "w", &fid))
                goto protocol_error;
            /* ignored */
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 52: /* lock */
        {
            uint32_t fid;
            FSFile *f;
            FSLock lock;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wbwddws", &fid, &lock.type, &lock.flags,
                           &lock.start, &lock.length,
                           &lock.proc_id, &lock.client_id))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                err = -P9_EPROTO;
            else
                err = fs->fs_lock(fs, f, &lock);
            free(lock.client_id);
            if (err < 0)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf), "b", err);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 54: /* getlock */
        {
            uint32_t fid;
            FSFile *f;
            FSLock lock;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wbddws", &fid, &lock.type,
                           &lock.start, &lock.length,
                           &lock.proc_id, &lock.client_id))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                err = -P9_EPROTO;
            else
                err = fs->fs_getlock(fs, f, &lock);
            if (err < 0) {
                free(lock.client_id);
                goto error;
            }
            buf_len = marshall(s, buf, sizeof(buf), "bddws",
                               &lock.type,
                               &lock.start, &lock.length,
                               &lock.proc_id, &lock.client_id);
            free(lock.client_id);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 70: /* link */
        {
            uint32_t dfid, fid;
            char *name;
            FSFile *f, *df;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wws", &dfid, &fid, &name))
                goto protocol_error;
            df = fid_find(s, dfid);
            f = fid_find(s, fid);
            if (!df || !f) {
                err = -P9_EPROTO;
            } else {
                err = fs->fs_link(fs, df, f, name);
            }
            free(name);
            if (err)
                goto error;
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 72: /* mkdir */
        {
            uint32_t fid, mode, gid;
            char *name;
            FSFile *f;
            FSQID qid;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wsww", &fid, &name, &mode, &gid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            err = fs->fs_mkdir(fs, &qid, f, name, mode, gid);
            if (err != 0)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf), "Q", &qid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 74: /* renameat */
        {
            uint32_t fid, new_fid;
            char *name, *new_name;
            FSFile *f, *new_f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wsws", &fid, &name, &new_fid, &new_name))
                goto protocol_error;
            f = fid_find(s, fid);
            new_f = fid_find(s, new_fid);
            if (!f || !new_f) {
                err = -P9_EPROTO;
            } else {
                err = fs->fs_renameat(fs, f, name, new_f, new_name);
            }
            free(name);
            free(new_name);
            if (err != 0)
                goto error;
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 76: /* unlinkat */
        {
            uint32_t fid, flags;
            char *name;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wsw", &fid, &name, &flags))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->fs_unlinkat(fs, f, name);
            }
            free(name);
            if (err != 0)
                goto error;
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 100: /* version */
        {
            uint32_t msize;
            char *version;
            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "ws", &msize, &version))
                goto protocol_error;
            s->msize = msize;
            //            printf("version: msize=%d version=%s\n", msize, version);
            free(version);
            buf_len = marshall(s, buf, sizeof(buf), "ws", s->msize, "9P2000.L");
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 104: /* attach */
        {
            uint32_t fid, afid, uid;
            char *uname, *aname;
            FSQID qid;
            FSFile *f;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "wwssw", &fid, &afid, &uname, &aname, &uid))
                goto protocol_error;
            err = fs->fs_attach(fs, &f, &qid, uid, uname, aname);
            if (err != 0)
                goto error;
            fid_set(s, fid, f);
            free(uname);
            free(aname);
            buf_len = marshall(s, buf, sizeof(buf), "Q", &qid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 108: /* flush */
        {
            uint16_t oldtag;
            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "h", &oldtag))
                goto protocol_error;
            /* ignored */
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 110: /* walk */
        {
            uint32_t fid, newfid;
            uint16_t nwname;
            FSQID *qids;
            char **names;
            FSFile *f;
            int i;

            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "wwh", &fid, &newfid, &nwname))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            names = (char**)mallocz(sizeof(names[0]) * nwname);
            qids = (FSQID*)malloc(sizeof(qids[0]) * nwname);
            for(i = 0; i < nwname; i++) {
                if (unmarshall(s, queue_idx, desc_idx, &offset, 
                               "s", &names[i])) {
                    err = -P9_EPROTO;
                    goto walk_done;
                }
            }
            err = fs->fs_walk(fs, &f, qids, f, nwname, names);
        walk_done:
            for(i = 0; i < nwname; i++) {
                free(names[i]);
            }
            free(names);
            if (err < 0) {
                free(qids);
                goto error;
            }
            buf_len = marshall(s, buf, sizeof(buf), "h", err);
            for(i = 0; i < err; i++) {
                buf_len += marshall(s, buf + buf_len, sizeof(buf) - buf_len,
                                    "Q", &qids[i]);
            }
            free(qids);
            fid_set(s, newfid, f);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 116: /* read */
        {
            uint32_t fid, count;
            uint64_t offs;
            uint8_t *buf;
            int n;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wdw", &fid, &offs, &count))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            buf = (uint8_t*)malloc(count + 4);
            n = fs->fs_read(fs, f, offs, buf + 4, count);
            if (n < 0) {
                err = n;
                free(buf);
                goto error;
            }
            put_le32(buf, n);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, n + 4);
            free(buf);
        }
        break;
    case 118: /* write */
        {
            uint32_t fid, count;
            uint64_t offs;
            uint8_t *buf1;
            int n;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wdw", &fid, &offs, &count))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            buf1 = (uint8_t*)malloc(count);
            if (memcpy_from_queue(s1, buf1, queue_idx, desc_idx, offset,
                                  count)) {
                free(buf1);
                goto protocol_error;
            }
            n = fs->fs_write(fs, f, offs, buf1, count);
            free(buf1);
            if (n < 0) {
                err = n;
                goto error;
            }
            buf_len = marshall(s, buf, sizeof(buf), "w", n);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 120: /* clunk */
        {
            uint32_t fid;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "w", &fid))
                goto protocol_error;
            fid_delete(s, fid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    default:
        printf("9p: unsupported operation id=%d\n", id);
        goto protocol_error;
    }
    return 0;
 error:
    virtio_9p_send_error(s, queue_idx, desc_idx, tag, err);
    return 0;
 protocol_error:
 fid_not_found:
    err = -P9_EPROTO;
    goto error;
}

VIRTIODevice *virtio_9p_init(VIRTIOBusDef *bus, FSDevice *fs,
                             const char *mount_tag, const simif_t* sim)

{
    VIRTIO9PDevice *s;
    int len;
    uint8_t *cfg;

    len = strlen(mount_tag);
    s = (VIRTIO9PDevice*)mallocz(sizeof(*s));
    virtio_init(s, bus,
                9, 2 + len, virtio_9p_recv_request, sim);
    s->device_features = 1 << 0;

    /* set the mount tag */
    cfg = s->config_space;
    cfg[0] = len;
    cfg[1] = len >> 8;
    memcpy(cfg + 2, mount_tag, len);

#ifdef DEBUG_VIRTIO
    printf("Config space: %#x%x", cfg[0], cfg[1]);
    for (int k = 0; k < len; k++) {
        putchar(cfg[2+k]);
    }
    putchar('\n');
#endif 

    s->fs = fs;
    s->msize = 8192;
    init_list_head(&s->fid_list);

    return (VIRTIODevice *)s;
}

virtio_base_t::virtio_base_t(
      const simif_t* sim,
      abstract_interrupt_controller_t *intctrl,
      uint32_t interrupt_id,
      std::vector<std::string> sargs)
  : sim(sim), intctrl(intctrl), interrupt_id(interrupt_id)
{}

virtio_base_t::~virtio_base_t() {

}

bool virtio_base_t::load(reg_t addr, size_t len, uint8_t *bytes) {
    if (len > 8) return false;

    if (len == 1) {
        // virtio_mmio_read will not return correct value here.
        uint8_t val = virtio_mmio_read(virtio_dev, addr, 0);
        read_little_endian_reg(val, 0, len, bytes);
        return true;
    }
    else if (len == 2) {
        // virtio_mmio_read will not return correct value here.
        uint16_t val = virtio_mmio_read(virtio_dev, addr, 1);
        read_little_endian_reg(val, 0, len, bytes);
        return true;
    }
    else if (len == 4) {
        uint32_t val = virtio_mmio_read(virtio_dev, addr, 2);
        read_little_endian_reg(val, 0, len, bytes);
    }
    else if (len == 8) {
        uint64_t low = virtio_mmio_read(virtio_dev, addr, 2);
        uint64_t high = virtio_mmio_read(virtio_dev, addr+4, 2);
        read_little_endian_reg(low | (high << 32), 0, len, bytes);
    }
    else {
        return false;
    }

    return true;

}

bool virtio_base_t::store(reg_t addr, size_t len, const uint8_t *bytes) {
    if (len > 8) return false;

    if (len == 1) {
        uint8_t val;
        write_little_endian_reg(&val, 0, len, bytes);
        virtio_mmio_write(virtio_dev, addr, val, 0);
        return true;
    }
    else if (len == 2) {
        uint16_t val;
        write_little_endian_reg(&val, 0, len, bytes);
        virtio_mmio_write(virtio_dev, addr, val, 1);
        return true;
    }
    else if (len == 4) {
        uint32_t val;
        write_little_endian_reg(&val, 0, len, bytes);
        virtio_mmio_write(virtio_dev, addr, val, 2);
    }
    else if (len == 8) {
        uint64_t val;
        write_little_endian_reg(&val, 0, len, bytes);
        virtio_mmio_write(virtio_dev, addr, val & 0xffffffff, 2);
        virtio_mmio_write(virtio_dev, addr+4, (val >> 32) & 0xffffffff, 2);
    }
    else {
        return false;   
    }
    return true;
}



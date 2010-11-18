/**
 * xiprd.c - an XIP-capable Ram-disk module for Linux.
 * 
 * An attempt to enable direct memory accesses 
 * to/from the ramdisk for mmap'd files
 *
 * Author: Sam Post <sampost@gmail.com>
 * License: Dual BSD/GPL
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/hdreg.h>

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Sam Post <sampost@gmail.com>");
MODULE_DESCRIPTION("Simple XIP-capable ram-backed block driver");

#define BD_NAME_MAX    32
#define DRV_NAME       "xiprd"
#define NUM_MINORS     16
#define SECTOR_SHIFT   9
#define SECTOR_SIZE    (1 << SECTOR_SHIFT)
#define KERNEL_SHIFT   9

/*
 * Module parameters: sector size, total bytes
 */
static int sector_size    = SECTOR_SIZE;
static u_long num_sectors = (1024 * 1024 * 1024ULL >> SECTOR_SHIFT);
module_param(sector_size, int, 0444);
module_param(num_sectors, ulong, 0444);
MODULE_PARM_DESC(num_sectors, "Number of sectors. Defaults to 1GiB/sector_size.");
MODULE_PARM_DESC(sector_size, "Sector byte size. Defaults to 512.");

/*
 * data structures: device struct, etc
 */
struct xiprd_dev {
    int                     major;
    char                    name[BD_NAME_MAX];
    spinlock_t              lock;
    struct gendisk        * disk;
    u64                     size;
    struct request_queue  * queue;
    u8                    * ramdisk;
};

struct xiprd_dev globaldev;
static sector_t kernel_sectors;

/*
 * block device file operations
 */
static int xiprd_getgeo(struct block_device *bdev, struct hd_geometry *geo);
static int xiprd_make_request(struct request_queue * q, struct bio * bi);

struct block_device_operations xiprd_bd_ops =
{
    .owner    = THIS_MODULE,
    .getgeo   = xiprd_getgeo,
};


static int xiprd_make_request(struct request_queue * q, struct bio * bi)
{
    struct xiprd_dev * dev;
    struct bio_vec * bvec;
    int    segno;
    char * dest, * buf;
    loff_t offset;

    dev = q->queuedata;

    /* convert from kernel sector to byte offset */
    offset = bi->bi_sector << KERNEL_SHIFT;

    spin_lock_irq(&dev->lock);
    bio_for_each_segment(bvec, bi, segno)
    {
        dest = dev->ramdisk + offset;

        buf = kmap_atomic(bvec->bv_page, KM_USER0);
        if(bio_data_dir(bi) == WRITE)
            memcpy(dest, buf, bvec->bv_len);
        else
            memcpy(buf, dest, bvec->bv_len);
        kunmap_atomic(buf, KM_USER0);

        offset += bvec->bv_len;
    }
    spin_unlock_irq(&dev->lock);

    bio_endio(bi, 0);
    return 0; /* non-zero causes re-submission to another (layered) device */
}

static int xiprd_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
    /* we are a RAM disk, no CHS. Make something up such that C*H*S = capacity. */
    geo->heads = (u8) (1 << 6);
    geo->sectors = (u8) (1 << 5);
    geo->cylinders = get_capacity(bdev->bd_disk) >> 11;
    return 0;
}

static int xiprd_init(void)
{
    struct xiprd_dev * dev = &globaldev;
    int    rc = 0;

    memset(dev, 0, sizeof(struct xiprd_dev));

    /* setup device name & size */
    strncpy(dev->name, DRV_NAME, BD_NAME_MAX);

    /* allocate the main RAM disk */
    dev->size = ((u64)num_sectors * (u64)sector_size);
    dev->ramdisk = vmalloc(dev->size);
    if(!dev->ramdisk)
    {
        rc = -ENOMEM;
        goto err_out;
    }

    /* register the block device, allocate and init a disk */
    rc = register_blkdev(0, dev->name);
    if(rc < 0)
        goto err_out;

    dev->disk = alloc_disk(NUM_MINORS);
    if(!dev->disk)
    {
        rc = -ENOMEM;
        goto err_out;
    }

    snprintf(dev->disk->disk_name, BD_NAME_MAX,
             "%s%u", dev->name, 0);
    dev->disk->major = dev->major;
    dev->disk->first_minor = 0 * NUM_MINORS;
    dev->disk->fops = &xiprd_bd_ops;
    dev->disk->private_data = dev;

    /* we don't need a block queue, directly handle bios */
    dev->disk->queue = blk_alloc_queue(GFP_KERNEL);
    if(!dev->disk->queue)
    {
        rc = -ENOMEM;
        goto err_out;
    }
    blk_queue_make_request(dev->disk->queue, xiprd_make_request);
    blk_queue_logical_block_size(dev->disk->queue, sector_size);

    dev->disk->queue->queuedata = dev; /* store for later use */

    kernel_sectors = ((u64)num_sectors*sector_size) >> KERNEL_SHIFT;
    set_capacity(dev->disk, kernel_sectors);

    spin_lock_init(&dev->lock);

    /* reads will (almost certainly) happen during this call */
    add_disk(dev->disk);

    return 0;

err_out:
    if(dev->ramdisk)
        vfree(dev->ramdisk);
    if(dev->disk)
    {
        del_gendisk(dev->disk);
        put_disk(dev->disk);
        blk_cleanup_queue(dev->disk->queue);
        unregister_blkdev(dev->major, dev->name);
    }

    return rc;
}

static void xiprd_exit(void)
{
    struct xiprd_dev * dev = &globaldev;
    
    if(dev->ramdisk)
        vfree(dev->ramdisk);
    if(dev->disk)
    {
        del_gendisk(dev->disk);
        put_disk(dev->disk);
        blk_cleanup_queue(dev->disk->queue);
        unregister_blkdev(dev->major, dev->name);
    }
}


module_init(xiprd_init);
module_exit(xiprd_exit);

/*
 * simple FAT32(VFAT) implementation
 *
 * small memory footprint (512 + alpha) without dynamic allocation
 * for memory constrained bootloaders
 *
 * Constraints: little-endian cpu, only single instance of a fat partition can be opened
 *
 * Copyright (C) 2017 Samsung Electronics
 *
 * Ikjoon Jang <ij.jang@samsung.com>
 *
 * This project is licensed under the terms of the MIT license.
 */
 
/* https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system
 
 FAT32 fs = { Reserved sectors - FATs - Roots - Files }
 
 */

#define XFAT_PATHMAX		128
#define FAT_DELETE_CHAR		(0xe5)

 
 /**************************************
 *  FAT32/VFAT internal data structures
 ***************************************/

 /* offset 0xb @ boot sector, only for disk layout(unaligenment), never instantized  */
struct xfat32_bpb {
	/* DOS 2.0 */
        uint16_t	sector_sz16;	/* 0x00 !bytes per logical sector (mostly 512) */
	uint8_t		cluster_sz;	/* 0x02 !sectors per a cluster */
	uint16_t	nr_reserved;	/* 0x03 !count of reserved sectors */
        uint8_t		nr_fats;	/* 0x05 !number of FATs */
        uint16_t	nr_dirents16;	/* 0x06 FAT12/16 max number of root directory entries */
        uint16_t	nr_sectors16;	/* 0x08 total logical sectors */
	uint8_t		media_type;	/* 0x0a */
	uint16_t	fat_size16;	/* 0x0b FAT12/16 logical sectors per a FAT */

	/* DOS 3.31 */
	uint16_t	track_size;	/* 0x0d physical sectors per track */
	uint16_t	nr_heads;	/* 0x0f number of heads, recommend 1 here */
	uint32_t	nr_hidden;	/* 0x11 count of hidden sectors */
	uint32_t	nr_sectors;	/* 0x15 !total logical sectors 32bit */

	/* FAT32 */
	uint32_t	fat_size;	/* 0x19 !sectors per FAT */
	uint16_t	flags;		/* 0x1d driver descriptions */
	uint16_t	version;	/* 0x1f version */
	uint32_t	cluster_root;	/* 0x21 cluster number of root directory */
	uint16_t	fs_info		/* 0x25 logical sector number of FS information sector */

	uint16_t	boot_sector;	/* 0x27 first logical sector of a copy of the three FAT32 boot sectors */
	/* 0x29 don't need anymore...
	uint8_t	reserved[38];
	*/
} __attribute__((packed));

/* 32bytes, disk layout = memory layout */
struct xfat32_dentry {
	uint8_t		file_name[8];
	uint8_t		file_ext[3];
	uint8_t		file_attr;	
	uint8_t		attr0;		/* 0x0c, VFAT: bit 3/4 for case information */
	uint8_t		attr1;		/* 0x0d, VFAT: create time in msec */
	uint16_t	ctime;		/* 0x0e */
	uint16_t	cdate;		/* 0x10 */
	uint16_t	adate;		/* 0x12 access time*/
	uint16_t	clust_hi;	/* 0x14 */
	uint16_t	mtime;		/* 0x16 last modified time */
	uint16_t	mdate;		/* 0x18 last modified date */
	uint16_t	clust_lo;	/* 0x1a */
	uint32_t	file_size;	/* 0x1c file size in bytes*/
} __attribute__((packed));

/***********************************
 *  XFAT internal data structures
 ***********************************/
struct xfat_storage_provider {
	void *buffer;		/* sector sized buffer provided by host */
	int  (*open)(const int sector_size);
	int  (*read_sectors)(struct xfat_storage_provider *host, void *buffer, const int sector, const nr);
	void (*close)(void);
};

struct xfat {
	int		slba;
	int		elba;
	struct xfat_storage_provider	*host;
	void		*buffer;
	
	int		cur_sector;	/* sector number currently buffer holds */
	
	/* from FAT32 BPB */
	int		root_cluster;
	int		cluster_sz;	/* sectors per cluster */
	int		cluster_begins;	/* head sector number for first cluster */
	
	int		nr_fats;
	int		data_sector;
	uint32_t	nr_sectors
	int		sz_fat;
	
};

static struct xfat xfat;

static inline int is_power_of_2(uint32_t v)
{
	return v && !(v & (v - 1));
}

static inline uint8_t read_byte(const uint8_t *buf, const int offset)
{
	return *(volatile uint8_t *)(&buf[offset]);
}

static inline uint16_t read_half(const uint8_t *buf, const int offset)
{
	const uintptr_t addr = (uintptr_t)&buf[offset];
	uint16_t ret;
	if (addr & 3)
		ret = (uint16_t)read_byte(buf, offset) | (uint16_t)(read_byte(buf, offset + 1) << 8);
	else
		ret = *(volatile uint16_t *)(&buf[offset]);
	return ret;
}

static inline uint32_t read_word(const uint8_t *buf, const int offset)
{
	const uintptr_t addr = (uintptr_t)&buf[offset];
	uint16_t ret;

	if (addr & 7)
		ret = (uint32_t)read_half(buf, offset) | (uint32_t)(read_half(buf, offset) << 16);
	else
		ret = *(volatile uint32_t *)(&buf[offset]);

	return ret;
}

static inline void read_unaligned(void *dst, const uint8_t *buf, const int offset, const int size)
{
	if (size == sizeof(uint32_t))
		*(uint32_t *)dst = read_word(buf, offset);
	else if (size == sizeof(uint16_t))
		*(uint16_t *)dst = read_half(buf, offset);
	else if (size == sizeof(uint8_t))
		*(uint8_t *)dst = read_byte(buf, offset);
	else
		*(uint8_t *)dst = *(volatile uint32_t *)0x3;
}

#define read_bpb_unit(bpb, member, buf) \
	read_unaligned(&bpb->member, buf, (int)(((struct xfat32_bpb *)0)->member), sizeof(bpb->member))

static int read_sector(const int sector)
{
	struct xfat_storage_provider *host = xfat.host;	
	
	if (xfat.cur_sector != sector) {
		if (host->read_sectors(xgpt.host, xgpt.buffer, lba, 1) < 0)
			return -1;
		else
			xfat.cur_sector = sector;
	}
	return 0;
}

static int read_bpb(void)
{
	int ret;
	struct xfat32_bpb *bpb = &xfat.bpb;
	uint8_t *ptr;

	/* boot sector */
	ret = host->read_sectors(xfat.buffer, host, 0, 1);
	if (ret < 0)
		return ret;

	/* read unaligned disk layout to running structure */
	ptr = xfat.buffer + 0x0b;

	read_bpb_unit(bpb, sector_sz, ptr);
	read_bpb_unit(bpb, cluster_sz, ptr);
	read_bpb_unit(bpb, nr_reserved, ptr);
        read_bpb_unit(bpb, nr_fats, ptr);
        read_bpb_unit(bpb, nr_dirents, ptr);
        read_bpb_unit(bpb, nr_sectors16, ptr);
	read_bpb_unit(bpb, media_type, ptr);
	read_bpb_unit(bpb, fat_size16, ptr);
	read_bpb_unit(bpb, track_size, ptr);
	read_bpb_unit(bpb, nr_heads, ptr);
	read_bpb_unit(bpb, nr_hidden, ptr);
	read_bpb_unit(bpb, nr_sectors, ptr);
	read_bpb_unit(bpb, fat_size, ptr);
	read_bpb_unit(bpb, flags, ptr);
	read_bpb_unit(bpb, version, ptr);
	read_bpb_unit(bpb, cluster_root, ptr);
	read_bpb_unit(bpb, fs_info, ptr);
	read_bpb_unit(bpb, boot_sector, ptr);
	
	if (read_half(xfat.buffer, 0x1fe) != 0x55aa)
		return -1;
	if (memcmp(xfat.buffer + 0x52, "FAT32", 5))
		return -1;
	if (is_power_of_2(bpb->sector_sz) != 512)	/* xfat only supports 512 bytes sector */
		return -1;
	if (!is_power_of_2(bpb->cluster_sz))
		return -1;
	if (bpb->nr_reserved < 1)	/* at least 1 for this boot sector */
		return -1;
	if (bpb->nr_fats != 1 && bpb->nr_fats != 2)	/* almost 2 */
		return -1;	
	if (bpb->media_type < 0xe5)	/* 0xe5 ... 0xff for FATID */
		return -1;	
	if (!bpb->fat_size)
		return -1;
	
	return 0;
}

static int is_delimeter(const char c)
{
	return !!(c == '/' || c == '\\');
}
static int valid_cluster(const uint32_t clust)
{
	return !!(clust >= 2 && clust <= 0xffffff
}

/* no recursive calls in resolving file path */
static int get_dentry(const char *filepath, struct xfat_dentry *d)
{
	/* interation info */
	struct diter {
		uint32_t	cluster;
		int		sector;
	} iter = { xfat.root_cluster, -1, 0 };
	
	int cluster = xfat.root_cluster;
	
	const char *s = filepath;
	const char *e;
	
	while (1) {
		while (*s && is_delimeter(*s))
			s++;
		
		if (!(*s))
			return -1;
		
		e = s;
		while (*e && !is_delimeter(*e))
			e++;
		
		/* find dentry for s ~ e */
		if (iter.sector >= xfat.cluster_sz) {
			int next_cluster = fat_entry(cluster);
			if (
		}
	}
	
	return 0;
}

int xfat_readfile(const char *filepath, void *buf, unsigned int offset, int size)
{
	struct xfat_dentry d;
	int ret;
	
	ret = get_dentry(filepath, &d);
	
	if (ret < 0)
		return -1;
}

int xfat_init(int lba, int nr_sectors, struct xfat_storage_provider *host)
{
	int ret;

	xfat.host = host;
	xfat.buffer = host->buffer;

	ret = read_bpb();
	if (ret < 0)
		return ret;
}

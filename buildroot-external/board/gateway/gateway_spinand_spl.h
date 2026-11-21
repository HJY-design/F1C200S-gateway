/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board-specific W25N01GV reader. SUNIV ROM uses 1024 payload bytes/page;
 * both SPL and U-Boot are packed with the same convention by make-nand-images.py.
 * Protocol: Winbond W25N01GV Rev G, sections 7.2-7.3, 8.2.2/3/7/14/15.
 * The fixed-page SUNIV approach follows Icenowy Zheng's October 2022 proposal.
 * gateway_spi_xfer() and udelay() must be provided by the including source.
 */
#ifndef GATEWAY_SPINAND_SPL_H
#define GATEWAY_SPINAND_SPL_H

#define GN_PAYLOAD_PAGE 1024U
#define GN_PHYSICAL_PAGE 2048U
#define GN_PAGES_PER_BLOCK 64U
#define GN_BOOT_BLOCKS 32U /* First 4 MiB only; never cross into kernel. */
#define GN_WAIT_POLLS 10000U

static u8 gn_blocks[GN_BOOT_BLOCKS]; /* 0 unknown, 1 good, 2 bad */
static u32 gn_cached_page = ~0U;
static u8 gn_cached_status;

static int gn_feature(u8 reg, u8 *value)
{
	u8 tx[] = { 0x0f, reg };

	return gateway_spi_xfer(tx, sizeof(tx), value, 1);
}

static int gn_wait(u8 *status)
{
	unsigned int i;
	int ret;

	for (i = 0; i < GN_WAIT_POLLS; i++) {
		ret = gn_feature(0xc0, status);
		if (ret)
			return ret;
		if (!(*status & 1))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int gn_page(u32 page)
{
	u8 tx[] = { 0x13, page >> 16, page >> 8, page };
	int ret;

	if (page == gn_cached_page)
		return 0;
	gn_cached_page = ~0U;
	ret = gateway_spi_xfer(tx, sizeof(tx), NULL, 0);
	if (ret)
		return ret;
	ret = gn_wait(&gn_cached_status);
	if (!ret)
		gn_cached_page = page;
	return ret;
}

static int gn_cache(u32 column, u8 *buf, u32 len)
{
	u8 tx[] = { 0x03, column >> 8, column, 0x00 };

	return gateway_spi_xfer(tx, sizeof(tx), buf, len);
}

static int gn_bad(u32 block)
{
	u8 marker;
	int ret;

	if (gn_blocks[block])
		return gn_blocks[block] == 2;
	ret = gn_page(block * GN_PAGES_PER_BLOCK);
	if (ret)
		return ret;
	/* Reserved OOB byte 0 is the factory BBM; ignore data ECC for this scan. */
	ret = gn_cache(GN_PHYSICAL_PAGE, &marker, 1);
	if (ret)
		return ret;
	gn_blocks[block] = marker == 0xff ? 1 : 2;
	return marker != 0xff;
}

static int gn_init(void)
{
	u8 reset = 0xff, id_cmd[] = { 0x9f, 0x00 }, id[3];
	u8 cfg, status, tx[3];
	int ret;

	memset(gn_blocks, 0, sizeof(gn_blocks));
	gn_cached_page = ~0U;
	/* On a warm reset, let an outstanding program/erase finish first. */
	ret = gn_wait(&status);
	if (ret)
		return ret;
	ret = gateway_spi_xfer(&reset, 1, NULL, 0);
	if (ret)
		return ret;
	/* RESET tRST is 500 us maximum; give it time before polling BUSY. */
	udelay(500);
	ret = gn_wait(&status);
	if (ret)
		return ret;
	ret = gateway_spi_xfer(id_cmd, sizeof(id_cmd), id, sizeof(id));
	if (ret)
		return ret;
	if (id[0] != 0xef || id[1] != 0xaa || id[2] != 0x21)
		return -ENODEV;
	ret = gn_feature(0xb0, &cfg);
	if (ret)
		return ret;
	/* Preserve OTP/lock bits, enable internal ECC and buffer-read mode. */
	tx[0] = 0x1f;
	tx[1] = 0xb0;
	tx[2] = cfg | 0x18;
	ret = gateway_spi_xfer(tx, sizeof(tx), NULL, 0);
	if (ret)
		return ret;
	ret = gn_wait(&status);
	if (ret)
		return ret;
	ret = gn_feature(0xb0, &cfg);
	if (ret)
		return ret;
	if ((cfg & 0x18) != 0x18 || (cfg & 0x40))
		return -EIO; /* OTP access must be off. */
	ret = gn_bad(0);
	/* This image starts at block 0. ROM recovery on a bad block 0 is untested. */
	return ret > 0 ? -EIO : ret;
}

static int gn_map(u32 logical_page, u32 *physical_page)
{
	u32 ordinal = logical_page / GN_PAGES_PER_BLOCK, block;
	int bad;

	for (block = 0; block < GN_BOOT_BLOCKS; block++) {
		bad = gn_bad(block);
		if (bad < 0)
			return bad;
		if (bad)
			continue;
		if (!ordinal) {
			*physical_page = block * GN_PAGES_PER_BLOCK +
					logical_page % GN_PAGES_PER_BLOCK;
			return 0;
		}
		ordinal--;
	}
	return -ENOSPC;
}

static int gn_read(u8 *buf, u32 addr, u32 len)
{
	u32 page, column, chunk;
	int ret;

	if (addr > GN_BOOT_BLOCKS * GN_PAGES_PER_BLOCK * GN_PAYLOAD_PAGE ||
	    len > GN_BOOT_BLOCKS * GN_PAGES_PER_BLOCK * GN_PAYLOAD_PAGE - addr)
		return -ENOSPC;
	while (len) {
		ret = gn_map(addr / GN_PAYLOAD_PAGE, &page);
		if (ret)
			return ret;
		ret = gn_page(page);
		if (ret)
			return ret;
		/* 10 = uncorrectable; 11 = invalid in buffer mode. Both fail closed. */
		if (gn_cached_status & 0x20)
			return -EBADMSG;
		column = addr % GN_PAYLOAD_PAGE;
		chunk = GN_PAYLOAD_PAGE - column;
		if (chunk > 60)
			chunk = 60; /* Controller FIFO: four command bytes + <=60 data. */
		if (chunk > len)
			chunk = len;
		ret = gn_cache(column, buf, chunk);
		if (ret)
			return ret;
		buf += chunk;
		addr += chunk;
		len -= chunk;
	}
	return 0;
}
#endif

/* SPDX-License-Identifier: GPL-2.0+ */
/* Protocol mock for the actual reader compiled into U-Boot SPL. No MMIO test. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
static u8 flash[32 * 64][2112];
static u8 ecc[32 * 64], cfg, id[3] = {0xef, 0xaa, 0x21};
static u32 selected, delays, page_reads, busy_count;
static int fail, stuck;
static void udelay(unsigned int us) { delays += us; }
static int gateway_spi_xfer(const u8 *tx, u32 txlen, u8 *rx, u32 rxlen)
{
    if (fail) return -ETIMEDOUT;
    assert(txlen + rxlen <= 64);
    switch (tx[0]) {
    case 0xff: assert(txlen == 1 && !rxlen); selected = 0; break;
    case 0x9f: assert(txlen == 2 && tx[1] == 0 && rxlen == 3); memcpy(rx,id,3); break;
    case 0x1f: assert(txlen == 3 && tx[1] == 0xb0 && !rxlen); cfg = tx[2]; break;
    case 0x0f:
        assert(txlen == 2 && rxlen == 1);
        if (tx[1] == 0xb0) *rx = cfg;
        else {
            assert(tx[1] == 0xc0);
            *rx = ecc[selected] | ((stuck || busy_count) ? 1 : 0);
            if (busy_count) busy_count--;
        }
        break;
    case 0x13:
        assert(txlen == 4 && !rxlen);
        selected = ((u32)tx[1]<<16) | ((u32)tx[2]<<8) | tx[3];
        assert(selected < 32 * 64); page_reads++; busy_count = 2; break;
    case 0x03: {
        u32 col = ((u32)tx[1]<<8) | tx[2];
        assert(txlen == 4 && tx[3] == 0 && col + rxlen <= 2112);
        memcpy(rx, flash[selected]+col, rxlen); break;
    }
    default: assert(0);
    }
    return 0;
}
#include "gateway_spinand_spl.h"

static u8 pattern(u32 n) { return (u8)((n * 37U) ^ (n >> 10) ^ (n >> 16)); }
static void fixture(unsigned int bad1, unsigned int bad2)
{
    u32 b, p, c, ordinal = 0;
    memset(flash,0xff,sizeof(flash)); memset(ecc,0,sizeof(ecc));
    selected=0; delays=0; page_reads=0; busy_count=0; fail=stuck=0; cfg=0;
    id[0]=0xef;
    for (b=0;b<32;b++) {
        if (b==bad1 || b==bad2) {
            flash[b*64][2048]=0; ecc[b*64]=0x20; continue;
        }
        for (p=0;p<64;p++) for (c=0;c<1024;c++)
            flash[b*64+p][c]=pattern((ordinal*64+p)*1024+c);
        ordinal++;
    }
}
static void check(u32 start, u32 count)
{
    u8 out[5000]; u32 i;
    assert(count <= sizeof(out));
    assert(gn_read(out,start,count)==0);
    for(i=0;i<count;i++) assert(out[i]==pattern(start+i));
}
int main(void)
{
    u8 out[100]; u32 reads;
    fixture(1,3); assert(!gn_init()); assert(cfg==0x18);
    check(0,80); check(997,3200); check(65510,4096); check(131000,4096);
    reads=page_reads; check(131000,20); assert(page_reads <= reads+1);
    ecc[0]=0x10; gn_cached_page=~0U; check(0,60); /* Corrected page accepted. */
    ecc[0]=0x20; gn_cached_page=~0U; assert(gn_read(out,0,60)==-EBADMSG);
    ecc[0]=0x30; gn_cached_page=~0U; assert(gn_read(out,0,60)==-EBADMSG);
    assert(gn_read(out,2097151,2)==-ENOSPC);
    assert(gn_read(out,32*64*1024-1024,1)==-ENOSPC); /* Good-block capacity. */
    stuck=1; gn_cached_page=~0U; assert(gn_read(out,0,1)==-ETIMEDOUT);
    assert(delays>=10000); stuck=0;
    fail=1; gn_cached_page=~0U; assert(gn_read(out,0,1)==-ETIMEDOUT); fail=0;
    fixture(99,99); id[0]=0; assert(gn_init()==-ENODEV);
    fixture(0,99); assert(gn_init()==-EIO);
    fixture(99,99); assert(!gn_init()); check(2097152-80,80);
    puts("SPINAND_SPL_PROTOCOL_TEST_PASS: page crossings, bad blocks, ECC, capacity, ID and timeouts (mock, no MMIO/hardware)");
    return 0;
}

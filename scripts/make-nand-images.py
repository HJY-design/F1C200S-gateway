"""Pack and check the W25N01GV gateway images; never access a flash device."""
import hashlib
import json
import pathlib
import struct
import subprocess
import zlib

base = pathlib.Path(__file__).resolve().parents[1]
images = base / 'output/images'
host = base / 'output/host'
boot = (images / 'u-boot-sunxi-with-spl.bin').read_bytes()
compiled_uboot = (base / 'output/build/uboot-2025.01/u-boot.bin').read_bytes()
for setting in (base / 'buildroot-external/board/gateway/uboot-default.env').read_text().splitlines():
    if setting and not setting.startswith('#'):
        assert setting.encode() + b'\0' in compiled_uboot, 'Built U-Boot environment mismatch: ' + setting
assert boot[4:12] == b'eGON.BT0', 'Missing SUNIV SPL header'
checksum, spl_length = struct.unpack_from('<II', boot, 12)
assert 32 <= spl_length <= 0x8000 and spl_length % 4 == 0
spl = bytearray(boot[:spl_length])
struct.pack_into('<I', spl, 12, 0x5f0a6c39)
assert sum(struct.unpack('<' + 'I'*(spl_length//4), spl)) & 0xffffffff == checksum
assert boot[0x8000:0x8004] == b'\x27\x05\x19\x56', 'Expected legacy U-Boot at logical 32KiB'
header = bytearray(boot[0x8000:0x8040])
header_crc = struct.unpack_from('>I', header, 4)[0]
struct.pack_into('>I', header, 4, 0)
assert zlib.crc32(header) == header_crc
payload_length, payload_crc = struct.unpack_from('>I', header, 12)[0], struct.unpack_from('>I', header, 24)[0]
assert zlib.crc32(boot[0x8040:0x8040+payload_length]) == payload_crc
assert 0x8040 + payload_length <= len(boot)
packed = bytearray()
for start in range(0, len(boot), 1024):
    packed += boot[start:start+1024].ljust(1024, b'\xff') + b'\xff'*1024
assert len(packed) <= 0x400000
unpacked = b''.join(packed[i:i+1024] for i in range(0,len(packed),2048))
assert unpacked[:len(boot)] == boot
(images / 'boot-rom-padded.bin').write_bytes(packed)

its = '''/dts-v1/;
/ {
    description = "F1C200S gateway NAND kernel + board device tree";
    #address-cells = <1>;
    images {
        kernel {
            description = "Linux 6.12.109 ARM926 zImage";
            data = /incbin/("zImage");
            type = "kernel";
            arch = "arm";
            os = "linux";
            compression = "none";
            load = <0x80008000>;
            entry = <0x80008000>;
            hash { algo = "sha256"; };
        };
        fdt {
            description = "F1C200S gateway, UART0/1/2, W25N01GV, ESP SPI1";
            data = /incbin/("suniv-f1c200s-gateway.dtb");
            type = "flat_dt";
            arch = "arm";
            compression = "none";
            hash { algo = "sha256"; };
        };
    };
    configurations {
        default = "gateway";
        gateway { kernel = "kernel"; fdt = "fdt"; };
    };
};
'''
(images / 'gateway-kernel.its').write_text(its, encoding='utf-8')
subprocess.run([str(host / 'bin/mkimage'), '-f', 'gateway-kernel.its', 'gateway-kernel.itb'], cwd=images, check=True)
fit = images / 'gateway-kernel.itb'
assert fit.stat().st_size <= 0x500000, 'Boot command reads 5MiB within an 8MiB kernel partition'

def fdt(dtb, node, prop, kind='x'):
    return subprocess.check_output([str(host/'bin/fdtget'), '-t',kind,str(dtb),node,prop],text=True).split()

parts = [('boot',0,0x400000), ('kernel',0x400000,0x800000), ('rootfs',0xc00000,0x7400000)]
for dtb in [images/'suniv-f1c200s-gateway.dtb', base/'output/build/uboot-2025.01/u-boot.dtb']:
    for label,offset,size in parts:
        node=f'/soc/spi@1c05000/flash@0/partitions/partition@{offset:x}'
        assert fdt(dtb,node,'label','s') == [label]
        assert [int(n,16) for n in fdt(dtb,node,'reg')] == [offset,size]
for name in ['kernel','fdt']:
    raw = bytes(int(n,16) for n in fdt(fit,'/images/'+name,'data','bx'))
    hash_value = bytes(int(n,16) for n in fdt(fit,'/images/'+name+'/hash','value','bx'))
    assert hashlib.sha256(raw).digest() == hash_value
    source='zImage' if name=='kernel' else 'suniv-f1c200s-gateway.dtb'
    assert raw == (images/source).read_bytes()

ubi = (images/'rootfs.ubi').read_bytes()
assert len(ubi) % 0x20000 == 0 and len(ubi) < 0x7400000
for off in range(0,len(ubi),0x20000):
    ec=ubi[off:off+64]
    assert ec[:4] == b'UBI#' and ec[4] == 1
    assert struct.unpack_from('>II',ec,16) == (2048,4096)
    assert (zlib.crc32(ec[:60]) ^ 0xffffffff) == struct.unpack_from('>I',ec,60)[0]
    vid=ubi[off+2048:off+2048+64]
    assert vid[:4] == b'UBI!' and vid[4] == 1
    assert (zlib.crc32(vid[:60]) ^ 0xffffffff) == struct.unpack_from('>I',vid,60)[0]
ubifs=(images/'rootfs.ubifs').read_bytes()
assert ubifs[:4] == b'\x31\x18\x10\x06'
# UBIFS superblock: min_io_size, leb_size, leb_cnt, max_leb_cnt at offset 32.
min_io,leb_size,leb_count,max_leb_count=struct.unpack_from('<IIII',ubifs,32)
assert (min_io,leb_size,max_leb_count) == (2048,126976,928)
assert leb_count * leb_size == len(ubifs)

manifest = dict(
    flash='W25N01GVZEIG', bytes=128*1024*1024,
    physical_page_bytes=2048, spare_bytes=64, eraseblock_bytes=131072,
    boot_payload_per_page=1024, boot_requires_good_block0=True,
    boot_oob_included=False, boot_factory_bad_blocks_skipped=True,
    partition_map=[dict(name=n,offset=o,bytes=s) for n,o,s in parts],
    kernel_read_bytes=0x500000, fit_sha256_verified=True,
    spl_egon_checksum_verified=True, uboot_legacy_crc_verified=True,
    boot_pack_roundtrip_verified=True, linux_uboot_partitions_match=True,
    ubi_min_io_bytes=min_io, ubi_vid_header_offset=2048, ubi_data_offset=4096,
    ubifs_leb_bytes=leb_size, ubifs_max_lebs=max_leb_count,
    ubi_headers_crc_verified=True,
    programmer_requires_nand_ecc_and_bad_block_support=True,
    hardware_boot_verified=False,
    files={n:dict(bytes=(images/n).stat().st_size,
                  sha256=hashlib.sha256((images/n).read_bytes()).hexdigest())
           for n in ['boot-rom-padded.bin','gateway-kernel.itb','rootfs.ubi','rootfs.ubifs']})
(images/'nand-image-layout.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print('NAND_IMAGE_LAYOUT_PASS: SPL checksum, U-Boot CRC, packing, FIT hashes, DT partitions, UBI geometry/CRC (no hardware boot)')

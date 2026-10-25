#!/bin/bash
# Host tests + patch validation for the Santoni bring-up.
#
# Called by shim/build.sh (and the xnu workflow) so that the three phase 4
# host tests and the unified-diff structure of the Santoni patches are checked
# on every build. Everything here is host-only: no device, no XNU tree.
#
#   tests/run_tests.sh            # normal run, binaries in out/
#   OUT_DIR=/tmp/t tests/run_tests.sh
#
# Exit status is non-zero if a test fails, a patch has a malformed hunk, or a
# requirement asserted below is missing from the code that the patch ships.
set -euo pipefail

cd "$(dirname "$0")/.."

CC="${HOST_CC:-${CC:-clang}}"
OUT="${OUT_DIR:-out}"
mkdir -p "$OUT"

P142="xnu/patches/0142-santoni-sdhci-emmc-driver.patch"
P143="xnu/patches/0143-santoni-persistent-console-log.patch"
P144="xnu/patches/0144-santoni-4k-unsigned-dyld-cache.patch"

echo "== host unit tests (-Wall -Wextra) =="
for t in test_sdhci_mock test_gpt_parser test_log_ring; do
	"$CC" -O2 -Wall -Wextra -I. "tests/$t.c" -o "$OUT/$t"
	case "$OUT" in
		/*) "$OUT/$t" ;;
		*)  "./$OUT/$t" ;;
	esac
done

echo "== unified diff structure (all xnu/patches) =="
python3 tools/check_patch.py xnu/patches/*.patch > "$OUT/check_patch.log"
tail -3 "$OUT/check_patch.log"

echo "== STOR-03: GPT CRC32 + SYSTEM parse + fenced log writes (0142) =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:0xEDB88320u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_crc32(hdr_raw, hdr.header_size)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_crc32(b, entry_bytes)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:uint64_t sys_first_block, sys_nblocks;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:strcmp(name, "system")' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SANTONI_LOG_SYSTEM_BLOCKS 8193u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:int santoni_sdhci_write_log_blocks(' \
	--added-contains 'bsd/dev/santoni_sdhci.c:lba < se.sys_first_block' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = sdhci_write_pio((uint32_t)lba, count, buf);' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:su.dma'
echo "ok   0142 ships CRC32, system partition and the 8193-sector log fence"

echo "== STOR-04: log flusher on SYSTEM, no Sweet userdata ring (0143) =="
python3 tools/check_patch.py "$P143" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_log_start();' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = santoni_sdhci_write_log_blocks(base + 1 + blk,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = santoni_sdhci_write_log_blocks(base, 1, bounce);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:PE_parse_boot_argn("sweetlog", &on, sizeof(on));' \
	--added-contains 'osfmk/console/serial_console.c:santoni_log_append(c, size);' \
	--added-contains 'osfmk/console/serial_console.c:#if !defined(ARM64_BOARD_CONFIG_SANTONI)' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:base = sys.first_block;' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:se.dma' \
	--added-lacks 'osfmk/console/serial_console.c:sweet_log_append(c, size);'
echo "ok   0143 routes Santoni to SYSTEM and not to sweet_log_append"

echo "== STOR-05: 4 KiB pages with a 16 KiB-signed dyld cache (0144) =="
python3 tools/check_patch.py "$P144" -q \
	--added-contains 'bsd/kern/ubc_subr.c:cd->pageSize != 14' \
	--added-contains 'bsd/kern/ubc_subr.c:cd->pageSize > PAGE_SHIFT' \
	--added-contains 'osfmk/vm/vm_shared_region.c:vm_map_cs_enforcement_set(vm_shared_region_vm_map(shared_region), false);'
echo "ok   0144 allows pageSize 14, skips 16 KiB hashing, disables CS enforcement"

echo "== STOR-06: SDHCI consensus fixes (CMD12-after-BLK_CNT_EN, DATA_INHIBIT, ==="
echo "==          recover, R2 CRC strip, SEC_COUNT fence, idempotent probe,     =="
echo "==          HC-mode takeover from LK's legacy MCI mode)                  =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_poll_state(SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_recover(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_rd(SDHCI_RESPONSE + 4 * (3 - i))' \
	--added-contains 'bsd/dev/santoni_sdhci.c:se.sec_count = sec_count;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:lba + (uint64_t)count > se.sec_count' \
	--added-contains 'bsd/dev/santoni_sdhci.c:static bool attempted;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:int init_error;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_core_check(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_CTRL_8BITBUS' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_enter_hc_mode(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:core_wr(CORE_HC_MODE, (hc_mode & ~CORE_HC_MODE_EN) | CORE_HC_MODE_EN);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:core_wr(CORE_HC_MODE, hc_mode | CORE_HC_MODE_EN | CORE_FF_CLK_SW_RST_DIS);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:HC_MODE_EN reads back 0' \
	--added-contains 'bsd/dev/santoni_sdhci.c:FF_CLK_SW_RST_DIS did not stick' \
	--added-contains 'bsd/dev/santoni_sdhci.c:card clock gate was closed' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_wr(SDHCI_INT_ENABLE, 0xffffffffu);' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:core not in HC mode' \
	--added-contains 'iokit/bsddev/IOKitBSDInit.cpp:santoni_sdhci_probe();' \
	--added-contains 'iokit/bsddev/IOKitBSDInit.cpp:IOLog("SANTONI: eMMC probe failed' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:return sdhci_stop_transmission();'
echo "ok   0142 fixes DATA_INHIBIT, CMD12/recover, R2 order, SEC_COUNT and probe"
python3 tools/check_patch.py "$P143" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:uint32_t on = 0;' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:uint32_t on = 1;'
echo "ok   0143 defaults the console log OFF (sweetlog=1 enables it)"

echo "== STOR-07: addressed commands carry LK's RCA, R2 is CRC-stripped, and  =="
echo "==          the RCA ladder diagnoses itself (VENDOR_SPEC, CMD5, CMD0)   =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SANTONI_CARD_RCA' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_send_status(uint32_t rca, uint32_t *r1)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_cmd(13, rca << 16, 0,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_response128(9, se.rca << 16, csd)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:out[i] = (lo << 8) | (hi >> 24);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_wr(CORE_VENDOR_SPEC, CORE_VENDOR_SPEC_BOARD_VAL);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_sleep_awake(uint32_t rca, uint32_t *r1)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_restore_state(uint32_t *r1)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_restore_controller(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_wr16(SDHCI_HOST_CONTROL2, handoff_hc2);' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:hc_wr(CORE_VENDOR_SPEC, CORE_VENDOR_SPEC_POR_VAL);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CMD0 did not complete either: no SDCLK' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:core_wr(CORE_VENDOR_SPEC' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:sdhci_cmd(13, 0, 0,' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:sdhci_read_response128(10,' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:card is not in transfer state'
echo "ok   0142 addresses CMD13/CMD9 with RCA and removes illegal TRAN-state CMD10"

echo "== STOR-08: HC datapath clock - vendor MCLK select, DLL CK_OUT_EN,    =="
echo "==          and status-enable all ones for the polled loop (0142)      =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define CORE_DLL_CONFIG         0x100' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define CORE_CK_OUT_EN          0x00040000u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define CORE_DLL_OFF            (CORE_DLL_RST | CORE_DLL_PDN)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_wr(CORE_VENDOR_SPEC, CORE_VENDOR_SPEC_BOARD_VAL);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_select_hc_clock(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:vs |= CORE_HC_MCLK_SEL_DFLT;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_clock_output(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:bool dll_live = (dll & CORE_DLL_EN) && !(dll & CORE_DLL_OFF) &&' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CORE_DLL_CONFIG 0x%08x CORE_DLL_STATUS 0x%08x' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_wr(SDHCI_SIGNAL_ENABLE, 0);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:card clock gate did not' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_wr16(SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN);' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:SDHCI_CLOCK_DIV' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:core_wr(CORE_VENDOR_SPEC' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:hc_wr(SDHCI_INT_ENABLE, 0);' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:core_wr(CORE_HC_MODE, hc_mode | want);'
echo "ok   0142 clocks the HC datapath, follows the DLL and enables status"

echo "== STOR-09: card clock comes from GCC sdcc1_apps_clk_src, not the    =="
echo "==          ignored SDHCI divider (mapped RCG page, M/N/D, UPDATE)     =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define GCC_SDCC1_APPS_PHYS      0x01842000u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define GCC_SDCC1_APPS_CMD_RCGR  0x004u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:se.gcc = ml_io_map(GCC_SDCC1_APPS_PHYS, GCC_SDCC1_APPS_SIZE);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_gcc_set_apps(const struct santoni_gcc_rate *r)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:gcc_wr(GCC_SDCC1_APPS_M, r->m);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:gcc_wr(GCC_SDCC1_APPS_N, r->n);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:gcc_wr(GCC_SDCC1_APPS_D, r->d);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:cfg &= ~(GCC_CFG_DIV_MASK | GCC_CFG_SEL_MASK | GCC_CFG_MODE_MASK);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:gcc_wr(GCC_SDCC1_APPS_CMD_RCGR, cmd | GCC_CMD_UPDATE);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:{ "400 kHz", 0x01u, 0xfcu, 0xfbu, 0x2017u },' \
	--added-contains 'bsd/dev/santoni_sdhci.c:{ "25 MHz",  0x01u, 0xfeu, 0xfdu, 0x211fu },' \
	--added-contains 'bsd/dev/santoni_sdhci.c:absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CMD0 -> error %d, %llu ns' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:gcc_wr(GCC_SDCC1_APPS_CBCR' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:SDHCI_DIVIDER_400KHZ'
echo "ok   0142 programs the GCC RCG and keeps the branch/AHB gates untouched"

echo "== STOR-10: 400 kHz uses UHS=0 feedback clock, 1-bit bus, DLL off;   =="
echo "==          CMD13/CMD5 precede CMD0 and CMD1 polls only while busy    =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_CTRL_UHS_MASK' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_snapshot_untouched(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_snapshot_standard(const char *tag)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_set_legacy_host(const char *why)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:new_hc2 = (uint16_t)(hc2 & ~SDHCI_CTRL_UHS_MASK);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SDHCI_CTRL_DMA_MASK |' \
	--added-contains 'bsd/dev/santoni_sdhci.c:(dll | CORE_DLL_RST | CORE_DLL_PDN) &' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_cmd1_poll(uint32_t *ocr)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:if (*ocr & 0x80000000u)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:command timeout is not "busy"' \
	--added-contains 'bsd/dev/santoni_sdhci.c:probe step 2: CMD5 awake RCA 2' \
	--added-contains 'bsd/dev/santoni_sdhci.c:GO_PRE_IDLE retry' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_set_legacy_host("rung 3 controller restore");' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:hc_wr(CORE_VENDOR_SPEC, CORE_VENDOR_SPEC_HS400_DIV2_VAL);'
echo "ok   0142 uses the low-speed feedback path and a bounded CMD1 poll"

echo "== STOR-11: CMD2/CMD3/CMD9/CMD7/CMD8 enumeration and 25 MHz 8-bit HS =="
echo "==          switch; R2 has no index check and CMD6 waits for DAT0        =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_response128(uint8_t index, uint32_t arg, uint32_t out[4])' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC, NULL);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_enumerate_card(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_response128(2, 0, cid)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_cmd(3, SANTONI_CARD_RCA << 16, 0,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_response128(9, se.rca << 16, csd)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_select_card(se.rca)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_pio(8, 0, 1, (void *)(se.dma + DMA_BOUNCE))' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_gcc_set_apps(&santoni_gcc_rates[SANTONI_GCC_RATE_25M])' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_switch_ext_csd(EXT_CSD_BUS_WIDTH,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:EXT_CSD_BUS_WIDTH_8, SDHCI_CTRL_8BITBUS, SDHCI_CTRL_BUS_MASK,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_switch_ext_csd(EXT_CSD_HS_TIMING,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:EXT_CSD_TIMING_HS, SDHCI_CTRL_HISPD, 0, "HS_TIMING=1")' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SDHCI_DATA_0_LVL_MASK' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_wait_card_ready(uint32_t rca, const char *what)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_R1_SWITCH_ERROR' \
	--added-contains 'bsd/dev/santoni_sdhci.c:if (r1 & SDHCI_R1_SWITCH_ERROR) {' \
	--added-contains 'bsd/dev/santoni_sdhci.c:se.enumerated = true;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = sdhci_enumerate_card();' \
	--added-contains 'bsd/dev/santoni_sdhci.c:if (se.enumerated) {' \
	--added-contains 'bsd/dev/santoni_sdhci.c:not downgrading the card bus' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC | SDHCI_CMD_INDEX'
echo "ok   0142 enumerates READY->TRAN, reads EXT_CSD 1-bit, then switches 25 MHz/8-bit/HS"

echo "== STOR-12: CMD17 single-block, CMD18/25 AUTO_CMD12, and 8-bit bus verification (0142) =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_TRNS_AUTO_CMD12   0x0004' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SDHCI_TRNS_AUTO_CMD12' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_verify_highspeed_bus(uint32_t expected_sec_count)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = sdhci_verify_highspeed_bus(se.sec_count);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:return sdhci_read_pio(count == 1 ? 17 : 18, (uint32_t)lba, count, buf);' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:sdhci_card_check(void)'
echo "ok   0142 uses CMD17 for 1 block, AUTO_CMD12 for multi-block, and verifies 8-bit bus"

echo "== STOR-13: cdev I/O uses uiomove, not physio/user-VA memcpy; B_PHYS ==="
echo "==          strategy buffers use copyin/copyout (0142)                  =="
python3 tools/check_patch.py "$P142" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_sdhci_uio(struct uio *uio, bool rd)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:return santoni_sdhci_uio(uio, true);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:kmem_alloc(kernel_map, &bounce, bsize, KMA_DATA,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = uiomove(buf + skip, (int)want, uio);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:!santoni_kernel_address(vaddr);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = copyout(bounce + skip,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = copyin((user_addr_t)(vaddr + done),' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:return physio('
echo "ok   0142 never hands a user VA to memcpy and keeps the cdev lock off uiomove"

echo "== WHYRED: SDM636 machine config inherits the LAVENDER board code,    =="
echo "==        and the built-in libpthread/HFS providers are enabled for it =="
P148="xnu/patches/0148-whyred-board-target.patch"
python3 tools/check_patch.py "$P148" -q \
	--added-contains 'makedefs/MakeInc.def:SUPPORTED_ARM64_MACHINE_CONFIGS = BCM2837 T6000 T8101 T8103 VMAPPLE SWEET SANTONI LAVENDER WHYRED' \
	--added-contains 'makedefs/MakeInc.def:MACHINE_FLAGS_ARM64_WHYRED = -DARM64_BOARD_CONFIG_WHYRED -DARM64_BOARD_CONFIG_LAVENDER -DARM64_BOARD_CONFIG_SWEET -DWHYRED=1' \
	--added-contains 'makedefs/MakeInc.def:ifeq ($(filter SWEET SANTONI LAVENDER WHYRED,$(CURRENT_MACHINE_CONFIG)),)' \
	--added-contains 'config/MASTER.arm64.whyred:machine' \
	--added-contains 'config/MASTER.arm64.whyred:CONFIG_KERNEL_INTEGRITY' \
	--added-contains 'config/MASTER.arm64.whyred:FILESYS_BASE =   [ devfs fdesc fifo mockfs sweet_hfs fs_compression' \
	--added-contains 'bsd/conf/Makefile.template:ifneq ($(filter SWEET SANTONI LAVENDER WHYRED,$(CURRENT_MACHINE_CONFIG)),)' \
	--added-contains 'libkern/conf/Makefile.template:ifneq ($(filter SWEET SANTONI LAVENDER WHYRED,$(CURRENT_MACHINE_CONFIG)),)'
grep -q 'MACHINE_CONFIG:-.*WHYRED' xnu/patches/0019-stage-libpthread.sh
grep -q 'MACHINE_CONFIG:-.*WHYRED' xnu/patches/0020-stage-bearssl.sh
grep -q 'MACHINE_CONFIG:-.*WHYRED' xnu/patches/0026-stage-hfs.sh
python3 - <<'PY'
import sys
sys.path.insert(0, 'tools')
import mkadt
w = mkadt.BOARDS['whyred']
assert (w['panel_w'], w['panel_h']) == (1080, 2160), w
assert w['soc'] == 'sdm636', w
assert mkadt.BOARDS['lavender']['panel_h'] == 2340
PY
echo "ok   0148 adds WHYRED, enables libpthread/HFS, and sets a 1080x2160 ADT"

# mockfs is the only filesystem that can mount the executable ram disk, and
# sweet_hfs the only one for the iOS root. Missing them from FILESYS_BASE made
# vfs_mountroot() return ENODEV (19) on every mount attempt: santoni shipped
# that bug, and 0145/0148 copied the same list into lavender and whyred.
python3 - <<'PY'
for p in ('xnu/patches/0145-lavender-board-target.patch',
          'xnu/patches/0148-whyred-board-target.patch'):
    lines = [l for l in open(p, encoding='utf-8')
             if l.startswith('+#  FILESYS_BASE')]
    assert lines, p
    assert 'devfs fdesc fifo mockfs sweet_hfs ' in lines[0], (p, lines[0])
PY
echo "ok   0145/0148 list mockfs and sweet_hfs, so md0 and the iOS root can mount"

echo "== SDHCI: a cached probe must still say where it stopped            =="
P149="xnu/patches/0149-santoni-sdhci-probe-failure-summary.patch"
python3 tools/check_patch.py "$P149" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:stage = "take-over";' \
	--added-contains 'bsd/dev/santoni_sdhci.c:stage = "EXT_CSD 8-bit verify";' \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_sdhci_failure_text(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:se.cmd1_error = err_cmd1;' \
	--added-contains 'iokit/bsddev/IOKitBSDInit.cpp:santoni_sdhci_failure_text());'
echo "ok   0149 reports the failing probe step, CMD1 result and SDHCI state"

echo "== SDHCI v5: a stuck RESET_ALL must not discard ABL's working card  =="
P150="xnu/patches/0150-sdhci-v5-reset-timeout-nonfatal.patch"
python3 tools/check_patch.py "$P150" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define RESET_TIMEOUT_NS        (1000ull * NSEC_PER_MSEC)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:bool reset_done;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:continuing without a completed reset' \
	--added-contains 'bsd/dev/santoni_sdhci.c:(void)sdhci_pwrctl_ack("BUS_OFF", reset_done);'
echo "ok   0150 keeps the SDCC v5 takeover alive after a stuck RESET_ALL"

echo "== SDHCI: the first data transfer must run at the width ABL used  =="
echo "==        and a failed one must say which side failed               =="
P151="xnu/patches/0151-sdhci-keep-abl-bus-width-and-report-data-errors.patch"
python3 tools/check_patch.py "$P151" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:(uint8_t)(handoff_ctrl & SDHCI_CTRL_BUS_MASK)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_data_failure(const char *what, uint32_t st)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SDHCI_INT_DATA_TIMEOUT' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_recover(bool open_ended)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:if (!open_ended) {'
echo "ok   0151 keeps ABL's bus width and names data-phase failures"

echo "== SDHCI: EXT_CSD is read in 1-bit after CMD0, and CMD8 reports its R1 =="
P152="xnu/patches/0152-sdhci-ext-csd-1bit-and-r1-diagnostics.patch"
python3 tools/check_patch.py "$P152" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define EXT_CSD_BUS_WIDTH_1     0u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_R1_ILLEGAL_COMMAND 0x00400000u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SDHCI_CMD_DATA, &r1);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_send_status(se.rca, &r1);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:EXT_CSD_BUS_WIDTH_1, 0, SDHCI_CTRL_BUS_MASK,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CMD8 failed (%d); ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:enumeration complete: 25 MHz, %u-bit, ' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:25 MHz, 8-bit, '
echo "ok   0152 reads EXT_CSD in 1-bit, prints R1, and names the bus it settled on"

echo "== SDHCI: EXT_CSD tries 1/8-bit and PIO/SDMA, and dumps the transfer regs =="
P153="xnu/patches/0153-sdhci-ext-csd-width-ladder-and-sdma.patch"
python3 tools/check_patch.py "$P153" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_SDMA_ADDR' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_TRNS_DMA' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_sdma(uint8_t index, uint32_t arg, uint32_t count, uint64_t pa)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_wr(SDHCI_SDMA_ADDR, (uint32_t)pa);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_pio_once(uint8_t index, uint32_t arg, uint32_t count, void *buf)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:retrying the same block ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_ext_csd(uint32_t *width)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:{ EXT_CSD_BUS_WIDTH_8, SDHCI_CTRL_8BITBUS,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:((ctrl & ~ctrl_set) & ctrl_clear) != 0' \
	--added-contains 'bsd/dev/santoni_sdhci.c:TRANSFER_MODE 0x%04x HOST_CONTROL 0x%02x CLOCK_CONTROL 0x%04x ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:rolling back bus width to 1-bit' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:forcing 1-bit failed'
echo "ok   0153 negotiates 1/8-bit, falls back to SDMA, and dumps the transfer registers"

echo "== SDHCI: the first data phase is probed across width/engine/preset    =="
echo "==        and a working combination is kept for the rest of the probe  =="
P154="xnu/patches/0154-sdhci-first-data-phase-matrix.patch"
python3 tools/check_patch.py "$P154" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_PROBE_TIMEOUT_NS' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_CTRL_PRESET_VAL_EN 0x0800u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_data_matrix(uint32_t *width)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:data probe #%u %u-bit %s bc=%u ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:data probe: EXT_CSD moved at ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:if (sdhci_data_matrix(width) == 0) {' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_cmd(16, SDHCI_BLOCK, 0,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:TMO 0x%02x ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:data probe: no configuration moved a byte'
echo "ok   0154 probes width/engine/preset on the first data phase and keeps a winner"

echo "== SDHCI: the data waits must also sample PRESENT_STATE              =="
echo "==        because this core does not always latch BRR/TC in INT_STATUS =="
P155="xnu/patches/0155-sdhci-poll-present-state-for-data-phase.patch"
python3 tools/check_patch.py "$P155" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_PSTATE_BUF_RD_READY 0x00000800u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_PSTATE_RD_ACTIVE' \
	--added-contains 'bsd/dev/santoni_sdhci.c:static bool santoni_data_moved;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:buffer ready ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:PRESENT_STATE from now on' \
	--added-contains 'bsd/dev/santoni_sdhci.c:never set TC; treating the ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:resetting the data path' \
	--added-contains 'bsd/dev/santoni_sdhci.c:for (uint32_t w = 0; w < count * (SDHCI_BLOCK / 4); w++) {' \
	--added-contains 'bsd/dev/santoni_sdhci.c:hc_rd(SDHCI_SDMA_ADDR) != santoni_sdma_start' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_ext_csd_sane(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:is not real (REV %u '
echo "ok   0155 falls back to PRESENT_STATE for data and transfer complete"

echo "== SDHCI: PRESENT_STATE transfer-active bits are 9/8, not 13/14,    =="
echo "==        and a stuck DAT_INHIBIT gets a bounded drain then a reset  =="
P156="xnu/patches/0156-sdhci-present-state-bits-and-drain.patch"
python3 tools/check_patch.py "$P156" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:PRESENT_STATE[11:8]' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_PSTATE_WR_ACTIVE    0x00000100u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_PSTATE_RD_ACTIVE    0x00000200u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_wait_transfer_complete(const char *what, bool all_bytes,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:data machine still busy ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0156' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CMD5 not needed' \
	--added-contains 'bsd/dev/santoni_sdhci.c:if (!card_in_transfer) {' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_wait_transfer_complete(tag, true, index == 25);' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:0x00002000u' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0155'
echo "ok   0156 fixes the PRESENT_STATE bits, drains the data path, skips CMD5 in TRAN"

echo "== SDHCI: SDMA goes first and a block only counts when it is EXT_CSD =="
echo "==        (an all-zero FIFO read is a false positive on this core)     =="
P157="xnu/patches/0157-sdhci-sdma-first-ext-csd-and-payload-check.patch"
python3 tools/check_patch.py "$P157" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0157' \
	--added-contains 'bsd/dev/santoni_sdhci.c:static bool sdhci_ext_csd_sane(void);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:PIO returned %u zero words; no data ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:FIFO returned %u ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_ext_csd_check(uint32_t lanes, const char *how)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:error = sdhci_read_sdma(8, 0, 1, se.dma_pa + DMA_BOUNCE);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:read works at %u-bit (SDMA)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:{ 1, 0, 1, 1, 1 },' \
	--added-contains 'bsd/dev/santoni_sdhci.c:words %08x %08x %08x %08x' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0156'
echo "ok   0157 tries SDMA first, rejects all-zero blocks and validates the payload"

echo "== PAYLOAD: reject a wrong-machine kernel or DTB before the ABL does =="
python3 tests/test_payload.py
echo "ok   check_payload matches whyred/lavender DTBs and the extractor walks a table"
python3 tests/test_prune_dtb.py >/dev/null
echo "ok   prune_dtb keeps the board ids and drops the nodes nobody reads"
python3 tests/test_zip_partial.py >/dev/null
echo "ok   zip_partial proves an entry before calling it the kernel"

# Optional deep check: if a patched XNU source tree is provided, the three
# patches must also apply to it as-is (git apply --check).
if [ -n "${XNU_SRC:-}" ]; then
	echo "== git apply --check against XNU_SRC=$XNU_SRC =="
	(cd "$XNU_SRC" && git apply --check \
	    "$OLDPWD/$P142" "$OLDPWD/$P143" "$OLDPWD/$P144")
	echo "ok   all three patches apply to $XNU_SRC"
fi

echo "run_tests: all hosts tests, patch checks and shipped-code assertions passed"

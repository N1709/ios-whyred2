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

# The canvas in the ADT is what iOS lays out for. mkadt.py defaults to the
# iPhone X canvas unless --native is passed, and backboardd then creates
# 1125x2436 surfaces while the panel is 1080x2160: SpringBoard runs, the screen
# stays black. lavender's build already passes --native; whyred must too.
grep -q 'mkadt.py out/\$BOARD_LC.adt --board "\$BOARD_LC" --native' shim/build.sh
echo "ok   the ADT canvas is the panel itself (mkadt.py --native)"

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

echo "== SDHCI: the card is still in ABL's HS400 timing, and only the host   =="
echo "==        was put back to legacy; reset HS_TIMING before the data phase =="
P158="xnu/patches/0158-sdhci-legacy-timing-before-ext-csd.patch"
python3 tools/check_patch.py "$P158" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0158' \
	--added-contains 'bsd/dev/santoni_sdhci.c:static void sdhci_ext_csd_clear(void);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:HS_TIMING=0 (legacy, before EXT_CSD)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define SDHCI_CTRL_TUNED_CLK    0x0080u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SDHCI_CTRL_EXEC_TUNING | SDHCI_CTRL_TUNED_CLK' \
	--added-contains 'bsd/dev/santoni_sdhci.c:memset((void *)(se.dma + DMA_BOUNCE), 0, SDHCI_BLOCK);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:status error ignored' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0157'
echo "ok   0158 returns the card to legacy timing and keeps a valid EXT_CSD on a status error"

echo "== SDHCI: prove the data path on LBA 0 before blaming CMD8/EXT_CSD   =="
P159="xnu/patches/0159-sdhci-lba0-data-path-self-test.patch"
python3 tools/check_patch.py "$P159" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_probe_lba0(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_sdma(17, 0, 1,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_read_pio_once(17, 0, 1,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:data self-test LBA0' \
	--added-contains 'bsd/dev/santoni_sdhci.c:MBR %s' \
	--added-contains 'bsd/dev/santoni_sdhci.c:HS_TIMING=0 took' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0159' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0158'
echo "ok   0159 reads MBR LBA 0 at 1 and 8 bit through SDMA and PIO"

echo "== SDHCI: CMD6 must not use the R1b busy handshake on this core, and =="
echo "==        LBA 0 must be read in ABL's handoff state before any CMD6 =="
P160="xnu/patches/0160-sdhci-lba0-in-handoff-state-and-cmd6-without-busy.patch"
python3 tools/check_patch.py "$P160" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_lba0_attempt(const char *width, const char *engine, bool use_sdma)' \
	--added-contains "bsd/dev/santoni_sdhci.c:ABL's handoff state" \
	--added-contains 'bsd/dev/santoni_sdhci.c:at %u-bit (handoff state%s)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:cycling the card clock to resample the lines' \
	--added-contains 'bsd/dev/santoni_sdhci.c:stale inhibit cleared by the clock cycle' \
	--added-contains 'bsd/dev/santoni_sdhci.c:cannot be reset in software' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CMD13 status ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0160' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0159'
echo "ok   0160 reads LBA 0 before CMD6 and drops the R1b busy wait"

echo "== SDHCI: CMD0 returns the card to 1-bit, so the host must follow =="
echo "==        before the first data command (the 0160 wedge)          =="
P161="xnu/patches/0161-sdhci-cmd0-returns-card-to-1-bit.patch"
python3 tools/check_patch.py "$P161" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:CMD0 reset the card to ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:host width 0x%02x -> 0x%02x' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_probe_lba0(const char *phase, uint32_t *lanes)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:1-bit after CMD0 (phase %s)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CMD0 left the card at ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CORE_VENDOR_SPEC_FUNC2      0x210' \
	--added-contains 'bsd/dev/santoni_sdhci.c:HC_SW_RST_REQ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:vendor reset cleared the inhibit' \
	--added-contains 'bsd/dev/santoni_sdhci.c:last=%llu us lba0=%s' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0161' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0160'
echo "ok   0161 follows CMD0 to 1-bit and keeps the vendor reset as recovery"

echo "== SDHCI: the core's own data timeout is 11 minutes at 400 kHz, so  =="
echo "==        shorten it and sweep the host width with no CMD6           =="
P162="xnu/patches/0162-sdhci-short-timeout-and-host-width-sweep.patch"
python3 tools/check_patch.py "$P162" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:TIMEOUT_CONTROL ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:0x%02x -> 0x03 (~164 ms at 400 kHz)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:{ SDHCI_CTRL_4BITBUS, "4-bit" },' \
	--added-contains 'bsd/dev/santoni_sdhci.c:host width 0x%02x -> ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:ok-%s' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0162' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0161'
echo "ok   0162 shortens the card timeout and finds the real width without CMD6"

echo "== SDHCI: a failed sweep must print the card's R1 and the transfer =="
echo "==        registers, then stop instead of 20 lines of EBUSY       =="
P163="xnu/patches/0163-sdhci-data-verdict-with-card-r1.patch"
python3 tools/check_patch.py "$P163" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:static uint32_t santoni_data_r1;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:SDHCI_CMD_DATA, &santoni_data_r1);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:MBR %s, R1 0x%08x, words' \
	--added-contains 'bsd/dev/santoni_sdhci.c:CMD16 %s -> R1 ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:"no-width e=%d r1=0x%08x"' \
	--added-contains 'bsd/dev/santoni_sdhci.c:data verdict: no width or engine moved ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:data verdict: BS 0x%04x BC 0x%04x TM ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0163' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0162'
echo "ok   0163 puts the card R1 in the data verdict and drops the EBUSY wall"
echo "== SDHCI v5: ABL hands over in CMDQ mode, so the alt FIFO must be =="
echo "==        off, and block/command are 32-bit register pairs       =="
P164="xnu/patches/0164-sdhci-v5-standard-fifo-and-32bit-writes.patch"
python3 tools/check_patch.py "$P164" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define CORE_FIFO_ALT_EN        0x00000400u' \
	--added-contains 'bsd/dev/santoni_sdhci.c:sdhci_v5_standard_fifo(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:alt FIFO VENDOR_SPEC3 0x%08x -> ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:(count << 16) | SDHCI_MAKE_BLKSZ(0, SDHCI_BLOCK)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:((uint32_t)SDHCI_MAKE_CMD(index, flags) << 16) | tr_mode' \
	--added-contains 'bsd/dev/santoni_sdhci.c:tr_mode = SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:VENDOR_SPEC3 ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:"PIO", false) ||' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0164' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0163'
echo "ok   0164 selects the standard FIFO and uses the v5 32-bit register pairs"
echo "== SDHCI: the first block ever read came back on one engine, so   =="
echo "==        EXT_CSD must use that engine before the other one        =="
P165="xnu/patches/0165-sdhci-ext-csd-uses-the-engine-that-moved-lba0.patch"
python3 tools/check_patch.py "$P165" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:static bool santoni_lba0_pio = true;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_lba0_pio = !use_sdma;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:LBA0 moved a block with ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:reading EXT_CSD the same way' \
	--added-contains 'bsd/dev/santoni_sdhci.c:EXT_CSD verdict: %s at %u-bit failed ' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:pass < 2; pass++' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0165' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0164'
echo "ok   0165 reads EXT_CSD once with the engine that moved LBA0, then stops"

echo "== SDHCI: the 0165 boot reached 8-bit HS but reported 1-bit; the     =="
echo "==        width must be recorded, and LBA0 alone proves nothing      =="
P166="xnu/patches/0166-sdhci-record-8bit-width-and-probe-the-volume-header.patch"
python3 tools/check_patch.py "$P166" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:bus_width = 8;' \
	--added-contains 'bsd/dev/santoni_sdhci.c:high-speed (HOST_CONTROL 0x%02x)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0166' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0165'
# The userspace init is a repo file, not an XNU source file: keep it out of
# the patch series (dxb applies patches inside the XNU tree, where tools/ does
# not exist, and a patch that touches it is skipped - which failed CI 40292548638).
grep -q 'SYS_lseek\s*199' tools/santoni-init.S
grep -q 'volume header at 1024' tools/santoni-init.S
echo "ok   0166 records the real 8-bit width and probes the HFS+ volume header"

echo "== LOGS: the on-screen name is the phone, not the donor board       =="
P167="xnu/patches/0167-sdhci-log-prefix-is-whyred.patch"
python3 tools/check_patch.py "$P167" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:"WHYRED SDHCI: ' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0167' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:"SANTONI SDHCI: '
grep -q '"WHYRED userspace: ' tools/santoni-init.S
grep -q '"WHYRED init: ' tools/santoni-init.S
echo "ok   0167 prints WHYRED instead of SANTONI (identifiers unchanged)"

echo "== ROOT: the HFSX volume staged in userdata is the root device      =="
P168="xnu/patches/0168-sdhci-name-the-root-device-in-the-banner.patch"
python3 tools/check_patch.py "$P168" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0168' \
	--added-contains 'bsd/dev/santoni_sdhci.c:root \"%s\", writes %s' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0167'
grep -q 'ROOTDEV="${ROOTDEV:-md0}"' shim/build.sh
grep -q 'rd=$ROOTDEV' shim/build.sh
grep -q 'ROOTDEV: emmcroot0' .github/workflows/xnu.yml
grep -q 'COMMON="emmcrw=1' .github/workflows/xnu.yml
grep -q 'EXTRA_CMDLINE="$COMMON $2"' .github/workflows/xnu.yml
# HFSX (case-sensitive HFS+) signs its volume header with HX, not BD.
grep -q '0x58' tools/santoni-init.S
echo "ok   0168 boots the HFSX root from userdata (rd=emmcroot0 emmcrw=1)"
echo "== CS: 0169 names the build and turns on the code-signing measurement ==="
P169="xnu/patches/0169-sdhci-name-the-code-signing-build.patch"
python3 tools/check_patch.py "$P169" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0169' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0168'
grep -q 'DIAG="sweetlog=1 cs_debug=2"' .github/workflows/xnu.yml
grep -q 'cs_enforcement_disable=1 unentitled_ios_sim_launch=1' .github/workflows/xnu.yml
echo "ok   0169 enables cs_debug and cs_enforcement_disable for the root boot"

echo "== EXEC: 0170 names the platform-exec build and lets iOS launchd exec =="
P170="xnu/patches/0170-sdhci-name-the-platform-exec-build.patch"
python3 tools/check_patch.py "$P170" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0170' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0169'
# launchd died with OS_REASON_EXEC / EXEC_EXIT_REASON_WRONG_PLATFORM
# (namespace 9, code 0xe). process_signature() only lets a PLATFORM_IOS
# binary through when this DEVELOPMENT tunable is set.
grep -q 'unentitled_ios_sim_launch=1' .github/workflows/xnu.yml
grep -q 'unentitled_ios_sim_launch' xnu/patches/0042-sweet-short-exec-log.patch
echo "ok   0170 passes unentitled_ios_sim_launch=1 for the iOS launchd exec"

echo "== MOUNT: 0171 gives the lavender/whyred kernel bindfs =="
P171="xnu/patches/0171-board-configs-bindfs.patch"
python3 tools/check_patch.py "$P171" -q \
	--added-contains 'config/MASTER.arm64.whyred:nullfs bindfs' \
	--added-contains 'config/MASTER.arm64.lavender:nullfs bindfs' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0171' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0170'
# /sbin/mount carries com.apple.private.bindfs-allow and bind-mounts the
# preboot firmware paths; without bindfs in FILESYS_BASE the vfs switch
# answers ENODEV(19) -> mount exits 74 -> launchd's PANIC_ON_EXIT panics.
python3 tools/check_patch.py xnu/patches/0037-sweet-config-bindfs.patch -q \
	--added-contains 'config/MASTER.arm64.sweet:nullfs bindfs'
echo "ok   0171 enables bindfs so mount-phase-1 can bind the preboot paths"

echo "== CACHE: 0172 skips the AMFI service the kernel does not provide =="
P172="xnu/patches/0172-sdhci-name-the-unsecure-cache-build.patch"
python3 tools/check_patch.py "$P172" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0172' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0171'
# launchd_cache_loader aborts with "Unable to find AMFI" (namespace 7,
# subcode 0x1) after mount-phase-1. The iOS 16 binary accepts this boot-arg
# and skips both the signature push and the AMFI connection.
grep -q 'launchd_unsecure_cache=1' .github/workflows/xnu.yml
echo "ok   0172 passes launchd_unsecure_cache=1 for the cache loader"

echo "== CACHE: 0173 prints the bytes the kernel validates =="
P173="xnu/patches/0173-sdhci-diagnose-cs-and-slide-data.patch"
python3 tools/check_patch.py "$P173" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0173' \
	--added-contains 'bsd/kern/ubc_subr.c:CODE SIGNING: CSPAGE' \
	--added-contains 'bsd/kern/ubc_subr.c:CODE SIGNING: CSBLOB' \
	--added-contains 'bsd/kern/ubc_subr.c:CODE SIGNING: CD len' \
	--added-contains 'osfmk/vm/vm_shared_region_pager.c:SHARED CACHE: slide fail' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0172'
# The prints are only useful when the debug level they are gated on is set,
# and they must stay inside the whyred config so other boards are unaffected.
grep -q 'cs_debug=2' .github/workflows/xnu.yml
grep -q 'ARM64_BOARD_CONFIG_WHYRED' "$P173"
echo "ok   0173 dumps CD/CSBLOB/CSPAGE/slide bytes under cs_debug=2"

echo "== ACL: 0175 drops config_ext_resolver like the SWEET config =="
P175="xnu/patches/0175-board-configs-no-ext-resolver.patch"
python3 tools/check_patch.py "$P175" -q \
	--added-contains 'config/MASTER.arm64.whyred:config_triggers config_searchfs' \
	--added-contains 'config/MASTER.arm64.lavender:config_triggers config_searchfs' \
	--added-lacks 'config/MASTER.arm64.whyred:config_ext_resolver' \
	--added-lacks 'config/MASTER.arm64.lavender:config_ext_resolver' \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0175' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0174'
# Container Library dirs carry ACL-only entries with a GUID ACE. With
# config_ext_resolver the kernel asks memberd/opendirectoryd (not running on
# iOS), the ACE check returns EWOULDBLOCK and installcoordinationd sees EACCES
# ("Failed to create data directory", NSPOSIXErrorDomain 13). SWEET removed
# the resolver in 0057 and its GUI milestone followed.
grep -q 'config_ext_resolver' xnu/patches/0057-sweet-no-ext-resolver.patch
grep -q 'sweetlog=1' .github/workflows/xnu.yml
echo "ok   0175 uses the compatibility ACL mapping and keeps the full boot log"

echo "== DRIVERS: the mirrors under drivers/ match the patch series =="
# mkdrv: the patch is canonical; a mirror can only be refreshed by the tool.
python3 tools/extract_drivers.py --check
echo "ok   driver mirrors are generated from the patches, not edited by hand"
# One folder per subsystem, one subfolder per driver, each with a README that
# states the hardware contract. Display and storage use the kernel's built-in
# drivers, so they must not carry a second copy here.
for d in drivers/audio/dsp drivers/gpu/adreno drivers/touch/dsx \
	drivers/touch/input drivers/touch/hid drivers/power/pmic \
	drivers/power/pmgr drivers/power/cpu drivers/key-store/keystore; do
	if [ ! -f "$d/README.md" ]; then
		echo "$d has no README" >&2
		exit 1
	fi
done
if [ -e drivers/unixnu ] || [ -e drivers/WhyredDisplay ] \
	|| [ -e drivers/WhyredStorage ] || [ -e drivers/WhyredBlock ] \
	|| find drivers -name '*-SHA256SUMS' | grep -q .; then
	echo "drivers/: display/storage stay built-in; no wrapper directory or checksums" >&2
	exit 1
fi
if find drivers -iname '*whyred*' | grep -q .; then
	echo "drivers/: a driver still carries the board name" >&2
	exit 1
fi
echo "ok   drivers/ is one folder per subsystem, one per driver, no board names"

echo "== FB TEST: 0176 paints the boot framebuffer without the console =="
P176="xnu/patches/0176-whyred-framebuffer-test-pattern.patch"
python3 tools/check_patch.py "$P176" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0176' \
	--added-contains 'bsd/dev/santoni_sdhci.c:PE_parse_boot_argn("sweetfbtest"' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#define WHYRED_FB_PHYS          0x9d400000ULL' \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_fb_test();' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0175'
# The whole point is that this does not depend on cons_ops_index: no vcputc,
# no console switch, just writes to the physical framebuffer ABL set up.
grep -q 'sweetfbtest=1' .github/workflows/xnu.yml
echo "ok   0176 paints four bars into the ABL framebuffer under sweetfbtest=1"

echo "== FB TEST: 0177 maps the bars from kernel_map, not io_submap =="
P177="xnu/patches/0177-whyred-fb-test-kernel-map.patch"
python3 tools/check_patch.py "$P177" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0177' \
	--added-contains 'bsd/dev/santoni_sdhci.c:ml_io_map_unmappable(WHYRED_FB_PHYS,' \
	--added-contains 'bsd/dev/santoni_sdhci.c:fb_size, VM_WIMG_IO);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:PE_current_console(&console);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:#include <vm/memory_types.h>' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0176' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:extern vm_offset_t ml_io_map_wcomb'
# The 0176 panic: io_submap is 8 MiB and a 1080x2160x32 mapping needs
# 9,331,200 B, so kmem_alloc() panicked with KERN_NO_SPACE (3) before the
# first pixel. Keep the numbers that prove the mismatch in the source.
grep -q 'define WHYRED_FB_STRIDE        4320u' drivers/storage/santoni_sdhci.c
test $((4320 * 2160)) -gt $((8 * 1024 * 1024))
# The WHYRED FB lines are only in the SYSTEM ring if the flusher starts first.
python3 - <<'PY'
s = open('drivers/storage/santoni_sdhci.c').read()
if s.index('\tsantoni_log_start();') > s.index('\tsantoni_fb_test();'):
    raise SystemExit('santoni_fb_test() runs before the console log')
PY
echo "ok   0177 maps the bars from kernel_map and logs them"

echo "== LOG 0178: the header is written before the first ring block =="
P178="xnu/patches/0178-whyred-log-header-before-blocks.patch"
python3 tools/check_patch.py "$P178" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:driver 0178' \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_log_write_header(uint64_t base, uint64_t total, uint64_t seq)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:herr = santoni_log_write_header(base, 0, 0);' \
	--added-contains 'bsd/dev/santoni_sdhci.c:header update failed' \
	--added-contains 'bsd/dev/santoni_sdhci.c:first flush ok' \
	--added-contains 'bsd/dev/santoni_sdhci.c:ring write failed' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0177'
# 0177: the flusher wrote 26 ring blocks, sector 0 stayed blank, and the
# header write that followed the first flush is what ended the flusher. Keep
# the header first and keep a header failure non-fatal; a ring block error is
# still fatal because that is the payload.
python3 - <<'PY'
s = open('drivers/storage/santoni_sdhci.c').read()
t = s[s.index('santoni_log_thread('):]
if 'santoni_log_write_header(base, 0, 0)' not in t:
    raise SystemExit('the header is not written before the first ring block')
hdr = t.index('header update failed')
first = t.index('first flush ok')
if hdr > first:
    raise SystemExit('the header failure is reported after the flush result')
if t.index('ring write failed') > hdr:
    raise SystemExit('the ring write failure is no longer the fatal one')
PY
echo "ok   0178 labels the log area first and survives a header failure"

echo "== SYNC: 0180 answers DKIOCSYNCHRONIZE instead of ENOTTY =="
P180="xnu/patches/0180-sdhci-synchronize-cache.patch"
python3 tools/check_patch.py "$P180" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:case DKIOCSYNCHRONIZE:' \
	--added-contains 'bsd/dev/santoni_sdhci.c:case DKIOCSYNCHRONIZECACHE:' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:driver 0179'
# HFS calls VNOP_IOCTL(jdev, DKIOCSYNCHRONIZE) before it advances the
# journal head. The driver fell through to "default: return ENOTTY", so every
# checkpoint printed "jnl: b(2, 0): flushing fs disk buffer returned 0x19"
# (0x19 is ENOTTY). The card cache is never enabled, so success is honest.
grep -q 'DKIOCSYNCHRONIZE' xnu/patches/0030-sweet-ufs-writes.patch
echo "ok   0180 turns the journal's cache barrier into a success"

echo "== LOG 0181: a ring write error retries instead of truncating =="
P181="xnu/patches/0181-sdhci-log-flusher-retry.patch"
python3 tools/check_patch.py "$P181" -q \
	--added-contains 'bsd/dev/santoni_sdhci.c:ring write failed' \
	--added-contains 'bsd/dev/santoni_sdhci.c:retrying' \
	--added-lacks 'bsd/dev/santoni_sdhci.c:stopping '
echo "ok   0181 keeps flushing when the card is busy"

echo "== MARK: 0182 keeps the Apple mark through the kernel boot =="
P182="xnu/patches/0182-sweet-boot-mark-in-kernel.patch"
python3 tools/check_patch.py "$P182" -q \
	--added-contains 'iokit/Kernel/arm/SweetAppleLogo.h:SWEET_APPLE_LOGO_W 208' \
	--added-contains 'iokit/Kernel/arm/SweetFramebuffer.cpp:boot mark drawn, console muted' \
	--added-contains 'iokit/Kernel/arm/SweetFramebuffer.cpp:sweet_console_muted = 1'
# The kernel must draw the same 208x256 mark the shim draws: a second
# hand-edited copy of the artwork is not evidence of the ported one.
python3 - <<'PY'
import re
patch = open('xnu/patches/0182-sweet-boot-mark-in-kernel.patch', encoding='utf-8').read()
sec = patch.split('diff --git a/iokit/Kernel/arm/SweetAppleLogo.h')[1].split('diff --git')[0]
kern = [int(x, 16) for line in sec.splitlines() if line.startswith('+')
        and not line.startswith('+++') for x in re.findall(r'0x([0-9a-fA-F]{2})', line)]
shim = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})',
        open('shim/apple_logo.h', encoding='utf-8').read())]
assert kern == shim, (len(kern), len(shim))
PY
echo "ok   0182 paints the shim's own mark after the fbcon clear"
# dxb only warns when `git apply --check` rejects a patch, so the workflow
# fails the build itself. The guard has to name every series it now depends on,
# or a skipped 018x/019x ships a kernel without the logo, the ring fix or the
# DSX page select.
grep -q '01\[89\]\[0-9\]' .github/workflows/xnu.yml
echo "ok   0182 a skipped 018x/019x patch fails the build instead of shipping"

echo "== SWIPE: 0183 proves the digitizer path without touch hardware =="
P183="xnu/patches/0183-sweet-hid-synthetic-swipe.patch"
python3 tools/check_patch.py "$P183" -q \
	--added-contains 'iokit/Kernel/arm/hid/SweetHID.cpp:sweetouchtest' \
	--added-contains 'iokit/Kernel/arm/hid/SweetHID.cpp:synthetic swipe' \
	--added-contains 'iokit/Kernel/arm/hid/SweetHID.cpp:touch(0, true, 540, 2390)'
# The touch has to begin inside the edge band touch() marks FromEdgeTip
# (y > 2399 - 48), or UIKit will not treat the swipe as the home gesture.
python3 - <<'PY'
import re
src = open('xnu/patches/0183-sweet-hid-synthetic-swipe.patch', encoding='utf-8').read()
assert '2399-48' in src or 'SWEET_TOUCH_EDGE_PX' in src
assert '2390' in src and '540' in src
PY
echo "ok   0183 starts the swipe in the bottom edge band"

echo "== TOUCH: 0184 bit-bangs the BLSP1 I2C bus and scans the addresses =="
P184="xnu/patches/0184-whyred-touch-bitbang-probe.patch"
python3 tools/check_patch.py "$P184" -q \
	--added-contains 'bsd/dev/santoni_touch.c:PE_parse_boot_argn("sweetouchi2c"' \
	--added-contains 'bsd/dev/santoni_touch.c:WHYRED_TLMM_BASE' \
	--added-contains 'bsd/dev/santoni_touch.c:WHYRED_SDA' \
	--added-contains 'bsd/dev/santoni_touch.c:WHYRED_SCL' \
	--added-contains 'bsd/dev/santoni_touch.c:i2c_write((uint8_t)(addr << 1))' \
	--added-contains 'bsd/dev/santoni_touch.c:santoni_touch_probe(void)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_touch_probe();' \
	--added-contains 'bsd/conf/files:bsd/dev/santoni_touch.c' \
	--added-lacks 'bsd/dev/santoni_touch.c:sweetouchtest'
# The default probe must not touch the reset pad: the log is supposed to
# describe the hand-off, and a reset pulse is a separate boot-arg.
python3 - <<'PY'
src = open('xnu/patches/0184-whyred-touch-bitbang-probe.patch', encoding='utf-8').read()
assert 'sweetouchreset' in src
assert 'sweetouchwait' in src
assert '"sweetouchi2c", &on' in src
# whyred's vendor DTS puts TLMM at 0x03000000; 0x03400000 would point at
# the CENTER tile and write an unrelated pad.
assert '0x03000000' in src and '0x03400000' not in src
# Ring 0198 (10 us, no reset pulse) read real bytes; ring 0199 (100 us, no
# reset) read all zero, so the 5 kHz theory is wrong and the 10 us half bit
# stays until the chip-state confound is removed.
assert 'TOUCH_I2C_DELAY_US\t10' in src
assert 'Ring 0198 read' in src and 'ring 0199 read' in src
PY
echo "ok   0184 scans on sweetouchi2c and pulses reset only on request"

echo "== TOUCH: 0185 paints the answer where the console no longer reaches =="
P185="xnu/patches/0185-whyred-touch-answer-panel.patch"
python3 tools/check_patch.py "$P185" -q \
	--added-contains 'bsd/dev/santoni_touch.c:sweetouchhalt' \
	--added-contains 'bsd/dev/santoni_touch.c:panel_report(acks, first_ack)' \
	--added-contains 'bsd/dev/santoni_touch.c:0x9d400000' \
	--added-lacks 'bsd/dev/santoni_touch.c:sweetouchtest'
# The paint must target the same scanout address the visible four-bar test
# writes; a different address would be off-screen and prove nothing.
python3 - <<'PY'
src = open('xnu/patches/0185-whyred-touch-answer-panel.patch', encoding='utf-8').read()
assert 'WHYRED_FB_PHYS\t\t0x9d400000ULL' in src
assert 'WHYRED_FB_STRIDE\t4320u' in src
# The halt must come after the paint: a halt before it stops the boot with
# the old screen still showing and answers nothing.
assert src.index('panel_report(acks, first_ack)') < src.index('for (;;)')
PY
echo "ok   0185 paints the ack code into the scanout buffer, then halts"

echo "== TOUCH: 0186 speaks RMI4 to the address that answered (0x20) =="
P186="xnu/patches/0186-whyred-touch-rmi4-identify.patch"
python3 tools/check_patch.py "$P186" -q \
	--added-contains 'bsd/dev/santoni_touch.c:#define RMI_ADDR' \
	--added-contains 'bsd/dev/santoni_touch.c:rmi_read(uint8_t addr' \
	--added-contains 'bsd/dev/santoni_touch.c:RMI_FUNCTION_TABLE' \
	--added-contains 'bsd/dev/santoni_touch.c:rmi_identify();' \
	--added-lacks 'bsd/dev/santoni_touch.c:sweetouchtest'
python3 - <<'PY'
src = open('xnu/patches/0186-whyred-touch-rmi4-identify.patch', encoding='utf-8').read()
# The ring answered at 0x20, so the dump must target that address and must
# not fall through to the Novatek page protocol for it.
assert '#define RMI_ADDR\t\t0x20' in src
assert 'if (addr == RMI_ADDR)' in src
# Identification must stay read-only: no control-register write in 0186.
assert 'rmi_write' not in src
PY
echo "ok   0186 reads version, function table and F01/F12 at 0x20"

echo "== TOUCH: 0187 speaks Synaptics DSX and polls F12 =="
P187="xnu/patches/0187-whyred-touch-dsx-protocol.patch"
python3 tools/check_patch.py "$P187" -q \
	--added-contains 'bsd/dev/santoni_touch.c:DSX_PDT_START' \
	--added-contains 'bsd/dev/santoni_touch.c:i2c_write(0xff)' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_identify(&dsx)' \
	--added-contains 'bsd/dev/santoni_touch.c:sweetouchtouch' \
	--added-lacks 'bsd/dev/santoni_touch.c:sweetouchtest'
python3 - <<'PY'
src = open('xnu/patches/0187-whyred-touch-dsx-protocol.patch', encoding='utf-8').read()
# The vendor driver selects a page with 0xFF <page> then writes the low
# address byte; the 0186 two-byte address wrote into register 0x00.
assert 'DSX_PDT_START\t\t0x00e9' in src
assert 'DSX_F01_QUERY_LEN\t21' in src
# Identification must stay read-only: the only 0xFF write is the page select.
assert src.count('i2c_write(0xff)') == 1
assert 'dsx_write' not in src
PY
echo "ok   0187 page-selects, reads the PDT at 0x00e9 and polls F12"

echo "== TOUCH: 0188 turns the reads into a diagnostic matrix =="
P188="xnu/patches/0188-whyred-touch-open-drain-diag.patch"
python3 tools/check_patch.py "$P188" -q \
	--added-contains 'bsd/dev/santoni_touch.c:scl_release(void)' \
	--added-contains 'bsd/dev/santoni_touch.c:TOUCH_LINE_WAIT_US' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_read_ex(uint8_t addr' \
	--added-contains 'bsd/dev/santoni_touch.c:diag same address twice' \
	--added-contains 'bsd/dev/santoni_touch.c:reset pulse low 20 ms, high 200 ms' \
	--added-contains 'bsd/dev/santoni_touch.c:delay(250000);' \
	--added-contains 'bsd/dev/santoni_touch.c:edge %d' \
	--added-contains 'bsd/dev/santoni_touch.c:poll %u s data'
python3 - <<'PY'
src = open('xnu/patches/0188-whyred-touch-open-drain-diag.patch', encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)
# High is always a release, never a push-pull drive.
assert 'pin_out(&touch_sda, 1)' not in body
assert 'pin_out(&touch_scl, 1)' not in body
assert body.count('scl_release();') >= 6
# Both address widths are exercised, and the page select is optional.
assert 'diag 00e9 a2' in body and 'diag 00e9 a2 nopage' in body
assert 'diag page1 0000 a1' in body
PY
echo "ok   0188 releases the lines, times reset and runs the read matrix"

echo "== TOUCH: 0189 tries every address framing against 0x00e9 =="
P189="xnu/patches/0189-whyred-touch-addressing-matrix.patch"
python3 tools/check_patch.py "$P189" -q \
	--added-contains 'bsd/dev/santoni_touch.c:enum dsx_frame' \
	--added-contains 'bsd/dev/santoni_touch.c:DFRAME_A2_LE' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_frame_works' \
	--added-contains 'bsd/dev/santoni_touch.c:using frame' \
	--added-contains 'bsd/dev/santoni_touch.c:bootloader mode; resetting once'
python3 - <<'PY'
src = open('xnu/patches/0189-whyred-touch-addressing-matrix.patch', encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)
# Every framing that could plausibly be right is exercised: both byte orders,
# with and without the page select, and the page select in the same write.
for name in ('page|a1', 'page+a1', 'page|a2be', 'page|a2le', 'a1',
             'a2be', 'a2le', 'page|a1dup'):
    assert '"%s"' % name in body, name
# A framing is only believed when the same register repeats and the slot
# above it reads differently, so a streaming pointer cannot win.
assert 'if (a[i] != b[i])' in body and 'differ = 1' in body
# On a failed repeat the log must carry both reads: which byte moved tells a
# streaming pointer from a lost address bit.
assert 'unstable' in body and 'printf(" ->")' in body
assert 'dsx_frame = best' in body
PY
echo "ok   0189 tries eight framings and keeps the repeatable one"

echo "== TOUCH: 0190 drives the same reads through the BLSP1 QUP =="
P190="xnu/patches/0190-whyred-qup-i2c-touch-reads.patch"
python3 tools/check_patch.py "$P190" -q \
	--added-contains 'bsd/dev/sweet_i2c.c:sweet_i2c_xfer' \
	--added-contains 'bsd/dev/sweet_i2c.c:QUP_I2C_MASTER_CONFIG' \
	--added-contains 'bsd/dev/sweet_i2c.c:QUP_STATE_PAUSE' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_read_q' \
	--added-contains 'bsd/dev/santoni_touch.c:sweeti2c' \
	--added-contains 'bsd/conf/files:bsd/dev/sweet_i2c.c'
python3 - <<'PY'
src = open('xnu/patches/0190-whyred-qup-i2c-touch-reads.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# The vendor v2 protocol, not a guess: two-tag mode must be enabled, the
# FIFO is filled in PAUSE and the core only runs afterwards.
assert 'MASTER_V2_TAGS\t\t1u' in body
assert 'qup[QUP_I2C_MASTER_CONFIG / 4] = MASTER_V2_TAGS;' in body
assert 'sweet_state_set(QUP_STATE_PAUSE)' in body

# A write followed by a read must use the non-stop tag so the read issues a
# repeated start; only the last buffer (a write-only transaction) stops.
assert '#define TAG_DATA_WR\t\t0x82u' in body
assert '#define TAG_DATA_WR_STOP\t0x83u' in body
assert '((rl ? TAG_DATA_WR : TAG_DATA_WR_STOP) << 16)' in body
assert '#define TAG_DATA_RD_STOP\t0x87u' in body

# Counts include the tag bytes, as i2c_msm_qup_xfer_init_reset_state does.
assert 'wl + TAG_LEN' in body and 'rl + RX_TAG_ECHO' in body

# The bit-bang stays as the fallback when the QUP does not come up, and the
# QUP path is selected per boot so the old diagnostic boots still work.
assert 'if (dsx_qup) {' in body
assert 'staying on bit-bang' in body
assert 'if (PE_parse_boot_argn("sweeti2c", &qup, sizeof(qup)) && qup)' in body

# The first two QUP boots hung before their first print because the GCC
# offsets were paired with the wrong base. sdm660.dtsi gives the block as
# clock-controller@100000 (reg 0x100000+0x94000), not 0x1800000: a write to
# 0x1852004 is outside the block and stalls the AXI bus. GPIO2/3 likewise
# live at the SOUTH tile 0x03102000.
assert '#define GCC_BASE\t\t0x00100000u' in body
assert '(GCC_BASE + 0x52004u)' in body
assert '(GCC_BASE + 0x19024u)' in body
assert '#define TLMM_SOUTH\t\t0x03100000u' in body
assert '0x01800000' not in body
assert '0x03002000' not in body
# msm_pinmux_set_mux writes the function's index in the pin group, so
# blsp_i2c1 on gpio2/3 is index 3, not a hardware mux number.
assert '#define PIN_I2C_SEL\t\t3u' in body
# The bring-up line and a 1.5 s dwell land on SYSTEM before the first GCC
# or QUP access, so a bus stall leaves the last block touched on the ring.
assert 'bring-up, GCC' in body
assert 'delay(1500000);' in body
assert body.index('bring-up, GCC') < body.index('sweet_clocks_on() != 0')

# The diag image walks itself back to fastboot instead of waiting for a key
# press: the vendor msm-poweroff.c "bootloader" path writes the IMEM reason
# and drops PS_HOLD. sdm660.dtsi places both at these addresses.
assert '#define IMEM_RESTART_REASON_PA\t0x146bf65cu' in body
assert '#define PSHOLD_PA\t\t0x010ac000u' in body
assert '#define RESTART_REASON_BOOTLOADER 0x77665500u' in body
assert '\t*pshold = 0;' in body
assert 'sweetouchreboot' in body

# blsp1_ahb_clk is a BRANCH_HALT_VOTED branch: a cleared BIT(31) alone is
# not a live slave, so require its NOC FSM. gcc_blsp1_qup1_i2c_apps_clk is a
# plain BRANCH_HALT branch and the vendor only rejects BIT(31) there - the
# 20:47 ring showed apps 0x00000001 (enable set, clock-off clear) while this
# check was demanding NOC 2, so the QUP was refused on every boot.
assert '#define BRANCH_NOC_ON\t\t(2u << 28)' in body
assert body.count('(*ahb_halt & BRANCH_NOC_MASK) == BRANCH_NOC_ON') == 1
assert body.count('(*app & BRANCH_NOC_MASK) == BRANCH_NOC_ON') == 0
assert 'if ((*app & BRANCH_CLK_OFF) == 0) {' in body
assert 'I2C apps clock stays halted' in body
assert 'refusing the QUP' in body
# A dwell after "clocks on" separates a GCC stall from a QUP stall, and a
# third one before the first SE write narrows a QUP stall to the slave.
assert body.count('delay(1500000);') == 4
assert 'QUP mapped, probing the SE' in body
# Ring 0196 died on the first QUP1 read with the watchdog dead too, so the
# control windows were probed first. Ring 0200 then died on that first
# control read and stayed off USB until the watchdog, so the probe is opt-in
# now: reading an unclocked BLSP1 sibling stalls the NoC.
assert 'sweet_probe_window(uint32_t base, uint32_t *ver)' in body
assert 'sweetqupctl' in body
assert body.index('sweetqupctl') < body.index('probe QUP2/QUP3 windows (controls)')
assert body.index('probe QUP2/QUP3 windows (controls)') > body.index('PE_parse_boot_argn("sweetqupctl"')
assert 'SE version 0x%08x, STATE 0x%08x' in body
assert 'WHYRED I2C: resetting the SE' in body

# A stall in the GCC or QUP window happens before the reset at the end of the
# probe, so the diag boot died with no USB at all and needed a held Power key.
# The way out is armed before the first clock write, on its own thread.
assert 'kernel_thread_start(whyred_reboot_watchdog' in body
assert 'tsleep(&whyred_reboot_watchdog, PZERO, "sweetwd",' in body
assert 'sweetouchwatchdog' in body
assert body.count('whyred_arm_reboot_watchdog();') == 1  # the probe's call
assert 'Arm the way out before the first clock or QUP register write' in body

# MX_WRITE_COUNT counts every byte pushed into the output FIFO, so a
# write-then-read also counts the read tag; without it the read never starts.
assert '(wl ? wl + TAG_LEN : 0) + (rl ? TAG_LEN : 0)' in body
assert 'rd_bytes = rl ? rl + RX_TAG_ECHO : 0' in body
# The FIFO depth comes from IO_MODES with the vendor tables, so the 21-byte
# F01 query is not refused with a made-up 16-byte limit.
assert 'static const int blk[4] = { 16, 16, 32, 0 };' in body
assert 'fifo_bytes = sweet_fifo_bytes(' in body
# The pin mux is the last step of init, so a failed init leaves the bit-bang
# pins (plain GPIO + pull-up) untouched.
assert '\tsweet_pins(1);\n\tsweet_i2c_ready = 1;' in body

# 0190 has to apply on top of 0189, not on the pre-0189 file. dxb only warns
# when a patch fails, and this hunk went stale when 0189 added
# DFRAME_PAGE_A1_DUMMY to the page-select condition: the whole QUP path was
# silently skipped on every build. Require the context line and the offsets
# that match the patched 0189 file.
context = [l[1:] for l in src.splitlines()
           if l.startswith(' ') and l[1:].strip()]
assert ' \tif (frame == DFRAME_PAGE_A1 || frame == DFRAME_PAGE_A1_DUMMY ||' in src
assert '\t    frame == DFRAME_PAGE_A2_BE ||' in context
assert '@@ -429,6 +496,10 @@' in src
PY
echo "ok   0190 adds the QUP master and keeps bit-bang as the fallback"

echo "== TOUCH: 0191 selects the DSX page only when it changes =="
P191="xnu/patches/0191-whyred-touch-page-cache.patch"
python3 tools/check_patch.py "$P191" -q \
	--added-contains 'bsd/dev/santoni_touch.c:static int dsx_page = -1;' \
	--added-contains 'bsd/dev/santoni_touch.c:hi != dsx_page' \
	--added-contains 'bsd/dev/santoni_touch.c:forget the cached page' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_page = -1;'
python3 - <<'PY'
src = open('xnu/patches/0191-whyred-touch-page-cache.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# The vendor's synaptics_rmi4_i2c_set_page() writes 0xFF <page> only when
# the page differs from the last one. Every read used to re-send the page
# select, which returns the part to page 0 mid-stream and is why the read
# back disagreed with the address phase.
assert body.count('dsx_page = hi;') == 4  # bit-bang + QUP, page select + combined
assert body.count('\tdsx_page = -1;') == 2  # reset + QUP init
# A failed page write must not be cached, or the next read skips the select:
# the cache is set only after both the rc/EIO block closes.
assert src.count(' \t\t}\n+\t\tdsx_page = hi;') == 2
PY
echo "ok   0191 caches the selected DSX page like the vendor driver"

echo "== TOUCH: 0192 reads a whole page instead of guessing an address =="
P192="xnu/patches/0192-whyred-touch-page-read.patch"
python3 tools/check_patch.py "$P192" -q \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_page_probe' \
	--added-contains 'bsd/dev/santoni_touch.c:0x4c, 0x00, 0x13, 0x06, 0x01, 0x12' \
	--added-contains 'bsd/dev/santoni_touch.c:f12off = 0x06 - base;'
python3 - <<'PY'
src = open('xnu/patches/0192-whyred-touch-page-read.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# Ring 0201: the first read after the page select lands on 0x00E9 and the
# second returns the next six bytes, so addressing is dead but the pointer is
# sequential. The probe must therefore read one long window and anchor it.
assert '#define DSX_PAGE_LEN\t128' in body
assert 'dsx_read_f(DSX_ADDR, 0x0000, page, sizeof(page), dsx_frame)' in body
assert 'base = DSX_PDT_START - off;' in body
assert 'for (poll = 0; poll < 10; poll++)' in body
# The call site is in the probe, after the bootloader-mode reset, and only
# reached when sweetouchi2c is set.
assert '\t\tdsx_page_probe();\n' in src
assert src.index('\n+\t\tdsx_page_probe();\n') < src.index('\n \t\ttouch_s = 0;\n')
PY
echo "ok   0192 reads 128 bytes, anchors F12 and polls its data register"

echo "== TOUCH: 0193 spends the one landing read on the PDT =="
P193="xnu/patches/0193-whyred-touch-pdt-first-read.patch"
python3 tools/check_patch.py "$P193" -q \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_PDT_WINDOW	256' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_pdt_scan(const char *tag)' \
	--added-contains 'bsd/dev/santoni_touch.c:at ctrl 0x%02x data 0x%02x' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_f12_poll(void)'
python3 - <<'PY'
src = open('xnu/patches/0193-whyred-touch-pdt-first-read.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# Ring 0201: 0x00E9 returned the F34 entry and the next read of 0x00E9
# returned the F01 entry, so only the first addressed read lands. The window
# must therefore be one 256-byte transfer from the PDT start, taken before the
# framing matrix, and it must be the first thing the probe does after the pin
# setup.
assert 'dsx_read_f(DSX_ADDR, DSX_PDT_START, win, sizeof(win),' in body
assert body.index('dsx_pdt_scan("first read")') < body.index('dsx_f12_poll();')
assert src.index('dsx_pdt_scan("first read")') < src.index('\n \t\tdsx_diag();\n')
assert '\n \t\tdsx_identify(&dsx);\n' in src
# A flash-programming table is F34+F01 only; the caller resets once and scans
# again rather than trusting a missing F12.
assert 'entries <= 3' in body
assert 'dsx_pdt_scan("after reset")' in body
# dsx_identify keeps fn = b[5] & 0x3f, so the scan must agree with it and also
# print fn4 in case the window is shifted by one byte.
assert '(e[5] & 0x3f) == DSX_F12_FUNC' in body
assert 'fn4 %02x fn5 %02x' in body
# The poll opens with a page-0 window read, which the page select does honour.
assert 'dsx_read_f(DSX_ADDR, 0x0000, page, sizeof(page)' in body
assert 'page[off] & 0x80' in body
PY
echo "ok   0193 reads the PDT first, retries after reset and polls F12"

echo "== TOUCH: 0194 re-arms the read pointer with the page select =="
P194="xnu/patches/0194-whyred-touch-page-reselect.patch"
python3 tools/check_patch.py "$P194" -q \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_PDT_WINDOW	24' \
	--added-contains 'bsd/dev/santoni_touch.c:static int dsx_f12_query;' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_six("reselect e9", DSX_PDT_START);' \
	--added-contains 'bsd/dev/santoni_touch.c:f->f12_ctrl = b[2];'
python3 - <<'PY'
src = open('xnu/patches/0194-whyred-touch-page-reselect.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# Ring 0199 recovered the whole table and it is three records long. A 256-byte
# window reads past the end marker and the F12 record lands off the six-byte
# grid (ring 0193: `81 4c 00 13 06 01 12`), so read exactly four records.
assert '#define DSX_PDT_WINDOW\t24' in body
# The record fields are query/cmd/ctrl/data/intr/fn from the LSB, not a
# big-endian pair: 0193 polled 0x01 instead of F12 data 0x06.
assert 'f->f12_query = b[0];' in body
assert 'f->f12_ctrl = b[2];' in body
assert 'f->f12_data = b[3];' in body
assert 'f->f01_query = b[0];' in body
# Accept the record at any offset, because the 0193 dump had one stray byte in
# front of it; the record itself names the data base.
assert 'static const uint8_t f12_rec[DSX_PDT_ENTRY_SIZE]' in body
assert '0x4c, 0x00, 0x13, 0x06, 0x01, 0x12' in body
assert 'F12 record at offset' in body
# The page select must be forced before the PDT read and before every sample:
# 0191 caches it, and the cache is what the E9/E3/E9 test has to defeat.
assert 'dsx_page = -1;' in body
assert body.count('dsx_page = -1;') >= 3
assert 'dsx_six("repeat cached"' in body
assert 'dsx_six("then e3"' in body
assert 'dsx_six("reselect e9"' in body
assert 'force FF <page>' in body
# whyred's DT has no synaptics,reset-gpio and GPIO 66 is the Novatek reset;
# ring 0197 showed that pulse parks the part in an all-zero state. The retry
# paths must not drive it.
assert 'dsx_reset(north)' not in body
assert 'pulsing GPIO 66 (it is the Novatek reset)' in body
# dsx_identify runs after the framing matrix, so its own walk starts wherever
# that left the stream; the scan above is the authoritative F12 source.
assert 'dsx.f12_data = (uint16_t)((dsx_f12_page << 8) |' in body
PY
echo "ok   0194 reads 24 bytes, fixes the PDT field offsets, reselects the page"

echo "== TOUCH: 0195 leaves the wakeup-gesture state without a page select =="
P195="xnu/patches/0195-whyred-touch-wake-lpwg.patch"
python3 tools/check_patch.py "$P195" -q \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_F01_CTRL	0x0d' \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_F12_QUERY	0x4c' \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_F12_CTRL	0x13' \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_F12_DATA	0x06' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_write_f(uint8_t addr' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_wake_normal(void)' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_wake_probe(void)'
python3 - <<'PY'
src = open('xnu/patches/0195-whyred-touch-wake-lpwg.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# Ring 0194: IRQ GPIO 67 moves on every tap while F12 data stays zero. That
# is the vendor's wakeup-gesture state. Leave it the way resume() does: F01
# control bits [2:0] = 0 (normal operation), configured bit kept.
assert '#define DSX_F01_CTRL\t0x0d' in body
assert 'dsx_f01_normal = (uint8_t)((v & ~0x07u) | 0x80u);' in body
assert 'dsx_write_f(DSX_ADDR, DSX_F01_CTRL, &dsx_f01_normal, 1)' in body
# Ring 0192/0193/0194: FF <page> is what turns every later read into zeros,
# so the wake probe must select page 0 by cache and never force it.
assert 'dsx_page = 0;' in body
assert 'dsx_page = -1;' not in body
# The one landing read is spent on F01 control, then the wake write re-arms
# the address before each F12 sample.
assert 'dsx_read_f(DSX_ADDR, DSX_F01_CTRL, &v, 1, dsx_frame)' in body
assert 'dsx_wake_normal();\t/* each sample re-arms the address */' in body
assert 'dsx_read_f(DSX_ADDR, DSX_F12_DATA, b, sizeof(b),' in body
# Addresses come from the 0193/0194 PDT dumps and the recovery ring.
assert 'DSX_F12_QUERY\t0x4c' in body
assert 'DSX_F12_CTRL\t0x13' in body
assert 'DSX_F12_DATA\t0x06' in body
# GPIO 66 is the Novatek reset; ring 0197 showed it parks the DSX in zeros.
assert 'dsx_reset(north)' not in body
# The old framing matrix, the identify walk and the page probe each spend the
# landing read, so 0195 drops them from the touch path.
assert 'dsx_diag()' not in body
assert 'dsx_identify(&dsx)' not in body
assert 'dsx_page_probe()' not in body
PY
echo "ok   0195 reads F01 first, wakes to normal operation, polls without page select"

echo "== TOUCH: 0196 issues the vendor software reset before the scan =="
P196="xnu/patches/0196-whyred-touch-sw-reset.patch"
python3 tools/check_patch.py "$P196" -q \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_F01_CMD	0x06' \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_F01_DATA	0x11' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_sw_reset(void)' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_pdt_walk(const char *tag)' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_reset_probe(void)' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_read_page(uint8_t addr'
python3 - <<'PY'
src = open('xnu/patches/0196-whyred-touch-sw-reset.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
removed = [l[1:] for l in src.splitlines()
           if l.startswith('-') and not l.startswith('---')]
body = '\n'.join(added)
gone = '\n'.join(removed)

# Ring 0195: the F01 control write ACKs but every read after it returns
# zero, so 0195's order was wrong. The vendor recovers the part with the F01
# software reset (command base 0x06, value 0x01) and rescans the PDT.
assert 'dsx_write_f(DSX_ADDR, DSX_F01_CMD, &cmd, 1)' in body
assert 'uint8_t cmd = 0x01;' in body
assert 'delay(300000);' in body
assert '#define DSX_F01_CMD\t0x06' in body
# The scan is the vendor one: addressed six-byte reads downwards from 0xE9,
# stopping at the 0x00 end marker, with the page select forced for each read.
assert 'addr = DSX_PDT_START;' in body
assert 'addr -= DSX_PDT_ENTRY_SIZE;' in body
assert 'dsx_read_page(DSX_ADDR, addr, e, sizeof(e))' in body
assert 'dsx_page = -1;\n\treturn dsx_read_f(addr, reg, buf, len, dsx_frame);' in body
assert 'end of PDT at 0x%04x' in body
# F01 base addresses come from the scanned record, not a guess.
assert 'dsx_f01_cmd = e[1];' in body
assert 'dsx_f01_ctrl = e[2];' in body
assert 'dsx_f01_data = e[3];' in body
# The control write happens only after the scan and only when unconfigured,
# and its value is read back first.
assert body.index('dsx_pdt_walk("after reset")') < body.index('dsx_wake_normal();')
assert 'if (!(v & 0x80))' in body
# The fixed-address probe and the 24-byte window are gone.
assert 'dsx_wake_probe' in gone
assert 'dsx_pdt_scan' in gone
# Both poll loops page-select before every sample, like the vendor read.
assert 'dsx_read_page(DSX_ADDR, reg, b, sizeof(b))' in body
assert 'dsx_read_page(DSX_ADDR, f->f12_data, b, sizeof(b))' in body
PY
echo "ok   0196 resets F01, walks the PDT downwards and page-selects every read"

echo "== TOUCH: 0197 probes inside the post-boot window, before eMMC init =="
P197="xnu/patches/0197-whyred-touch-early-probe.patch"
python3 tools/check_patch.py "$P197" -q \
	--added-contains 'bsd/dev/santoni_touch.c:santoni_touch_early(void)' \
	--added-contains 'bsd/dev/santoni_touch.c:sweetouchearly' \
	--added-contains 'bsd/dev/santoni_touch.c:touch_wait_until_ms(uint32_t target_ms)' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_early_ran' \
	--added-contains 'bsd/dev/santoni_sdhci.c:santoni_touch_early();'
python3 - <<'PY'
src = open('xnu/patches/0197-whyred-touch-early-probe.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# The window: the probe is called from the top of santoni_sdhci_init(), so it
# runs before DT lookup, eMMC enumeration and the framebuffer test.
assert 'santoni_touch_early();' in body
# The call sits in santoni_sdhci_init() above the /santoni-sdhci lookup, so it
# also runs before eMMC enumeration and the framebuffer test.
assert 'if (SecureDTLookupEntry(NULL, "/santoni-sdhci"' in src
assert 'touch_wait_until_ms(at);' in body
assert 'absolutetime_to_nanoseconds(mach_absolute_time(), &ns);' in body
assert 'if (at > 4000)' in body
# The early path only dumps; the poll and the panel band stay late.
assert 'dsx_reset_probe();' in body
assert 'dsx_early_found = dsx_f12_found;' in body
# The timestamps survive into the late probe, so a lost early console line
# still tells us which window the dump ran in.
assert 'dsx_early_start_ms = touch_now_ms();' in body
assert 'dsx_early_end_ms = touch_now_ms();' in body
assert 'early probe ran %llu..%llu ms' in body
assert 'late probe reuses the early PDT' in body
assert 'dsx_early_ran && dsx_early_found' in body
# One watchdog for both paths.
assert 'whyred_arm_reboot_watchdog();' in body
assert 'reboot && !dsx_early_ran' in body
PY
grep -q 'SWEETEARLY=MS' tools/build-touch-diag.sh
grep -q 'sweetouchearly=\$early' tools/build-touch-diag.sh
echo "ok   0197 runs the PDT dump before eMMC init and reuses it late"

echo "== TOUCH: 0198 programs F12 the way the vendor reference driver does =="
P198="xnu/patches/0198-whyred-touch-f12-reporting.patch"
python3 tools/check_patch.py "$P198" -q \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_f12_program(void)' \
	--added-contains 'bsd/dev/santoni_touch.c:#define DSX_F12_RPT_DEFAULT	0x1f' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_bits_present(const uint8_t *bytes' \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_f12_touch_reg'
python3 - <<'PY'
src = open('xnu/patches/0198-whyred-touch-f12-reporting.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# Query registers, exactly where synaptics_rmi4_f12_init() reads them:
# size of query_5 at query_base+4, its data at +5; query_8 at +7 and +8.
assert '#define DSX_F12_Q5_SIZE\t0x04' in body
assert '#define DSX_F12_Q5_DATA\t0x05' in body
assert '#define DSX_F12_Q8_SIZE\t0x07' in body
assert '#define DSX_F12_Q8_DATA\t0x08' in body
# Reporting is control 28 and the vendor value is RPT_DEFAULT (0x1F), not 0x01.
assert '#define DSX_F12_RPT_DEFAULT\t0x1f' in body
assert 'rpt = DSX_F12_RPT_DEFAULT;' in body
assert 'dsx_write_f(DSX_ADDR, (uint16_t)dsx_f12_report_ctrl, &rpt, 1)' in body
assert 'f12 ctrl28 readback' in body
# control register offsets are the count of present registers below them,
# because each control register is one byte (vendor ctrl_20/ctrl_28 offset).
assert 'dsx_bits_present(q5 + 1, q5n - 1, 28)' in body
assert 'dsx_f12_report_ctrl = dsx_f12_ctrl + ctrl28;' in body
# The 2D data starts after data0 (the gesture register), not after every
# declared data register.
assert 'data0 = (q8[1] & 0x01) ? 1 : 0;' in body
assert 'dsx_f12_touch_reg = dsx_f12_data + data0;' in body
# Never program a part whose PDT is the shifted stream: F01 must be the
# standard 00/06/0D/11 record.
assert 'dsx_f01_query != 0x00 || dsx_f01_cmd != 0x06' in body
assert 'dsx_f01_ctrl != 0x0d || dsx_f01_data != 0x11' in body
assert 'not programming F12' in body
# Poll and the late touch test use the computed data register.
assert 'dsx_f12_touch_reg ? (uint16_t)dsx_f12_touch_reg :' in body
assert 'dsx.f12_data = dsx_f12_touch_reg ?' in body
PY
echo "ok   0198 parses query_5/query_8, writes RPT_DEFAULT and polls data0+1"
echo "== TOUCH: 0199 leaves visible breadcrumbs before the ring exists =="
P199="xnu/patches/0199-whyred-touch-early-marks.patch"
python3 tools/check_patch.py "$P199" -q \
	--added-contains 'bsd/dev/santoni_touch.c:touch_early_mark(int step, uint32_t color)' \
	--added-contains 'bsd/dev/santoni_touch.c:TOUCH_FB_PHYS'
python3 - <<'PY2'
src = open('xnu/patches/0199-whyred-touch-early-marks.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# The markers exist because the ring is only flushed after eMMC init, so a
# hang inside the early probe leaves no new ring at all.
assert 'touch_early_mark(1, 0xffff0000);' in body
assert 'touch_early_mark(2, 0xffff8000);' in body
assert 'touch_early_mark(3, 0xffffff00);' in body
assert 'touch_early_mark(4, 0xff00ff00);' in body
assert 'touch_early_mark(5, 0xff00ffff);' in body
assert 'touch_early_mark(6, 0xff0000ff);' in body
assert 'touch_early_mark(7, 0xffff00ff);' in body
assert 'touch_early_mark(8, 0xffffffff);' in body
# PE_state.video.v_baseAddr is the physical ABL address at this point, not a
# virtual pointer: 0199's first build dereferenced it and hung before the first
# band appeared. The window must come from an explicit IO mapping.
assert '(uintptr_t)PE_state' not in body
assert 'fb = (volatile uint32_t *)ml_io_map_unmappable(' in body
assert 'TOUCH_FB_PHYS + (uint64_t)TOUCH_FB_ROW0 * TOUCH_FB_STRIDE' in body
# Marks are drawn at distinct rows so the highest visible row is the step.
assert 'TOUCH_FB_ROW0 + (uint32_t)step * 70u' in body
PY2
echo "ok   0199 paints one band per early-probe step on the boot framebuffer"

echo "== TOUCH: 0200 splits the band that never appeared =="
P200="xnu/patches/0200-whyred-touch-early-marks-detail.patch"
python3 tools/check_patch.py "$P200" -q \
	--added-contains 'bsd/dev/santoni_touch.c:dsx_early_marks_on' \
	--added-contains 'bsd/dev/santoni_touch.c:touch_early_mark(7, 0xff800000)' \
	--added-contains 'bsd/dev/santoni_touch.c:early_pdt_colour[i]' \
	--added-contains 'bsd/dev/santoni_touch.c:TOUCH_FB_ROWS'
python3 - <<'PY2'
import re
src = open('xnu/patches/0200-whyred-touch-early-marks-detail.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# Band 6 (blue) was the last one visible, so 0200 refines exactly the step
# between "bus cleared" and "PDT walked", and says so in the patch.
assert 'band 6 (blue)' in body
# The extra bands are only for the early probe: the late probe runs the same
# helpers and would repaint them long after anyone can read the panel.
assert 'if (dsx_early_marks_on)' in body
assert 'dsx_early_marks_on = 1;' in body and 'dsx_early_marks_on = 0;' in body
# Every step must have its own colour, or "highest band" is ambiguous.
steps = [(int(n), int(c, 16)) for n, c in
         re.findall(r'touch_early_mark\((\d+), (0x[0-9a-f]+)\)', body)]
steps += [(10 + i, int(c, 16)) for i, c in
          enumerate(re.findall(r'(0x[0-9a-f]+),\t/\* (?:10|11|12|13|14|15)', body))]
colours = [c for _, c in steps]
assert len(colours) == len(set(colours)), colours
assert max(n for n, _ in steps) == 19
# The window must still cover the last band row (40 + 19*70 + 50).
assert '1450u' in body
PY2
echo "ok   0200 adds per-transaction bands 7..19 inside the early probe"

echo "== TOUCH: 0201 paints the eMMC phases after the early probe =="
P201="xnu/patches/0201-whyred-touch-emmc-phases.patch"
python3 tools/check_patch.py "$P201" -q \
	--added-contains 'bsd/dev/santoni_touch.c:santoni_touch_mark' \
	--added-contains 'bsd/dev/santoni_touch.c:touch_phase_mark' \
	--added-contains 'bsd/dev/santoni_sdhci.c:TOUCH_PHASE_MARK(0, 0xff000080)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:TOUCH_PHASE_MARK(10, 0xff8b4513)' \
	--added-contains 'bsd/dev/santoni_sdhci.c:TOUCH_PHASE_MARK(9, 0xff006060)'
python3 - <<'PY2'
import re
src = open('xnu/patches/0201-whyred-touch-emmc-phases.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# The bands are only for the early-probe boot: a normal boot must not paint.
assert 'if (!dsx_early_ran)' in body
assert 'return;' in body
# The marks must cover the whole eMMC init, not just its entry.
for phase in ('0,', '1,', '2,', '3,', '4,', '5,', '6,', '7,', '8,',
              '9,', '10,'):
    assert 'TOUCH_PHASE_MARK(' + phase in body, phase
# The last phase (ring started) must follow the first (probe returned).
assert body.index('TOUCH_PHASE_MARK(0') < body.index('TOUCH_PHASE_MARK(10')
# One colour per phase, so the highest band is unambiguous.
cols = re.findall(r'TOUCH_PHASE_MARK\(\d+, (0x[0-9a-f]+)\)', body)
assert len(cols) == 11, cols
assert len(cols) == len(set(cols)), cols
# 11 marks at 64 rows, 46 tall, from row 1450 must stay on the panel.
assert '1450u' in body and '64u' in body and '46u' in body
assert 1450 + 10 * 64 + 46 <= 2160
# The mark before santoni_log_start() proves eMMC init finished; the one
# after it proves the ring started (that is where the four bars are painted).
i9 = body.index('TOUCH_PHASE_MARK(9')
i10 = body.index('TOUCH_PHASE_MARK(10')
assert i9 < i10
# Phase 9 is added immediately before santoni_log_start(), phase 10 right
# after it (both are anchored on that context line in the patch).
assert '\n \tsantoni_log_start();\n+\tTOUCH_PHASE_MARK(10' in src
PY2
echo "ok   0201 marks eleven eMMC-init phases below the touch bands"

echo "== TOUCH: 0202 proves the early probe returned =="
P202="xnu/patches/0202-whyred-touch-early-end-mark.patch"
python3 tools/check_patch.py "$P202" -q \
	--added-contains 'bsd/dev/santoni_touch.c:touch_fb' \
	--added-contains 'bsd/dev/santoni_touch.c:touch_end_mark' \
	--added-contains 'bsd/dev/santoni_touch.c:TOUCH_FB_ROWS' \
	--added-contains 'bsd/dev/santoni_touch.c:2160u' \
	--added-contains 'bsd/dev/santoni_touch.c:0xff00ff40'
python3 - <<'PY2'
src = open('xnu/patches/0202-whyred-touch-early-end-mark.patch',
           encoding='utf-8').read()
added = [l[1:] for l in src.splitlines()
         if l.startswith('+') and not l.startswith('+++')]
body = '\n'.join(added)

# One mapping for the whole frame: the 0201 second window is exactly the
# variable this patch removes, so the added code must map only once.
assert body.count('ml_io_map_unmappable') == 1, body.count('ml_io_map_unmappable')
assert 'TOUCH_FB_PHYS +' not in body
# The end mark sits after the last printf and before the return, in the gap
# between the white band (ends at row 1419) and phase 0 (starts at 1450).
# The call is added immediately before the function's return, so a band at
# that row can only come from code that fell through to the return.
assert '\n+\ttouch_end_mark();\n \treturn 0;' in src
assert '1425' in body and '1445' in body
# The phase block still has to fit on the panel: last row (step 10).
assert 1450 + 10 * 64 + 46 <= 2160
PY2
echo "ok   0202 maps once and paints a return band before the first phase"

echo "== LOG PULL: the by-name symlink decides the SYSTEM branch =="
# SYSTEM is /dev/block/bootdevice/by-name/system -> mmcblk0p61, so the old
# "grep system $resolved_path" check always fell through to the userdata tail
# and reported "wrong area" even when the log was there. Stub adb, feed a
# well-formed 8193-block area, and require the decoded text.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"
cat > "$tmp/bin/adb" <<'STUB'
#!/bin/sh
case "$1" in
get-state) echo device ;;
shell)
	case "$2" in
	*by-name*) echo "system /dev/block/mmcblk0p61" ;;
	blockdev*) echo 6241124352 ;;
	*) exit 1 ;;
	esac ;;
exec-out) cat "$FAKE_AREA" ;;
*) exit 1 ;;
esac
STUB
chmod +x "$tmp/bin/adb"
python3 - "$tmp/area.bin" <<'PY'
import struct, sys
text = b'SWEET EARLY 20: kprintf startup complete\n'
ring = bytearray(4 * 1024 * 1024)
ring[:len(text)] = text
hdr = struct.pack('<12sIIQQ', b'SANTONILOG1', 1, len(ring), len(text), 1)
area = hdr.ljust(512, b'\0') + bytes(ring)
open(sys.argv[1], 'wb').write(area[:8193 * 512])
PY
PATH="$tmp/bin:$PATH" FAKE_AREA="$tmp/area.bin" \
	tools/sweet-log-pull.sh "$tmp/out.txt" 2>"$tmp/err"
grep -q 'SWEET EARLY 20: kprintf startup complete' "$tmp/out.txt"
grep -q 'SYSTEM partition' "$tmp/err"
# A headerless ring with a real boot-log start must still decode: 0177 left
# 26 blocks of text and a blank sector 0, and the old script threw that away.
python3 - "$tmp/headerless.bin" <<'PY'
import sys
text = (b'iBoot version: whyred-shim\n'
        b'WHYRED SDHCI: driver 0178 (root "userdata", writes enabled)\n'
        b'BSD root: emmcroot0, major 2, minor 0\n')
area = bytearray(8193 * 512)
area[512:512 + len(text)] = text
open(sys.argv[1], 'wb').write(area)
PY
PATH="$tmp/bin:$PATH" FAKE_AREA="$tmp/headerless.bin" \
	tools/sweet-log-pull.sh "$tmp/salvaged.txt" 2>"$tmp/salvaged-err"
grep -q 'driver 0178' "$tmp/salvaged.txt"
grep -q 'salvaged' "$tmp/salvaged-err"
# A blank area must fail loudly (exit 2 + the flusher hint), not decode noise.
dd if=/dev/zero of="$tmp/blank.bin" bs=512 count=8193 status=none
PATH="$tmp/bin:$PATH" FAKE_AREA="$tmp/blank.bin" \
	tools/sweet-log-pull.sh "$tmp/bad.txt" 2>"$tmp/bad-err" && exit 1 || true
grep -q 'no SANTONILOG1 header' "$tmp/bad-err"
grep -q 'WHYRED LOG: console log flusher active' "$tmp/bad-err"
echo "ok   log pull reads the SYSTEM area through the by-name symlink"

echo "== PAYLOAD: reject a wrong-machine kernel or DTB before the ABL does =="
python3 tests/test_payload.py
echo "ok   check_payload matches whyred/lavender DTBs and the extractor walks a table"
python3 tests/test_prune_dtb.py >/dev/null
echo "ok   prune_dtb keeps the board ids and drops the nodes nobody reads"
python3 tests/test_zip_partial.py >/dev/null
echo "ok   zip_partial proves an entry before calling it the kernel"
python3 tests/test_payload_cmdline.py >/dev/null
echo "ok   payload_cmdline reads the line the shim really uses, not boot.img"

echo "== IMAGES: manual runs choose normal / log / both =="
# Both images come from the same kernel; the normal one must carry no log
# capture at all (no sweetlog ring, no cs_debug page dumps, no kprintf spam)
# and the log one must carry all three. The switches that keep the unsealed
# root alive belong to both.
grep -q 'KPRINTF_SPAM=' shim/build.sh
grep -q 'build_mode "-log"' .github/workflows/xnu.yml
grep -q 'shim-boot-log.img' .github/workflows/xnu.yml
grep -q 'case "$MODE" in' .github/workflows/xnu.yml
grep -q 'build_mode:' .github/workflows/xnu.yml
grep -q -- '- normal' .github/workflows/xnu.yml
grep -q -- '--reject sweetlog=1 --reject cs_debug=2' .github/workflows/xnu.yml
grep -q -- '--reject=-enable_kprintf_spam' .github/workflows/xnu.yml
grep -q -- '--require sweetlog=1 --require cs_debug=2' .github/workflows/xnu.yml
grep -q 'DIAG="sweetlog=1 cs_debug=2"' .github/workflows/xnu.yml
# Display profile: the normal image keeps the boot log off the panel (so the
# graphical boot and the Apple logo stay visible) and the log image keeps
# printing, because the ring and the UART are the only diagnostics there is.
# ignition_level= is rejected on every image: dyld's libignition then misses
# the shared cache and pid 1 dies with "no dyld cache" (NOTES 0029j/0029k).
grep -Fq 'KPRINTF_MUTE="${KPRINTF_MUTE:-0}"' shim/build.sh
grep -Fq 'DISPLAY_ARGS="disable_kprintf_output=1 sweetfb_stayon=1 sweetfb_hz=60"' shim/build.sh
grep -Fq 'DISPLAY_ARGS="disable_kprintf_output=0"' shim/build.sh
grep -Fq 'KPRINTF_MUTE="$4"' .github/workflows/xnu.yml
# The panel console is a separate switch from the mute: disable_kprintf_output
# is inert in this kernel, so the only thing that really stops the console
# being drawn over the GUI is sweetfb_noconsole=1 (patch 0067). A manual run
# picks text or hidden, and the built image must carry exactly that.
grep -Fq 'PANEL_NOCONSOLE="${PANEL_NOCONSOLE:-0}"' shim/build.sh
grep -Fq 'DISPLAY_ARGS="$DISPLAY_ARGS sweetfb_noconsole=1"' shim/build.sh
grep -Fq 'PANEL_NOCONSOLE="$5"' .github/workflows/xnu.yml
grep -Fq 'panel_console:' .github/workflows/xnu.yml
grep -Fq -- '--require sweetfb_noconsole=1' .github/workflows/xnu.yml
grep -Fq -- '--reject sweetfb_noconsole=1' .github/workflows/xnu.yml
grep -Fq 'build_mode ""     ""     ""     1 "$NOCONSOLE"' .github/workflows/xnu.yml
grep -Fq -- '--require disable_kprintf_output=1' .github/workflows/xnu.yml
grep -Fq -- '--require disable_kprintf_output=0' .github/workflows/xnu.yml
grep -Fq -- "--reject 'ignition_level='" .github/workflows/xnu.yml
grep -Fq 'build_mode "-log" "$DIAG" "$SPAM" 0' .github/workflows/xnu.yml
grep -Fq 'build_mode ""     ""     ""     1' .github/workflows/xnu.yml
# Only the normal image may mute the framebuffer console.
python3 - <<'PY'
wf = open('.github/workflows/xnu.yml').read()
normal = wf.split('if [ -f images/shim-boot.img ]; then', 1)[1].split('\n          fi', 1)[0]
log = wf.split('if [ -f images/shim-boot-log.img ]; then', 1)[1].split('\n          fi', 1)[0]
assert 'disable_kprintf_output=1' in normal, 'normal image is not muted'
assert 'disable_kprintf_output=1' not in log, 'log image must not be muted'
print('ok   the normal image mutes the panel, the log image still prints')
PY
# The command line is assembled by shell assignments in the workflow. An
# unquoted "COMMON=emmcrw=1 cs_enforcement_disable=1 ..." sets three shell
# variables and leaves COMMON="emmcrw=1"; the 0177 images went out without
# the two code-signing switches that way. Evaluate the lines the workflow
# really runs, so quoting cannot be lost silently again.
python3 - <<'PY'
import subprocess
lines = [l.strip() for l in open('.github/workflows/xnu.yml')
         if l.strip().startswith(('COMMON=', 'DIAG=', 'SPAM='))]
script = '\n'.join(lines) + '\nprintf "%s\\n" "$COMMON"\n'
out = subprocess.run(['bash', '-c', script], capture_output=True,
                     text=True, check=True).stdout
need = ['emmcrw=1', 'cs_enforcement_disable=1',
        'unentitled_ios_sim_launch=1', 'launchd_unsecure_cache=1']
missing = [n for n in need if n not in out.split()]
if missing:
    raise SystemExit('COMMON lost %s -> %r' % (' '.join(missing), out))
# The normal image must not record a console ring: sweetlog moved to DIAG.
if 'sweetlog=1' in out.split():
    raise SystemExit('COMMON carries sweetlog=1: the normal image would '
                     'still capture a log')
PY
grep -q -- '--require cs_enforcement_disable=1' .github/workflows/xnu.yml
grep -q -- '--require unentitled_ios_sim_launch=1' .github/workflows/xnu.yml
# The four-bar scanout probe is a workflow choice now: off by default, on for
# a diagnostic run, and the payload check must follow the same choice.
grep -q 'fb_test_pattern:' .github/workflows/xnu.yml
grep -q -- '--require sweetfbtest=1' .github/workflows/xnu.yml
grep -q -- '--reject sweetfbtest=1' .github/workflows/xnu.yml
echo "ok   normal image has no log capture, log image keeps ring and dumps"
echo "ok   the colour-bar probe is an off/on workflow input"

# Optional deep check: if a patched XNU source tree is provided, the three
# patches must also apply to it as-is (git apply --check).
if [ -n "${XNU_SRC:-}" ]; then
	echo "== git apply --check against XNU_SRC=$XNU_SRC =="
	(cd "$XNU_SRC" && git apply --check \
	    "$OLDPWD/$P142" "$OLDPWD/$P143" "$OLDPWD/$P144")
	echo "ok   all three patches apply to $XNU_SRC"
fi

echo "== TOUCH: a bit-bang-only image can be packed without the QUP =="
# Every QUP1 register access has killed this SoC and needed a held Power key.
# SWEETI2C=0 never maps the QUP window, so it is safe to boot while the phone
# is away from its owner, and it is the only way dsx_diag() (the 0189 framing
# matrix and the F12 poll) has ever run against the app-mode controller.
grep -Fq 'qup=${SWEETI2C:-1}' tools/build-touch-diag.sh
grep -Fq 'reset=${SWEETRESET:-1}' tools/build-touch-diag.sh
grep -Fq 'out=${OUT_IMAGE:-images/shim-boot-touch-diag.img}' tools/build-touch-diag.sh
grep -Fq 'sweeti2c=$qup' tools/build-touch-diag.sh
grep -Fq 'sweetouchreset=$reset' tools/build-touch-diag.sh
grep -Fq 'cp shim/out/shim-boot.img "$out"' tools/build-touch-diag.sh
echo "ok   SWEETI2C=0 / SWEETRESET=0 / OUT_IMAGE= pack a bit-bang-only image"

echo "== TOUCH: the recovery I2C probe keeps the vendor framing =="
# The probe is how the known-good QUP is compared with the XNU bit-bang. If it
# ever stops sending the page select as its own transaction, or loses the
# repeated-start variant, the comparison is no longer meaningful.
grep -Fq 'MODE 0: page select (ff PAGE), then write REG + repeated start read LEN' tools/i2cprobe.c
grep -Fq 'pagebuf[0] = (char)0xff;' tools/i2cprobe.c
grep -Fq 'I2C_M_RD' tools/i2cprobe.c
grep -Fq 'I2C_SLAVE_FORCE' tools/i2cprobe.c
echo "ok   tools/i2cprobe.c does page select, one address byte, repeated start"

# The E9/E3/E9 test is the one that separates "address phase works, page select
# is the bug" from "the pointer really does auto-advance". It must keep using
# mode 2 as the vendor framing and mode 0 as ours, and it must force continuous
# reporting mode instead of unbinding the driver (unbinding kills vcc_i2c).
[ -x tools/recovery-dsx-test.sh ]
grep -Fq 'for a in 0xe9 0xe3 0xe9' tools/recovery-dsx-test.sh
grep -Fq 'echo 0 > /sys/bus/i2c/devices/${BUS}-0020/wake_gesture' tools/recovery-dsx-test.sh
grep -Fq 'for mode in 2 0' tools/recovery-dsx-test.sh
if grep -Fq 'unbind' tools/recovery-dsx-test.sh; then
	echo "fail recovery-dsx-test.sh must not unbind the DSX driver"
	exit 1
fi
echo "ok   tools/recovery-dsx-test.sh runs the E9/E3/E9 page-select test"

# A phone that was just plugged in enumerates slowly, and the 2026-11-05
# cycle saw adb for two checks and then exited while ABL was still coming up.
# Wait 600 s for fastboot and treat an authorized adb device as rebootable.
grep -Fq 'for _ in $(seq 1 600); do' tools/touch-cycle.sh
grep -Fq 'adb_state=$(adb get-state 2>/dev/null)' tools/touch-cycle.sh
grep -Fq '[ "$adb_state" = "device" ]' tools/touch-cycle.sh
# ABL can expose fastboot before its bulk endpoints and the udev uaccess ACL
# are ready: on 2026-11-05 that produced SendBuffer() EACCES and no boot.
grep -Fq 'until fastboot boot "$img"; do' tools/touch-cycle.sh
grep -Fq 'fastboot boot failed (attempt' tools/touch-cycle.sh
echo "ok   the cycle waits 600 s for fastboot and retries a failed boot"

echo "run_tests: all hosts tests, patch checks and shipped-code assertions passed"

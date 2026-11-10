// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host model of board's Adreno 509 GPU power-and-clock readings, driving drivers/Gpu/GpuCore.h
// and GpuRegs.h against known register layouts and verified readback values.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../GpuCore.h"
#include "../GpuRegs.h"

namespace {

int checks = 0;

void
expect (bool condition, const char *what)
{
    if (!condition) {
        printf ("FAIL: %s\n", what);
        checks++;
    }
}

// The thirteen block offsets Gpu.cpp reads.
const uint32_t kOffsets[kGpuLdoWordCount] = {
    0x00U, 0x04U, 0x08U, 0x0cU, 0x10U, 0x14U, 0x18U, 0x1cU, 0x20U, 0x24U, 0x28U, 0x2cU, 0x30U
};

// Simulated census readback: same-beat cx_on=1, gx_on=0.
const uint32_t kCensus[kGpuLdoWordCount] = {
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x80000038U, 0x00000000U, 0x0000000dU, 0x00000000U, 0x00000000U, 0x00000000U
};

}  // namespace

int
main (void)
{
    // -----------------------------------------------------------------------
    // 1. LDO census compared by the layer
    // -----------------------------------------------------------------------
    GpuLdoCensus c81;
    gpu_ldo_census (kOffsets, kCensus, kGpuLdoWordCount, &c81);

    expect (c81.words == 13 && c81.named == 13, "every offset read is one the vendor names");
    expect (c81.dead == 0, "no word read 0xffffffff, so nothing here is a non-reading");
    expect (c81.comparable == 8, "eight table entries are reachable inside the DT extent");
    expect (c81.agree == 1, "LDO_LD_DATA_REG expects 0 and reads 0, so one entry matches");
    expect (c81.differ == 7, "seven comparable words differ from the vendor's word");
    expect (c81.nonzero_expected == 7 && c81.nonzero_read_zero == 7,
            "all seven non-zero expectations read zero -- the list never ran");
    expect (c81.no_expectation == 5, "five words in the window have no table entry at all");
    expect (c81.table_outside == 1, "the table's ninth entry (ADC_CTL 0x34) is outside the DT extent");
    expect (c81.zero == 11, "eleven of thirteen words are zero, not thirteen");
    expect (gpu_ldo_expect_inside_window () == 8 && kGpuLdoExpectCount == 9,
            "the nine-entry table has eight entries inside the window");
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (c81, true)), "list-never-written") == 0,
            "census with cx_on control is the 'list-never-written' shape");

    // -----------------------------------------------------------------------
    // 2. The control is load-bearing: the identical words, without it
    // -----------------------------------------------------------------------
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (c81, false)), "list-unwritten-blind") == 0,
            "without cx_on the same census may only say the list is unwritten, not who failed to write it");

    // -----------------------------------------------------------------------
    // 3. A page that does not answer is not a disagreement
    // -----------------------------------------------------------------------
    uint32_t dead[kGpuLdoWordCount];
    for (uint32_t i = 0; i < kGpuLdoWordCount; i++) {
        dead[i] = kGpuNoAnswer;
    }
    GpuLdoCensus cdead;
    gpu_ldo_census (kOffsets, dead, kGpuLdoWordCount, &cdead);
    expect (cdead.dead == 13 && cdead.comparable == 0 && cdead.differ == 0 && cdead.agree == 0,
            "a census of all-ones must produce no comparisons at all");
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (cdead, true)), "dead-page") == 0,
            "and the shape has to say dead-page");

    // -----------------------------------------------------------------------
    // 4. The mapping guard: one word read from an offset the vendor does not name
    // -----------------------------------------------------------------------
    uint32_t shifted[kGpuLdoWordCount];
    uint32_t shifted_values[kGpuLdoWordCount];
    memcpy (shifted, kOffsets, sizeof (shifted));
    memcpy (shifted_values, kCensus, sizeof (shifted_values));
    shifted[9] = 0x34U;               // one word walked past the census, onto ADC_CTL's offset
    GpuLdoCensus cshift;
    gpu_ldo_census (shifted, shifted_values, kGpuLdoWordCount, &cshift);
    expect (cshift.named == 12 && cshift.words == 13,
            "a word read from an offset the name table does not cover drops named below words");
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (cshift, true)), "unnamed-mapping") == 0,
            "and the verdict refuses to describe a block it is no longer reading");

    shifted[9] = 0x28U;
    GpuLdoCensus cswap;
    gpu_ldo_census (shifted, shifted_values, kGpuLdoWordCount, &cswap);
    expect (cswap.named == 13 && cswap.agree == 1 && cswap.comparable == 8,
            "a named offset replacing a named offset keeps named == 13");

    // -----------------------------------------------------------------------
    // 5. PWRSWITCH_CTL: named bits, and the residue that has no name
    // -----------------------------------------------------------------------
    GpuLdoCtrl ctrl;
    gpu_ldo_decode_ctrl (kCensus[7], &ctrl);
    expect (ctrl.answered && ctrl.clamp_io && ctrl.pdn && ctrl.under_sw_ctrl && !ctrl.bypass,
            "0x80000038 decodes as CLAMP_IO | PDN | UNDER_SW_CTRL with bypass clear");
    expect (ctrl.unnamed == 0x00000008U, "BIT(3) has no name in msm_gfx_ldo.c");

    GpuLdoCtrl ctrl_dead;
    gpu_ldo_decode_ctrl (kGpuNoAnswer, &ctrl_dead);
    expect (!ctrl_dead.answered && !ctrl_dead.pdn,
            "0xffffffff must not be reported as a powered-down rail");

    uint32_t planned = 0;
    expect (gpu_ldo_ctrl_probe_word (kCensus[7], &planned) && planned == 0x20000038U,
            "clearing CLAMP_IO and setting EN_LDOAP_CTRL_CPR gives 0x20000038");
    uint32_t planned_dead = 0xdeadbeefU;
    expect (!gpu_ldo_ctrl_probe_word (kGpuNoAnswer, &planned_dead) && planned_dead == 0xdeadbeefU,
            "refuses an unanswering page");

    // -----------------------------------------------------------------------
    // 6. The corner a reset value describes
    // -----------------------------------------------------------------------
    GpuLdoVref top;
    gpu_ldo_decode_vref (0x0000007fU, &top);
    expect (top.val == 127U && top.uv == 1010000U,
            "seven-bit mask allows val 127 = 1,010,000 uV");
    expect (!top.in_driver_range && !top.in_dt_range,
            "above MAX_LDO_VOLTAGE (980,000) and above 925,000");

    GpuLdoVref reset_value;
    gpu_ldo_decode_vref (0x00000000U, &reset_value);
    expect (reset_value.uv == kLdoMinUv && reset_value.in_driver_range && !reset_value.in_dt_range,
            "vref=0 decodes to 375,000 uV");

    // -----------------------------------------------------------------------
    // 7. The settling predicate has two inputs, not one
    // -----------------------------------------------------------------------
    expect (!gpu_ldo_vendor_settled (0x00000000U) && gpu_ldo_vendor_settled (0x00000004U) &&
            !gpu_ldo_status_settled (0x00000004U),
            "waits on READY or SETTLED");
    expect (!gpu_ldo_status_ready (kGpuNoAnswer), "neither bit claimed from a dead page");

    // -----------------------------------------------------------------------
    // 8. GDSC: write sequence and PWR_ON logic
    // -----------------------------------------------------------------------
    const uint32_t as_found = kGdscSwCollapse | kGdscHwControl;
    const uint32_t after_init = gpu_gdsc_init_word (as_found);
    const uint32_t after_release = gpu_gdsc_release_word (after_init);
    expect (after_init == (kGdscWaitVal | kGdscSwCollapse),
            "gdsc_init clears HW_CONTROL and sets wait timers, leaves SW_COLLAPSE alone");
    expect (after_release == kGdscWaitVal,
            "gdsc_toggle_logic clears SW_COLLAPSE");
    expect (!gpu_gdsc_pwr_on (after_release) && gpu_gdsc_collapsed (after_init),
            "word sequence written never sets PWR_ON itself");

    expect (gpu_gdsc_pwr_on (kGpuNoAnswer) && !gpu_gdsc_powered (kGpuNoAnswer),
            "all-ones page has PWR_ON and SW_COLLAPSE set, not powered");
    expect (gpu_gdsc_pwr_on (0x80000001U) && !gpu_gdsc_powered (0x80000001U),
            "PWR_ON with collapse still set is not powered");

    const uint32_t cx_sample = 0xa0282000U;
    const uint32_t cxhw_sample = 0x80001fe0U;
    const uint32_t gx_sample = 0x08282000U;
    expect ((cx_sample & kGdscWaitMask) == kGdscWaitVal &&
            (gx_sample & kGdscWaitMask) == kGdscWaitVal,
            "reports 0x00282000 in both GDSCRs' wait fields");
    expect (!gpu_gdsc_collapsed (cx_sample) && !gpu_gdsc_collapsed (gx_sample),
            "SW_COLLAPSE is clear in both");
    expect (gpu_gdsc_pwr_on (cxhw_sample) && gpu_gdsc_powered (cx_sample) &&
            !gpu_gdsc_pwr_on (gx_sample) && !gpu_gdsc_powered (gx_sample),
            "CX is up and GX is not");

    // -----------------------------------------------------------------------
    // 9. The AHB gate
    // -----------------------------------------------------------------------
    expect (gpu_ahb_gate_open (0x00000001U), "a voted, answering AHB branch opens the gate");
    expect (!gpu_ahb_gate_open (0x00000000U), "an unvoted one does not");
    expect (!gpu_ahb_gate_open (kGpuNoAnswer), "0xffffffff does not open the gate");

    // -----------------------------------------------------------------------
    // 10. Branch status
    // -----------------------------------------------------------------------
    expect (gpu_branch_requested (0x80000001U) && gpu_branch_halted (0x80000001U) &&
            !gpu_branch_running (0x80000001U),
            "branch requested but halted is not running");
    expect (gpu_branch_running (0x00000001U) && !gpu_branch_running (0x80000000U),
            "branch running only when requested and un-halted");

    // -----------------------------------------------------------------------
    // 11. Chip ID gate determination
    // -----------------------------------------------------------------------
    expect (gpu_chipid_gate (true, false, true) == GPU_CHIPID_NO_GX &&
            strcmp (gpu_chipid_gate_name (gpu_chipid_gate (true, false, true)), "no-gx") == 0,
            "cx up, gx not -> names no-gx");
    expect (gpu_chipid_gate (false, false, false) == GPU_CHIPID_NO_CX,
            "all gates shut -> names no-cx");
    expect (gpu_chipid_gate (true, true, false) == GPU_CHIPID_NO_GFX3D &&
            gpu_chipid_gate (true, true, true) == GPU_CHIPID_READS,
            "all three open -> reads chip ID");

    // -----------------------------------------------------------------------
    // 12. MEM_ACC decoding
    // -----------------------------------------------------------------------
    GpuMemAcc mem_dead;
    gpu_mem_acc_decode (kGpuNoAnswer, &mem_dead);
    expect (!mem_dead.answered && mem_dead.corner == 0U && mem_dead.field == 0U,
            "refused page reports no corner at all");
    expect (gpu_mem_acc_decode_before_mask (kGpuNoAnswer) == kMemAccMinCorner,
            "masking first would have called it corner 1");
    GpuMemAcc mem_live;
    gpu_mem_acc_decode (0x00000000U, &mem_live);
    expect (mem_live.answered && mem_live.corner == kMemAccMaxCorner && mem_live.corner_named,
            "live zero is corner 2");

    // -----------------------------------------------------------------------
    // 13. Empty census
    // -----------------------------------------------------------------------
    GpuLdoCensus none;
    gpu_ldo_census (kOffsets, kCensus, 0, &none);
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (none, true)), "no-words") == 0,
            "zero words read has its own shape");

    // -----------------------------------------------------------------------
    // 14. board Adreno 509 hardware identity and register constants
    // -----------------------------------------------------------------------
    expect (kGpuChipidValue == 0x05000900U, "board GPU chipid matches Adreno 509 (0x05000900)");
    expect (kGpuPhys == 0x05000000ULL, "GPU core physical base is 0x05000000");
    expect (kGpuSize == 0x40000U, "GPU core window size is 256 KiB");
    expect (kGpuChipid == 0x10200U, "Chip ID register offset is +0x10200");
    expect (kGpuccPhys == 0x05065000ULL, "GPUCC physical base is 0x05065000");
    expect (kGpuccSize == 0x10000U, "GPUCC window size is 64 KiB");
    expect (kGccPhys == 0x00100000ULL, "GCC physical base is 0x00100000");
    expect (kMemAccPhys == 0x01fcf004ULL, "MEM_ACC physical base is 0x01fcf004");

    if (checks != 0) {
        printf ("FAIL: %d board GPU assertions did not hold\n", checks);
        return 1;
    }
    printf ("PASS: board GPU core decoders & Adreno 509 identity (0x05000900) verified\n");
    return 0;
}

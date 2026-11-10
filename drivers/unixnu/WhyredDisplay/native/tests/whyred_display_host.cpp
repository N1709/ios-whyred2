// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host model for drivers/WhyredDisplay: GOP presenter wire ABI,
// continuous splash memory layout, 1080x2160 test-pattern generation,
// and NativeLegacyDisplay state verification.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../NativeGOPWire.h"
#include "../NativeLegacyRequest.h"

/* ------------------------------------------- 1. Hardware Geometry Constants */

#define WHYRED_SPLASH_PHYS   0x9d400000ULL
#define WHYRED_SPLASH_SIZE   0x02400000ULL  /* 36 MiB cont_splash_mem */
#define WHYRED_FB_PHYS       0x9d400000ULL
#define WHYRED_FB_WIDTH      1080u
#define WHYRED_FB_HEIGHT     2160u
#define WHYRED_FB_BPP        4u
#define WHYRED_FB_STRIDE     (WHYRED_FB_WIDTH * WHYRED_FB_BPP)  /* 4320 bytes */
#define WHYRED_FB_SIZE       (WHYRED_FB_STRIDE * WHYRED_FB_HEIGHT) /* 9331200 bytes */

static void test_framebuffer_geometry(void)
{
  /* The framebuffer must fit comfortably inside the 36 MiB splash reservation */
  assert(WHYRED_FB_PHYS >= WHYRED_SPLASH_PHYS);
  assert((WHYRED_FB_PHYS + WHYRED_FB_SIZE) <= (WHYRED_SPLASH_PHYS + WHYRED_SPLASH_SIZE));

  assert(WHYRED_FB_STRIDE == 4320u);
  assert(WHYRED_FB_SIZE == 9331200u);
  assert(WHYRED_FB_SIZE < WHYRED_SPLASH_SIZE);

  /* Splash size is exactly 36 MiB */
  assert(WHYRED_SPLASH_SIZE == (36ULL * 1024 * 1024));
}

/* --------------------------------- 2. GOP Wire Protocol and ABI Verification */

static void test_gop_wire_abi(void)
{
  assert(sizeof(NGPWait) == 8);
  assert(sizeof(NGPSwap) == 48);
  assert(sizeof(NGPComplete) == 16);
  assert(sizeof(NGPMode) == 40);
  assert(sizeof(NGPPresent) == 32);
  assert(sizeof(NGPTransfer) == 16);

  NGPMode mode = {
    .version = NGP_VERSION,
    .format = NGP_BGRX8888,
    .width = WHYRED_FB_WIDTH,
    .height = WHYRED_FB_HEIGHT,
    .stride = WHYRED_FB_STRIDE,
    .bytes = WHYRED_FB_SIZE,
  };

  assert(mode.version == 1);
  assert(mode.format == NGP_BGRX8888);
  assert(mode.width == 1080);
  assert(mode.height == 2160);
  assert(mode.stride == 4320);
  assert(mode.bytes == 9331200);

  NGPPresent present = {
    .address = WHYRED_FB_PHYS,
    .bytes = WHYRED_FB_SIZE,
    .width = WHYRED_FB_WIDTH,
    .height = WHYRED_FB_HEIGHT,
    .stride = WHYRED_FB_STRIDE,
    .format = NGP_BGRX8888,
  };

  assert(present.address == 0x9d400000ULL);
  assert(present.bytes == 9331200ULL);
}

/* ---------------- 3. Test Pattern Generation (from ios-whyred2 0176/0177) */

static void test_pattern_generator(void)
{
  /* 4 color bars from whyred patch 0176:
   * Red, Green, Blue, Light Gray */
  static const uint32_t bars[4] = {
    0xffe02020, 0xff20e020, 0xff2020e0, 0xffe8e8e8
  };

  /* Simulate test pattern rendering on a scaled slice */
  const uint32_t test_w = 64;
  const uint32_t test_h = 16;
  const uint32_t bar_w = test_w / 4;
  uint32_t frame[test_w * test_h];
  memset(frame, 0, sizeof(frame));

  for (uint32_t y = 0; y < test_h; y++) {
    uint32_t *line = frame + y * test_w;
    for (uint32_t x = 0; x < test_w; x++) {
      line[x] = bars[x / bar_w];
    }
  }

  /* Verify pixels in each quadrant */
  for (uint32_t y = 0; y < test_h; y++) {
    uint32_t *line = frame + y * test_w;
    assert(line[0] == bars[0]);
    assert(line[bar_w - 1] == bars[0]);
    assert(line[bar_w] == bars[1]);
    assert(line[2 * bar_w - 1] == bars[1]);
    assert(line[2 * bar_w] == bars[2]);
    assert(line[3 * bar_w - 1] == bars[2]);
    assert(line[3 * bar_w] == bars[3]);
    assert(line[test_w - 1] == bars[3]);
  }
}

/* ----------------------- 4. NativeLegacyDisplay Request State Verification */

static void test_legacy_request_states(void)
{
  /* Single layer states */
  assert(native_legacy_single_layer_state(1, 0, 0));
  assert(native_legacy_single_layer_state(0x80000007U, 0x80000006U, 0xff000000U));

  for (uint32_t updated = 0; updated < 16; updated++) {
    for (uint32_t disabled = 0; disabled < 16; disabled++) {
      int expected = !(disabled & ~updated) && (updated & ~disabled) == 1;
      assert(native_legacy_single_layer_state(updated, disabled, 0) == expected);
      assert(native_legacy_single_layer_state(updated | 0x80000000U,
             disabled | 0x80000000U, 0xff000000U) == expected);
    }
  }

  assert(!native_legacy_single_layer_state(0x80000001U, 0, 0xff000000U));
  assert(!native_legacy_single_layer_state(0x80000001U, 0x80000000U, 0xffff0000U));
  assert(!native_legacy_single_layer_state(1, 0, 0xff000000U));
  assert(!native_legacy_single_layer_state(0x101, 0x100, 0));
  assert(!native_legacy_single_layer_state(0x80000001U, 0x80000000U, 0));

  /* Blank states */
  assert(native_legacy_blank_state(0x80000007U, 0x80000007U, 0xff000000U));
  assert(native_legacy_blank_state(0x80000000U, 0x80000000U, 0xff000000U));
  assert(native_legacy_blank_state(3, 3, 0));
  assert(!native_legacy_blank_state(0, 0, 0));
  assert(!native_legacy_blank_state(0x80000007U, 0x80000006U, 0xff000000U));
  assert(!native_legacy_blank_state(0x80000007U, 0x80000007U, 0));

  /* Pixel fill helper */
  uint32_t pixels[24];
  memset(pixels, 0xcc, sizeof(pixels));
  assert(native_legacy_fill_pixels(pixels, 64, 16, 3, 4, 0xff000000U));
  for (unsigned row = 0; row < 4; ++row) {
    for (unsigned col = 0; col < 3; ++col) {
      assert(pixels[row * 4 + col] == 0xff000000U);
    }
    assert(pixels[row * 4 + 3] == 0xcccccccc);
  }
  for (unsigned i = 16; i < 24; ++i) {
    assert(pixels[i] == 0xcccccccc);
  }

  /* Frame scan helper */
  unsigned char image[4 * 4 * 4];
  memset(image, 0, sizeof(image));
  uint64_t scan[5];
  assert(native_legacy_frame_scan(image, sizeof(image), 16, 4, 4, scan) &&
         !scan[0] && scan[1] == UINT64_MAX && scan[2] == UINT64_MAX && !scan[3] && !scan[4]);
}

/* ------------------------------------------------------------- 5. Main Runner */

int main(void)
{
  printf("whyred_display_host: running display assertions\n");

  test_framebuffer_geometry();
  printf("  [PASS] Framebuffer geometry & 36 MiB splash reservation\n");

  test_gop_wire_abi();
  printf("  [PASS] GOP wire ABI, modes & presentation structs\n");

  test_pattern_generator();
  printf("  [PASS] 4-bar test pattern generation & readback\n");

  test_legacy_request_states();
  printf("  [PASS] NativeLegacyDisplay request & layer states\n");

  printf("whyred_display_host: all assertions passed\n");
  return 0;
}

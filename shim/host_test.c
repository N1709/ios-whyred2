/* Render the console layout on the host: ./out/host_test && view out/host.ppm */
#include <stdio.h>
#include "console.h"

static uint8_t host_fb[FB_H * FB_STRIDE];

int main(void)
{
	con_fb = (volatile uint32_t *)host_fb;
	con_clear(RED);
	con_str("SHIM", "lavender bare-metal shim");
	con_hex("MMFR0", 0x0000000000101122ULL);
	con_hex2("PSCI", "ver=", 0x10001, "smccc=", 0x10002);
	con_clear(BLUE);
	con_str("PANIC", "sync exception");
	con_hex("ESR", 0x96000010);
	FILE *f = fopen("out/host.ppm", "wb");
	if (!f)
		return 1;
	fprintf(f, "P6 %d %d 255\n", FB_W, FB_H);
	for (int y = 0; y < FB_H; y++) {
		for (int x = 0; x < FB_W; x++) {
			uint32_t px = ((uint32_t *)host_fb)[y * (FB_STRIDE / 4) + x];
			uint8_t rgb[3] = { (uint8_t)((px >> 16) & 0xff), (uint8_t)((px >> 8) & 0xff), (uint8_t)(px & 0xff) };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	return 0;
}

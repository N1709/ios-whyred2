#pragma once
#include <stdint.h>

/* ARM generic architectural timer driver prototypes */
void timer_arm_ticks(uint32_t ticks);
void timer_arm_deadline(uint64_t deadline);
void timer_disarm(void);
uint64_t timer_get_freq(void);
uint64_t timer_get_ticks(void);

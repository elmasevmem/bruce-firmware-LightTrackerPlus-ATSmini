#pragma once
#if defined(ATS_MINI) && !defined(LITE_VERSION)
#include <Arduino.h>

// ATS Mini built-in SI4732 FM radio with RDS (opened from the FM menu)
void si4732_fm_setup();
void si4732_fm_loop();
#endif


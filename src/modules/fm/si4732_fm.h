#pragma once
#if defined(ATS_MINI) && !defined(LITE_VERSION)
#include <Arduino.h>

// ATS Mini dahili Si4732 ile FM radyo + RDS alıcı (FM menüsünden çağrılır)
void si4732_fm_setup();
void si4732_fm_loop();
#endif


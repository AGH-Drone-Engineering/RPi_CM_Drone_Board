#pragma once

// Wall-clock port for the duty-cycle tracker's persisted Toff deadline.
// Auto-selects by platform; inject a custom implementation via cfg.clock.
// isValid()==false: deadline falls back to tick counting, lost on reboot.

#if defined(__ZEPHYR__)
#  include "zephyr/ZephyrClock.h"
   using DefaultClock = ZephyrClock;

#elif defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#  include "esp/EspClock.h"
   using DefaultClock = EspClock;

#elif defined(ARDUINO)
#  include "null/NullClock.h"
   using DefaultClock = NullClock;

#else
#  include "posix/PosixClock.h"
   using DefaultClock = PosixClock;
#endif

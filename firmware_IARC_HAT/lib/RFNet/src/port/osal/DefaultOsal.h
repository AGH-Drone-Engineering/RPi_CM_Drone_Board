#pragma once

// OSAL — OS abstraction (queues, mutexes, timers, tasks), selected below by platform.
// Custom backend: pass to the RFNode(radio, osal, cfg) constructor overload; must satisfy
// the IOsal.h/IQueue.h/IMutex.h/ITimer.h contracts.

#if defined(__ZEPHYR__)
   // No ZephyrOsal shipped yet
   // RFNode's 2-arg constructor can't rescue this: RFNode unconditionally holds a
   // DefaultOsal member, so the error must be cleared (implement IOsal/IQueue/IMutex/ITimer
   // against Zephyr's k_queue/k_mutex/k_thread, add the include+alias below) before RFNet
   // builds on Zephyr at all, via either constructor.
#  error "RFNet: DefaultOsal has no Zephyr backend yet. Implement IOsal/IQueue/IMutex/ITimer \
for Zephyr (see osal_posix/PosixOsal.h or osal_freertos/FreeRtosOsal.h for the shape), put it \
at osal_zephyr/ZephyrOsal.h, and alias DefaultOsal = ZephyrOsal here."

#elif defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#  include "osal_freertos/FreeRtosOsal.h"
   using DefaultOsal = FreeRtosOsal;

#elif defined(ARDUINO)
#  include "osal_baremetal/BaremetalOsal.h"
   using DefaultOsal = BaremetalOsal;

#else
#  include "osal_posix/PosixOsal.h"
   using DefaultOsal = PosixOsal;
#endif

#pragma once

#include <cstdint>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)
// Sync All, when a step ran out of memory: restarts to clear the heap fragmentation, then
// reopens Sync All on its own with the services in `doneMask` (bit i = service i, see
// plannerSync::serviceAt()) already done, so only the rest run. Used at most once per run.
void silentRestartToSyncAll(uint32_t doneMask);

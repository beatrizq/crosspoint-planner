#pragma once

#include <cstdint>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session. The live frontlight
// state rides along in the same flag so the reboot is invisible: the light
// comes back exactly as it was, regardless of the Restore Light on Wake
// preference.

void silentRestart();            // home screen
void silentRestartToReader();    // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToSettings();  // settings screen

// Reboots immediately after an activity releases exclusive raw storage. The
// RTC target ensures setup() lands on Home instead of resuming a reader.
void restartToHomeAfterStorageHandoff();
// Sync All, when a step ran out of memory: restarts to clear the heap fragmentation, then
// reopens Sync All on its own with the services in `doneMask` (bit i = service i, see
// plannerSync::serviceAt()) already done, so only the rest run. Used at most once per run.
void silentRestartToSyncAll(uint32_t doneMask);

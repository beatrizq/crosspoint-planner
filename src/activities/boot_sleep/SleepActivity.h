#pragma once
#include <string>

#include "activities/Activity.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  // forceQuickResume mirrors main.cpp's enterDeepSleep() own parameter of the
  // same name (a genuine short press when Short Power Button Click is Quick
  // Resume) -- kept as its own bool, not folded into fromTimeout, since the
  // two are independent: this is never true when fromTimeout is.
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false,
                         bool forceQuickResume = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout), forceQuickResume(forceQuickResume) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog) const;
  bool renderTransparentOverlayPng(const std::string& path) const;
  bool renderSleepOverlayPath(const std::string& path) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen() const;
  void renderBlankSleepScreen() const;

  bool fromTimeout = false;
  bool forceQuickResume = false;
};

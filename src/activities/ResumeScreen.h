#pragma once

#include <cstdint>

// The top-level screens a Quick Resume wake can reopen. Stored as a number in CrossPointState, so
// the order is append-only.
enum class ResumeScreen : uint8_t {
  None = 0,
  Home,
  ReadMenu,
  Planner,
  Tasks,
  Calendar,
  Settings,
  FileBrowser,
  Library
};

#include "QuickPickActivity.h"

#include <CivilTime.h>
#include <GCalEventCache.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <TodoistStore.h>
#include <TodoistTaskCache.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "activities/home/MainMenuActivity.h"
#include "activities/planner/PlannerLabels.h"
#include "activities/planner/RescheduleTaskActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/OptionsMenuActivity.h"
#include "companion/CompanionRenderer.h"
#include "companion/CompanionTracker.h"
#include "components/StatusBand.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HomeAppOrder.h"
#include "util/PlannerActions.h"
#include "util/PlannerSync.h"

namespace {
// One integer step down from a prior 5 (a user-requested ~15% size
// reduction) -- the sprite renderer only supports whole-number scale steps
// (nearest-neighbor pixel blocks, no fractional scaling), so this is the
// closest available size.
constexpr int MAX_SCALE = 4;
constexpr int PAD = 14;
constexpr int TAIL_LENGTH = 16;
constexpr int BUBBLE_GAP = 4;
constexpr int MARGIN = 24;
// Between the sprite and the embedded Tasks section's own title below it.
constexpr int LABEL_GAP = 20;
// Floor on the bubble's text column, so a one-word task name still leaves
// room for the tail and rounded corners rather than shrinking to fit it
// exactly.
constexpr int MIN_BUBBLE_TEXT_WIDTH = 80;
// The companion figure + bubble's own share of the space below the header;
// the embedded Tasks section gets the rest.
constexpr int COMPANION_BUDGET_PERCENT = 55;

// Gap between the glance strip above the companion figure and the bubble
// below it (hovering the figure inverts the bubble itself -- see render()'s
// own comment -- rather than drawing a selection box around this gap).
constexpr int SELECTION_BOX_PADDING = 10;

// The Tasks section's own title row (the Todoist projects, and Logs) and the
// thick rule under it, which is the top line of the tab bar.
// LyraTheme::drawHeader() puts a title 3px into its rect and the 3px rule at the
// rect's bottom edge, so the row is the title font's line height plus the 3px
// above, a small gap, and the 3px rule. The tab bar starts directly under it,
// with no gap: a focused bar's grey background begins at its own top edge, and
// any space left here showed as a white stripe between the rule and the grey.
constexpr int SECTION_TITLE_PAD = 14;
// The selected project's pill, matching LyraTheme::drawTabBar()'s tab pill (its
// hPaddingInSelection and cornerRadius, which are file-local there): padding
// either side of the text, and the corner radius.
constexpr int PROJECT_PILL_PADDING = 8;
constexpr int PROJECT_PILL_RADIUS = 6;

// onEnter() lands focus on the title row's first entry, where Right1 means "go Home".
// A reflexive second press of the same physical button right after arriving -- e.g.
// pressing it again because the e-ink refresh did not yet visibly confirm the first
// press landed -- would otherwise bounce straight back out to the main screen unasked.
// Any Right1 release within this window of onEnter() is ignored rather than acted on;
// long enough to absorb a reflexive double-press, short enough that a deliberate press
// still feels immediate.
constexpr unsigned long RIGHT1_ENTRY_GRACE_MS = 500;

// Same dither-overlay technique as PlannerScreenActivity::dimText() (and
// BleNotificationsActivity's own local copy): this e-ink panel has no real
// greyscale, so "dimmed" text is solid text with a checkerboard of pixels
// punched back out over it. Call right after drawText() at the same
// position -- used for the Calendar glance line's own trailing date/time,
// which reads as secondary detail next to the event's own summary.
void dimText(const GfxRenderer& renderer, const int x, const int y, const int fontId, const char* text) {
  if (text == nullptr || text[0] == '\0') return;
  const int width = renderer.getTextWidth(fontId, text);
  const int height = renderer.getLineHeight(fontId);
  for (int py = y; py < y + height; py++) {
    for (int px = x; px < x + width; px++) {
      if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }
  }
}

// Same technique, for a row that can be selected (inverted) -- the embedded
// Tasks section's own rows, unlike the glance strip's date/time above, which
// never has a selection to worry about. No-op when !ink: punching white
// pixels out of already-white-on-black text would just make holes in it, not
// dim it (same guard PlannerScreenActivity::dimText() uses).
void dimText(const GfxRenderer& renderer, const int x, const int y, const int fontId, const char* text,
             const bool ink) {
  if (!ink) return;
  dimText(renderer, x, y, fontId, text);
}

// The Tasks section's title row: every entry on one line, the selected one in
// bold and the rest regular (while the row has focus, the selected one is
// regular too, inside its pill, like a selected tab). Each entry sits in a slot of its text plus
// PROJECT_PILL_PADDING either side, with `gap` between slots -- the same layout
// the tab bar uses, so the selected entry's pill (drawn only while the row has
// focus, the same rounded shape LyraTheme::drawTabBar() gives its selected tab,
// solid black with white text) fits its slot and the text never moves between the
// focused and unfocused states. When the slots do not all fit, the row scrolls
// just far enough to keep the selected one fully on screen (the ones that still
// do not fit after it are simply not drawn), and a selected entry wider than the
// whole row is truncated.
void drawProjectTitleRow(const GfxRenderer& renderer, const int textY, const int pillY, const int pillHeight,
                         const int width, const int sidePadding, const int gap, const std::vector<const char*>& names,
                         const size_t selected, const bool focused) {
  // The row starts at the content edge, the same place the tab bar's first tab pill
  // does, so the first entry's pill (when shown) sits exactly over the tab pill's spot
  // and its *text* -- PROJECT_PILL_PADDING in -- lines up with the first tab's text and
  // the task rows' text below (see renderTasksTab()). It ends the same distance in from
  // the right edge.
  const int left = sidePadding;
  const int available = width - left * 2;
  const auto slotWidthOf = [&](const size_t i) {
    return renderer.getTextWidth(UI_12_FONT_ID, names[i],
                                 i == selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR) +
           2 * PROJECT_PILL_PADDING;
  };

  size_t first = 0;
  while (first < selected) {
    int span = 0;
    for (size_t i = first; i <= selected; i++) span += slotWidthOf(i) + (i > first ? gap : 0);
    if (span <= available) break;
    first++;
  }

  int x = left;
  for (size_t i = first; i < names.size(); i++) {
    const bool isSelected = i == selected;
    const bool pill = isSelected && focused;
    // Bold marks the selection while the row is unfocused; inside the pill the
    // text is regular weight, like a selected tab's. The slot is sized for the
    // bold text either way, so the regular text is centred in it.
    const auto style = (isSelected && !focused) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int slotWidth = slotWidthOf(i);
    if (x + slotWidth > left + available) {
      if (i == first) {
        // Too wide for the whole row: fill what there is, text truncated to fit.
        const int room = available - 2 * PROJECT_PILL_PADDING;
        const std::string shown = renderer.truncatedText(UI_12_FONT_ID, names[i], room, style);
        if (pill) renderer.fillRoundedRect(x, pillY, available, pillHeight, PROJECT_PILL_RADIUS, Color::Black);
        renderer.drawText(UI_12_FONT_ID, x + PROJECT_PILL_PADDING, textY, shown.c_str(), !pill, style);
      }
      break;
    }
    if (pill) renderer.fillRoundedRect(x, pillY, slotWidth, pillHeight, PROJECT_PILL_RADIUS, Color::Black);
    const int drawnWidth = renderer.getTextWidth(UI_12_FONT_ID, names[i], style);
    renderer.drawText(UI_12_FONT_ID, x + (slotWidth - drawnWidth) / 2, textY, names[i], !pill, style);
    x += slotWidth + gap;
  }
}

// How much of the header's own vertical space this screen's own layout
// treats as occupied, now that the header shows only a theme's
// clock/date/battery combo (see render()'s own comment) rather than the
// full title+subtitle metrics.headerHeight was sized for. Sized for
// LyraTheme's own clock row -- a headerClockIconSize (24px, LyraTheme.cpp)
// icon starting 5px into the header, plus a little breathing room -- now
// that that row matches the glance strip's own icon size instead of the
// smaller text-only reading it used to be. Moot on RoundedRaffTheme, which
// draws nothing into a null-title header at all (see render()'s own
// comment).
constexpr int HEADER_CONTENT_HEIGHT = 36;
// The header-focus highlight (see render()'s own comment) is drawn a little
// shorter than HEADER_CONTENT_HEIGHT, not the full amount: HEADER_CONTENT_
// HEIGHT already includes some breathing room below the actual clock/
// battery content, and using every pixel of it for the highlight too left
// its own bottom edge touching the glance strip's own icon/text right below.
constexpr int HEADER_HIGHLIGHT_HEIGHT = HEADER_CONTENT_HEIGHT - 6;

// Row separator between the embedded Tasks section's own rows.
constexpr int SEPARATOR_HEIGHT = 2;

// Same font selection as PlannerScreenActivity's own titleFontId()/
// subtitleFontId() (Tasks/Calendar) -- not inherited (this
// class doesn't derive from that base), but kept in lockstep so the embedded
// section reads exactly like the real Tasks screen it's a scaled-down
// rendering of.
int taskRowTitleFontId() {
  return SETTINGS.plannerFontSize == CrossPointSettings::PLANNER_FONT_SMALL ? UI_10_FONT_ID : UI_12_FONT_ID;
}

int taskRowSubtitleFontId() {
  return SETTINGS.plannerFontSize == CrossPointSettings::PLANNER_FONT_SMALL ? SMALL_FONT_ID : UI_10_FONT_ID;
}
}  // namespace

void QuickPickActivity::onEnter() {
  Activity::onEnter();
  // Home from here (and from the Calendar and Alerts screens opened from here) lands
  // back on the Planner tile.
  MainMenuActivity::setReturnArea(HomeArea::Planner);
  // Re-reads the glance strip's own Calendar source from disk, the same
  // "hydrate on entry" PlannerScreenActivity::onEnter() -> loadCaches()
  // already does for Calendar (CalendarActivity's own loadCaches() override)
  // -- QuickPickActivity extends Activity directly,
  // so it never gets that hook. Without this, Sync All's own reboot (see
  // SyncAllActivity::onExit(), which fires whenever WiFi was activated, i.e.
  // on every real sync) starts these fresh from their constructors, and this
  // screen would otherwise read them exactly as they were before that sync
  // (hasSynced() == false, if this is a first sync) until something else --
  // Calendar's own onEnter() -- happened to load it first. A no-op
  // (returns false, changes nothing) when the file doesn't exist yet.
  GCAL_EVENTS.loadFromFile();
  // One I2C read to resolve the calendar day, so currentMood() is cheap from
  // the render path -- same reasoning as HomeActivity::onEnter()'s own call.
  // Needed here specifically because this screen can be reached directly
  // from sleep (CrossPointState reconstruction) without ever passing through
  // Home first.
  COMPANION.refreshForDisplay();
  lastCompanionRefreshMs = millis();
  lastCompanionMood = COMPANION.currentMood();
  idleVariant = static_cast<uint8_t>(esp_random() % companion::IDLE_BUBBLE_VARIANT_COUNT);
  taskSelectedRow = 0;
  // Always lands on the first project, on its first tab -- selectProject()
  // sets activeKind to OVERDUE, first in tab order, so wanting it resolves to
  // index 0 whether or not that tab currently exists (see
  // TaskTabModel::rebuildVisibleTabs()).
  projectEntries.clear();
  rebuildProjectEntries();
  selectProject(0);
  // Lands focused on the title row, on its first entry (selectProject(0) above) --
  // always there, whatever the section below it holds. The first entry is always a
  // project (or the generic Tasks group), never Logs, which is last.
  focus = Focus::Projects;
  band.setIncludeSyncHint(true);
  band.poll();
  enteredAtMs = millis();
  requestUpdate(true);
}

void QuickPickActivity::rebuildEmbeddedTabs() {
  if (onLogsEntry()) {
    // Logs is a list with no tabs of its own (the tab bar is hidden for it).
    visibleTabs.assign(1, taskTabModel::TaskTabKind::LOGS);
    activeKind = taskTabModel::TaskTabKind::LOGS;
  } else {
    if (activeKind == taskTabModel::TaskTabKind::LOGS) activeKind = taskTabModel::TaskTabKind::OVERDUE;
    const int restored = taskTabModel::rebuildVisibleTabs(activeKind, visibleTabs, currentScope(),
                                                          /*includeLogs=*/false);
    // A project with nothing under any tab (every task completed) still shows
    // one, empty, rather than a tab bar with no tabs.
    if (visibleTabs.empty()) visibleTabs.push_back(taskTabModel::TaskTabKind::TODAY);
    activeKind = visibleTabs[static_cast<size_t>(restored) < visibleTabs.size() ? static_cast<size_t>(restored) : 0];
  }

  // The new tab's list can be shorter than the old one (or empty), so the
  // row cursor has to be pulled back inside it -- same reasoning
  // TasksActivity::rebuildTabs() clamps selectedIndex for.
  const int rows = taskTabModel::countFor(activeKind, currentScope());
  if (taskSelectedRow >= rows) taskSelectedRow = std::max(0, rows - 1);
  if (taskSelectedRow < 0) taskSelectedRow = 0;
}

void QuickPickActivity::afterRowAction() {
  // A completion/reschedule can change which tabs exist (a task moving out
  // of Overdue, say, or the last Overdue task disappearing entirely) --
  // same reasoning TasksActivity's own performTaskCompletion() rebuilds for.
  // Completing a project's last task also takes its entry off the title row.
  rebuildProjectEntries();
  rebuildEmbeddedTabs();
  if (onLogsEntry()) tabBarFocused = false;
  requestUpdate(true);
}

void QuickPickActivity::showTaskRowOptions(const size_t cacheIndex) {
  const auto& tasks = TODOIST_TASKS.getTasks();
  if (cacheIndex >= tasks.size()) return;
  // Captured by id, not carried as cacheIndex: the popup below sits on top
  // for as long as the user takes to answer, and every step after this one
  // re-resolves the id to a (possibly different) cache index rather than
  // trusting a cacheIndex that could have gone stale in the meantime.
  const std::string taskId = tasks[cacheIndex].id;
  // Todoist has no way to reschedule a single occurrence of a recurring task
  // without replacing its recurrence entirely -- simplest is to just not
  // offer Reschedule for one at all.
  const bool canReschedule = !tasks[cacheIndex].isRecurring;

  std::vector<std::string> options;
  options.push_back(tr(STR_COMPLETE_TASK));
  options.push_back(tr(STR_FOCUS_SESSION));
  if (canReschedule) options.push_back(tr(STR_RESCHEDULE_TASK));
  const int rescheduleIdx = canReschedule ? 2 : -1;

  startActivityForResult(
      std::make_unique<OptionsMenuActivity>(renderer, mappedInput, StrId::STR_OPTIONS, std::move(options)),
      [this, taskId, rescheduleIdx](const ActivityResult& result) {
        if (mappedInput.isPressed(MappedInputManager::Button::Right2)) {
          swallowConfirmRelease = true;
        }
        if (result.isCancelled || mappedInput.isPressed(MappedInputManager::Button::Right1)) {
          swallowBackRelease = true;
        }
        if (result.isCancelled) return;
        const int idx = std::get<OptionPickResult>(result.data).index;
        if (idx == 0) {
          completeTaskRow(taskId);
        } else if (idx == 1) {
          offerFocusSessionForTask(taskId);
        } else if (idx == rescheduleIdx) {
          offerRescheduleRow(taskId);
        }
      });
}

void QuickPickActivity::completeTaskRow(const std::string& taskId) {
  const auto& tasks = TODOIST_TASKS.getTasks();
  size_t cacheIndex = tasks.size();
  for (size_t i = 0; i < tasks.size(); i++) {
    if (tasks[i].id == taskId) {
      cacheIndex = i;
      break;
    }
  }
  if (cacheIndex >= tasks.size()) return;  // gone already

  // Asked rather than done: completing pushes to Todoist and cannot be undone
  // from the device -- same prompt TasksActivity itself shows.
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_TODOIST_COMPLETE_PROMPT),
                                                                tasks[cacheIndex].content),
                         [this, taskId](const ActivityResult& result) {
                           if (mappedInput.isPressed(MappedInputManager::Button::Right2)) {
                             swallowConfirmRelease = true;
                           }
                           if (result.isCancelled || mappedInput.isPressed(MappedInputManager::Button::Right1)) {
                             swallowBackRelease = true;
                           }
                           if (result.isCancelled) return;

                           // Re-resolve: the popup sat on top for as long as the user took to answer.
                           const auto& tasks2 = TODOIST_TASKS.getTasks();
                           size_t idx = tasks2.size();
                           for (size_t i = 0; i < tasks2.size(); i++) {
                             if (tasks2[i].id == taskId) {
                               idx = i;
                               break;
                             }
                           }
                           if (idx < tasks2.size()) {
                             RenderLock lock(*this);
                             plannerActions::completeTask(idx);
                           }
                           afterRowAction();
                         });
}

void QuickPickActivity::offerRescheduleRow(const std::string& taskId) {
  std::vector<std::string> options;
  options.push_back(tr(STR_PICK_DATE));
  options.push_back(tr(STR_TASKS_TAB_NO_DATE));

  startActivityForResult(
      std::make_unique<OptionsMenuActivity>(renderer, mappedInput, StrId::STR_RESCHEDULE_TASK, std::move(options)),
      [this, taskId](const ActivityResult& result) {
        if (mappedInput.isPressed(MappedInputManager::Button::Right2)) {
          swallowConfirmRelease = true;
        }
        if (result.isCancelled || mappedInput.isPressed(MappedInputManager::Button::Right1)) {
          swallowBackRelease = true;
        }
        if (result.isCancelled) return;
        const int idx = std::get<OptionPickResult>(result.data).index;
        if (idx == 0) {
          offerRescheduleDatePickerRow(taskId);
        } else if (idx == 1) {
          clearTaskDueDateRow(taskId);
        }
      });
}

void QuickPickActivity::offerRescheduleDatePickerRow(const std::string& taskId) {
  // Only ever reached for a non-recurring task -- showTaskRowOptions() leaves
  // Reschedule off the menu entirely for a recurring one.
  const auto& tasks = TODOIST_TASKS.getTasks();
  size_t cacheIndex = tasks.size();
  for (size_t i = 0; i < tasks.size(); i++) {
    if (tasks[i].id == taskId) {
      cacheIndex = i;
      break;
    }
  }
  if (cacheIndex >= tasks.size()) return;  // gone already
  const uint16_t seed = tasks[cacheIndex].dueDays != todoist::DUE_NONE
                            ? tasks[cacheIndex].dueDays
                            : todoist::dueDaysFromIso(TODOIST_TASKS.getSyncDate().c_str());

  startActivityForResult(std::make_unique<RescheduleTaskActivity>(renderer, mappedInput, seed),
                         [this, taskId](const ActivityResult& result) {
                           if (mappedInput.isPressed(MappedInputManager::Button::Right2)) {
                             swallowConfirmRelease = true;
                           }
                           if (result.isCancelled || mappedInput.isPressed(MappedInputManager::Button::Right1)) {
                             swallowBackRelease = true;
                           }
                           if (result.isCancelled) return;
                           const auto* date = std::get_if<DateResult>(&result.data);
                           if (!date) return;

                           // Re-resolve: the picker sat on top for as long as the user took to answer.
                           const auto& tasks2 = TODOIST_TASKS.getTasks();
                           size_t idx = tasks2.size();
                           for (size_t i = 0; i < tasks2.size(); i++) {
                             if (tasks2[i].id == taskId) {
                               idx = i;
                               break;
                             }
                           }
                           if (idx < tasks2.size()) {
                             RenderLock lock(*this);
                             plannerActions::rescheduleTask(idx, date->packedDate);
                           }
                           afterRowAction();
                         });
}

void QuickPickActivity::clearTaskDueDateRow(const std::string& taskId) {
  const auto& tasks = TODOIST_TASKS.getTasks();
  size_t cacheIndex = tasks.size();
  for (size_t i = 0; i < tasks.size(); i++) {
    if (tasks[i].id == taskId) {
      cacheIndex = i;
      break;
    }
  }
  if (cacheIndex >= tasks.size()) return;  // gone already

  {
    RenderLock lock(*this);
    plannerActions::rescheduleTask(cacheIndex, todoist::DUE_NONE);
  }
  afterRowAction();
}

void QuickPickActivity::offerFocusSessionForTask(const std::string& taskId) {
  const auto& tasks = TODOIST_TASKS.getTasks();
  size_t cacheIndex = tasks.size();
  for (size_t i = 0; i < tasks.size(); i++) {
    if (tasks[i].id == taskId) {
      cacheIndex = i;
      break;
    }
  }
  if (cacheIndex >= tasks.size()) return;  // gone already
  const std::string capturedText = tasks[cacheIndex].content;

  startActivityForResult(std::make_unique<OptionsMenuActivity>(renderer, mappedInput, StrId::STR_FOCUS_SESSION,
                                                               plannerActions::focusSessionDurationOptions()),
                         [this, capturedText, taskId](const ActivityResult& result) {
                           if (mappedInput.isPressed(MappedInputManager::Button::Right2)) {
                             swallowConfirmRelease = true;
                           }
                           if (result.isCancelled || mappedInput.isPressed(MappedInputManager::Button::Right1)) {
                             swallowBackRelease = true;
                           }
                           if (result.isCancelled) return;
                           const int idx = std::get<OptionPickResult>(result.data).index;
                           if (idx < 0 || idx >= plannerActions::FOCUS_SESSION_DURATIONS_COUNT) return;
                           plannerActions::beginFocusSession(capturedText, taskId,
                                                             plannerActions::FOCUS_SESSION_DURATIONS_MINUTES[idx],
                                                             renderer, mappedInput);
                         });
}

int QuickPickActivity::embeddedRowCount() const { return taskTabModel::countFor(activeKind, currentScope()); }

bool QuickPickActivity::embeddedOnLogs() const { return activeKind == taskTabModel::TaskTabKind::LOGS; }

uint32_t QuickPickActivity::currentScope() const {
  return projectIndex < projectEntries.size() ? projectEntries[projectIndex].scope : taskTabModel::ALL_PROJECTS;
}

bool QuickPickActivity::onLogsEntry() const {
  return projectIndex < projectEntries.size() && projectEntries[projectIndex].isLogs;
}

void QuickPickActivity::rebuildProjectEntries() {
  const bool hadSelection = projectIndex < projectEntries.size();
  const bool wasLogs = hadSelection && projectEntries[projectIndex].isLogs;
  const uint32_t wasScope = hadSelection ? projectEntries[projectIndex].scope : 0;

  projectEntries.clear();
  projectEntries.reserve(TODOIST_MAX_PROJECTS + 2);
  for (const auto& project : TODOIST_TASKS.getProjects()) {
    if (taskTabModel::countInProject(project.hash) == 0) continue;
    projectEntries.push_back(ProjectEntry{project.hash, project.name, false});
  }
  // Tasks whose project is unknown (a cache from before projects were tracked,
  // or a project the last sync could not name) still need a home, and with no
  // named project at all there must still be somewhere for the section below to
  // read from -- one generic entry, named like the app, covers both.
  if (taskTabModel::countInProject(taskTabModel::UNKNOWN_PROJECT) > 0 || projectEntries.empty()) {
    projectEntries.push_back(
        ProjectEntry{taskTabModel::UNKNOWN_PROJECT, homeAppOrder::displayName(homeAppOrder::AppId::Tasks), false});
  }
  projectEntries.push_back(ProjectEntry{0, tr(STR_LOGS), true});

  projectIndex = 0;
  if (hadSelection) {
    for (size_t i = 0; i < projectEntries.size(); i++) {
      if (projectEntries[i].isLogs == wasLogs && (wasLogs || projectEntries[i].scope == wasScope)) {
        projectIndex = i;
        break;
      }
    }
  }
}

void QuickPickActivity::selectProject(const size_t index) {
  if (index >= projectEntries.size()) return;
  projectIndex = index;
  // The new entry has its own rows and its own set of non-empty tabs, so the tab
  // set is rebuilt and lands on its first tab, the same as entering this screen
  // does -- not on whichever kind the previous entry happened to be showing.
  // OVERDUE is first in tab order, so wanting it resolves to index 0 whether or
  // not that tab currently exists (see TaskTabModel::rebuildVisibleTabs()). The
  // cursor lands on the tab bar rather than wherever the previous entry's cursor
  // happened to be -- always a valid, visible place to be (see tabBarFocused's
  // own comment) -- except on Logs, which has no tab bar.
  taskSelectedRow = 0;
  activeKind = taskTabModel::TaskTabKind::OVERDUE;
  rebuildEmbeddedTabs();
  tabBarFocused = !onLogsEntry();
}

void QuickPickActivity::stepProject(const int delta) {
  const int count = static_cast<int>(projectEntries.size());
  if (count < 2) return;
  selectProject(static_cast<size_t>(((static_cast<int>(projectIndex) + delta) % count + count) % count));
  if (focus != Focus::Projects) focusEmbeddedFirst();
  requestUpdate(true);
}

void QuickPickActivity::focusEmbeddedFirst() {
  if (onLogsEntry()) {
    // No tab bar on Logs: the first row, or -- with nothing logged yet -- the
    // title row, so the cursor is never on something that is not there.
    if (embeddedRowCount() > 0) {
      focus = Focus::Embedded;
      tabBarFocused = false;
      embeddedRow() = 0;
    } else {
      focus = Focus::Projects;
    }
    return;
  }
  focus = Focus::Embedded;
  tabBarFocused = true;
}

void QuickPickActivity::switchTab(const int delta) {
  const int tabCount = static_cast<int>(visibleTabs.size());
  if (tabCount <= 1) return;

  int index = 0;
  for (int i = 0; i < tabCount; i++) {
    if (visibleTabs[static_cast<size_t>(i)] == activeKind) {
      index = i;
      break;
    }
  }
  // Wraps both ways, same as every other tabbed screen in this app.
  index = ((index + delta) % tabCount + tabCount) % tabCount;
  activeKind = visibleTabs[static_cast<size_t>(index)];
  taskSelectedRow = 0;
  // A row cursor can't sit on a tab with no rows; fall back to the tab bar,
  // which is always a valid place to be (see tabBarFocused's own comment).
  if (taskTabModel::countFor(activeKind, currentScope()) == 0) tabBarFocused = true;
  requestUpdate();
}

void QuickPickActivity::clearSelectedLogTaskRow() {
  if (activeKind != taskTabModel::TaskTabKind::LOGS) return;
  const int entryIndex = taskTabModel::logEntryIndexForRow(taskSelectedRow);
  if (entryIndex < 0) return;
  const auto& entries = TODOIST_TASKS.getCompletedTodayEntries();
  if (static_cast<size_t>(entryIndex) >= entries.size() || !entries[static_cast<size_t>(entryIndex)].pending) {
    return;  // Synced -- nothing local left to undo.
  }

  {
    RenderLock lock(*this);
    TODOIST_TASKS.cancelCompletedLogEntry(static_cast<size_t>(entryIndex));
    TODOIST_TASKS.saveToFile();
  }
  afterRowAction();
}

// -- input --------------------------------------------------------------------

void QuickPickActivity::loop() {
  // A finished focus session's task: open its Select menu now that this screen is
  // up. The task may be gone by now (done elsewhere), in which case there is
  // nothing to offer and the screen is just shown.
  if (!pendingOptionsTaskId.empty()) {
    const std::string taskId = std::move(pendingOptionsTaskId);
    pendingOptionsTaskId.clear();
    const auto& tasks = TODOIST_TASKS.getTasks();
    for (size_t i = 0; i < tasks.size(); i++) {
      if (tasks[i].id == taskId) {
        showTaskRowOptions(i);
        return;
      }
    }
  }

  // An alert arriving (or being cleared from the Alerts screen) or a calendar event
  // ending changes what the band shows -- repaint, and keep focus on a stop that
  // still exists.
  if (band.poll()) requestUpdate();
  reconcileFocus();

  // Re-checks the companion's mood (in particular, whether it's now inside
  // its sleep window, or a day has rolled over) on the same idle timer
  // HomeActivity::loop() uses -- this screen can sit open just as long (see
  // lastCompanionRefreshMs's own comment in the header), so a repaint is
  // owed here too, not only on the next visit to Home.
  constexpr unsigned long COMPANION_REFRESH_INTERVAL_MS = 60000;
  if (millis() - lastCompanionRefreshMs >= COMPANION_REFRESH_INTERVAL_MS) {
    lastCompanionRefreshMs = millis();
    COMPANION.refreshForDisplay();
    const auto mood = COMPANION.currentMood();
    if (mood != lastCompanionMood) {
      lastCompanionMood = mood;
      // Fresh remark for the mood that's now showing -- see idleVariant's
      // own comment.
      idleVariant = static_cast<uint8_t>(esp_random() % companion::IDLE_BUBBLE_VARIANT_COUNT);
      if (companionVisible()) requestUpdate();
    }
  }

  // A press seen here is a fresh one, so nothing is owed any more.
  if (mappedInput.wasPressed(MappedInputManager::Button::Right1)) swallowBackRelease = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right2)) swallowConfirmRelease = false;

  // Side Up/Down (unlabelled, see this file's own header comment): which of the two Todoist
  // filters the section below the companion shows, from whichever focus stop
  // the cursor is on, instead of the app-jump shortcut every other screen's
  // side buttons have (see this file's own header comment). A fresh press each,
  // same guard reasoning as Right1/Right2 above.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) upPressSeen = true;
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) downPressSeen = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (upPressSeen) stepProject(-1);
    upPressSeen = false;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (downPressSeen) stepProject(1);
    downPressSeen = false;
    return;
  }

  // Right1 (the "Apps" button) is a per-focus shortcut to a related screen
  // now (see this file's own header comment) -- never "leave", since this
  // screen is Home and there is nowhere left to leave to.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right1)) {
    if (swallowBackRelease) {
      // The tail of the press that cancelled a popup pushed from this screen.
      // Acting on it would fire the shortcut below instead of just closing
      // the popup that press already closed.
      swallowBackRelease = false;
      return;
    }
    if (millis() - enteredAtMs < RIGHT1_ENTRY_GRACE_MS) {
      // See RIGHT1_ENTRY_GRACE_MS's own comment -- a reflexive press this
      // soon after landing on Home is not a deliberate shortcut request.
      return;
    }
    switch (focus) {
      case Focus::Header:
        // Back to the main screen: this screen is no longer Home (see
        // MainMenuActivity), and Settings is one press from there.
        activityManager.goHome();
        break;
      case Focus::Glance:
        activityManager.goToCalendar();
        break;
      case Focus::Alerts:
      case Focus::Companion:
#ifdef ENABLE_BLE_NOTIFY_SPIKE
        activityManager.goToBleNotifications();
#endif
        break;
      case Focus::Projects:
        // On the first entry there is nothing before it to step back to, so Right1
        // is "« Home" there (labelled so, see render()); from any other entry it
        // steps to the previous one.
        if (projectIndex == 0) {
          activityManager.goHome();
        } else {
          stepProject(-1);
        }
        break;
      case Focus::Embedded:
        if (!tabBarFocused) {
          // Back: from a row, up to the tab bar of the section it belongs to
          // (labelled Back, see render()) -- the same two-level Back the real
          // Tasks/Calendar/Settings screens have. Logs has no tab bar, so Back
          // goes to the title row there.
          if (onLogsEntry()) {
            focus = Focus::Projects;
          } else {
            tabBarFocused = true;
          }
          requestUpdate();
          break;
        }
        // On the tab bar: Random (see this file's own header comment) -- drops
        // onto a random row of the active tab, without leaving. Nothing to
        // randomize on a log view -- Clear is the only action there.
        if (!embeddedOnLogs()) {
          const int count = embeddedRowCount();
          if (count > 0) {
            embeddedRow() = static_cast<int>(esp_random() % static_cast<uint32_t>(count));
            tabBarFocused = false;
            requestUpdate();
          }
        }
        break;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left1)) {
    // Reverse of Left2 below -- see this file's own header comment for the
    // full Tasks/Header/Glance/Companion loop order.
    switch (focus) {
      case Focus::Projects:
        // Up from the title row: the companion figure, or the top block when
        // the companion is hidden.
        if (companionVisible()) {
          focus = Focus::Companion;
        } else {
          focusTopBlockLast();
        }
        break;
      case Focus::Companion:
        // Up into the band: its last stop (the alert if there is one, else the
        // calendar rows, else the status bar).
        focusTopBlockLast();
        break;
      case Focus::Header:
      case Focus::Glance:
      case Focus::Alerts: {
        // Up through the band's stops; off the top of the screen -- wrap to the
        // last row of the active tab, or its tab bar when that tab is empty (the
        // stops are always all reachable regardless of whether the active tab has
        // any rows in it).
        StatusBand::Stop stop = bandStopFor(focus);
        if (band.step(stop, /*forward=*/false)) {
          focus = focusForBandStop(stop);
        } else {
          focusEmbeddedLast();
        }
        break;
      }
      case Focus::Embedded:
        if (tabBarFocused) {
          // Off the top of the tab bar -- move onto the title row above it.
          focus = Focus::Projects;
        } else if (embeddedRow() == 0) {
          // Off the top of the row list -- move onto the tab bar, "the
          // start of the embedded section" (see this file's own header
          // comment), or onto the title row on Logs, which has no tab bar.
          if (onLogsEntry()) {
            focus = Focus::Projects;
          } else {
            tabBarFocused = true;
          }
        } else {
          const int count = embeddedRowCount();
          embeddedRow() = (embeddedRow() + count - 1) % count;
        }
        break;
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left2)) {
    // Forward direction of the loop: last Tasks-section row -> header ->
    // glance -> companion -> wraps to the Tasks section's own tab bar (see
    // this file's own header comment).
    switch (focus) {
      case Focus::Embedded: {
        const int count = embeddedRowCount();
        if (tabBarFocused) {
          if (count == 0) {
            // No rows to move onto -- the tab bar doubles as "the last row"
            // when the active tab is empty, so this continues straight to
            // the top block, same as the row branch below does.
            focusTopBlockFirst();
          } else {
            tabBarFocused = false;
            embeddedRow() = 0;
          }
        } else if (embeddedRow() == count - 1) {
          // Off the bottom of the list -- move onto the top block (alerts, or
          // the header) instead of wrapping straight to the companion figure.
          focusTopBlockFirst();
        } else {
          embeddedRow() = (embeddedRow() + 1) % count;
        }
        break;
      }
      case Focus::Header:
      case Focus::Glance:
      case Focus::Alerts: {
        // Down through the band's stops, then on to the companion.
        StatusBand::Stop stop = bandStopFor(focus);
        if (band.step(stop, /*forward=*/true)) {
          focus = focusForBandStop(stop);
        } else {
          focusAfterTopBlock();
        }
        break;
      }
      case Focus::Companion:
        // Onto the title row, the start of the Tasks section.
        focus = Focus::Projects;
        break;
      case Focus::Projects:
        // Down from the title row into the section: its tab bar, or its first
        // row on Logs -- and on past it when Logs has nothing logged.
        focusEmbeddedFirst();
        if (focus == Focus::Projects) focusTopBlockFirst();
        break;
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right2)) {
    if (swallowConfirmRelease) {
      swallowConfirmRelease = false;
      return;
    }
    switch (focus) {
      case Focus::Alerts:
        // Dismiss the alert on top (the newest pending one); the next-newest takes
        // its place, and with the last one gone focus moves up to what is left of
        // the band (reconcileFocus()).
        band.handleRight2(StatusBand::Stop::Alert);
        reconcileFocus();
        requestUpdate();
        break;
      case Focus::Companion:
        // The bubble only carries the companion's own remarks now -- nothing
        // to act on here.
        break;
      case Focus::Projects:
        stepProject(1);
        break;
      case Focus::Header:
        // No per-row or per-suggestion action to offer for the header itself
        // (see this file's own header comment) -- Sync All instead. Returns
        // here, via goToCompanion(), not to the main screen.
        activityManager.goToSyncAll([] { activityManager.goToCompanion(); });
        break;
      case Focus::Glance:
        // Nothing to select here -- Right1 (Calendar) is this stop's only
        // action.
        break;
      case Focus::Embedded:
        if (tabBarFocused) {
          // Cycles the currently-visible tabs without leaving the tab bar --
          // same convention every other tabbed screen in this app uses (see
          // this file's own header comment).
          switchTab(1);
          break;
        }
        if (activeKind == taskTabModel::TaskTabKind::LOGS) {
          clearSelectedLogTaskRow();
        } else {
          const int cacheIndex = taskTabModel::taskCacheIndexForRow(activeKind, taskSelectedRow, currentScope());
          if (cacheIndex >= 0) showTaskRowOptions(static_cast<size_t>(cacheIndex));
        }
        break;
    }
  }
}

// -- render ---------------------------------------------------------------

void QuickPickActivity::renderTasksTab(const int top, const int height) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  // The rows' text starts where the first tab's *text* does -- the tab pill's inset
  // (PROJECT_PILL_PADDING mirrors LyraTheme::drawTabBar()'s hPaddingInSelection) in
  // from the content edge -- not at the pill's own left edge.
  const int textX = metrics.contentSidePadding + PROJECT_PILL_PADDING;
  const int textWidth = pageWidth - textX - metrics.contentSidePadding;
  const bool rowsFocused = focus == Focus::Embedded && !tabBarFocused;

  const int totalRows = taskTabModel::countFor(activeKind, currentScope());
  if (totalRows == 0) {
    const char* empty = activeKind == taskTabModel::TaskTabKind::LOGS ? tr(STR_LOG_EMPTY) : tr(STR_TODOIST_NO_TASKS);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height / 2, empty);
    return;
  }

  // Title, plus a dimmed second line where a row has something to say there: its
  // label and, on Upcoming, its due date (Logs' rows always carry the Cached/
  // Synced tag). A row with neither is a single, thinner line -- so rows are not
  // all the same height, and pages are packed by height rather than by count.
  const int titleFont = taskRowTitleFontId();
  const int subtitleFont = taskRowSubtitleFontId();
  const int rowPad = std::max(6, renderer.getLineHeight(titleFont) * 2 / 5);
  const int oneLineHeight = renderer.getLineHeight(titleFont) + rowPad;
  const int twoLineHeight = oneLineHeight + renderer.getLineHeight(subtitleFont);
  const bool onLogs = activeKind == taskTabModel::TaskTabKind::LOGS;
  const bool showsDates = taskTabModel::rowsHaveSubtitle(activeKind) && !onLogs;

  // Which rows are two lines, one bit per row (at most TODOIST_MAX_CACHED_TASKS
  // rows on any tab), found in one pass over the tasks rather than a per-row
  // lookup -- 15 bytes of stack, no allocation.
  uint32_t twoLineBits[(TODOIST_MAX_CACHED_TASKS + 31) / 32] = {};
  const auto setTwoLine = [&twoLineBits](const int r) { twoLineBits[r / 32] |= 1u << (r % 32); };
  const auto isTwoLine = [&twoLineBits](const int r) { return (twoLineBits[r / 32] >> (r % 32) & 1u) != 0; };
  if (onLogs) {
    for (int r = 0; r < totalRows && r < static_cast<int>(TODOIST_MAX_CACHED_TASKS); r++) setTwoLine(r);
  } else {
    const auto& tasks = TODOIST_TASKS.getTasks();
    int r = 0;
    for (size_t t = 0; t < tasks.size() && r < static_cast<int>(TODOIST_MAX_CACHED_TASKS); t++) {
      if (!taskTabModel::matchesKind(activeKind, t, currentScope())) continue;
      if (!tasks[t].labels.empty() || (showsDates && tasks[t].dueDays != todoist::DUE_NONE)) setTwoLine(r);
      r++;
    }
  }
  const auto heightOf = [&](const int r) { return isTwoLine(r) ? twoLineHeight : oneLineHeight; };

  // The page holding the selected row: rows are packed top to bottom until the
  // next would not fit, then a new page starts, the same from any starting
  // point so a page never shifts as the selection moves within it.
  int pageStart = 0;
  int pageEnd = totalRows;
  {
    int start = 0;
    int used = 0;
    bool found = false;
    for (int r = 0; r < totalRows; r++) {
      const int h = heightOf(r);
      if (used + h > height && r > start) {
        if (taskSelectedRow < r) {
          pageStart = start;
          pageEnd = r;
          found = true;
          break;
        }
        start = r;
        used = 0;
      }
      used += h;
    }
    if (!found) pageStart = start;
  }

  int rowY = top;
  for (int i = pageStart; i < pageEnd; i++) {
    const int rowHeight = heightOf(i);
    const bool selected = rowsFocused && i == taskSelectedRow;
    const bool ink = !selected;

    if (selected) renderer.fillRect(0, rowY, pageWidth, rowHeight);

    const int textY = rowY + rowPad / 2;
    if (onLogs) {
      const int entryIndex = taskTabModel::logEntryIndexForRow(i);
      if (entryIndex < 0) {
        rowY += rowHeight;
        continue;
      }
      const auto& entry = TODOIST_TASKS.getCompletedTodayEntries()[static_cast<size_t>(entryIndex)];
      const auto shown = renderer.truncatedText(titleFont, entry.title.c_str(), textWidth);
      renderer.drawText(titleFont, textX, textY, shown.c_str(), ink);
      const int subY = textY + renderer.getLineHeight(titleFont);
      const char* tag = entry.pending ? tr(STR_LOG_CACHED) : tr(STR_LOG_SYNCED);
      renderer.drawText(subtitleFont, textX, subY, tag, ink);
      dimText(renderer, textX, subY, subtitleFont, tag, ink);
    } else {
      const int cacheIndex = taskTabModel::taskCacheIndexForRow(activeKind, i, currentScope());
      if (cacheIndex < 0) {
        rowY += rowHeight;
        continue;
      }
      const auto& task = TODOIST_TASKS.getTasks()[static_cast<size_t>(cacheIndex)];
      const auto shown = renderer.truncatedText(titleFont, task.content.c_str(), textWidth);
      renderer.drawText(titleFont, textX, textY, shown.c_str(), ink);
      // The due date (only on tabs that show it) and the label, joined with a
      // middle dot when both are there. A row with neither is one line and draws
      // no subtitle at all.
      if (isTwoLine(i)) {
        const bool showDate = showsDates && task.dueDays != todoist::DUE_NONE;
        char when[16] = "";
        if (showDate) planner::formatDayLabel(task.dueDays, when, sizeof(when));
        char subtitle[TodoistTask::LABELS_MAX_LEN + 32];
        if (showDate && !task.labels.empty()) {
          snprintf(subtitle, sizeof(subtitle), "%s  \xC2\xB7  %s", when, task.labels.c_str());
        } else if (showDate) {
          snprintf(subtitle, sizeof(subtitle), "%s", when);
        } else {
          snprintf(subtitle, sizeof(subtitle), "%s", task.labels.c_str());
        }
        const auto shownSubtitle = renderer.truncatedText(subtitleFont, subtitle, textWidth);
        const int subtitleY = textY + renderer.getLineHeight(titleFont);
        renderer.drawText(subtitleFont, textX, subtitleY, shownSubtitle.c_str(), ink);
        dimText(renderer, textX, subtitleY, subtitleFont, shownSubtitle.c_str(), ink);
      }
    }

    const bool lastOnPage = i + 1 >= pageEnd;
    if (!selected && !lastOnPage) {
      renderer.fillRectDither(textX, rowY + rowHeight - SEPARATOR_HEIGHT, textWidth, SEPARATOR_HEIGHT,
                              Color::LightGray);
    }
    rowY += rowHeight;
  }
}

StatusBand::Stop QuickPickActivity::bandStopFor(const Focus f) {
  switch (f) {
    case Focus::Header:
      return StatusBand::Stop::StatusBar;
    case Focus::Glance:
      return StatusBand::Stop::Calendar;
    case Focus::Alerts:
      return StatusBand::Stop::Alert;
    default:
      return StatusBand::Stop::None;
  }
}

QuickPickActivity::Focus QuickPickActivity::focusForBandStop(const StatusBand::Stop stop) {
  switch (stop) {
    case StatusBand::Stop::Calendar:
      return Focus::Glance;
    case StatusBand::Stop::Alert:
      return Focus::Alerts;
    default:
      return Focus::Header;
  }
}

bool QuickPickActivity::companionVisible() { return SETTINGS.showCompanion != 0; }

void QuickPickActivity::reconcileFocus() {
  // A band stop that has gone away (the last alert was dismissed, the last event
  // ended) falls back to the nearest one still there.
  const StatusBand::Stop stop = bandStopFor(focus);
  if (stop != StatusBand::Stop::None) focus = focusForBandStop(band.clamp(stop));
  if (focus == Focus::Companion && !companionVisible()) focus = Focus::Projects;
}

void QuickPickActivity::focusTopBlockFirst() { focus = Focus::Header; }

void QuickPickActivity::focusTopBlockLast() { focus = focusForBandStop(band.last()); }

void QuickPickActivity::focusAfterTopBlock() { focus = companionVisible() ? Focus::Companion : Focus::Projects; }

void QuickPickActivity::focusEmbeddedLast() {
  focus = Focus::Embedded;
  const int count = embeddedRowCount();
  // Logs has no tab bar to stand on when it is empty -- the title row is the
  // last stop there.
  if (count == 0 && onLogsEntry()) {
    focus = Focus::Projects;
    return;
  }
  tabBarFocused = count == 0;
  embeddedRow() = count == 0 ? 0 : count - 1;
}

void QuickPickActivity::render(RenderLock&&) {
  renderer.clearScreen();
  reconcileFocus();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // The top of the screen is the shared status band (see StatusBand): the status
  // bar, today's calendar events, then the newest pending alert, each present only
  // when there is something to show, each a focus stop. This screen carries no
  // title of its own, so the band is drawn untitled, with the status row sized to
  // its clock/battery content (HEADER_CONTENT_HEIGHT) rather than the theme's full
  // title-band header height. Title and subtitle are both left out of the status
  // bar: on Lyra the clock/date/battery combo is drawn unconditionally, and the
  // title and its rule only inside `if (title)`. That is Lyra-specific --
  // RoundedRaffTheme's drawHeader() returns immediately on a null title, so on that
  // theme the status row goes blank.
  const int headerTop = metrics.topPadding;
  const int topBlockBottom = band.drawUntitled(renderer, headerTop, HEADER_CONTENT_HEIGHT, bandStopFor(focus));
  const int contentTop = topBlockBottom + SELECTION_BOX_PADDING;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.buttonHintsGap;
  const int totalContentHeight = contentBottom - contentTop;
  const int contentWidth = pageWidth - MARGIN * 2;
  const int maxTextWidth = contentWidth - PAD * 2;

  // The companion figure (bubble + sprite) is optional (Settings -> Companion ->
  // Show Companion). Hidden, none of it is drawn or measured and the Tasks
  // section below starts right under the top block instead.
  // Hidden: the title row sits as far below the status row as a real screen's
  // title does (LyraTheme::drawHeader() puts it at batteryBarHeight + 3 below the
  // header's top; this row's text sits 4px into the section, and the status row
  // itself occupies HEADER_CONTENT_HEIGHT), so with nothing between them the two
  // screens' titles line up. Anything above it -- calendar rows, an alert --
  // already ends in its own padding, which is the breathing room then.
  int figureBottom = topBlockBottom + std::max(0, metrics.batteryBarHeight - 1 - HEADER_CONTENT_HEIGHT);
  if (companionVisible()) {
    const auto id = CompanionTracker::activeId();
    const auto mood = COMPANION.currentMood();

    // The bubble only carries the companion's own remarks: the sleeping line,
    // or a mood-flavored idle line. BLE alerts live in the alert area above,
    // never here. Never a task suggestion -- see this file's own header comment.
    const std::string text = mood == companion::Mood::Sleeping ? std::string(tr(STR_COMPANION_SLEEPING_BUBBLE))
                                                               : companion::idleBubbleText(mood, idleVariant);
    const auto textFit =
        companion::fitBubbleText(renderer, UI_10_FONT_ID, text, maxTextWidth, MIN_BUBBLE_TEXT_WIDTH, 4);
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int bubbleH = static_cast<int>(textFit.lines.size()) * lineH + PAD * 2;
    const int bubbleBlock = bubbleH + TAIL_LENGTH + BUBBLE_GAP;
    const int bubbleWidth = textFit.textWidth + PAD * 2;

    // The companion figure (bubble + sprite) gets a fixed share of the space
    // below the header; the embedded Tasks section gets what is left over.
    const int companionBudget = totalContentHeight * COMPANION_BUDGET_PERCENT / 100;
    int scale = 1;
    for (int candidate = MAX_SCALE; candidate >= 1; candidate--) {
      if (companion::poseWidth(candidate) > contentWidth) continue;
      if (bubbleBlock + companion::poseHeight(candidate) <= companionBudget) {
        scale = candidate;
        break;
      }
    }

    const int spriteW = companion::poseWidth(scale);
    const int spriteH = companion::poseHeight(scale);
    const int centreX = pageWidth / 2;
    const int bubbleX = centreX - bubbleWidth / 2;
    const int spriteTop = contentTop + bubbleBlock;

    // Hovering the companion figure (see this file's own header comment): the
    // bubble itself is drawn filled -- black background, white text -- rather
    // than a selection-box outline around the figure, so the focus stop reads
    // as the bubble itself. Filled from within drawSpeechBubble
    // itself (see its own `filled` comment) so the black exactly follows the
    // bubble's rounded body and tail, not a plain inverted bounding rect that
    // would also blacken the paper-colored corners just outside the curve.
    const bool bubbleFocused = focus == Focus::Companion;
    companion::drawSpeechBubble(renderer, bubbleX, contentTop, bubbleWidth, bubbleH, TAIL_LENGTH,
                                companion::TailSide::Bottom, /*lineWidth=*/2, bubbleFocused);
    const Rect textBounds{bubbleX + PAD, contentTop, textFit.textWidth, bubbleH};
    int textY = contentTop + PAD;
    for (const auto& line : textFit.lines) {
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, textY, line.c_str(), /*black=*/!bubbleFocused);
      textY += lineH;
    }

    companion::drawPose(renderer, id, mood, centreX - spriteW / 2, spriteTop, scale);

    figureBottom = spriteTop + spriteH + LABEL_GAP;
  }

  // Straight from the sprite (or the top block, with the companion hidden) to
  // the embedded Tasks section below -- no mood label in between any more. The
  // side buttons carry no labels on this screen, so nothing constrains where the
  // section starts or how wide it is.
  const int sectionTop = figureBottom;

  // The section below is a scaled-down rendering of the real Tasks screen (see
  // this file's own header comment): its own title row -- the Todoist projects
  // under the Filter, and Logs, one of them in bold (see drawProjectTitleRow())
  // -- the thick rule under it (the top line of the tab bar), then the tab bar
  // itself (visibleTabs mirrors the real screen's own tab bar exactly, via the
  // shared TaskTabModel), except on Logs, which has none. Drawn directly rather
  // than through GUI.drawHeader(), which only takes one title string; the font,
  // the 3px offset and the 3px rule are the ones LyraTheme::drawHeader() uses for
  // a title.
  const int sectionTitleHeight = renderer.getLineHeight(UI_12_FONT_ID) + SECTION_TITLE_PAD;
  {
    std::vector<const char*> names;
    names.reserve(projectEntries.size());
    for (const auto& entry : projectEntries) names.push_back(entry.name.c_str());
    // The pill leaves 2px clear above and below inside the band the rule bounds
    // (the 3px rule sits at the bottom of the title row), like the tab pill does.
    const int pillHeight = renderer.getLineHeight(UI_12_FONT_ID) + 7;
    drawProjectTitleRow(renderer, sectionTop + 4, sectionTop + 2, pillHeight, pageWidth, metrics.contentSidePadding,
                        metrics.tabSpacing, names, projectIndex, focus == Focus::Projects);
  }
  renderer.drawLine(0, sectionTop + sectionTitleHeight - 3, pageWidth - 1, sectionTop + sectionTitleHeight - 3, 3,
                    true);
  const int tabBarTop = sectionTop + sectionTitleHeight;

  // tabBarFocused (not activeKind) decides whether the bar is drawn
  // "focused" (matches PlannerScreenActivity's own "index 0 is the tab
  // bar" convention, the same one loop() mirrors for Left1/Left2/Right2).
  std::vector<TabInfo> tabs;
  tabs.reserve(visibleTabs.size());
  for (const auto kind : visibleTabs) {
    tabs.push_back(TabInfo{taskTabModel::tabLabel(kind), kind == activeKind});
  }
  const bool showTabBar = !onLogsEntry();
  if (showTabBar) {
    GUI.drawTabBar(renderer, Rect{0, tabBarTop, pageWidth, metrics.tabBarHeight}, tabs,
                   focus == Focus::Embedded && tabBarFocused);
  }

  const int listTop = tabBarTop + (showTabBar ? metrics.tabBarHeight : 0);
  const int listHeight = std::max(0, contentBottom - listTop);
  renderTasksTab(listTop, listHeight);

  // Right1 is a per-focus shortcut now (see this file's own header comment),
  // not a fixed "leave" action -- blank wherever there's nothing to open.
  const char* backLabel = "";
  const char* confirmLabel = "";
  const char* leftLabel = tr(STR_DIR_UP);
  const char* rightLabel = tr(STR_DIR_DOWN);
  switch (focus) {
    case Focus::Header:
      backLabel = tr(STR_HOME);
      // Right2 is Sync All instead of Clear/Select -- there's no per-row or
      // per-suggestion action to offer for the header itself.
      confirmLabel = tr(STR_SYNC_ALL);
      break;
    case Focus::Glance:
      backLabel = homeAppOrder::displayName(homeAppOrder::AppId::Calendar);
      break;
    case Focus::Alerts:
#ifdef ENABLE_BLE_NOTIFY_SPIKE
      backLabel = homeAppOrder::displayName(homeAppOrder::AppId::Notifications);
      confirmLabel = tr(STR_DISMISS);
#endif
      break;
    case Focus::Companion:
#ifdef ENABLE_BLE_NOTIFY_SPIKE
      backLabel = homeAppOrder::displayName(homeAppOrder::AppId::Notifications);
#endif
      break;
    case Focus::Projects:
      // Previous / next entry, labelled with where a press would land -- the
      // same preview the tab bar's Right2 gives for the next tab. On the first
      // entry Right1 has nothing to step back to and goes Home instead.
      if (projectEntries.size() > 1) {
        const size_t count = projectEntries.size();
        backLabel = projectIndex == 0 ? tr(STR_HOME) : projectEntries[projectIndex - 1].name.c_str();
        confirmLabel = projectEntries[(projectIndex + 1) % count].name.c_str();
      }
      break;
    case Focus::Embedded:
      if (tabBarFocused) {
        // Random (see loop()'s own Right1 handler) -- from the tab bar one row
        // is already enough to roll onto, and there is nothing to roll on a
        // Logs tab.
        if (!embeddedOnLogs() && embeddedRowCount() > 0) backLabel = tr(STR_RANDOM);
        // Same touch PlannerScreenActivity's own tab bar gives Right2: its
        // label previews the tab a press would switch to.
        if (visibleTabs.size() > 1) {
          int index = 0;
          for (size_t i = 0; i < visibleTabs.size(); i++) {
            if (visibleTabs[i] == activeKind) {
              index = static_cast<int>(i);
              break;
            }
          }
          confirmLabel = taskTabModel::tabLabel(
              visibleTabs[static_cast<size_t>((index + 1) % static_cast<int>(visibleTabs.size()))]);
        }
      } else {
        // A row is focused: Right1 goes Back up to the tab bar (see loop()'s
        // own Right1 handler); Right2 is whatever the row offers.
        backLabel = tr(STR_BACK);
        if (activeKind == taskTabModel::TaskTabKind::LOGS) {
          const int entryIndex = taskTabModel::logEntryIndexForRow(taskSelectedRow);
          const auto& entries = TODOIST_TASKS.getCompletedTodayEntries();
          const bool onCachedRow = entryIndex >= 0 && static_cast<size_t>(entryIndex) < entries.size() &&
                                   entries[static_cast<size_t>(entryIndex)].pending;
          confirmLabel = onCachedRow ? tr(STR_CLEAR_BUTTON) : "";
        } else {
          confirmLabel = taskTabModel::taskCacheIndexForRow(activeKind, taskSelectedRow, currentScope()) >= 0
                             ? tr(STR_SELECT)
                             : "";
        }
      }
      break;
  }
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, leftLabel, rightLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

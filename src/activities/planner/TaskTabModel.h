#pragma once
#include <TodoistTask.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * The Tasks tab set and row-resolution rules, shared between the real
 * TasksActivity screen and QuickPickActivity's own scaled-down embedded
 * rendering of it below the companion figure -- extracted so the two never
 * disagree about which task is in which tab, or about the "keep the same
 * tab selected across a rebuild" state machine (see TasksActivity's own
 * class doc comment for the full reasoning behind that).
 *
 * The tabs are the labels of the open project's tasks (alphabetical), with
 * Unlabelled last for tasks that carry none. A task with several labels shows
 * under each of them. Every tab lists its tasks by due date, oldest first, with
 * undated tasks last. LOGS is today's completed tasks
 * (TodoistTaskCache::getCompletedTodayEntries(), a distinct vector from the live
 * task list, not a filter over it) -- absorbed from the old standalone Logs
 * screen.
 */
namespace taskTabModel {

// Not a tab index: which tabs are on screen depends on the labels the open
// project's tasks carry (and, for LOGS, on what's been completed today), so the
// two are mapped through a caller-owned `visibleTabs` vector, same as
// TasksActivity's own convention. The default value is "no tab": it matches no
// task and is never in a rebuilt list, so passing it as the tab to keep makes a
// rebuild land on the first tab.
struct TaskTab {
  enum class Type : uint8_t { LABEL, UNLABELLED, LOGS };
  Type type = Type::LABEL;
  std::string label;  // LABEL only

  static TaskTab logs() { return TaskTab{Type::LOGS, {}}; }
  static TaskTab unlabelled() { return TaskTab{Type::UNLABELLED, {}}; }
  static TaskTab withLabel(std::string name) { return TaskTab{Type::LABEL, std::move(name)}; }

  bool isLogs() const { return type == Type::LOGS; }
  bool operator==(const TaskTab& other) const { return type == other.type && label == other.label; }
  bool operator!=(const TaskTab& other) const { return !(*this == other); }
};

// Which project's tasks a lookup below covers. ALL_PROJECTS is no restriction at
// all -- what the real Tasks screen uses. Any other value is a
// todoist::hashProjectId() (TodoistTask::projectHash); UNKNOWN_PROJECT is the
// group of tasks whose project is not in the cached project list (no id, or a
// project the last sync could not name), so no task falls out of every group.
constexpr uint32_t ALL_PROJECTS = 0xFFFFFFFFu;
constexpr uint32_t UNKNOWN_PROJECT = 0;

// Whether the live task at `cacheIndex` (TODOIST_TASKS.getTasks()) belongs to
// `tab`, within `projectScope`. Always false for LOGS -- a log row is never a
// live-task cache row; see logEntryIndexForRow() for how a Logs row is actually
// resolved.
bool matchesTab(const TaskTab& tab, size_t cacheIndex, uint32_t projectScope = ALL_PROJECTS);

// How many live tasks are in `projectScope`, whatever tab they would land on --
// what decides which projects the Companion's title row lists.
int countInProject(uint32_t projectScope);

// Row count for `tab`, within `projectScope`: a scan over the live tasks, or all
// of today's completed entries for LOGS (the log is not split by project).
int countFor(const TaskTab& tab, uint32_t projectScope = ALL_PROJECTS);

// The cache indexes of `tab`'s tasks in display order -- oldest due date first,
// undated last, ties in cache order -- written to `out`, which needs room for
// TODOIST_MAX_CACHED_TASKS. Returns how many. Zero for LOGS.
int rowsFor(const TaskTab& tab, uint32_t projectScope, uint8_t* out);

// The live-task cache index behind visible row `row` under a real tab, or -1:
// the tab's tasks in due-date order, oldest first, undated last (ties keep cache
// order). Always -1 for LOGS -- use logEntryIndexForRow() there instead; kept as
// a distinct function (rather than one that silently returns a meaningless
// value) so a caller cannot mix the two index spaces up by accident.
int taskCacheIndexForRow(const TaskTab& tab, int row, uint32_t projectScope = ALL_PROJECTS);

// The index into TODOIST_TASKS.getCompletedTodayEntries() behind visible Log
// row `row`, or -1 out of range. Meaningful only under LOGS.
int logEntryIndexForRow(int row);

// Whether a row under `tab` draws a second, dimmed line: the due date on a task
// tab (an undated task has none, so its line stays empty), or the Cached/Synced
// tag for LOGS.
bool rowsHaveSubtitle(const TaskTab& tab);

// The text of a task row's second line: "Overdue", "Today" or the date, and
// empty for an undated task. Writes "" to `out` when there is nothing to show.
void dueLabel(const TodoistTask& task, char* out, size_t outSize);

// Recomputes which tabs currently have rows into `visibleTabs`: one per label the
// tasks in `projectScope` carry (alphabetical, ignoring case), then Unlabelled
// when any task carries none, then LOGS whenever `includeLogs` (which is what
// keeps this list from ever being empty). With `includeLogs` false the list can
// come back empty -- callers that leave Logs out give it a screen of its own.
// Keeps `wanted` selected where it survives the rebuild and falling back to index
// 0 -- the first visible tab -- otherwise. Mirrors TasksActivity::
// rebuildTabs()'s own state machine exactly, so the embedded section and the
// real screen never disagree about the tab set. Returns the index `wanted` (or
// that fallback) landed at in the rebuilt `visibleTabs`. A caller that just wants
// the default tab passes a default-constructed TaskTab.
int rebuildVisibleTabs(const TaskTab& wanted, std::vector<TaskTab>& visibleTabs, uint32_t projectScope = ALL_PROJECTS,
                       bool includeLogs = true);

// Tab bar label for `tab`.
const char* tabLabel(const TaskTab& tab);

}  // namespace taskTabModel

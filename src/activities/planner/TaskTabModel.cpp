#include "TaskTabModel.h"

#include <I18n.h>
#include <TodoistDate.h>
#include <TodoistTaskCache.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "PlannerLabels.h"

namespace taskTabModel {

namespace {
bool inProjectScope(const TodoistTask& task, const uint32_t projectScope) {
  if (projectScope == ALL_PROJECTS) return true;
  if (projectScope != UNKNOWN_PROJECT) return task.projectHash == projectScope;
  // The unmatched group: no project id at all, or one that is not in the cached
  // project list.
  if (task.projectHash == 0) return true;
  for (const auto& project : TODOIST_TASKS.getProjects()) {
    if (project.hash == task.projectHash) return false;
  }
  return true;
}

constexpr const char* LABEL_SEPARATOR = ", ";

// Calls fn(const char* start, size_t length) for each label of a task's joined label string ("a, b").
template <typename Fn>
void forEachLabel(const std::string& labels, Fn fn) {
  size_t pos = 0;
  while (pos < labels.size()) {
    const size_t end = labels.find(LABEL_SEPARATOR, pos);
    const size_t length = (end == std::string::npos ? labels.size() : end) - pos;
    if (length > 0) fn(labels.c_str() + pos, length);
    if (end == std::string::npos) break;
    pos = end + strlen(LABEL_SEPARATOR);
  }
}

bool hasLabel(const std::string& labels, const std::string& label) {
  bool found = false;
  forEachLabel(labels, [&](const char* start, const size_t length) {
    if (length == label.size() && strncmp(start, label.c_str(), length) == 0) found = true;
  });
  return found;
}

bool taskMatchesTab(const TaskTab& tab, const TodoistTask& task) {
  switch (tab.type) {
    case TaskTab::Type::LABEL:
      return !tab.label.empty() && hasLabel(task.labels, tab.label);
    case TaskTab::Type::UNLABELLED:
      return task.labels.empty();
    case TaskTab::Type::LOGS:
      return false;
  }
  return false;
}

int compareLabels(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()); }
}  // namespace

// A stable insertion sort: at most 60 entries, and it needs no heap.
int rowsFor(const TaskTab& tab, const uint32_t projectScope, uint8_t* out) {
  if (tab.isLogs()) return 0;
  const auto& tasks = TODOIST_TASKS.getTasks();
  int count = 0;
  for (size_t i = 0; i < tasks.size() && count < static_cast<int>(TODOIST_MAX_CACHED_TASKS); i++) {
    if (!inProjectScope(tasks[i], projectScope) || !taskMatchesTab(tab, tasks[i])) continue;
    const uint16_t due = tasks[i].dueDays;  // DUE_NONE is the largest value, so undated sorts last
    int at = count;
    while (at > 0 && tasks[out[at - 1]].dueDays > due) {
      out[at] = out[at - 1];
      at--;
    }
    out[at] = static_cast<uint8_t>(i);
    count++;
  }
  return count;
}

bool matchesTab(const TaskTab& tab, const size_t cacheIndex, const uint32_t projectScope) {
  const auto& tasks = TODOIST_TASKS.getTasks();
  if (cacheIndex >= tasks.size()) return false;
  return inProjectScope(tasks[cacheIndex], projectScope) && taskMatchesTab(tab, tasks[cacheIndex]);
}

int countInProject(const uint32_t projectScope) {
  int count = 0;
  for (const auto& task : TODOIST_TASKS.getTasks()) {
    if (inProjectScope(task, projectScope)) count++;
  }
  return count;
}

int countFor(const TaskTab& tab, const uint32_t projectScope) {
  if (tab.isLogs()) return static_cast<int>(TODOIST_TASKS.getCompletedTodayEntries().size());

  const auto& tasks = TODOIST_TASKS.getTasks();
  int count = 0;
  for (const auto& task : tasks) {
    if (inProjectScope(task, projectScope) && taskMatchesTab(tab, task)) count++;
  }
  return count;
}

int taskCacheIndexForRow(const TaskTab& tab, const int row, const uint32_t projectScope) {
  if (row < 0 || tab.isLogs()) return -1;
  uint8_t rows[TODOIST_MAX_CACHED_TASKS];
  const int count = rowsFor(tab, projectScope, rows);
  return row < count ? static_cast<int>(rows[row]) : -1;
}

int logEntryIndexForRow(const int row) {
  if (row < 0 || static_cast<size_t>(row) >= TODOIST_TASKS.getCompletedTodayEntries().size()) return -1;
  return row;
}

bool rowsHaveSubtitle(const TaskTab&) { return true; }

void dueLabel(const TodoistTask& task, char* out, const size_t outSize) {
  if (out == nullptr || outSize == 0) return;
  out[0] = '\0';
  if (task.dueDays == todoist::DUE_NONE) return;
  if (task.overdue) {
    snprintf(out, outSize, "%s", tr(STR_OVERDUE));
    return;
  }
  const uint16_t today = todoist::dueDaysFromIso(TODOIST_TASKS.getSyncDate().c_str());
  if (today != todoist::DUE_NONE && task.dueDays == today) {
    snprintf(out, outSize, "%s", tr(STR_TODAY));
    return;
  }
  planner::formatDayLabel(task.dueDays, out, outSize);
}

int rebuildVisibleTabs(const TaskTab& wanted, std::vector<TaskTab>& visibleTabs, const uint32_t projectScope,
                       const bool includeLogs) {
  visibleTabs.clear();

  // The distinct labels of the tasks in scope, alphabetical. Labels are short and the task list is
  // capped at 60, so a sorted vector is plenty.
  std::vector<std::string> labels;
  labels.reserve(16);
  bool anyUnlabelled = false;
  for (const auto& task : TODOIST_TASKS.getTasks()) {
    if (!inProjectScope(task, projectScope)) continue;
    if (task.labels.empty()) anyUnlabelled = true;
    forEachLabel(task.labels, [&](const char* start, const size_t length) {
      const std::string name(start, length);
      const auto at =
          std::lower_bound(labels.begin(), labels.end(), name,
                           [](const std::string& a, const std::string& b) { return compareLabels(a, b) < 0; });
      if (at != labels.end() && compareLabels(*at, name) == 0) return;
      labels.insert(at, name);
    });
  }

  visibleTabs.reserve(labels.size() + 2);
  for (auto& name : labels) visibleTabs.push_back(TaskTab::withLabel(std::move(name)));
  if (anyUnlabelled) visibleTabs.push_back(TaskTab::unlabelled());
  // Logs always shows when asked for -- it's a log view, not a filter, so an
  // empty state ("nothing completed today yet") is itself useful feedback
  // rather than noise to hide. It's also what keeps this list from ever being
  // empty.
  if (includeLogs) visibleTabs.push_back(TaskTab::logs());

  for (size_t i = 0; i < visibleTabs.size(); i++) {
    if (visibleTabs[i] == wanted) return static_cast<int>(i);
  }
  // Falls back to the first visible tab when the selected one just emptied -
  // completing the last task under a label, say, which takes its tab away while
  // the user is standing on it. (An empty list, possible only without Logs,
  // leaves 0 pointing at nothing -- the caller owns that case.)
  return 0;
}

const char* tabLabel(const TaskTab& tab) {
  switch (tab.type) {
    case TaskTab::Type::LABEL:
      return tab.label.c_str();
    case TaskTab::Type::UNLABELLED:
      return tr(STR_UNLABELLED);
    case TaskTab::Type::LOGS:
      return tr(STR_LOGS);
  }
  return "";
}

}  // namespace taskTabModel

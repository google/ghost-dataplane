#ifndef PRODKERNEL_GHOST_SCHEDULERS_NET_CGROUP_WATCHER_H_
#define PRODKERNEL_GHOST_SCHEDULERS_NET_CGROUP_WATCHER_H_

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/strings/match.h"

namespace ghost {
namespace cgroup_watcher {

// Scrapes the control group hierarchy to see which control groups have ghOSt
// enabled. This code then moves all tasks in the ghOSt-enabled cgroups to the
// ghOSt scheduling class.
//
// Example:
// cgroup_watcher::ScrapeCgroups();
// You will generally want to periodically call this method to handle cgroup
// changes that have happened since the last call.

// The path prefix to the control group (cgroup) file system interface.
constexpr absl::string_view kCgroupPathPrefix = "/sys/fs/cgroup/cpu";
//"/dev/cgroup/cpu";
// When a control group wants its tasks to run in ghOSt, it creates this file in
// its cgroup file system directory.
constexpr absl::string_view kGhostFile = "cpu.ghost_enabled";

// Scrapes the control group file system for all cgroups that should have their
// tasks scheduled by ghOSt and returns the number of tasks moved into ghOSt.
// If provided, `skipdir` and its hierarchy are omitted from directory
// traversal.
// containers_to_tasks_rules is a map from
// <container pattern> --> <task name pattern>. We call this the "rule".
// Currently, patterns are just substrings to be searched for. For a given
// container, we check if <container pattern> is a substring of the path. For
// every task under a container matching the pattern, we check if
// <task name pattern> is a substring of the task name. The task name is taken
// from "/proc/<pid>/comm".
int ScrapeCgroups(absl::string_view skipdir,
                  absl::flat_hash_map<std::string, std::string>&
                                                containers_to_tasks_rules);

// Get a task name by reading /proc/<tid>/comm
std::string GetTaskName(pid_t tid);
}  // namespace cgroup_watcher
}  // namespace ghost

#endif  // PRODKERNEL_GHOST_SCHEDULERS_NET_CGROUP_WATCHER_H_

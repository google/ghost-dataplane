
#include <sched.h>

#include <algorithm>

#include "absl/strings/match.h"
#include "kernel/ghost_uapi.h"
#include "lib/base.h"
#include "lib/ghost.h"
#include "lib/logging.h"

#include "schedulers/netPreemptDelay/cgroup_watcher.h"

#include <string>              
#include <fstream>             
#include <streambuf>           
#include <iostream>            
#include <filesystem>          
namespace fs = std::filesystem;



namespace ghost {
namespace cgroup_watcher {
//std::string GetTaskName(pid_t tid) {
//  const std::string comm_path = absl::StrFormat("/proc/%d/comm", tid);
//  std::string contents;
//  if (file::GetContents(comm_path, &contents, file::Defaults()).ok()) {
//    absl::StripTrailingAsciiWhitespace(&contents);
//    return contents;
//  } else {
    // Sometimes the path is inaccessible. This function does best effort to get
    // a task name. We shouldn't crash the program if we fail. We therefore
    // return an empty string instead.
//    return "";
//  }
//}

std::string GetTaskName(pid_t tid) {                                  
  const std::string comm_path = absl::StrFormat("/proc/%d/comm", tid);
                                                                      
  std::ifstream t(comm_path);                                         
  std::string contents((std::istreambuf_iterator<char>(t)),           
               std::istreambuf_iterator<char>());                     
  return contents;                                                    
}                                                                     



namespace {
// Return `true` if the thread with thread identifier `tid` was moved to the
// `scheduler` scheduling class and `false` otherwise.
bool MoveToScheduler(pid_t tid, int scheduler) {
  if (sched_getscheduler(tid) == scheduler) {
    // The task is already in the `scheduler` scheduling class.
    return false;
  }

  int ret = 0;
  if (scheduler == SCHED_GHOST) {
    ret = SchedTaskEnterGhost(tid);
  } else {
    sched_param param = {0};
    ret = sched_setscheduler(tid, scheduler, &param);
  }

  if (ret == 0) {
    // Trust but verify.
    CHECK_EQ(sched_getscheduler(tid), scheduler);
    return true;
  }

  // The `setscheduler` operation could fail if thread `tid` is dying, which
  // is not an error case.
  //PCHECK(ret == -1 && errno == ESRCH);
  return false;
}

// Moves the tasks in the control group at `control_group_path` to ghOSt
// and returns the number moved.
int MoveTasksToGhost(absl::string_view control_group_path,
                     absl::string_view name_pattern) {
  constexpr absl::string_view kTasksFileName = "tasks";

  fs::path dir (control_group_path);            
  fs::path file (kTasksFileName);           
  std::string tasks_path = dir / file;

  //std::cout << "Moving CGROUP path " << control_group_path << " Task Pattern " << name_pattern << "\n";

  // If no tasks file, the container is probably on its way out.
  //if (!file::Exists(tasks_path, file::Defaults()).ok())
  //  return 0;
  if( !fs::exists(tasks_path) ){
    //std::cout << "FIle " << tasks_path << " Does not exist\n";
    return 0;
  }

  // The file at `tasks_path` contains one TID per line.
  int moved = 0;
  std::ifstream file_to_read(tasks_path);
  std::string line;
  while (std::getline(file_to_read, line)){
//    std::cout << "Line : " << line << "\n";
//  for (const std::string& line :
//       FileLines(tasks_path, FileLineIterator::NO_LINEFEED)) {
    pid_t tid = std::stoi(line);
    std::string task_name = GetTaskName(tid);

    // TODO(oweisse): We may want to use actual regex. For now, simpe substrings
    // are enough.
    if (name_pattern == "*" || absl::StrContains(task_name, name_pattern)) {
      if (MoveToScheduler(tid, SCHED_GHOST)) {
        ++moved;
        absl::FPrintF(stderr, "Moved task %s under %s\n",
                      task_name, control_group_path);
	
	std::string task_name = GetTaskName(tid);
	std::string task_added = absl::StrFormat("Moved task %s pid=%d under %s\n",
                       				task_name, tid, control_group_path);
        std::ofstream ghost_log;
        ghost_log.open("/tmp/muppet", std::ios_base::app);
        ghost_log << task_added;
        ghost_log.close();

      }
    }
  }
  return moved;
}

// Moves tasks in the control group at `path` to ghOSt (if specified by the
// control group) and then recursively checks all sub-cgroups.
//
// This is the recursive helper method for `ScrapeCgroups`.
//
// Returns the number of tasks moved to ghOSt.
int ScrapeDirectory(absl::string_view skipdir,
                    absl::string_view path,
                    const absl::flat_hash_map<std::string, std::string>&
                                                containers_to_tasks_rules) {
  if (skipdir == path) {
    return 0;
  }

  int moved = 0;
  absl::string_view cgroup_name = path.substr(kCgroupPathPrefix.size());

  //std::cout << "Cgroup Name: " << cgroup_name << "\n";

  if (containers_to_tasks_rules.contains(cgroup_name)) {
    const std::string& task_name_pattern =
        containers_to_tasks_rules.find(cgroup_name)->second;
    moved += MoveTasksToGhost(path, task_name_pattern);
  }

  for (const auto& [cgroup_pattern, task_name_pattern] :
           containers_to_tasks_rules) {
    if (absl::StrContains(cgroup_name, cgroup_pattern)) {
      moved += MoveTasksToGhost(path, task_name_pattern);
    }
  }

  // Recurse on all subdirectories.
//  std::vector<std::pair<std::string, FileStat>> results;
//  CHECK_OK(file::MatchAndStat(file::JoinPath(path, "*"), &results, file::Defaults()));
//  for (const auto& [child_path, stat] : results) {
//    if (stat.IsDirectory()) {
//      moved += ScrapeDirectory(skipdir, child_path, containers_to_tasks_rules);
//    }
//  }

  for (const auto & file : fs::directory_iterator(path)){
    absl::string_view child_path = file.path().c_str();
    if(fs::is_directory(file.path()))
      moved += ScrapeDirectory(skipdir, child_path, containers_to_tasks_rules);
  }

  return moved;
}
}  // namespace

int ScrapeCgroups(absl::string_view skipdir,
                  absl::flat_hash_map<std::string, std::string>&
                                                containers_to_tasks_rules) {
  return ScrapeDirectory(skipdir, kCgroupPathPrefix, containers_to_tasks_rules);
}

}  // namespace cgroup_watcher
}  // namespace ghost

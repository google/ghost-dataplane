/*

Copyright 2021 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

 https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include <fcntl.h>

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "schedulers/netPreemptDelay/cgroup_watcher.h"
#include "schedulers/netPreemptDelay/net_scheduler.h"
#include "absl/debugging/symbolize.h"
#include "absl/flags/parse.h"
#include "lib/agent.h"
#include "lib/channel.h"
#include "lib/enclave.h"
#include "lib/topology.h"

#include <filesystem>          
namespace fs = std::filesystem;



ABSL_FLAG(int32_t, globalcpu, -1,
          "Global cpu. If -1, then defaults to <firstcpu>)");
ABSL_FLAG(std::string, ghost_cpus, "1-5", "cpulist");
ABSL_FLAG(bool, enforce_affinity, true, "Enforce affinity requests");
ABSL_FLAG(absl::Duration, cgroup_period, absl::Seconds(1),
          "The period of cgroup scrapes");

namespace ghost {

void ParseNetConfig(NetConfig* config) {
  int globalcpu = absl::GetFlag(FLAGS_globalcpu);
  bool enforce_affinity = absl::GetFlag(FLAGS_enforce_affinity);
  CpuList ghost_cpus =
      ghost::MachineTopology()->ParseCpuStr(absl::GetFlag(FLAGS_ghost_cpus));
  CHECK_GT(ghost_cpus.Size(), 1);

  if (globalcpu < 0) {
    CHECK_EQ(globalcpu, -1);
    globalcpu = ghost_cpus.GetNthCpu(0).id();
    absl::SetFlag(&FLAGS_globalcpu, globalcpu);
  }

  CHECK(ghost_cpus.IsSet(globalcpu));

  Topology* topology = MachineTopology();
  config->topology_ = topology;
  config->cpus_ = ghost_cpus;
  config->global_cpu_ = topology->cpu(globalcpu);
  config->enforce_affinity_mask_ = enforce_affinity;

  printf("Dumping config->cpus_ bitmap\n");
  std::cout << config->cpus_ << std::endl;
  for (const Cpu cpu : config->cpus_)
    std::cout << cpu << ",";
  std::cout << std::endl;
}

// Puts the calling thread to sleep for a duration of `duration`. Wakes up
// and returns immediately when a signal is received. If `duration` is 0 or
// negative, returns immediately.
//
// We implement our own `SleepFor` function rather than use `absl::SleepFor`
// because Abseil's function does not immediately return when a signal is
// received. We need to return immediately when SIGINT is received so that the
// process can quickly exit.
void SleepFor(absl::Duration duration) {
  // `kMaxSleep` is the maximum duration a thread can sleep for at once.
  constexpr absl::Duration kMaxSleep =
      absl::Seconds(std::numeric_limits<time_t>::max());
  while (duration > absl::ZeroDuration()) {
    absl::Duration to_sleep = std::min(duration, kMaxSleep);

    struct timespec sleep_time = absl::ToTimespec(to_sleep);
    int ret = nanosleep(&sleep_time, &sleep_time);
    if (ret == -1) {
      CHECK_EQ(errno, EINTR);
      // We received a signal, so return immediately.
      return;
    }
    CHECK_EQ(ret, 0);
    duration -= to_sleep;
  }
}

}  // namespace ghost

int main(int argc, char* argv[]) {
  ghost::Notification exit;

  absl::InitializeSymbolizer(argv[0]);
  absl::ParseCommandLine(argc, argv);
  ghost::NetConfig config;
  ghost::ParseNetConfig(&config);

  printf("Core map\n");

  int n = 0;
  for (const ghost::Cpu& c : config.topology_->all_cores()) {
    printf("( ");
    for (const ghost::Cpu& s : c.siblings()) printf("%2d ", s.id());
    printf(")%c", ++n % 8 == 0 ? '\n' : '\t');
  }
  printf("\n");

  printf("L3 siblings map\n");
  n = 0;
  for (const ghost::Cpu& c : config.topology_->all_cores()) {
    printf("( ");
    for (const ghost::Cpu& s : c.l3_siblings()) printf("%2d ", s.id());
    printf(")%c", ++n % 8 == 0 ? '\n' : '\t');
  }
  printf("\n");

  printf("Initializing...\n");

  // Using new so we can destruct the object before printing Done
  auto uap =
      new ghost::AgentProcess<ghost::FullNetAgent<ghost::LocalEnclave>,
                              ghost::NetConfig>(config);

  ghost::Ghost::InitCore();
  printf("Initialization complete, ghOSt active\n");
  fflush(stdout);

  // In the future we'll let the agent know which enclave to use via cmdline.
  // Until then look for the first one that's available.
  ghost::GhostThread::SetGlobalEnclaveCtlFdOnce();

  // TODO(brho): this is racy - uap could be deleted already
  ghost::GhostSignals::AddHandler(SIGINT, [&exit](int) {
    static bool first = true;  // We only modify the first SIGINT.

    if (first) {
      first = false;
      exit.Notify();
      return false;  // We'll exit on subsequent SIGTERMs.
    }
    return true;
  });

  // TODO(brho): this is racy - uap could be deleted already
  ghost::GhostSignals::AddHandler(SIGUSR1, [uap](int) {
    uap->Rpc(ghost::NetScheduler::kDebugRunqueue);
    return false;
  });

  const absl::Duration sleep_duration = absl::GetFlag(FLAGS_cgroup_period);
  // The agent may have been started from a cgroup within a ghost-enabled cgroup
  // hierarchy. In that case ScrapeCgroups needs to skip the agent's container.
  //const std::string cwd = file_util::LinuxFileOps::GetCWD();
  const std::string cwd = fs::current_path();

  // We use a cgroup mygroup1 and move the DPDK threads to this cgroup.
  // We also get all the docker threads by scraping the docker cgroup.
  absl::flat_hash_map<std::string, std::string> containers_to_tasks_rules;
  containers_to_tasks_rules["mygroup1"] = "*";
  containers_to_tasks_rules["docker"] = "memcached";

  // Keep scraping cgroups periodically until exit notification.
  while (!exit.HasBeenNotified()) {
    if (int moved = ghost::cgroup_watcher::ScrapeCgroups(cwd,
                                                     containers_to_tasks_rules);
        moved != 0) {
      printf("Moved %d tasks into ghost\n", moved);
    }
    ghost::SleepFor(sleep_duration);
  }

  delete uap;

  printf("Done!\n");
  return 0;
}

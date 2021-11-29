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

#include "schedulers/netPreemptDelay/net_scheduler.h"
#include <vector>

#include "cgroup_watcher.h"
#include "absl/strings/match.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"

namespace ghost {
namespace {

Cpu PickNextCpu(const CpuList& avail_cpus, const NetTask& task,
                bool enforce_affinity) {
  CpuList allowed_cpus = avail_cpus;

  if (enforce_affinity) {
    allowed_cpus = task.allowed_cpulist;
    allowed_cpus.Intersection(avail_cpus);
  }

  //CHECK(!allowed_cpus.Empty());

  // Returning an Uninitialized CPU results in the caller doing a Yield().
  // It turns out that for the SLL, it's better to Yield rather than try to
  // stuff a task on any CPU outside of its allowed_cpus range. (~7% better QPS
  // wise)
  if (allowed_cpus.Empty()) {
    return Cpu(Cpu::UninitializedType::kUninitialized);
  }

  // No history about which CPU this task previously ran on. Pick any CPU.
  if (!task.cpu.valid()) {
    return allowed_cpus.Front();
  }

  Cpu previous_cpu = task.cpu;

  // Prefer CPU where task previously ran if it's available.
  if (allowed_cpus.IsSet(previous_cpu.id())) {
    return previous_cpu;
  }

  // TODO(pranavvaish): These multiple "Intersection"s are suboptimal? We could
  // instead store an id in each CpuRep for CCX (similar to NUMA) and then loop
  // over the list once and get the Cpu that is the best in terms of cache
  // locality?
  CpuList allowed_siblings = allowed_cpus;
  allowed_siblings.Intersection(previous_cpu.siblings());
  Cpu sibling_cpu = allowed_siblings.Front();
  if (sibling_cpu.valid()) {
    return sibling_cpu;
  }

  CpuList allowed_l3_siblings = allowed_cpus;
  allowed_l3_siblings.Intersection(previous_cpu.l3_siblings());
  Cpu l3_sibling_cpu = allowed_l3_siblings.Front();
  if (l3_sibling_cpu.valid()) {
    return l3_sibling_cpu;
  }

  // Try picking a cpu from the same NUMA node.
  // This step is a no-op for Skylake machines where L3 siblings = socket CPUs.
  for (const Cpu& allowed_cpu : allowed_cpus) {
    if (previous_cpu.numa_node() == allowed_cpu.numa_node()) {
      return allowed_cpu;
    }
  }

  // None of the locality preferred CPUs were allowed, fall back to any allowed
  // cpu.
  return allowed_cpus.Front();
}

}  // namespace

void NetTask::SetRuntime(absl::Duration new_runtime,
                            bool update_elapsed_runtime) {
  CHECK_GE(new_runtime, runtime);
  if (update_elapsed_runtime) {
    elapsed_runtime += new_runtime - runtime;
  }
  runtime = new_runtime;
}

void NetTask::UpdateRuntime() {
  SetRuntime(absl::Nanoseconds(status_word.runtime()),
             /*update_elapsed_runtime=*/true);
}

NetScheduler::NetScheduler(Enclave* enclave, CpuList cpulist,
                           std::shared_ptr<TaskAllocator<NetTask>> allocator,
                           Cpu global_cpu)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      global_cpuid_(global_cpu.id()),
      global_home_cpu_(global_cpu),
      global_channel_(GHOST_MAX_QUEUE_ELEMS, /*node=*/0) {
  if (!cpus().IsSet(global_cpu.id())) {
    Cpu c = cpus().Front();
    CHECK(c.valid());
    global_home_cpu_ = c;
    global_cpuid_ = c.id();
  }
}

NetScheduler::~NetScheduler() {}

void NetScheduler::EnclaveReady() {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    cs->agent = enclave()->GetAgent(cpu);
    CHECK_NE(cs->agent, nullptr);
  }
}

bool NetScheduler::Available(const Cpu& cpu) {
  CpuState* cs = cpu_state(cpu);
  return cs->agent->cpu_avail();
}

// We validate state is consistent before actually tearing anything down since
// tear-down involves pings and agents potentially becoming non-coherent as they
// are removed sequentially.
void NetScheduler::ValidatePreExitState() {
  // In the case that agent quits before all serving threads die, we forcibly
  // drain threads into CFS. num_tasks_ remains non-zero in that case.
  CHECK_EQ(run_queue_.size(), 0);
  CHECK_EQ(run_queue_low_priority_.size(), 0);
}

void NetScheduler::DumpAllTasks() {
  fprintf(stderr, "task        state   P\n");
  allocator()->ForEachTask([](Gtid gtid, const NetTask* task) {
    absl::FPrintF(stderr, "%-12s%-8d%c\n", gtid.describe(), task->run_state,
                  task->prio_boost ? 'P' : '-');
    return true;
  });
}

void NetScheduler::DumpState(const Cpu& agent_cpu, int flags) {
  if (flags & kDumpAllTasks) {
    DumpAllTasks();
  }

  if (!(flags & kDumpStateEmptyRQ) && run_queue_.empty()) {
    return;
  }

  // cpuhist stores number of unavailable CPUs in each round of GlobalSchedule.
  // cpuhist[0] accumulates the common case where there was at least one cpu
  // available to schedule tasks on.
  for (int i = 1; i < kCpuHistLen; i++) {
    fprintf(stderr, " %d:[%d],", i, cpuhist_[i]);
    cpuhist_[i] = 0;
  }

  fprintf(stderr, "\n");

  if (verbose() > 5) {
    fprintf(stderr, "SchedState: ");
    for (const Cpu& cpu : cpus()) {
      CpuState* cs = cpu_state(cpu);
      fprintf(stderr, "%d:", cpu.id());
      if (!cs->current) {
        fprintf(stderr, "none ");
      } else {
        Gtid gtid = cs->current->gtid;
        absl::FPrintF(stderr, "%s ", gtid.describe());
      }
    }
  }

  fprintf(stderr, " rq_l=%ld, globaliter: %d\n", run_queue_.size(),
          globaliter_);
  globaliter_ = 0;

  fprintf(
      stderr, "global_mig: %d, unavail/tot: %f, succ/tot: %f, other/tot: %f\n",
      global_migrations_,
      static_cast<double>(txn_fail_unavail_ * 100) /
          static_cast<double>(txn_total_),
      static_cast<double>(txn_success_ * 100) / static_cast<double>(txn_total_),
      static_cast<double>(txn_fail_other_ * 100) /
          static_cast<double>(txn_total_));

  global_migrations_ = 0;
  txn_fail_unavail_ = 0;
  txn_fail_other_ = 0;
  txn_success_ = 0;
  txn_total_ = 0;

  fprintf(stderr, "\n");
}

NetScheduler::CpuState* NetScheduler::cpu_state_of(const NetTask* t) {
  CHECK(t->cpu.valid());
  CHECK(t->oncpu() || t->pending());
  CpuState* cs = &cpu_states_[t->cpu.id()];
  CHECK((cs->current == t) || (cs->next == t));
  return cs;
}

void NetScheduler::UpdateTaskRuntime(NetTask* task,
                                        absl::Duration new_runtime,
                                        bool update_elapsed_runtime) {
  task->SetRuntime(new_runtime, update_elapsed_runtime);
}

void NetScheduler::CpuEnteringSwitchto(int cpu) {
  CHECK_GE(cpu, 0);
  CHECK_LT(cpu, MAX_CPUS);
  Cpu switchto_cpu = topology()->cpu(cpu);
  CpuState* cs = cpu_state(switchto_cpu);
  CHECK(!cs->in_switchto);
  cs->in_switchto = true;
}

void NetScheduler::CpuLeavingSwitchto(int cpu) {
  CHECK_GE(cpu, 0);
  CHECK_LT(cpu, MAX_CPUS);
  Cpu switchto_cpu = topology()->cpu(cpu);
  CpuState* cs = cpu_state(switchto_cpu);
  CHECK(cs->in_switchto);
  cs->in_switchto = false;
}

// TODO(oweisse): Infer task priority via something a bit more sophisticated
// than the task's name.
void NetScheduler::AdjustTaskPriority(NetTask& task) {
  std::string task_name = cgroup_watcher::GetTaskName(task.gtid.tid());
  absl::FPrintF(stderr, "Added task %d: %s\n", task.gtid.tid(), task_name);

  task.high_priority = true;
  if (!absl::StrContains(task_name, "EngineThread")) { //EngineThread
    absl::FPrintF(stderr, "Making the task low priority\n");
    task.high_priority = false;
  }
}

void NetScheduler::TaskNew(NetTask* task, const Message& msg) {
  const ghost_msg_payload_task_new* payload =
      static_cast<const ghost_msg_payload_task_new*>(msg.payload());

  UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                    /* update_elapsed_runtime = */ false);
  task->seqnum = msg.seqnum();
  task->run_state = NetTask::BLOCKED;  // Needed in the runnable case anyway.

  if (payload->runnable) {
    Enqueue(task);
  }

  cpu_set_t allowed_cpus;
  CPU_ZERO(&allowed_cpus);
  // TODO (ashwinch) Get affinity from payload in a few places including
  // TaskNew, when user does sched_setaffinity() and when cgroup cpuset
  // changes.
  CHECK(!sched_getaffinity(task->gtid.tid(), sizeof(allowed_cpus),
                           &allowed_cpus));
  CHECK_LE(CPU_COUNT(&allowed_cpus), MAX_CPUS);

  for (const Cpu& cpu : cpus()) {
    if (CPU_ISSET(cpu.id(), &allowed_cpus))
      task->allowed_cpulist.Set(cpu.id());
  }
  CHECK(!task->allowed_cpulist.Empty())

  AdjustTaskPriority(*task);

  num_tasks_++;
}

void NetScheduler::TaskRunnable(NetTask* task, const Message& msg) {
  const ghost_msg_payload_task_wakeup* payload =
      static_cast<const ghost_msg_payload_task_wakeup*>(msg.payload());

  CHECK(task->blocked());
  // A pending task must have called SyncCpuState when it blocked after which it
  // cannot stay in a pending state.
  CHECK(!task->pending());

  // A non-deferrable wakeup gets the same preference as a preempted task.
  // This is because it may be holding locks or resources needed by other
  // tasks to make progress.
  task->prio_boost = !payload->deferrable;

  Enqueue(task);
}

void NetScheduler::TaskDeparted(NetTask* task, const Message& msg) {
  GHOST_DPRINT(0, stderr, "Task %s departed.", task->gtid.describe());

  const ghost_msg_payload_task_departed* payload =
      reinterpret_cast<const ghost_msg_payload_task_departed*>(msg.payload());
  if (payload->from_switchto)
    CpuLeavingSwitchto(payload->cpu);

  allocator()->FreeTask(task);
  num_tasks_--;
}

void NetScheduler::TaskDead(NetTask* task, const Message& msg) {
  CHECK(task->blocked());
  allocator()->FreeTask(task);
  num_tasks_--;
}

void NetScheduler::TaskBlocked(NetTask* task, const Message& msg) {
  const ghost_msg_payload_task_blocked* payload =
      reinterpret_cast<const ghost_msg_payload_task_blocked*>(msg.payload());

  // TASK_BLOCKED could have raced with an agent-driven preemption that
  // asserted 'prio_boost'.
  task->prio_boost = false;

  // States other than the typical ON_CPU are possible here:
  // We could be QUEUED if agent preemption raced with task blocking.
  if (task->oncpu() || task->pending()) {
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuState* cs = cpu_state_of(task);
    // Task isn't pending anymore, so sync CpuState where it ran.
    if (task->pending()) {
      CHECK_EQ(cs->next, task);
      CHECK_EQ(cs->current, nullptr);
      CHECK(SyncCpuState(task->cpu));
    }
    cs->current = nullptr;
    task->run_state = NetTask::BLOCKED;
  } else if (payload->from_switchto) {
    // 'task' must have been blocked at the time it was a switchto target.
    CHECK_EQ(task->run_state, NetTask::BLOCKED);
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuLeavingSwitchto(payload->cpu);
  } else {
    // Task must be queued if agent driven preemption raced with task blocking.
    CHECK(task->queued());
    RemoveFromRunqueue(task, NetTask::BLOCKED);
  }

  if (!payload->from_switchto) {
    // Task must have been scheduled by the agent.
    task->start_submit = absl::InfiniteFuture();
  }
}

void NetScheduler::TaskPreempted(NetTask* task, const Message& msg) {
  const ghost_msg_payload_task_preempt* payload =
      reinterpret_cast<const ghost_msg_payload_task_preempt*>(msg.payload());

  task->preempted = true;
  task->prio_boost = false;

  // States other than the typical ON_CPU are possible here:
  // We could be QUEUED from a TASK_NEW that was immediately preempted, or if
  // agent initiated preemption raced with kernel preemption.
  if (task->oncpu() || task->pending()) {
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuState* cs = cpu_state_of(task);
    // Task isn't pending anymore, so sync CpuState where it ran.
    if (task->pending()) {
      CHECK_EQ(cs->next, task);
      CHECK_EQ(cs->current, nullptr);
      CHECK(SyncCpuState(task->cpu));
    }
    cs->current = nullptr;
    Enqueue(task);
  } else if (payload->from_switchto) {
    // 'task' must have been blocked at the time it was a switchto target.
    CHECK_EQ(task->run_state, NetTask::BLOCKED);
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuLeavingSwitchto(payload->cpu);
    Enqueue(task);
  } else {
    CHECK(task->queued());
  }

  if (!payload->from_switchto) {
    task->start_submit = absl::InfiniteFuture();
  }
}

void NetScheduler::TaskYield(NetTask* task, const Message& msg) {
  const ghost_msg_payload_task_yield* payload =
      reinterpret_cast<const ghost_msg_payload_task_yield*>(msg.payload());

  // TASK_YIELD could have raced with an agent-driven preemption that
  // asserted 'prio_boost'.
  task->prio_boost = false;

  // States other than the typical ON_CPU are possible here:
  // We could be QUEUED if agent-initiated preemption raced with task
  // yielding.
  if (task->oncpu() || task->pending()) {
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuState* cs = cpu_state_of(task);
    // Task isn't pending anymore, so sync CpuState where it ran.
    if (task->pending()) {
      CHECK_EQ(cs->next, task);
      CHECK_EQ(cs->current, nullptr);
      CHECK(SyncCpuState(task->cpu));
    }
    cs->current = nullptr;
    Yield(task);
  } else if (payload->from_switchto) {
    // 'task' must have been blocked at the time it was a switchto target.
    CHECK_EQ(task->run_state, NetTask::BLOCKED);
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuLeavingSwitchto(payload->cpu);
    Yield(task);
  } else {
    CHECK(task->queued());
  }

  if (!payload->from_switchto) {
    task->start_submit = absl::InfiniteFuture();
  }
}

void NetScheduler::TaskSwitchto(NetTask* task, const Message& msg) {
  const ghost_msg_payload_task_switchto* payload =
      reinterpret_cast<const ghost_msg_payload_task_switchto*>(msg.payload());

  CpuEnteringSwitchto(payload->cpu);

  // TASK_SWITCHTO could have raced with an agent-driven preemption that
  // asserted 'prio_boost'.
  task->prio_boost = false;

  // States other than the typical ON_CPU are possible here:
  // We could be QUEUED if agent preemption raced with task blocking.
  if (task->oncpu() || task->pending()) {
    UpdateTaskRuntime(task, absl::Nanoseconds(payload->runtime),
                      /* update_elapsed_runtime= */ true);
    CpuState* cs = cpu_state_of(task);
    // Task isn't pending anymore, so sync CpuState where it ran.
    if (task->pending()) {
      CHECK_EQ(cs->next, task);
      CHECK_EQ(cs->current, nullptr);
      CHECK(SyncCpuState(task->cpu));
    }
    cs->current = nullptr;
    task->run_state = NetTask::BLOCKED;
  } else {
    // Task must be queued if agent driven preemption raced with task blocking.
    CHECK(task->queued());
    RemoveFromRunqueue(task, NetTask::BLOCKED);
  }
}

bool NetScheduler::PreemptTask(NetTask* prev, NetTask* next,
                                  StatusWord::BarrierToken agent_barrier) {
  GHOST_DPRINT(2, stderr, "PREEMPT(%d)\n", prev->cpu.id());

  CHECK_NE(prev, nullptr);
  CHECK(!prev->pending());
  CHECK(prev->oncpu());

  if (prev == next) {
    return true;
  }

  CHECK(!next || !next->oncpu());

  RunRequest* req = enclave()->GetRunRequest(prev->cpu);
  if (next) {
    req->Open({
        .target = next->gtid,
        .target_barrier = next->seqnum,
        .agent_barrier = agent_barrier,
    });

    if (!req->Commit()) {
      return false;
    }
  } else {
    req->OpenUnschedule();
    CHECK(req->Commit());
  }

  CpuState* cs = cpu_state(prev->cpu);
  cs->current = next;

  if (next) {
    next->run_state = NetTask::ON_CPU;
    next->cpu = prev->cpu;
    // Normally, each task's runtime is updated when a message about that task
    // is received or when the task's sched item is updated by the orchestrator.
    // Neither of those events occurs for an agent-initiated QoS-related
    // preemption, so update the runtime of 'prev' here.
    prev->UpdateRuntime();
  }
  return true;
}

void NetScheduler::Yield(NetTask* task) {
  // An oncpu() task can do a sched_yield() and get here via NetTaskYield().
  // We may also get here if the scheduler wants to inhibit a task from
  // being picked in the current scheduling round (see GlobalSchedule()).
  CHECK(task->oncpu() || task->queued() || task->blocked());
  task->run_state = NetTask::YIELDING;
  yielding_tasks_.emplace_back(task);
}

void NetScheduler::Unyield(NetTask* task) {
  CHECK(task->yielding());

  auto it = std::find(yielding_tasks_.begin(), yielding_tasks_.end(), task);
  CHECK(it != yielding_tasks_.end());
  yielding_tasks_.erase(it);

  Enqueue(task);
}

void NetScheduler::Enqueue(NetTask* task) {
  task->run_state = NetTask::QUEUED;

  std::deque<NetTask*>* queue =
      task->high_priority ? &run_queue_ : &run_queue_low_priority_;


  if (task->prio_boost || task->preempted) {
    queue->push_front(task);
  } else {
    queue->push_back(task);
  }
}

NetTask* NetScheduler::Dequeue() {
  if (!run_queue_.empty()) {
    NetTask* task = run_queue_.front();
    run_queue_.pop_front();
    return task;
  }

  // No high priority task available. Try dequeuing a low priority task
  if (run_queue_low_priority_.empty()) return nullptr;

  NetTask* task = run_queue_low_priority_.front();
  run_queue_low_priority_.pop_front();
  return task;
}

void NetScheduler::RemoveFromRunqueue(NetTask* task, int run_state) {
  CHECK(task->queued());

  auto it = std::find(run_queue_.begin(), run_queue_.end(), task);
  if (it != run_queue_.end()) {
    run_queue_.erase(it);
  } else {
    // task is not in the normal run_queue_, check run_queue_low_priority_
    it = std::find(run_queue_low_priority_.begin(),
                   run_queue_low_priority_.end(), task);
    CHECK(it != run_queue_low_priority_.end());
    run_queue_low_priority_.erase(it);
  }

  task->run_state = run_state;
}

bool NetScheduler::SyncCpuState(const Cpu& cpu) {
  CHECK(cpu.valid());
  CpuState* cs = cpu_state(cpu);
  NetTask* next = cs->next;
  bool txn_completed = true;

  if (next) {
    CHECK(!next->queued());
    RunRequest* req = enclave()->GetRunRequest(cpu);
    CHECK(req->committed());
    cs->next = nullptr;
    if (req->state() == GHOST_TXN_COMPLETE) {
      cs->current = next;
      next->run_state = NetTask::ON_CPU;
      next->prio_boost = false;
      next->cpu = cpu;
      txn_completed = true;
      ++txn_success_;
    } else {
      Cpu uninit(Cpu::UninitializedType::kUninitialized);
      next->cpu = uninit;
      next->start_submit = absl::InfiniteFuture();
      Enqueue(next);
      txn_completed = false;
      if (req->state() == GHOST_TXN_CPU_UNAVAIL) {
        ++txn_fail_unavail_;
      } else {
        ++txn_fail_other_;
      }
    }
  }

  return txn_completed;
}

void NetScheduler::ReplaceExistingTask(const Cpu& cpu, NetTask *next) {
  CpuState* cs = cpu_state(cpu);
  NetTask* prev = cs->current;
  //CHECK(prev);
  //CHECK(prev->oncpu());
  bool in_sync = PreemptTask(prev, next, 0);

  // Even if 'preempted' is true, we're setting 'prio_boost' and only one
  // of them should be true.
  prev->preempted = false;
  prev->prio_boost = true;
  Enqueue(prev);

  if (in_sync) {
    next->prio_boost = false;
    next->preempted = false;
    cs->next = nullptr;  // in_sync is true, there is no next.
    // When in_sync is true, cs->current was already set by PreemptTask
  } else {
    cs->current = nullptr;
    Yield(next);
  }
  next->start_submit = MonotonicNow();
  ++txn_total_;
}

// TODO(ashwinch) We should test scheduler policy once the Virtual Enclave
// design has been implemented. http://shortn/_VFFcfIqGSU
void NetScheduler::GlobalSchedule(const StatusWord& agent_sw,
                                     StatusWord::BarrierToken agent_sw_last) {
  // TODO (ashwinch) Move these into header to avoid re-init in fast path.
  // Need to add a fast reset bitmap routine.
  CpuList avail_cpus = MachineTopology()->EmptyCpuList();
  CpuList avail_cpus_low_priority = MachineTopology()->EmptyCpuList();
  CpuList assigned_cpus = MachineTopology()->EmptyCpuList();
  int unavail = 0;
  absl::BitGen bitgen;

  // Construct a set of available CPUs to schedule on.
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    RunRequest* req = enclave()->GetRunRequest(cpu);
    // Available(cpu) == True iff it is either running an Idle task or a ghOSt
    // task. Since we don't support preeemption of ghost tasks yet, we only pick
    // CPUs if they're running the Idle task.
    if (!Available(cpu)) {
      unavail++;

      // If we lost this CPU to another sched class, check if we had a pending
      // transaction to synchronize. Skipping cs->next check here will break any
      // cpustate adjustment that may have occurred in the message handlers that
      // get called before GlobalSchedule.
      if (cs->next && req->committed()) {
        SyncCpuState(cpu);
      }

      continue;
    }

    if (cpu.id() == GetGlobalCPUId() || cs->current) continue;

    // CPU has a pending transaction.
    if (!req->committed()) {
      continue;
    }

    // Transaction committed. Check if we need to sync cpustate metadata
    // depending on whether the transaction was successful or not.
    SyncCpuState(cpu);

    // Check if this CPU is really available, since SynCpuState may have just
    // sync'd with a previously pending transaction.
    if (!cs->current && !cs->in_switchto)
      avail_cpus.Set(cpu.id());
    else if(cs->current && absl::StrContains(cgroup_watcher::GetTaskName(cs->current->gtid.tid()), "memcached") && \
                 absl::Bernoulli(bitgen, 1.0/50.0) && (MonotonicNow() - cs->current->start_submit) > absl::Microseconds(300))
      avail_cpus_low_priority.Set(cpu.id());
    else if(cs->current && !absl::StrContains(cgroup_watcher::GetTaskName(cs->current->gtid.tid()), "memcached"))
      avail_cpus_low_priority.Set(cpu.id());
  }

  while (!avail_cpus.Empty() || !avail_cpus_low_priority.Empty()) {
    NetTask* next = Dequeue();

    if (!next) break;

    CHECK(!next->pending());

    // The chosen task is oncpu or has pending messages. Make it ineligible
    // for selection in this scheduling round.
    //
    // Specifically this catches a race where the last task in a switchto
    // chain schedules (e.g. due to a preemption). The kernel reconciles
    // the switchto by producing a TASK_WAKEUP followed by TASK_PREEMPTED.
    //
    // The 'seqnum != task_barrier' check detects that the agent is making
    // a scheduling decision with incomplete information (i.e. it has seen
    // TASK_WAKEUP but not TASK_PREEMPTED) and punts on the task in this
    // round.
    //
    // N.B. kernel guarantees that when 'next' is observed to be offcpu
    // its barrier will be stable.
    if (next->status_word.on_cpu() ||
        next->seqnum != next->status_word.barrier()) {
      Yield(next);
      continue;
    }

    Cpu cpu{Cpu::UninitializedType::kUninitialized};

    if (!avail_cpus.Empty())
      cpu = PickNextCpu(avail_cpus, *next, enforce_affinity_mask_);

    // We couldn't find an available cpu. If the task is high priority, let's
    // see if we can preempt a lower priority task.
    if (!cpu.valid() && !avail_cpus_low_priority.Empty())
    {
      cpu = PickNextCpu(avail_cpus_low_priority, *next, enforce_affinity_mask_);
      if (cpu.valid()) {
        avail_cpus_low_priority.Clear(cpu.id());
        ReplaceExistingTask(cpu, next);
        continue;
      }
    }

    // This task cannot be assigned to any avail CPUs in this round.
    if (!cpu.valid()) {
      Yield(next);
      continue;
    }

    CpuState* cs = cpu_state(cpu);
    CHECK_EQ(cs->next, nullptr);
    CHECK_EQ(cs->current, nullptr);

    cs->next = next;
    // Open the RunRequest for a batched commit later.
    RunRequest* req = enclave()->GetRunRequest(cpu);
    req->Open({
        .target = next->gtid,
        .target_barrier = next->seqnum,
    });
    avail_cpus.Clear(cpu.id());
    assigned_cpus.Set(cpu.id());

    // Mark all tasks PENDING although their txns may complete inline after
    // SubmitRunRequests below. Their run states will be adjusted when we
    // asynchronously check for txn completions.
    next->run_state = NetTask::PENDING;
    next->cpu = cpu;
    if (next->preempted) {
      // fold 'preempted' into 'prio_boost' in case txn fails to commit.
      next->prio_boost = true;
      next->preempted = false;
    }
    next->start_submit = MonotonicNow();
    ++txn_total_;
  }

  // Tell the kernel to run Tasks on desired CPUs.
  // Send all Open RunRequests as a batched commit.
  if (!assigned_cpus.Empty()) enclave()->SubmitRunRequests(assigned_cpus);

  // Yielding tasks are moved back to the runqueue having skipped one round
  // of scheduling decisions.
  if (!yielding_tasks_.empty()) {
    for (NetTask* t : yielding_tasks_) {
      CHECK_EQ(t->run_state, NetTask::YIELDING);
      Enqueue(t);
    }
    yielding_tasks_.clear();
  }

  // On large systems the cpuhist can get long and noisy. So keep the last slot
  // of the histogram for accumulating unavail CPUs that exceed the histogram
  // size.
  if (unavail > (kCpuHistLen - 1))
    cpuhist_[kCpuHistLen - 1]++;
  else
    cpuhist_[unavail]++;

  globaliter_++;
}

void NetScheduler::PickNextGlobalCPU() {
  int32_t global_cpu_id = GetGlobalCPUId();
  Cpu target(Cpu::UninitializedType::kUninitialized);

  for (const Cpu& cpu : cpus()) {
    RunRequest* req = enclave()->GetRunRequest(cpu);

    // Check if global-agent can return to its home cpu if not already on it.
    if ((global_home_cpu_.id() != global_cpu_id) &&
        Available(global_home_cpu_)) {
      RunRequest* req = enclave()->GetRunRequest(global_home_cpu_);
      // Before landing the global agent back on its `home` CPU, check if it has
      // an un-reaped transaction completion, otherwise we risk preempting a
      // running task without synchronizing CpuState on that CPU.
      if (req->committed()) {
        SyncCpuState(global_home_cpu_);
        target = global_home_cpu_;
        break;
      }
    }

    // restrict search to cpus not in_switchto.
    if (cpu_state(cpu)->in_switchto) continue;

    // Only consider cpus in the same numa node as preferred cpu.
    if (cpu.numa_node() != global_home_cpu_.numa_node()) continue;

    // Pick another target CPU that is currently not running the global-agent.
    if ((cpu.id() != global_cpu_id) && Available(cpu) && req->committed()) {
      target = cpu;
      break;
    }
  }

  if (!target.valid()) return;

  CpuState* cs = cpu_state(target);
  RunRequest* req = enclave()->GetRunRequest(target);
  CHECK(req->committed());

  if (cs->next) {
    SyncCpuState(target);
  }

  NetTask* prev = cs->current;

  if (prev) {
    CHECK(prev->oncpu());
    // Vacate CPU for running Global agent.
    bool in_sync = PreemptTask(prev, nullptr, 0);
    CHECK(in_sync);
    // Set 'prio_boost' to make it reschedule asap in case 'prev' is
    // holding a critical resource.
    //
    // XXX ideally this would be 'preempted = true' since 'prio_boost'
    // indicates a non-deferrable wakeup but this could run afoul of
    // the '!preempted' check in DispatchMessage().
    prev->prio_boost = true;
    Enqueue(prev);
    cs->current = nullptr;
  }

  SetGlobalCPU(target);
  enclave()->GetAgent(target)->Ping();
  ++global_migrations_;
}

void NetScheduler::SetSchedCFS(const NetTask* task) {
  struct sched_param param = {0};
  int policy = SCHED_OTHER;
  absl::FPrintF(stderr, "task tid: %d\n", task->gtid.tid());
  const int ret = sched_setscheduler(task->gtid.tid(), policy, &param);

  if (!ret) {
    CHECK_EQ(sched_getscheduler(task->gtid.tid()), policy);
  }

  // A successful setsched on a task will deliver a MSG_TASK_DEPARTED. The
  // message handler will FreeTask(task) and release its status word in the
  // kernel.
}

std::unique_ptr<NetScheduler> SingleThreadNetScheduler(Enclave* enclave,
                                                       CpuList cpulist,
                                                       Cpu global_cpu) {
  auto allocator =
      std::make_shared<SingleThreadMallocTaskAllocator<NetTask>>();
  auto scheduler = absl::make_unique<NetScheduler>(
      enclave, std::move(cpulist), std::move(allocator), global_cpu);
  return scheduler;
}

void NetAgent::AgentThread() {
  Channel& global_channel = global_scheduler_->GetDefaultChannel();
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));
  if (verbose() > 1) {
    absl::PrintF("> %s tid: %d\n", gtid().describe(), gtid().tid());
  }
  SignalReady();
  WaitForEnclaveReady();

  PeriodicEdge debug_out(absl::Seconds(1));

  while (!Finished()) {
    StatusWord::BarrierToken agent_barrier = status_word().barrier();
    // Check if we're assigned as the Global agent. If not, then block and
    // yield this CPU.
    if (cpu().id() != global_scheduler_->GetGlobalCPUId()) {
      RunRequest* req = enclave()->GetRunRequest(cpu());
      req->LocalYield(agent_barrier, /* flags */ 0);
    } else {
      // Check if the kernel wants us to vacate this CPU. If so, pick another
      // CPU to assign as the global agent and yield current CPU. This happens
      // when a task of higher priority becomes runnable on the global cpu.
      if (boosted_priority()) {
        global_scheduler_->PickNextGlobalCPU();
        continue;
      }

      // Drain the IPC message queue and update the task state machinery
      // accordingly.
      Message msg;
      while (!(msg = global_channel.Peek()).empty()) {
        global_scheduler_->DispatchMessage(msg);
        global_channel.Consume(msg);
      }

      // Make a scheduling decision based on updated task states and
      // availability of CPUs.
      global_scheduler_->GlobalSchedule(status_word(), agent_barrier);

    }
  }
}

}  //  namespace ghost

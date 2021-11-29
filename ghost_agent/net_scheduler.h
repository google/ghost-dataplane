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

#ifndef PRODKERNEL_GHOST_SCHEDULERS_NET_NET_SCHEDULER_H_
#define PRODKERNEL_GHOST_SCHEDULERS_NET_NET_SCHEDULER_H_

#include "lib/agent.h"
#include "lib/scheduler.h"
#include "lib/topology.h"
#include "shared/prio_table.h"

namespace ghost {

// An implementation of Task, bespoke to Net scheduling requirements.
// Maintains task state which is updated by events from the kernel via IPC
// messages and status words via shared memory.
struct NetTask : public Task {
  explicit NetTask(Gtid muppet_task_gtid, struct ghost_sw_info sw_info)
      : Task(muppet_task_gtid, sw_info) {}
  ~NetTask() override = default;

  // Task blocked in kernel.
  inline bool blocked() const { return run_state == BLOCKED; }

  // Task queued in userspace runqueue.
  inline bool queued() const { return run_state == QUEUED; }

  // Task scheduled on a CPU by agent. NOTE: This is different than the
  // SW_TASK_ONCPU, which reflects whether task is on a CPU from the kernel
  // point of view.
  inline bool oncpu() const { return run_state == ON_CPU; }

  // Task called yield() or agent put it in temp runqueue to make it skip a
  // round of scheduling. e.g. a task previously preempted by agent hasn't
  // departed its CPU, and yet makes its way to head of runqueue.
  inline bool yielding() const { return run_state == YIELDING; }
  // TODO (ashwinch) Functions inside class are implicitly inline.
  // Task was assigned as part of a transaction that didn't commit in the
  // scheduler loop.
  inline bool pending() const { return run_state == PENDING; }

  // TODO(ashwinch) Use enum class. go/totw/86
  enum {
    BLOCKED = 0,
    QUEUED = 1,
    ON_CPU = 2,
    YIELDING = 3,
    PENDING = 4,
  };
  absl::Time start_submit = absl::InfiniteFuture();
  int run_state = BLOCKED;
  // The last CPU this task ran on.
  Cpu cpu{Cpu::UninitializedType::kUninitialized};

  // Sets time spent on CPU by this task as reported by the kernel. Runtime may
  // be updated via explicit syscall, status word update or via message payload.
  void SetRuntime(absl::Duration runtime, bool update_elapsed_runtime);

  // Used when runtime needs to be updated either via explicit syscall or status
  // word. e.g. for cases like agent driven preemption, where kernel does not
  // deliver a message against preempted task with runtime in message payload.
  void UpdateRuntime();

  // Priority boosting for jumping past regular fifo ordering in the runqueue.
  //
  // A task's priority is boosted on a kernel preemption or a !deferrable
  // wakeup - basically when it may be holding locks or other resources
  // that prevent other tasks from making progress.
  bool prio_boost = false;

  // Cumulative runtime in ns.
  absl::Duration runtime = absl::ZeroDuration();
  // Accrued CPU time in ns.
  absl::Duration elapsed_runtime = absl::ZeroDuration();

  // Whether the last execution was preempted or not.
  bool preempted = false;

  // Set of permitted CPUs based on cpu_set_t acquired from sched_getaffinity().
  CpuList allowed_cpulist = MachineTopology()->EmptyCpuList();

  // Is this task high priority? Reserved for the Hub and Spokes of USPS.
  bool high_priority;
};

// Heap comparator. "Smaller" elements are at the top of the heap and are popped
// first.
struct NetTaskLess {
  bool operator()(const NetTask* e1, const NetTask* e2) const {
    // Priority boosted and preempted tasks should go before those that aren't.
    if (e1->prio_boost && !e2->prio_boost) return true;
    if (e2->prio_boost && !e1->prio_boost) return false;
    if (e1->preempted && !e2->preempted) return true;
    if (e2->preempted && !e1->preempted) return false;

    // Tasks that have been run for less time should be preferred.
    //
    // TODO(ashwinch): this is biased against long running tasks and
    // therefore should be based on elapsed_runtime in the last-n-msecs.
    return e1->elapsed_runtime < e2->elapsed_runtime;
  }
};

// Implements workload specific scheduler harness. Includes implementation of
// common callbacks such as message dispatch and update of task state machinery.
// Additionally includes any customization required to maintain workload
// specific runqueue information.
class NetScheduler : public BasicDispatchScheduler<NetTask> {
 public:
  // TODO(ashwinch) Replace with Enclave&
  explicit NetScheduler(Enclave* enclave, CpuList cpulist,
                        std::shared_ptr<TaskAllocator<NetTask>> allocator,
                        Cpu global_cpu);
  ~NetScheduler() final;

  void EnclaveReady() final;
  Channel& GetDefaultChannel() final { return global_channel_; };

  // Callbacks dispatched in response to kernel IPC messages for 'task'. Updates
  // task state in each callback and adjusts runqueue as needed.
  void TaskNew(NetTask* task, const Message& msg) final;
  void TaskRunnable(NetTask* task, const Message& msg) final;
  void TaskDead(NetTask* task, const Message& msg) final;
  void TaskDeparted(NetTask* task, const Message& msg) final;
  void TaskYield(NetTask* task, const Message& msg) final;
  void TaskBlocked(NetTask* task, const Message& msg) final;
  void TaskSwitchto(NetTask* task, const Message& msg) final;
  void TaskPreempted(NetTask* task, const Message& msg) final;

  // Count of tasks in userspace runqueue.
  bool Empty() { return num_tasks_ == 0; }
  // Sanity checks before shutting down agents.
  void ValidatePreExitState();

  // Find and remove task from runqueue.
  void RemoveFromRunqueue(NetTask* task, int run_state);
  // Sanity checks on all tasks currently in runqueue.
  void CheckRunQueue();

  // Main scheduler loop. Implements bespoke scheduler policy to assign tasks
  // to remote CPUs.
  void GlobalSchedule(const StatusWord& agent_sw,
                      StatusWord::BarrierToken agent_sw_last);

  // Sync per cpu metadata after submitting a transaction. Returns True if there
  // was no pending transaction or it if completed successfully. Returns False
  // if the transaction failed and enqueues pending task back in runqueue on
  // failure.
  bool SyncCpuState(const Cpu& cpu);

  // Get CPU ID of currently assigned global agent.
  int32_t GetGlobalCPUId() {
    return global_cpuid_.load(std::memory_order_acquire);
  }
  // Update global agent CPU ID assignment.
  void SetGlobalCPU(const Cpu& cpu) {
    global_cpuid_.store(cpu.id(), std::memory_order_release);
  }

  void SetEnforceAffinityMask(bool enforce_affinity) {
    enforce_affinity_mask_ = enforce_affinity;
  }

  // Called from TaskNew, to set task priority properly.
  void AdjustTaskPriority(NetTask& task);

  // Kick out (preempt) existing task on `cpu` with `next`.
  void ReplaceExistingTask(const Cpu &cpu, NetTask *next);

  // Find another CPU to assign as the global agent when currently assigned
  // global agent gets preempted.
  void PickNextGlobalCPU();

  // Calls into the kernel to migrate 'task' out to CFS.
  void SetSchedCFS(const NetTask* task);

  // Periodic dump of any runqueue, CPU state for debug.
  void DumpState(const Cpu& cpu, int flags) final;
  std::atomic<bool> debug_runqueue_ = false;

  static constexpr int kDebugRunqueue = 1;

 private:
  void CpuEnteringSwitchto(int cpu);
  void CpuLeavingSwitchto(int cpu);

  // Agent driven preemption to evict 'prev' and replace with 'next' on its CPU.
  bool PreemptTask(NetTask* prev, NetTask* next,
                   StatusWord::BarrierToken agent_barrier);
  // Put task in yielding_tasks runqueue. This runqueue serves as a temp queue
  // for tasks that call yield() or for when the agent wants to inhibit the task
  // from its current round of scheduling. Typically used when we want to bias
  // against choosing this task for scheduling temporarily.
  void Yield(NetTask* task);
  // Bring task out of temp runqueue and requeue back into global runqueue.
  void Unyield(NetTask* task);
  // Enqueue into global runqueue.
  void Enqueue(NetTask* task);
  // Dequeue from global runqueue.
  NetTask* Dequeue();
  // Debug to dump all task state as seen currently by agents.
  void DumpAllTasks();

  // Update elapsed runtime of task spent on CPU as tracked by the kernel into
  // userspace task metadata.
  void UpdateTaskRuntime(NetTask* task, absl::Duration new_runtime,
                         bool update_elapsed_runtime);
  // Check if a CPU is available to schedule a task on. True iff CPU is either
  // running a task from our sched class or the idle thread.
  bool Available(const Cpu& cpu);

  // A per CPU struct to capture scheduler state.
  struct CpuState {
    // Currently assigned task on this CPU.
    NetTask* current = nullptr;
    // Task chosen to run next on this CPU.
    NetTask* next = nullptr;
    // Agent assigned for this CPU.
    const Agent* agent = nullptr;
    bool in_switchto = false;
  } ABSL_CACHELINE_ALIGNED;

  CpuState* cpu_state_of(const NetTask* task);
  CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }
  CpuState cpu_states_[MAX_CPUS];

  // CPU ID assigned to run the global agent.
  std::atomic<int32_t> global_cpuid_;
  Cpu global_home_cpu_;
  Channel global_channel_;

  bool enforce_affinity_mask_;

  // Counts number of tasks in global runqueue.
  int num_tasks_ = 0;
  // Global runqueues.
  std::deque<NetTask*> run_queue_;
  std::deque<NetTask*> run_queue_low_priority_;
  // Runqueue to house Yielded tasks or temporarily inhibited tasks.
  std::vector<NetTask*> yielding_tasks_;
  // Counts number of scheduler iterations during each debug_out.Edge
  int globaliter_ = 0;
  // Histogram of num of unavailable CPUs during each scheduler iteration.
  // The 11th slot accumulates > 10 unavailable CPUs.
  static constexpr int kCpuHistLen = 12;
  int cpuhist_[kCpuHistLen];

  // Stats dumped at the periodic debug edge.
  // e.g. PeriodicEdge debug_out(absl::Seconds(1)); will dump these every ~1
  // second. Stats are reset at end of each period.
  int global_migrations_ = 0;  // Num of times global agent migrated.
  int txn_fail_unavail_ = 0;   // Txn failures due to CPU unavailability.
  int txn_fail_other_ = 0;  // Txn failures for reasons other than CPU_UNAVAIL.
  int txn_success_ = 0;     // Txn's successfully completed.
  int txn_total_ = 0;       // Total txn's made in GlobalSchedule().
};

std::unique_ptr<NetScheduler> SingleThreadNetScheduler(Enclave* enclave,
                                                       CpuList cpulist,
                                                       Cpu global_cpu);

// Operates as the Global or Satellite agent depending on input from the
// global_scheduler->GetGlobalCPU callback.
class NetAgent : public Agent {
 public:
  // TODO (ashwinch) Make this explicit go/totw/142. Here and in other
  // schedulers too. Also convert to use references since all args are
  // non-optional. http://go/cstyle#Inputs_and_Outputs.
  NetAgent(Enclave* enclave, Cpu cpu, NetScheduler* global_scheduler)
      : Agent(enclave, cpu), global_scheduler_(global_scheduler) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return global_scheduler_; }

 private:
  NetScheduler* global_scheduler_;
};

class NetConfig : public AgentConfig {
 public:
  NetConfig() {}
  NetConfig(Topology* topology, CpuList cpulist, Cpu global_cpu)
      : AgentConfig(topology, std::move(cpulist)),
        global_cpu_(global_cpu),
        enforce_affinity_mask_(false) {}

  Cpu global_cpu_{Cpu::UninitializedType::kUninitialized};

  // Set whether affinity masks per task should be respected. Systems using
  // affinity typically have classical schedulers in mind. NetScheduler can
  // choose whether to respect them or not.
  bool enforce_affinity_mask_;
};

// An global agent scheduler.  It runs a single-threaded Net scheduler on the
// global_cpu.
template <class ENCLAVE>
class FullNetAgent : public FullAgent<ENCLAVE> {
 public:
  explicit FullNetAgent(NetConfig config) : FullAgent<ENCLAVE>(config) {
    global_scheduler_ = SingleThreadNetScheduler(
        &this->enclave_, *this->enclave_.cpus(), config.global_cpu_);
    global_scheduler_->SetEnforceAffinityMask(config.enforce_affinity_mask_);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullNetAgent() override {
    global_scheduler_->ValidatePreExitState();

    // Terminate global agent before satellites to avoid a false negative error
    // from ghost_run(). e.g. when the global agent tries to schedule on a CPU
    // without an active satellite agent.
    int global_cpuid = global_scheduler_->GetGlobalCPUId();

    if (this->agents_.front()->cpu().id() != global_cpuid) {
      // Bring the current globalcpu agent to the front.
      for (auto it = this->agents_.begin(); it != this->agents_.end(); it++) {
        if (((*it)->cpu().id() == global_cpuid)) {
          auto d = std::distance(this->agents_.begin(), it);
          std::iter_swap(this->agents_.begin(), this->agents_.begin() + d);
          break;
        }
      }
    }

    CHECK_EQ(this->agents_.front()->cpu().id(), global_cpuid);

    this->TerminateAgentTasks();
  }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return absl::make_unique<NetAgent>(&this->enclave_, cpu,
                                       global_scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case NetScheduler::kDebugRunqueue:
        global_scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<NetScheduler> global_scheduler_;
};

}  // namespace ghost

#endif  // PRODKERNEL_GHOST_SCHEDULERS_NET_NET_SCHEDULER_H_

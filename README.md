# Multi-Container Runtime

## Team Information

- Debargho Banerji (`PES1UG24CS141`)
- Debaditya Chakrabarti (`PES1UG24CS140`)

## Project Summary

This project implements a lightweight Linux container runtime in C with two integrated components:

1. A long-running user-space supervisor in [`boilerplate/engine.c`](boilerplate/engine.c) that launches and tracks multiple containers, exposes a CLI, collects logs, and manages container lifecycle.
2. A kernel-space memory monitor in [`boilerplate/monitor.c`](boilerplate/monitor.c) that registers container host PIDs, performs periodic RSS checks, reports soft-limit violations, and kills processes that cross the hard limit.

The runtime supports the required CLI contract from `project-guide.md`:

```text
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs <id>
engine stop <id>
```

## Repository Contents

- [`Makefile`](Makefile): top-level wrapper for `make`, `make ci`, and `make clean`
- [`boilerplate/Makefile`](boilerplate/Makefile): builds user-space binaries and the kernel module
- [`boilerplate/engine.c`](boilerplate/engine.c): supervisor daemon, CLI client path, control IPC, logging pipeline
- [`boilerplate/monitor.c`](boilerplate/monitor.c): Linux kernel module for memory monitoring
- [`boilerplate/monitor_ioctl.h`](boilerplate/monitor_ioctl.h): shared `ioctl` definitions
- [`boilerplate/memory_hog.c`](boilerplate/memory_hog.c): memory-stressing workload
- [`boilerplate/cpu_hog.c`](boilerplate/cpu_hog.c): CPU-bound workload
- [`boilerplate/io_pulse.c`](boilerplate/io_pulse.c): I/O-oriented workload
- [`project-guide.md`](project-guide.md): assignment specification

## Environment and Prerequisites

The project guide requires:

- Ubuntu 22.04 or 24.04 in a VM
- Secure Boot disabled for module loading
- No WSL

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Optional environment preflight:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
cd ..
```

## Build, Load, and Run Instructions

### 1. Build the project

From the repository root:

```bash
make ci
make
```

- `make ci` builds only the user-space binaries and matches the CI-safe smoke check.
- `make` builds the user-space binaries and `boilerplate/monitor.ko`.

### 2. Prepare the container root filesystems

Create an Alpine base rootfs and one writable copy per live container:

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

If a helper workload should run inside a container, copy it into the corresponding rootfs first:

```bash
cp ./boilerplate/memory_hog ./rootfs-alpha/
cp ./boilerplate/cpu_hog ./rootfs-beta/
cp ./boilerplate/io_pulse ./rootfs-beta/
```

### 3. Load the kernel module

```bash
sudo insmod ./boilerplate/monitor.ko
ls -l /dev/container_monitor
```

Expected result: `/dev/container_monitor` exists before the supervisor starts.

### 4. Start the supervisor

Run the supervisor in one terminal:

```bash
sudo ./boilerplate/engine supervisor ./rootfs-base
```

The supervisor creates the control socket at `/tmp/mini_runtime.sock`, owns the logging pipeline, tracks container metadata, and reaps exited children.

### 5. Launch and inspect containers

In a second terminal:

```bash
sudo ./boilerplate/engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./boilerplate/engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96 --nice 5
sudo ./boilerplate/engine ps
sudo ./boilerplate/engine logs alpha
```

Foreground execution:

```bash
sudo ./boilerplate/engine run memtest ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 48
echo $?
```

According to the current `engine.c` implementation:

- default limits are `40 MiB` soft and `64 MiB` hard
- `run` returns the child exit code, or `128 + signal` if signaled
- if the `run` client receives `SIGINT` or `SIGTERM`, it forwards a stop request to the supervisor and keeps waiting for final status

### 6. Run scheduler experiments

Example CPU-priority comparison:

```bash
cp ./boilerplate/cpu_hog ./rootfs-alpha/
cp ./boilerplate/cpu_hog ./rootfs-beta/

sudo ./boilerplate/engine start cpu-a ./rootfs-alpha /cpu_hog --nice 0
sudo ./boilerplate/engine start cpu-b ./rootfs-beta /cpu_hog --nice 10
sudo ./boilerplate/engine ps
sudo ./boilerplate/engine logs cpu-a
sudo ./boilerplate/engine logs cpu-b
```

Example CPU-vs-I/O comparison:

```bash
cp ./boilerplate/cpu_hog ./rootfs-alpha/
cp ./boilerplate/io_pulse ./rootfs-beta/

sudo ./boilerplate/engine start cpu-job ./rootfs-alpha /cpu_hog --nice 0
sudo ./boilerplate/engine start io-job ./rootfs-beta /io_pulse --nice 0
sudo ./boilerplate/engine logs cpu-job
sudo ./boilerplate/engine logs io-job
```

Record completion time, throughput, or responsiveness from the produced logs and compare the runs.

### 7. Stop containers, inspect logs, and unload everything

```bash
sudo ./boilerplate/engine stop alpha
sudo ./boilerplate/engine stop beta
dmesg | tail -n 50
sudo pkill -INT -f "./boilerplate/engine supervisor"
sudo rmmod monitor
make clean
```

`make clean` removes user-space build outputs, `logs/`, and `/tmp/mini_runtime.sock`.

## Implementation Overview

### Supervisor architecture

The runtime uses one binary in two roles:

- `engine supervisor ...`: long-running daemon
- `engine <command> ...`: short-lived CLI client

The supervisor maintains an in-memory linked list of container metadata, including:

- container ID
- rootfs path
- command
- host PID
- start time
- memory limits
- nice value
- log path
- exit code or signal
- final reason
- stop intent

### Control IPC

The CLI and supervisor communicate over a UNIX domain socket at `/tmp/mini_runtime.sock`. The client serializes a `control_request_t`, the supervisor processes it, and replies with `control_response_t`.

This control path is separate from the logging path, satisfying the project requirement for two distinct IPC mechanisms.

### Logging pipeline

Container `stdout` and `stderr` are redirected to a pipe owned by the supervisor. Producer threads read from container pipes and push `log_item_t` entries into a bounded shared buffer. A dedicated logger thread removes entries and appends them to `logs/<container-id>.log`.

The bounded buffer uses:

- one mutex for mutual exclusion
- one `not_empty` condition variable for consumers
- one `not_full` condition variable for producers

### Kernel memory monitor

The kernel module exposes `/dev/container_monitor`. The supervisor registers and unregisters host PIDs through `ioctl`. The module stores monitored entries in a kernel linked list guarded by a mutex and uses a periodic timer to:

- detect exited tasks and remove stale entries
- log the first soft-limit crossing
- send `SIGKILL` on hard-limit crossing

## Demo with Screenshots

The project guide requires annotated screenshots for the items below. The current repository checkout does not contain screenshot assets, so this section is structured as a checklist for the required demo evidence. Replace each placeholder with an actual image and a one-line caption before submission.

| # | Required Demo Item | What to Capture | Status |
| --- | --- | --- | --- |
| 1 | Multi-container supervision | Two or more containers running under one supervisor | Pending screenshot |
| 2 | Metadata tracking | `engine ps` output with tracked metadata | Pending screenshot |
| 3 | Bounded-buffer logging | `logs/<id>.log` plus evidence of producer/logger activity | Pending screenshot |
| 4 | CLI and IPC | CLI command issued and supervisor response over control channel | Pending screenshot |
| 5 | Soft-limit warning | `dmesg` or log showing soft-limit warning | Pending screenshot |
| 6 | Hard-limit enforcement | `dmesg` or log showing kill, plus `ps` metadata reflecting final state | Pending screenshot |
| 7 | Scheduling experiment | Side-by-side experiment output or measurement table | Pending screenshot |
| 8 | Clean teardown | No zombies, threads exiting, supervisor shutdown evidence | Pending screenshot |

Suggested caption format:

- `Figure 1. Two containers (alpha and beta) are running concurrently under a single supervisor process.`
- `Figure 2. The ps command shows host PID, state, configured limits, and final reason for tracked containers.`

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime isolates containers using Linux namespaces created through `clone()`. PID namespaces give the container its own process-ID view, so `ps` inside the container sees a container-local process tree instead of host PIDs. UTS namespaces isolate hostname state, allowing each container to set its own hostname without affecting the host. Mount namespaces isolate mount-table changes, which is necessary so the container can mount its own `/proc` without mutating the host mount layout.

Filesystem isolation is implemented with `chroot()` into the container-specific rootfs directory. That changes the process-visible filesystem root to the assigned `rootfs-alpha`, `rootfs-beta`, and so on. This does not create a separate kernel or separate physical resources; all containers still share the same host kernel, scheduler, memory manager, and device model. That is why container isolation is lighter weight than a virtual machine but also why kernel-enforced mechanisms remain necessary for safe resource control.

### 2. Supervisor and Process Lifecycle

A long-running parent supervisor is useful because container state is not reconstructible from independent one-shot CLI processes. The supervisor is the stable owner of metadata, control IPC, log handling, and child reaping. When it receives `start` or `run`, it creates a child with `clone()`, records the host PID and configuration, and keeps tracking the container after the CLI process exits.

This design also makes signal handling coherent. `SIGCHLD` notifies the supervisor that child state changed, so it can call `waitpid()` and avoid zombies. `stop` sets an internal `stop_requested` flag before sending termination so the final reason can distinguish manual stop from hard-limit kill. Without a stable parent process, there would be no reliable place to classify termination causes, keep logs open, or preserve container metadata across multiple CLI calls.

### 3. IPC, Threads, and Synchronization

The project uses two IPC mechanisms for two different purposes. The control plane uses a UNIX domain socket because the traffic is request/response oriented and needs structured messages between short-lived clients and the supervisor. The logging plane uses pipes because container `stdout` and `stderr` are stream-oriented file descriptors that naturally connect process output to the supervisor.

There are two major shared data structures. The first is the container metadata list, protected by `metadata_lock`. Without that mutex, concurrent CLI handling, child reaping, logging lookups, and stop requests could race, causing stale pointers, inconsistent state, or duplicate container IDs. The second is the bounded log buffer, protected by a mutex and coordinated with `not_empty` and `not_full` condition variables. Without those primitives, producers could overwrite unread entries, consumers could read partially written data, and the system could deadlock or lose shutdown notifications when the buffer transitions between empty and full states.

The shutdown path matters as much as the steady state. Producer threads exit when the pipe closes, the bounded buffer has an explicit shutdown flag, and the logger thread drains remaining entries before exiting. That is the mechanism that prevents dropped output when containers terminate abruptly.

### 4. Memory Management and Enforcement

RSS measures the resident portion of a process's address space: pages currently present in physical memory. It does not include all virtual memory that a process may have mapped, nor does it directly represent total system-wide memory pressure from caches or unrelated kernel allocations. It is still a practical signal for per-process enforcement because it reflects memory that is actively resident and consuming RAM.

Soft and hard limits are different policies because they serve different operational goals. A soft limit is advisory: it warns that the process is consuming more memory than intended but allows it to continue. A hard limit is mandatory: once crossed, the process is killed. Separating these thresholds gives the system both observability and enforcement.

The enforcement mechanism belongs in kernel space because the kernel has authoritative visibility into process memory and the power to act even if the user-space supervisor is delayed, blocked, or compromised. A pure user-space monitor could observe too late, lose races with the workload, or fail to terminate the target reliably under stress.

### 5. Scheduling Behavior

This project uses the runtime as an experimental platform rather than implementing a scheduler. The `--nice` flag changes the priority of container processes, which allows experiments about fairness and CPU allocation. Two CPU-bound workloads with different nice values reveal how Linux favors the less-niced task with more CPU time. A CPU-bound workload running beside an I/O-oriented workload demonstrates a different goal: the scheduler tries to preserve responsiveness for the task that sleeps and wakes frequently while still making progress on the CPU-heavy task.

The important OS point is that Linux scheduling is a tradeoff among throughput, fairness, and responsiveness. The project makes those goals visible by running identical or contrasting workloads under controlled priority settings and comparing observable outcomes such as completion time or progress logs.

## Design Decisions and Tradeoffs

### Namespace isolation

Choice: use `clone()` with PID, UTS, and mount namespaces, then `chroot()` into a per-container rootfs.

Tradeoff: `chroot()` is simpler to implement than `pivot_root()`, but it provides weaker filesystem isolation semantics and depends more heavily on correct setup.

Justification: for an assignment-scale runtime, this keeps the implementation tractable while still exercising the core OS mechanisms required by the project.

### Supervisor architecture

Choice: use a long-running supervisor with short-lived CLI clients.

Tradeoff: this introduces more moving pieces than a single-process launcher, including persistent metadata, IPC handling, and signal coordination.

Justification: it is the right model for multi-container lifecycle management, background execution, `ps`, `logs`, and correct child reaping.

### IPC and logging

Choice: split control IPC and logging IPC into a UNIX domain socket plus pipe-based bounded-buffer logging.

Tradeoff: the design is more complex than direct terminal output or a single shared IPC mechanism because it adds threads, queueing, and synchronization.

Justification: the responsibilities are fundamentally different. Control traffic is discrete request/response messaging, while logs are asynchronous byte streams. Separating them keeps the design clearer and makes concurrency reasoning easier.

### Kernel monitor

Choice: store monitored processes in a kernel linked list protected by a mutex, with periodic timer-based RSS checks.

Tradeoff: periodic checking is simpler than fully event-driven accounting, but it introduces sampling granularity and some enforcement delay.

Justification: the timer model is sufficient for demonstrating soft and hard memory limits, keeps the module readable, and integrates cleanly with the assignment's `ioctl`-driven registration model.

### Scheduling experiments

Choice: use the provided CPU-bound, memory-oriented, and I/O-oriented workloads with different `nice` values.

Tradeoff: these are synthetic workloads, so they do not represent all real applications.

Justification: they isolate scheduler behavior well enough to make comparisons defensible and reproducible inside the required VM setup.

## Scheduler Experiment Results

The project guide requires raw measurements and at least one comparison. The current repository does not include recorded benchmark outputs, so this section should be completed after running the experiments in the target VM.

Recommended result table format:

| Experiment | Workload A | Workload B | Configuration | Measured Outcome | Interpretation |
| --- | --- | --- | --- | --- | --- |
| CPU priority comparison | `cpu_hog` | `cpu_hog` | `nice 0` vs `nice 10` | Fill after run | Lower nice should receive more CPU time |
| CPU vs I/O comparison | `cpu_hog` | `io_pulse` | both `nice 0` | Fill after run | I/O-oriented task should remain comparatively responsive |

Include:

- raw timestamps or completion times from logs
- any `ps` snapshots used during the run
- a short explanation of what the measurements show about fairness, throughput, or responsiveness

## Cleanup Verification Checklist

Use this checklist while preparing the final demo:

- `SIGCHLD` reaping leaves no zombies
- producer threads exit when container pipes close
- logger thread drains the queue before shutdown
- control socket is removed on cleanup
- `logs/` and temporary files are removed by `make clean`
- kernel monitored entries are freed on module unload

## Notes for Final Submission

This README now matches the structure required by `project-guide.md`, but two evidence-driven sections still require VM-generated artifacts before final submission:

- annotated screenshots for the demo checklist
- measured scheduler experiment results

Those should be filled with real outputs from your Ubuntu VM run rather than guessed or synthesized values.

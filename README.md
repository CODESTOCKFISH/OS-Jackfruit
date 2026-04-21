# Multi-Container Runtime

## 1. Team Information

- Team Member 1: Debargho Banerji - `PES1UG24CS141`
- Team Member 2: Debaditya Chakrabarti - `PES1UG24CS140`

## 2. Build, Load, and Run Instructions

### Environment

- Ubuntu 22.04 or 24.04 VM
- Secure Boot disabled for kernel module loading
- Root privileges required for module load, namespace setup, `chroot()`, and `/proc` mounting

### Install Dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget
```

### Build and Preflight

From the repository root:

```bash
make
```

CI-safe smoke check:

```bash
make ci
```

Optional environment check:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
cd ..
```

### Prepare the Alpine Root Filesystem

```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp boilerplate/cpu_hog rootfs-base/
cp boilerplate/io_pulse rootfs-base/
cp boilerplate/memory_hog rootfs-base/
```

Create one writable copy per container before each run:

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### Load the Kernel Module and Start the Supervisor

```bash
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor
sudo ./boilerplate/engine supervisor ./rootfs-base
```

### CLI Contract Implemented

```bash
sudo ./boilerplate/engine start <id> <container-rootfs> "<command>" [--soft-mib N] [--hard-mib N] [--nice N]
sudo ./boilerplate/engine run   <id> <container-rootfs> "<command>" [--soft-mib N] [--hard-mib N] [--nice N]
sudo ./boilerplate/engine ps
sudo ./boilerplate/engine logs <id>
sudo ./boilerplate/engine stop <id>
```

### Cleanup

Stop the supervisor with `Ctrl+C`, then clean up:

```bash
pgrep -af "/boilerplate/engine" || echo "no engine processes"
ps -ef | grep "[d]efunct" || echo "no defunct processes"
sudo rmmod monitor
make clean
```

## 3. Demo with Screenshots

The current `screenshots/` folder still contains your earlier placeholder captures. Because this pass focused on fixing the code first, the commands below are the exact sequences to rerun on your Ubuntu VM and capture as the final submission screenshots.

### Screenshot 1 - Multi-container Supervision

What to show:

- One supervisor process managing at least two live containers at the same time
- `engine ps` output showing both tracked containers

Commands:

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp boilerplate/cpu_hog rootfs-alpha/
cp boilerplate/io_pulse rootfs-beta/

sudo ./boilerplate/engine start alpha ./rootfs-alpha "/cpu_hog 30" --soft-mib 48 --hard-mib 128 --nice 0
sudo ./boilerplate/engine start beta ./rootfs-beta "/io_pulse 60 200" --soft-mib 48 --hard-mib 128 --nice 0
sudo ./boilerplate/engine ps
```

### Screenshot 2 - Metadata Tracking

What to show:

- `engine ps` output with container ID, PID, state, limits, exit status, reason, start time, and log path

Commands:

```bash
sudo ./boilerplate/engine ps
```

### Screenshot 3 - Bounded-Buffer Logging

What to show:

- Container output retrieved through `engine logs`
- Persistent `logs/<id>.log` file contents
- Evidence of the pipeline threads or producer/consumer path

Commands:

```bash
cp -a rootfs-base rootfs-log
cp boilerplate/io_pulse rootfs-log/

sudo ./boilerplate/engine start logger ./rootfs-log "/io_pulse 120 200" --soft-mib 48 --hard-mib 128 --nice 0
ps -T -C engine -o pid,tid,comm,stat,time,args
sudo ./boilerplate/engine logs logger
sudo tail -n 20 logs/logger.log
```

### Screenshot 4 - CLI and IPC

What to show:

- The supervisor running in one terminal
- A client command such as `start`, `ps`, or `stop` in another terminal
- The supervisor response proving the UNIX domain socket control path is active

Commands:

```bash
cp -a rootfs-base rootfs-ipc
cp boilerplate/cpu_hog rootfs-ipc/

sudo ./boilerplate/engine start ipc ./rootfs-ipc "/cpu_hog 60" --soft-mib 48 --hard-mib 128 --nice 0
sudo ./boilerplate/engine stop ipc
```

### Screenshot 5 - Soft-Limit Warning

What to show:

- Kernel log warning for a soft-limit event
- `engine ps` showing the container still tracked rather than killed

Commands:

```bash
cp -a rootfs-base rootfs-soft
cp boilerplate/memory_hog rootfs-soft/

sudo ./boilerplate/engine start softwarn ./rootfs-soft "/memory_hog 8 1000" --soft-mib 24 --hard-mib 256 --nice 0
sleep 4
sudo ./boilerplate/engine ps
sudo dmesg | tail -n 30
```

### Screenshot 6 - Hard-Limit Enforcement

What to show:

- Kernel log entry for hard-limit enforcement
- `engine ps` showing the final reason as `hard_limit_killed`

Commands:

```bash
cp -a rootfs-base rootfs-hard
cp boilerplate/memory_hog rootfs-hard/

sudo ./boilerplate/engine start hardkill ./rootfs-hard "/memory_hog 8 500" --soft-mib 24 --hard-mib 40 --nice 0
sleep 4
sudo ./boilerplate/engine ps
sudo dmesg | tail -n 30
```

### Screenshot 7 - Scheduling Experiment

What to show:

- Two concurrent CPU-bound containers with different `nice` values
- Their measured completion times

Commands:

```bash
rm -f hi.out lo.out
cp -a rootfs-base rootfs-hi
cp -a rootfs-base rootfs-lo
cp boilerplate/cpu_hog rootfs-hi/
cp boilerplate/cpu_hog rootfs-lo/

(
  start=$(date +%s.%N)
  sudo ./boilerplate/engine run hi ./rootfs-hi "/cpu_hog 20" --nice 0
  end=$(date +%s.%N)
  awk -v s="$start" -v e="$end" 'BEGIN { printf "hi elapsed=%.3f sec\n", e-s }'
) >hi.out 2>&1 &

(
  start=$(date +%s.%N)
  sudo ./boilerplate/engine run lo ./rootfs-lo "/cpu_hog 20" --nice 10
  end=$(date +%s.%N)
  awk -v s="$start" -v e="$end" 'BEGIN { printf "lo elapsed=%.3f sec\n", e-s }'
) >lo.out 2>&1 &

wait
cat hi.out
cat lo.out
```

### Screenshot 8 - Clean Teardown

What to show:

- No lingering supervisor or container processes
- No defunct processes
- `/dev/container_monitor` removed after module unload

Commands:

```bash
pgrep -af "/boilerplate/engine" || echo "no engine processes"
ps -ef | grep "[d]efunct" || echo "no defunct processes"
sudo rmmod monitor
test -e /dev/container_monitor && ls /dev/container_monitor || echo "/dev/container_monitor removed"
```

## 4. Engineering Analysis

### Isolation Mechanisms

Each container is created with `clone()` using `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS`, so the child gets an isolated PID namespace, hostname namespace, and mount table. The child marks the mount tree private, enters its assigned filesystem with `chroot()`, changes to `/`, and mounts a fresh `/proc`. That isolates process visibility and the filesystem view, while the host kernel, scheduler, and physical memory remain shared.

### Supervisor and Process Lifecycle

The long-running supervisor owns the global container table, logging pipeline, signal handling, and cleanup. Short-lived CLI clients connect over a UNIX domain socket, send one request, receive one response, and exit. `SIGCHLD` handling plus `waitpid(..., WNOHANG)` allows the supervisor to reap child processes promptly and preserve final metadata such as exit code, signal, and stop reason.

### IPC, Threads, and Synchronization

The control path uses a UNIX domain socket between CLI clients and the supervisor. The logging path uses pipes from each container's `stdout` and `stderr` into the supervisor. Producer threads read pipe data and push it into a bounded circular buffer. A logger thread pops entries and writes them into per-container log files. The bounded buffer uses one mutex and two condition variables so producers block when the queue is full, consumers block when it is empty, and shutdown can wake both sides cleanly. Container metadata uses a separate mutex to avoid races between CLI commands, child reaping, and stop escalation.

In kernel space, the monitored-process list uses a mutex because both the `ioctl` path and the delayed-work monitor path run in sleepable context. That lets the monitor safely call RSS helpers and free list nodes without doing sleepable work in timer interrupt context.

### Memory Management and Enforcement

The kernel module tracks RSS because RSS reflects the amount of physical memory currently resident for a process. It does not equal total virtual address space, and it does not capture every kernel-side memory overhead, but it is a useful enforcement metric. A soft limit is a warning threshold and is reported only once. A hard limit is an enforcement threshold and results in `SIGKILL`. Kernel-space enforcement is important because the kernel has direct authority over process accounting and signal delivery.

### Scheduling Behavior

The runtime uses Linux's default scheduler and exposes `nice` values through the CLI so experiments can vary scheduling weight without changing the kernel scheduler itself. CPU-bound workloads compete directly for processor time, while I/O-bound workloads naturally sleep between operations. Running them concurrently shows how Linux balances fairness, responsiveness, and throughput.

## 5. Design Decisions and Tradeoffs

### Namespace Isolation

- Design choice: PID, UTS, and mount namespaces with `chroot()`
- Tradeoff: `chroot()` is simpler than `pivot_root()`, but it is a weaker filesystem jail
- Justification: it satisfies the assignment scope with lower implementation complexity

### Supervisor Architecture

- Design choice: one long-lived supervisor plus short-lived CLI clients
- Tradeoff: this requires an explicit control-plane IPC design
- Justification: it centralizes metadata, reaping, logging, and cleanup

### IPC and Logging

- Design choice: UNIX domain socket for commands and pipe-based logging into a bounded buffer
- Tradeoff: the design is more complex than direct writes because it needs queue synchronization and thread management
- Justification: it clearly demonstrates two IPC mechanisms and a producer-consumer logging pipeline

### Kernel Monitor

- Design choice: character device plus `ioctl` registration with periodic delayed-work RSS checks
- Tradeoff: sampling is simpler than event-driven accounting but not instantaneous
- Justification: it keeps the kernel-user contract small while still supporting soft warnings and hard kills

### Scheduling Experiment

- Design choice: reusable workload binaries with `nice` as the scheduling control
- Tradeoff: `nice` adjusts scheduler weight but does not expose every scheduling knob
- Justification: it is easy to reproduce and directly tied to Linux scheduling policy

## 6. Scheduler Experiment Results

Record the final Ubuntu VM measurements from Screenshot 7 here before submission. The runtime is already wired for the experiment above; you only need to replace the sample values with the numbers from your own VM run.

Example format:

| Experiment | Workloads | Configuration | Measurement | Observation |
| --- | --- | --- | --- | --- |
| CPU vs CPU | `hi` vs `lo` using `cpu_hog 20` | `nice 0` vs `nice 10` | `hi = <fill> sec`, `lo = <fill> sec` | Lower `nice` should receive a slightly larger CPU share and usually finish earlier |

Raw output block to replace with your actual run:

```text
Container 'hi' finished: exited
hi elapsed=<fill> sec

Container 'lo' finished: exited
lo elapsed=<fill> sec
```

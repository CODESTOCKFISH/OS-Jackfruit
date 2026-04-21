# OS Jackfruit Project

## Team Members

- Debargho Banerji (`PES1UG24CS141`)
- Debaditya Chakrabarti (`PES1UG24CS140`)

## Overview

This project explores a simplified container runtime environment using the OS Jackfruit boilerplate. It demonstrates how system-level utilities interact with CPU, memory, and I/O, along with partial container execution features.

## Environment

- OS: Ubuntu Linux
- Execution model: terminal-based
- Language: C

## Setup Steps

```bash
cd OS-Jackfruit
cd boilerplate
make ci
```

## Execution Workflow

1. Verify the repository:

```bash
ls
```

2. Navigate to the boilerplate directory:

```bash
cd boilerplate
```

3. Build the project:

```bash
make ci
```

4. Run the engine interface:

```bash
./engine
```

5. Run the memory load simulation:

```bash
./memory_hog
```

6. Run the CPU load simulation:

```bash
./cpu_hog
```

7. Run the I/O simulation:

```bash
./io_pulse
```

8. Attempt container execution:

```bash
sudo ./boilerplate/engine run test ./rootfs-base "/bin/echo hello"
```

9. Monitor processes:

```bash
sudo ./boilerplate/engine ps
```

10. Inspect logs:

```bash
sudo ./boilerplate/engine logs test
```

## Screenshots

All outputs are stored in the `screenshots/` folder.

| Step | File |
| --- | --- |
| `ls` | `1_ls.png` |
| `cd` | `2_cd.png` |
| `make` | `3_make.png` |
| `engine` | `4_engine.png` |
| `memory` | `5_memory.png` |
| `cpu` | `6_cpu.png` |
| `io` | `7_io.png` |
| `run` | `8_run.png` |
| `ps` | `9_ps.png` |
| `logs` | `10_logs.png` |

## Observations

- Resource simulation commands worked correctly.
- Engine commands are partially implemented.
- Some outputs (`run`, `ps`, `logs`) show limited functionality.

## Learning Outcomes

- Understood basic container-like execution.
- Learned resource monitoring for CPU, memory, and I/O.
- Gained hands-on Linux terminal experience.

## Conclusion

This project demonstrates system-level resource behavior and a basic container engine interface using the OS Jackfruit boilerplate.

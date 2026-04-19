# OS Jackfruit Project
Team Members : Debargho Banerji :PES1UG24CS141
               Debaditya Chakrabarti :PES1UG24CS140

##  Overview
This project explores a simplified container runtime environment using the OS Jackfruit boilerplate. It demonstrates how system-level utilities interact with CPU, memory, and I/O, along with partial container execution features.

---

##  Environment
- OS: Ubuntu Linux
- Terminal-based execution
- Language: C (precompiled binaries provided)

---

## ⚙️ Setup Steps

Bash

cd OS-Jackfruit
cd boilerplate
make ci
##  Execution Workflow

markdown
---

##  Execution Workflow

### 1. Verify Repository
bash
ls
2. Navigate to Boilerplate
cd boilerplate
3. Build the Project
make ci
4.Engine Interface
./engine
5. Memory Load Simulation
./memory_hog
6. CPU Load Simulation
./cpu_hog
7. I/O Simulation
./io_pulse
8. Container Execution Attempt
sudo ./boilerplate/engine run test ./rootfs-base "/bin/echo hello"
9. Process Monitoring
sudo ./boilerplate/engine ps
10. Logs Inspection
sudo ./boilerplate/engine logs test


---

## 📸 Screenshots section



```markdown
## Screenshots

All outputs are stored in the `screenshots/` folder:

| Step | File |
|------|------|
| ls | 1_ls.png |
| cd | 2_cd.png |
| make | 3_make.png |
| engine | 4_engine.png |
| memory | 5_memory.png |
| cpu | 6_cpu.png |
| io | 7_io.png |
| run | 8_run.png |
| ps | 9_ps.png |
| logs | 10_logs.png |

---

##  Observations

- Resource simulation commands worked correctly
- Engine commands are partially implemented
- Some outputs (run, ps, logs) show limited functionality

---

## Learning Outcomes

- Understood basic container-like execution
- Learned resource monitoring (CPU, memory, I/O)
- Gained hands-on Linux terminal experience

---

## Conclusion

This project demonstrates system-level resource behavior and a basic container engine interface using the OS Jackfruit boilerplate.

---

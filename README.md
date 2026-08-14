# Evaluation-of-Dynamic-Task-Allocation-and-Reliability-in-Resource-Constrained-ESP8266-Mesh-Networks

## 🚀 Research Abstract
As massive AI datacenters scale, network bottlenecks force expensive processors to sit idle, wasting immense power and water. This repository contains the source code for a hardware simulator built with an ESP8266 mesh network to test how AI workloads crash under pressure.

## 🛠️ Hardware & Software Stack
Based on the implementation in `master.ino`, the simulator utilizes the following stack:
* **Microcontrollers:** 3x ESP8266 modules (1 Master Router, 2 Worker Nodes)
* [cite_start]**Networking:** `painlessMesh` library for asynchronous mesh topology[cite: 1].
* [cite_start]**Serialization:** `ArduinoJson` for fast, lightweight data packet transmission[cite: 1].
* [cite_start]**Telemetry Display:** Hardware-integrated OLED dashboard using the `U8g2` library (SH1106 128x64)[cite: 1, 4].
* [cite_start]**Control Center:** An embedded `ESP8266WebServer` hosting a dynamic Web UI for real-time control and CSV data extraction[cite: 1, 5, 13].

## 📦 Task Architecture
Instead of arbitrary data, the master node functions as an active task generator simulating heavy AI computational requests. 
* [cite_start]**Payload Structure:** Tasks are formatted as JSON packets containing a sequence ID, a generation timestamp (`send_ts_ms`), and a fixed processing requirement (`workload_ms: 100`) instructing the worker to process for 100 milliseconds[cite: 55, 56].
* [cite_start]**Stress Testing:** The Web UI allows real-time dynamic scaling of the task generation rate (5, 10, 20, or 30 tasks per second) over configurable time limits (60 to 600 seconds)[cite: 19].
* [cite_start]**Fault Tolerance:** The master continuously checks for offline nodes (5000ms threshold) and `ACK` timeouts[cite: 2, 58]. [cite_start]The UI even includes a "Simulate Kill" function to purposefully crash worker nodes and test the network's recovery logic[cite: 32, 33, 97].

## 🔀 Load Balancing Algorithms
The master node actively routes the generated tasks using three distinct algorithms:
1. [cite_start]**Round Robin (`RR`):** Blindly alternates distributing tasks equally among alive workers[cite: 12, 50, 51].
2. [cite_start]**Greedy (`GREEDY`):** Averages the load by sending tasks to the first worker that explicitly reports its state as "idle"[cite: 12, 51].
3. [cite_start]**Load Aware (`LOADAWARE`):** Mathematically routes tasks to the worker with the lowest current queue length (`queueLen`)[cite: 12, 52, 53].

## 📊 Findings
By stress-testing these algorithms across 50,000 tasks, the raw telemetry logs revealed a critical hardware paradox:
* **The Winner:** Load Aware routing flawlessly balanced heavy workloads under standard conditions.
* **The Glitch:** At maximum stress (20 tasks/sec), the Load Aware algorithm triggered an impossible **4.29-billion-millisecond latency underflow**. 
* **The Conclusion:** The worker chip logged a task completion timestamp *before* the task was officially sent, resulting in a negative time jump. Because the hardware cannot process negative time, the 32-bit integer wrapped backward to its absolute maximum limit. This validates Clock Skew theorem on this hardware, proving that dynamic AI routing will crash without active time-synchronization filters.

## 📺 Video Demonstration
[link]

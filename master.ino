#define ENABLE_WEB_UI
#define ENABLE_SIMULATION

#include <Arduino.h>
#include <painlessMesh.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <map>
#include <vector>
#include <algorithm>

// -------------------- Config --------------------
#define MESH_PREFIX "Sankeerth_Cluster"
#define MESH_PASSWORD "20092009"
#define MESH_PORT 5555

const uint32_t ACK_TIMEOUT_MS = 5000;
const uint32_t OFFLINE_THRESHOLD_MS = 5000;
const uint8_t MAX_RETRIES = 3;

// Mapped correctly to your Master Node wiring
const uint8_t LED_MASTER_GREEN = 14;  
const uint8_t LED_MASTER_ORANGE = 13; 

// ---- OLED SETUP (SOFTWARE I2C) ----
U8G2_SH1106_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ D2, /* data=*/ D1, /* reset=*/ U8X8_PIN_NONE);

// -------------------- Globals --------------------
painlessMesh mesh;
ESP8266WebServer server(80);

String csvBuffer = ""; 

struct WorkerInfo {
  uint32_t lastSeenMs = 0;
  String state = "unknown";
  uint16_t queueLen = 0;
  int rssi = 0;
  bool simulatedOffline = false;
  uint32_t recoveryStartMs = 0;
  uint32_t tasksCompleted = 0;
  uint32_t autoRestoreMs = 0; 
};
std::map<uint32_t, WorkerInfo> workers;

struct MasterTask {
  uint32_t seq;
  uint32_t send_ts_ms;
  uint8_t retries;
  uint32_t assignedTo;
};
std::map<uint32_t, MasterTask> outstanding;

uint32_t seqCounter = 0;
uint32_t tasksSent = 0;
uint32_t tasksCompleted = 0;

// ---- Research Variables ----
bool generatorRunning = false;
uint32_t generatorRatePerSec = 5;
uint32_t testDurationMs = 120000; // Default to 2 minutes (120s)
uint32_t testStartMs = 0;
uint32_t lastGenMs = 0;
uint32_t currentRunTasksSent = 0; // THE FIX: Brought back for batch tracking!

bool batchInProgress = false;
uint32_t batchCompleted = 0;
uint32_t batchLost = 0;
uint32_t batchStartMs = 0;

enum AllocAlgo { RR, GREEDY, LOADAWARE };
AllocAlgo currentAlgo = LOADAWARE;
std::vector<uint32_t> workerList;
size_t rrIndex = 0;

// -------------------- Web UI --------------------
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8"><title>Sankeerth Research UI</title>
<style>
:root { --bg-color: #f5f5f7; --text-color: #111; --box-bg: #ffffff; --card-bg: #fafafa; --border-color: #ccc; --stats-color: #86868b;}
body.dark-mode { --bg-color: #121212; --text-color: #f1f1f1; --box-bg: #1e1e1e; --card-bg: #2a2a2a; --border-color: #444; --stats-color: #aaaaaa;}
body {font-family:-apple-system, sans-serif; background:var(--bg-color); color:var(--text-color); padding: 20px; transition: background 0.3s, color 0.3s;} 
.container { max-width: 600px; margin: auto; background: var(--box-bg); padding: 20px; border-radius: 12px; box-shadow: 0 4px 10px rgba(0,0,0,0.2); transition: background 0.3s;}
.w {border:1px solid var(--border-color); padding:15px; margin:10px 0; border-radius: 8px; background: var(--card-bg); transition: background 0.3s;}
button { padding: 8px 15px; border-radius: 6px; border: none; background: #007aff; color: white; cursor: pointer; font-weight: bold; margin-right: 5px; margin-bottom: 5px;}
button.danger { background: #ff3b30; }
button.success { background: #34c759; }
button.warning { background: #ff9500; }
button.dark-toggle { background: #555555; float: right; margin-top: 5px; }
button.download { background: #34c759; width: 100%; padding: 15px; font-size: 16px; margin-top: 15px;}
select { padding: 8px; border-radius: 6px; margin-bottom: 5px; font-weight: bold; background: var(--box-bg); color: var(--text-color); border: 1px solid var(--border-color);}
.inline-select { margin-right: 10px; }
#csv-counter { font-size: 14px; color: var(--stats-color); margin-top: 10px; display: block; font-weight: bold;}
.controls-box { border: 1px solid var(--border-color); padding: 15px; border-radius: 8px; margin-bottom: 15px; background: var(--box-bg); transition: background 0.3s;}
</style>
</head><body>
<div class="container">
<button class="dark-toggle" onclick="toggleDarkMode()">&#x1F319; Theme</button>
<h2 style="margin-top:0;">Sankeerth Mesh</h2>
<div id="status" style="margin-bottom:15px; font-weight:bold;">Loading...</div>

<div class="controls-box">
  <select id="algo" onchange="setAlgo()">
    <option value="roundrobin">Round Robin</option>
    <option value="greedy">Greedy</option>
    <option value="loadaware" selected>Load Aware</option>
  </select>
  <br>
  <select id="rate" onchange="updateParams()">
    <option value="5">5 Tasks/sec</option>
    <option value="10">10 Tasks/sec</option>
    <option value="20">20 Tasks/sec</option>
    <option value="30">30 Tasks/sec</option>
  </select>
  <select id="time_limit" onchange="updateParams()">
    <option value="60">1 Min (60s)</option>
    <option value="120" selected>2 Mins (120s)</option>
    <option value="300">5 Mins (300s)</option>
    <option value="600">10 Mins (600s)</option>
  </select>
  <br><br>
  <button onclick="startTest()">&#x25B6; Start Test Run</button>
  <button class="warning" onclick="fetch('/control',{method:'POST',body:JSON.stringify({action:'stop'})})">&#x23F8; Pause</button>
  <button class="danger" onclick="resetTest()">&#x21BB; Reset & Clear</button>
</div>

<button class="download" onclick="downloadCSV()">&#x2B07; DOWNLOAD CSV DATA</button>
<span id="csv-counter">Data Points Collected: 0</span>

<h3>Active Workers</h3>
<div id="workers"></div>
</div>
<script>
if(localStorage.getItem('theme') === 'dark') { document.body.classList.add('dark-mode'); }
function toggleDarkMode() {
  document.body.classList.toggle('dark-mode');
  localStorage.setItem('theme', document.body.classList.contains('dark-mode') ? 'dark' : 'light');
}

let csvData = "algorithm,seq,worker_id,send_ts,comp_ts,latency_ms,status,rssi\n";
let dataCount = 0;
let uiTimers = {}; 

async function pollData() {
  try {
    let res = await fetch('/data');
    let text = await res.text();
    if (text.length > 0) {
      csvData += text;
      dataCount += (text.match(/\n/g) || []).length;
      document.getElementById('csv-counter').innerText = "Data Points Collected: " + dataCount;
    }
  } catch(e) {}
}
setInterval(pollData, 1000);

function downloadCSV() {
    let blob = new Blob([csvData], { type: 'text/csv' });
    let url = window.URL.createObjectURL(blob);
    let a = document.createElement('a');
    a.href = url;
    a.download = 'Sankeerth_Research_Data.csv';
    a.click();
}

function updateParams() {
  let r = document.getElementById('rate').value;
  let t = document.getElementById('time_limit').value;
  fetch('/control', {method:'POST', body:JSON.stringify({action:'update_params', rate:r, time_limit:t})});
}

function startTest() {
  let r = document.getElementById('rate').value;
  let t = document.getElementById('time_limit').value;
  fetch('/control', {method:'POST', body:JSON.stringify({action:'start', rate:r, time_limit:t})});
}

function resetTest() {
  csvData = "algorithm,seq,worker_id,send_ts,comp_ts,latency_ms,status,rssi\n";
  dataCount = 0;
  document.getElementById('csv-counter').innerText = "Data Points Collected: 0";
  fetch('/control', {method:'POST', body:JSON.stringify({action:'reset'})});
}

function killNode(id) {
  let el = document.getElementById('time_' + id);
  let t = el ? parseInt(el.value) : 0;
  if (t > 0) { uiTimers[id] = t; } else { uiTimers[id] = -1; }
  fetch('/control',{method:'POST',body:JSON.stringify({action:'simulate_offline', node:id, duration:t})});
}

function restoreNode(id) {
  delete uiTimers[id]; 
  fetch('/control',{method:'POST',body:JSON.stringify({action:'restore', node:id})});
}

setInterval(() => {
  for (let id in uiTimers) {
    if (uiTimers[id] > 0) {
      uiTimers[id]--;
      let span = document.getElementById('timer_span_' + id);
      if (span) span.innerHTML = ' | <span style="color:red; font-weight:bold;">Dead: ' + uiTimers[id] + 's left</span>';
      if (uiTimers[id] === 0) { restoreNode(id); } 
    }
  }
}, 1000);

async function refresh(){
  let s=await fetch('/status'); let j=await s.json();
  
  document.getElementById('status').innerText=`Time: ${j.elapsed_s}s / ${j.limit_s}s | Total Comp: ${j.tasks_completed} | Algo: ${j.algorithm}`;
  
  let w=document.getElementById('workers'); 
  let html = '';
  
  j.workers.forEach(function(x){ 
    let currentDropdown = document.getElementById('time_' + x.id);
    let selectedTime = currentDropdown ? currentDropdown.value : "30";

    let tVal = uiTimers[x.id];
    let deadTxt = '<span id="timer_span_'+x.id+'"></span>'; 
    if (tVal > 0) {
      deadTxt = '<span id="timer_span_'+x.id+'"> | <span style="color:red; font-weight:bold;">Dead: '+tVal+'s left</span></span>';
    } else if (tVal === -1 || x.simulated_offline) {
      deadTxt = '<span id="timer_span_'+x.id+'"> | <span style="color:red; font-weight:bold;">Dead (Manual)</span></span>';
    }
    
    let selectMenu = `<select id="time_${x.id}" class="inline-select">
      <option value="0" ${selectedTime=="0"?"selected":""}>Manual</option>
      <option value="10" ${selectedTime=="10"?"selected":""}>10s</option>
      <option value="20" ${selectedTime=="20"?"selected":""}>20s</option>
      <option value="30" ${selectedTime=="30"?"selected":""}>30s</option>
      <option value="40" ${selectedTime=="40"?"selected":""}>40s</option>
      <option value="60" ${selectedTime=="60"?"selected":""}>60s</option></select>`;

    html += '<div class="w">' +
      '<b>Worker ID:</b> '+x.id+' <br><b>State:</b> '+x.state+ deadTxt +' | <b>Queue:</b> '+x.queue_length+'<br><br>' +
      selectMenu +
      '<button class="danger" onclick="killNode('+x.id+')">Simulate Kill</button>' +
      '<button class="success" onclick="restoreNode('+x.id+')">Restore Node</button>' +
      '</div>';
  });
  w.innerHTML = html;
}
function setAlgo(){ fetch('/control',{method:'POST',body:JSON.stringify({action:'set_algo',algo:document.getElementById('algo').value})}); }
setInterval(refresh, 2000); 
refresh();
</script>
</body></html>
)rawliteral";

// -------------------- Utilities --------------------
uint32_t nowMs() { return millis(); }

void emitCSVRow(const char *row) { 
  Serial.println(row); 
  csvBuffer += String(row) + "\n";
  if (csvBuffer.length() > 4000) csvBuffer = ""; 
}

const char* getAlgoName() {
  if (currentAlgo == RR) return "RoundRobin";
  if (currentAlgo == GREEDY) return "Greedy";
  return "LoadAware";
}

// -------------------- Mesh Callbacks --------------------
void receivedCallback(uint32_t from, String &msg) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg)) return;
  const char *type = doc["type"];
  if (!type) return;

  if (strcmp(type, "heartbeat") == 0) {
    WorkerInfo &w = workers[from];
    w.lastSeenMs = nowMs();
    w.state = doc["state"] | "idle";
    w.queueLen = doc["queue_length"] | 0;
    w.rssi = doc["rssi"] | 0;
    if (std::find(workerList.begin(), workerList.end(), from) == workerList.end()) workerList.push_back(from);
  } 
  else if (strcmp(type, "ack") == 0) {
    uint32_t seq = doc["seq"] | 0;
    uint32_t completion_ts = doc["completion_ts_ms"] | nowMs();
    auto it = outstanding.find(seq);
    if (it != outstanding.end()) {
      MasterTask t = it->second;
      uint32_t latency = completion_ts - t.send_ts_ms;
      char buf[128];
      
      snprintf(buf, sizeof(buf), "%s,%u,%u,%u,%u,%u,completed,%d", 
               getAlgoName(), (unsigned)t.seq, (unsigned)from, 
               (unsigned)t.send_ts_ms, (unsigned)completion_ts, 
               (unsigned)latency, workers[from].rssi);
               
      emitCSVRow(buf);
      outstanding.erase(it);
      tasksCompleted++;
      workers[from].tasksCompleted++;
      if (batchInProgress) batchCompleted++;
      if (workers[from].simulatedOffline && workers[from].recoveryStartMs) {
        workers[from].simulatedOffline = false;
        workers[from].recoveryStartMs = 0;
      }
    }
  }
}

void newConnectionCallback(uint32_t nodeId) {
  if (std::find(workerList.begin(), workerList.end(), nodeId) == workerList.end()) workerList.push_back(nodeId);
}

// -------------------- Task Allocation --------------------
uint32_t pickWorkerForTask() {
  uint32_t now = nowMs();
  std::vector<uint32_t> alive;
  for (auto id : workerList) {
    if (workers.count(id) && (now - workers[id].lastSeenMs) <= OFFLINE_THRESHOLD_MS && !workers[id].simulatedOffline) alive.push_back(id);
  }
  if (alive.empty()) return 0;
  if (currentAlgo == RR) {
    rrIndex = rrIndex % alive.size();
    return alive[rrIndex++];
  } else if (currentAlgo == GREEDY) {
    for (auto id : alive) if (workers[id].state == "idle") return id;
    return alive[0];
  } else { 
    uint32_t best = alive[0];
    uint16_t bestQ = workers[best].queueLen;
    for (auto id : alive) {
      if (workers[id].queueLen < bestQ) { best = id; bestQ = workers[id].queueLen; }
    }
    return best;
  }
}

void sendTask(uint32_t nodeId, uint32_t seq, uint32_t workload_ms) {
  StaticJsonDocument<256> doc;
  doc["type"] = "task";
  doc["seq"] = seq;
  doc["send_ts_ms"] = nowMs();
  doc["workload_ms"] = workload_ms;
  String out;
  serializeJson(doc, out);
  mesh.sendSingle(nodeId, out);
}

void checkAckTimeouts() {
  uint32_t now = nowMs();
  std::vector<uint32_t> toRequeue;
  for (auto it = outstanding.begin(); it != outstanding.end(); ) {
    MasterTask &t = it->second;
    if ((now - t.send_ts_ms) > ACK_TIMEOUT_MS) {
      if (t.retries < MAX_RETRIES) {
        t.retries++;
        toRequeue.push_back(t.seq);
        it++;
      } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s,%u,0,%u,0,0,lost,0", getAlgoName(), (unsigned)t.seq, (unsigned)t.send_ts_ms);
        emitCSVRow(buf);
        if (batchInProgress) batchLost++;
        it = outstanding.erase(it); 
      }
    } else it++;
  }
  for (auto seq : toRequeue) {
    if (!outstanding.count(seq)) continue;
    MasterTask &t = outstanding[seq];
    uint32_t newWorker = pickWorkerForTask();
    if (newWorker) {
      t.assignedTo = newWorker;
      t.send_ts_ms = nowMs();
      sendTask(newWorker, seq, 100);
    }
  }
}

void checkAutoRestore() {
  uint32_t now = nowMs();
  for (auto &kv : workers) {
    if (kv.second.simulatedOffline && kv.second.autoRestoreMs > 0) {
      if (now >= kv.second.autoRestoreMs) {
        StaticJsonDocument<128> c;
        c["type"] = "cmd";
        c["cmd"] = "restore";
        String out; serializeJson(c, out);
        mesh.sendSingle(kv.first, out);
        kv.second.simulatedOffline = false;
        kv.second.autoRestoreMs = 0; 
      }
    }
  }
}

void generatorLoop() {
  if (!generatorRunning) return;
  
  uint32_t now = nowMs();
  
  if (now - testStartMs >= testDurationMs) {
    generatorRunning = false;
    digitalWrite(LED_MASTER_ORANGE, LOW);
    
    if (batchInProgress) {
      uint32_t totalTime = now - testStartMs;
      char buf[128]; snprintf(buf, sizeof(buf), "%s,SUMMARY,ALL,0,0,%u,batch_complete,0", getAlgoName(), (unsigned)totalTime); emitCSVRow(buf);
      emitCSVRow("----------------,---,---,---,---,---,----------------,---");
      batchInProgress = false; 
    }
    return;
  }
  
  digitalWrite(LED_MASTER_ORANGE, HIGH);
  uint32_t intervalMs = 1000 / max(1U, generatorRatePerSec);
  
  if (now - lastGenMs >= intervalMs) {
    lastGenMs = now;
    seqCounter++;
    MasterTask t;
    t.seq = seqCounter;
    t.send_ts_ms = now;
    t.retries = 0;
    
    uint32_t worker = pickWorkerForTask();
    if (worker == 0) {
      t.assignedTo = 0;
      outstanding[t.seq] = t;
    } else {
      t.assignedTo = worker;
      outstanding[t.seq] = t;
      sendTask(worker, t.seq, 100); 
      tasksSent++;
      currentRunTasksSent++; // THE FIX: Counting for batch summary!
    }
  }
}

// -------------------- Web Handlers --------------------
void handleRoot() { server.send_P(200, "text/html", index_html); }
void handleData() { server.send(200, "text/plain", csvBuffer); csvBuffer = ""; }
void handleStatus() {
  StaticJsonDocument<768> doc;
  doc["tasks_sent"] = tasksSent;
  doc["tasks_completed"] = tasksCompleted;
  
  doc["limit_s"] = testDurationMs / 1000;
  doc["elapsed_s"] = generatorRunning ? (nowMs() - testStartMs) / 1000 : 0;
  doc["run_sent"] = currentRunTasksSent;
  
  doc["algorithm"] = getAlgoName();
  JsonArray arr = doc.createNestedArray("workers");
  uint32_t now = nowMs();
  for (auto &kv : workers) {
    JsonObject w = arr.createNestedObject();
    w["id"] = kv.first;
    w["state"] = kv.second.state;
    w["queue_length"] = kv.second.queueLen;
    w["simulated_offline"] = kv.second.simulatedOffline;
    uint32_t rem = 0;
    if (kv.second.simulatedOffline && kv.second.autoRestoreMs > now) {
      rem = (kv.second.autoRestoreMs - now) / 1000;
    }
    w["kill_remaining"] = rem;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}
void handleControl() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  String body = server.hasArg("plain") ? server.arg("plain") : String();
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) { server.send(400, "text/plain", "bad json"); return; }
  const char *action = doc["action"];

  if (strcmp(action, "update_params") == 0) {
    if (doc.containsKey("rate")) generatorRatePerSec = doc["rate"].as<uint32_t>();
    if (doc.containsKey("time_limit")) testDurationMs = doc["time_limit"].as<uint32_t>() * 1000;
    server.send(200, "text/plain", "params updated");
    return;
  }

  if (strcmp(action, "start") == 0) { 
    if (doc.containsKey("rate")) generatorRatePerSec = doc["rate"].as<uint32_t>();
    if (doc.containsKey("time_limit")) testDurationMs = doc["time_limit"].as<uint32_t>() * 1000;
    generatorRunning = true; 
    testStartMs = nowMs(); 
    lastGenMs = 0; 
    currentRunTasksSent = 0; // Reset this for the new batch
    batchInProgress = true; 
    batchStartMs = nowMs();
    batchCompleted = 0; 
    batchLost = 0;
    server.send(200, "text/plain", "started"); return; 
  }
  if (strcmp(action, "stop") == 0) { 
    generatorRunning = false; digitalWrite(LED_MASTER_ORANGE, LOW); 
    server.send(200, "text/plain", "stopped"); return; 
  }
  if (strcmp(action, "reset") == 0) {
    generatorRunning = false; tasksSent = 0; tasksCompleted = 0; seqCounter = 0;
    currentRunTasksSent = 0;
    batchInProgress = false; outstanding.clear(); csvBuffer = "";
    digitalWrite(LED_MASTER_ORANGE, LOW);
    server.send(200, "text/plain", "reset complete"); return;
  }
  if (strcmp(action, "set_algo") == 0) {
    const char *algo = doc["algo"];
    if (algo) {
      if (strcmp(algo, "roundrobin")==0) currentAlgo = RR;
      else if (strcmp(algo, "greedy")==0) currentAlgo = GREEDY;
      else currentAlgo = LOADAWARE;
    }
    server.send(200, "text/plain", "algo set"); return;
  }
  if (strcmp(action, "simulate_offline") == 0) {
    uint32_t node = doc["node"].as<uint32_t>();
    uint32_t duration = doc["duration"].as<uint32_t>();
    
    if (node && workers.count(node)) {
      StaticJsonDocument<128> c;
      c["type"] = "cmd"; c["cmd"] = "simulate_offline";
      String out; serializeJson(c, out); mesh.sendSingle(node, out);
      workers[node].simulatedOffline = true;
      if (duration > 0) workers[node].autoRestoreMs = nowMs() + (duration * 1000);
      else workers[node].autoRestoreMs = 0; 
      server.send(200, "text/plain", "simulated");
    }
    return;
  }
  if (strcmp(action, "restore") == 0) {
    uint32_t node = doc["node"].as<uint32_t>();
    if (node && workers.count(node)) {
      StaticJsonDocument<128> c;
      c["type"] = "cmd"; c["cmd"] = "restore";
      String out; serializeJson(c, out); mesh.sendSingle(node, out);
      workers[node].simulatedOffline = false; workers[node].autoRestoreMs = 0; 
      server.send(200, "text/plain", "restore sent");
    }
    return;
  }
  server.send(400, "text/plain", "unknown action");
}

// -------------------- Setup & Loop --------------------
void setup() {
  Serial.begin(115200);
  pinMode(LED_MASTER_GREEN, OUTPUT); digitalWrite(LED_MASTER_GREEN, HIGH);
  pinMode(LED_MASTER_ORANGE, OUTPUT); digitalWrite(LED_MASTER_ORANGE, LOW);
  
  u8g2.begin(); 
  u8g2.clearBuffer(); 
  u8g2.setFont(u8g2_font_6x10_tr); 
  u8g2.drawStr(0,10,"Sankeerth Project..."); 
  u8g2.sendBuffer();
  
  mesh.setDebugMsgTypes(ERROR | STARTUP); 
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  
  mesh.onReceive([](unsigned int from, String &msg) { receivedCallback((uint32_t)from, msg); });
  mesh.onNewConnection([](unsigned int nodeId) { newConnectionCallback((uint32_t)nodeId); });
  
  server.on("/", handleRoot); 
  server.on("/data", handleData); 
  server.on("/status", handleStatus);
  server.on("/control", handleControl); 
  server.begin();
}

void loop() {
  mesh.update();
  server.handleClient();
  generatorLoop();
  checkAckTimeouts();
  checkAutoRestore(); 

  // Master Ping
  static uint32_t lastMasterPing = 0;
  if (nowMs() - lastMasterPing > 2000) {
    lastMasterPing = nowMs();
    StaticJsonDocument<64> pingDoc;
    pingDoc["type"] = "master_ping";
    String pingStr;
    serializeJson(pingDoc, pingStr);
    mesh.sendBroadcast(pingStr);
  }

  // The Batch Summary Trigger (Needs currentRunTasksSent)
  if (batchInProgress && !generatorRunning && currentRunTasksSent > 0 && (batchCompleted + batchLost >= currentRunTasksSent)) {
    uint32_t totalTime = nowMs() - batchStartMs;
    char buf[128]; snprintf(buf, sizeof(buf), "%s,SUMMARY,ALL,0,0,%u,batch_complete,0", getAlgoName(), (unsigned)totalTime); emitCSVRow(buf);
    emitCSVRow("----------------,---,---,---,---,---,----------------,---");
    batchInProgress = false; 
  }

  // --- FULL SCREEN LONG-FORM OLED DASHBOARD ---
  static uint32_t lastDisplay = 0;
  if (nowMs() - lastDisplay > 1000) {
    lastDisplay = nowMs();
    
    char l2[32], l3[32], l4[32], l5[32], l6[32];
    
    // Time Limit & Rate
    uint32_t totalSec = testDurationMs / 1000;
    snprintf(l2, sizeof(l2), "Rate: %u/s | Time: %us", (unsigned)generatorRatePerSec, (unsigned)totalSec);
    
    // Algorithm
    const char* algoLong = "Load Aware";
    if (currentAlgo == RR) algoLong = "Round Robin";
    else if (currentAlgo == GREEDY) algoLong = "Greedy";
    snprintf(l3, sizeof(l3), "Algo: %s", algoLong);
    
    // Completed Tasks
    snprintf(l4, sizeof(l4), "Completed: %u tasks", (unsigned)tasksCompleted);
    
    if (!workerList.empty()) {
      uint32_t id = workerList[0];
      snprintf(l5, sizeof(l5), "Worker 1: %s", workers[id].state.c_str());
      if (workerList.size() > 1) {
        uint32_t id2 = workerList[1]; 
        snprintf(l6, sizeof(l6), "Worker 2: %s", workers[id2].state.c_str());
      } else {
        snprintf(l6, sizeof(l6), "Worker 2: Offline");
      }
    } else { 
      snprintf(l5, sizeof(l5), "Worker 1: Offline"); 
      snprintf(l6, sizeof(l6), "Worker 2: Offline"); 
    }
    
    u8g2.clearBuffer(); 
    u8g2.setFont(u8g2_font_6x10_tr); 
    
    String ipHeader = "IP: " + WiFi.softAPIP().toString();
    u8g2.drawStr(0, 10, ipHeader.c_str());
    
    u8g2.drawStr(0, 21, l2); 
    u8g2.drawStr(0, 32, l3); 
    u8g2.drawStr(0, 43, l4); 
    u8g2.drawStr(0, 54, l5); 
    u8g2.drawStr(0, 64, l6); 
    
    u8g2.sendBuffer();
  }
}
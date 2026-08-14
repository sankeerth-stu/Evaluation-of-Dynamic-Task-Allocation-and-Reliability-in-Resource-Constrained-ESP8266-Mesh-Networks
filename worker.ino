#include <Arduino.h>
#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <vector>

// -------------------- Config --------------------
#define MESH_PREFIX "Sankeerth_Cluster"
#define MESH_PASSWORD "20092009"
#define MESH_PORT 5555

const uint8_t LED_RED = 5;     // D1 - Disconnected or Killed
const uint8_t LED_ORANGE = 14; // D5 - Working on Task or Queue > 0
const uint8_t LED_GREEN = 13;  // D7 - Active and Connected, Idle

// -------------------- Globals --------------------
painlessMesh mesh;
uint32_t masterNodeId = 0; 
bool isKilled = false;     
uint32_t lastHeartbeat = 0;
uint32_t lastMasterPingMs = 0; // Tracks the last time we heard the Master

struct JobItem {
  uint32_t seq;
  uint32_t workload;
};
std::vector<JobItem> queue;

bool isProcessing = false;
uint32_t activeSeq = 0;
uint32_t activeEndMs = 0;

// -------------------- Utilities --------------------
void clearData() {
  queue.clear();
  isProcessing = false;
  activeSeq = 0;
  activeEndMs = 0;
}

// Scans to ensure the Master is actually still around (10 second timeout)
bool isMasterConnected() {
  if (masterNodeId == 0) return false; 
  if (millis() - lastMasterPingMs > 10000) return false; // Master went silent!
  
  // Double verify the physical mesh connection
  auto nodes = mesh.getNodeList();
  for (auto id : nodes) {
    if (id == masterNodeId) return true;
  }
  return false;
}

// -------------------- LED Rules --------------------
void updateLEDs() {
  // Rule 1: If killed or Master is offline -> Red Only
  if (isKilled || !isMasterConnected()) {
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_ORANGE, LOW);
    digitalWrite(LED_GREEN, LOW);
  } 
  // Rule 2: If processing OR if there are tasks waiting in the queue -> Orange Only
  else if (isProcessing || !queue.empty()) {
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_ORANGE, HIGH);
    digitalWrite(LED_GREEN, LOW);
  } 
  // Rule 3: Idle (queue is 0) and Master is online -> Green Only
  else {
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_ORANGE, LOW);
    digitalWrite(LED_GREEN, HIGH);
  }
}

// -------------------- Core Logic --------------------
void receivedCallback(uint32_t from, String &msg) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg)) return;

  const char* type = doc["type"];
  if (!type) return;

  // STRICT SECURITY BOUNCER: 
  // Accepts the new "master_ping", "cmd", or "task". Ignores all worker chatter.
  if (strcmp(type, "master_ping") == 0 || strcmp(type, "cmd") == 0 || strcmp(type, "task") == 0) {
    masterNodeId = from; // Verify Master and lock ID
    lastMasterPingMs = millis(); // Reset the 10-second timeout
  } else {
    return; // Bounce the message (worker heartbeats)
  }

  // Handle Master Commands
  if (strcmp(type, "cmd") == 0) {
    const char* cmd = doc["cmd"];
    if (cmd && strcmp(cmd, "simulate_offline") == 0) {
      isKilled = true;
      clearData(); // INSTANT AMNESIA
    } 
    else if (cmd && strcmp(cmd, "restore") == 0) {
      isKilled = false; // WAKE UP
    }
  } 
  // Handle Tasks
  else if (strcmp(type, "task") == 0) {
    if (isKilled) return; // Ignore tasks if we are dead
    uint32_t seq = doc["seq"] | 0;
    uint32_t wl = doc["workload_ms"] | 100;
    queue.push_back({seq, wl});
  }
}

void processTasks() {
  // Total Amnesia: Drop everything if Master is unplugged or we are killed
  if (isKilled || !isMasterConnected()) {
    if (isProcessing || !queue.empty()) {
      clearData(); 
    }
    return; 
  }

  // Start processing next task smoothly
  if (!isProcessing && !queue.empty()) {
    isProcessing = true;
    activeSeq = queue.front().seq;
    activeEndMs = millis() + queue.front().workload;
    queue.erase(queue.begin()); 
  }

  // Finish task and report back
  if (isProcessing && millis() >= activeEndMs) {
    isProcessing = false;
    
    if (masterNodeId != 0) {
      StaticJsonDocument<128> doc;
      doc["type"] = "ack";
      doc["seq"] = activeSeq;
      doc["completion_ts_ms"] = millis();
      String out;
      serializeJson(doc, out);
      mesh.sendSingle(masterNodeId, out); 
    }
  }
}

void sendHeartbeat() {
  // Only stop sending pulses if physically disconnected from the mesh entirely
  if (mesh.getNodeList().empty()) return; 

  if (millis() - lastHeartbeat > 2000) {
    lastHeartbeat = millis();
    
    StaticJsonDocument<128> doc;
    doc["type"] = "heartbeat";
    
    // If killed, tell the Master we are "dead" so the routing tunnel stays open!
    if (isKilled) {
      doc["state"] = "dead";
    } else {
      doc["state"] = (isProcessing || !queue.empty()) ? "working" : "idle";
    }
    
    doc["queue_length"] = queue.size();
    doc["rssi"] = WiFi.RSSI();
    
    String out;
    serializeJson(doc, out);
    
    if (isMasterConnected()) {
      mesh.sendSingle(masterNodeId, out);
    } else {
      mesh.sendBroadcast(out); 
    }
  }
}

// -------------------- Setup & Loop --------------------
void setup() {
  Serial.begin(115200);
  
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_ORANGE, OUTPUT);
  pinMode(LED_RED, OUTPUT);

  updateLEDs(); // Sets to Red immediately on boot

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  
  WiFi.softAP(MESH_PREFIX, MESH_PASSWORD, 1, 1); 
  
  mesh.onReceive([](unsigned int from, String &msg) { 
    receivedCallback(from, msg); 
  });
}

void loop() {
  mesh.update();
  processTasks();
  updateLEDs(); // Strict LED enforcement every loop
  sendHeartbeat();
}

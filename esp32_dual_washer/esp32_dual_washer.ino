/*
 * Rinse · 双轨固件（BLE 小程序 + HomeKit 苹果家庭）
 *
 * 同一颗 ESP32 上同时跑：
 *   - 蓝牙 BLE Server（小程序直连 + 蓝牙配网）
 *   - HomeSpan HAP Server（iPhone 家庭 App 控制）
 * 状态机作为唯一源真理，两路适配层都向它读写
 *
 * 配网流程：
 *   1. 小程序蓝牙连接 ESP32
 *   2. 在小程序"配网"页输入 WiFi 名 + 密码
 *   3. 小程序通过 BLE 发送 {"cmd":"wifi","ssid":"xxx","pwd":"xxx"}
 *   4. ESP32 保存到 NVS，重启，自动连 WiFi
 *   5. 连上后 iPhone 在"家庭"App 添加配件（配对码 466-22-6688）
 *
 * 不再需要电脑插线改程序！
 */

#include "HomeSpan.h"        // HomeSpan 库
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>     // NVS 持久化 WiFi
#include <Update.h>          // OTA 固件升级
#include <mbedtls/base64.h> // base64 解码 OTA 数据

// ===== 固件版本（BLE OTA 升级用） =====
#define FW_VERSION "1.1.0"

// ===== 引脚（ESP32-S3 适配版） =====
// ESP32-S3 与 ESP32 引脚差异：
//   - ESP32 的 GPIO34 是"输入专用"，ESP32-S3 没有这个引脚
//   - ESP32-S3 的 ADC1 通道是 GPIO1~GPIO10
//   - ESP32-S3 的 LEDC API 已升级（Arduino core 3.x）
// 避开 ESP32-S3 的禁用引脚：
//   - 0/3（Strapping/USB）、8-13（Flash/PSRAM）、15-18（JTAG）
//   - 19-20（USB-CDC）、26-32（SPI Flash）、33-37（SPI PSRAM）
// BTS7960 驱动：ENA(PWM) + IN1(正转) + IN2(反转)
#define PIN_MOTOR_PWM    4    // BTS7960 ENA — 电机 PWM（LEDC）
#define PIN_MOTOR_IN1    5    // BTS7960 IN1 — 正转
#define PIN_MOTOR_IN2    6    // BTS7960 IN2 — 反转
#define PIN_VALVE        7    // 进水阀继电器
#define PIN_PUMP        18    // 排水泵继电器
// 浮球开关已移除：纯计时器驱动，不判断水位
#define PIN_TEMP        14    // DS18B20 单总线

// ===== UUID =====
#define BLE_SERVICE_UUID "0000FFE0-0000-1000-8000-00805F9B34FB"
#define BLE_CHAR_UUID    "0000FFE1-0000-1000-8000-00805F9B34FB"

// ===== 状态机 =====
// 立式洗衣机：无甩干环节（甩干仅滚筒机具备）
enum Stage : uint8_t {
  STAGE_IDLE=0, STAGE_FILL, STAGE_WASH, STAGE_RINSE,
  STAGE_DRAIN, STAGE_DONE, STAGE_ALARM
};
const char* STAGE_NAMES[] = {
  "idle","fill","wash","rinse","drain","done","alarm"
};

struct ModeParam {
  const char* name;
  uint32_t fillMs, washMs, rinseMs, drainMs;
  uint8_t  rinseCycles;
};
static const ModeParam MODES[] = {
  // standard 1800s: 进水30+洗涤1350+排水60+(漂洗120+排水60)*2 = 1800 ✓
  {"standard", 30000, 1350000, 120000, 60000, 2},  // 30+1350+60+(120+60)*2=1800s=30:00
  // quick 900s: 进水20+洗涤500+排水60+(漂洗260+排水60)*1 = 900 ✓
  {"quick",     20000,  500000, 260000, 60000, 1},  // 20+500+60+(260+60)*1=900s=15:00
  // gentle 1500s: 进水30+洗涤1050+排水60+(漂洗120+排水60)*2 = 1500 ✓
  {"gentle",    30000, 1050000, 120000, 60000, 2},  // 30+1050+60+(120+60)*2=1500s=25:00
  // rinse 600s: (漂洗240+排水60)*2 = 600 ✓
  {"rinse",         0,       0, 240000, 60000, 2},  // (240+60)*2=600s=10:00
};
const uint8_t MODE_COUNT = sizeof(MODES)/sizeof(MODES[0]);

// ===== 全局运行态（状态机单一源真理） =====
Stage curStage = STAGE_IDLE;
uint8_t  curModeIdx = 0;
uint32_t stageElapsedBeforePause = 0;
const ModeParam* curParam = &MODES[0];
bool     isPaused = false;
bool     isStopping = false;
bool     otaActive = false;
uint32_t otaTotalSize = 0;
uint32_t otaWritten = 0;
uint32_t stageStartMs = 0;
uint32_t stageDurMs = 0;
uint8_t  rinseCount = 0;
String   alarmMsg = "";
uint32_t lastTempMs = 0;
float    curTemp = 25;

// ===== WiFi 配网状态 =====
Preferences prefs;
String wifiSsid = "";
String wifiPwd  = "";
bool   wifiConnected = false;
uint32_t wifiConnStartMs = 0;
String  wifiIp = "";

enum Src : uint8_t { SRC_BLE, SRC_HAP, SRC_INTERNAL };
Src lastSrc = SRC_INTERNAL;

// ===== 硬件层 =====
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define MOTOR_PWM_FREQ    1000
  #define MOTOR_PWM_RES     13
  void motorPwmInit(){ ledcAttach(PIN_MOTOR_PWM, MOTOR_PWM_FREQ, MOTOR_PWM_RES); }
  void motorPwmWrite(uint32_t duty){ ledcWrite(PIN_MOTOR_PWM, duty); }
#else
  #define MOTOR_PWM_CH      0
  #define MOTOR_PWM_FREQ    1000
  #define MOTOR_PWM_RES     13
  void motorPwmInit(){ ledcSetup(MOTOR_PWM_CH, MOTOR_PWM_FREQ, MOTOR_PWM_RES); ledcAttachPin(PIN_MOTOR_PWM, MOTOR_PWM_CH); }
  void motorPwmWrite(uint32_t duty){ ledcWrite(MOTOR_PWM_CH, duty); }
#endif

void setMotor(bool on, bool fwd=true){
  if(!on){ motorPwmWrite(0); digitalWrite(PIN_MOTOR_IN1, LOW); digitalWrite(PIN_MOTOR_IN2, LOW); return; }
  digitalWrite(PIN_MOTOR_IN1, fwd ? HIGH : LOW);
  digitalWrite(PIN_MOTOR_IN2, fwd ? LOW  : HIGH);
  motorPwmWrite(820);
}
void valve(bool on){ digitalWrite(PIN_VALVE, on?HIGH:LOW); }
void pump(bool on){ digitalWrite(PIN_PUMP, on?HIGH:LOW); }
uint8_t waterLevelPct(){ return 0; }

OneWire oneWire(PIN_TEMP);
DallasTemperature dallas(&oneWire);
float readTempC(){
  if(millis() - lastTempMs > 2000){
    dallas.requestTemperatures();
    float t = dallas.getTempCByIndex(0);
    curTemp = (t == DEVICE_DISCONNECTED_C || t < -50 || t > 125) ? -1 : t;
    lastTempMs = millis();
  }
  return curTemp;
}
uint32_t remainSeconds(){
  if(curStage==STAGE_IDLE || curStage==STAGE_DONE || curStage==STAGE_ALARM) return 0;
  if(stageDurMs==0) return 0;
  uint32_t elapsed = isPaused ? stageElapsedBeforePause : (millis()-stageStartMs);
  if(elapsed >= stageDurMs) return 0;
  return (stageDurMs - elapsed)/1000;
}

void broadcastState(Src from);

void enterStage(Stage s, Src from=SRC_INTERNAL){
  curStage = s;
  stageStartMs = millis();
  switch(s){
    case STAGE_FILL:  valve(true);  pump(false); setMotor(false);     stageDurMs=curParam->fillMs;  break;
    case STAGE_WASH:  valve(false); pump(false); setMotor(true,true);  stageDurMs=curParam->washMs;  break;
    case STAGE_RINSE: valve(true);  pump(false); setMotor(true,true);  stageDurMs=curParam->rinseMs; break;
    case STAGE_DRAIN: valve(false); pump(true);  setMotor(false);       stageDurMs = isStopping ? 0 : curParam->drainMs; break;
    case STAGE_DONE:  valve(false); pump(false); setMotor(false);       stageDurMs=0; isStopping=false; break;
    default: break;
  }
  broadcastState(from);
}

void raiseAlarm(const String& msg, Src from=SRC_INTERNAL){
  alarmMsg = msg; valve(false); pump(false); setMotor(false);
  curStage = STAGE_ALARM; broadcastState(from);
}

void cmdStart(const String& mode, Src from){
  String m = mode.length()?mode:String(curParam->name);
  const ModeParam* p = nullptr;
  for(uint8_t i=0;i<MODE_COUNT;i++) if(String(MODES[i].name)==m){ p=&MODES[i]; curModeIdx=i; break; }
  if(!p){ raiseAlarm("unknown mode", from); return; }
  rinseCount = 0; alarmMsg = ""; isStopping = false;
  if(curStage!=STAGE_IDLE && curStage!=STAGE_DONE && curStage!=STAGE_ALARM){
    enterStage(STAGE_DRAIN, from);
  }
  curParam = p; isPaused = false;
  if(curParam->fillMs > 0)      enterStage(STAGE_FILL, from);
  else if(curParam->washMs > 0) enterStage(STAGE_WASH, from);
  else                          enterStage(STAGE_RINSE, from);
}

void cmdPause(Src from){
  if(curStage==STAGE_IDLE || curStage==STAGE_DONE || curStage==STAGE_ALARM) return;
  stageElapsedBeforePause = millis() - stageStartMs;
  isPaused = true; setMotor(false); valve(false); pump(false);
  broadcastState(from);
}

void cmdResume(Src from){
  if(!isPaused) return;
  isPaused = false;
  stageStartMs = millis() - stageElapsedBeforePause;
  stageElapsedBeforePause = 0;
  switch(curStage){
    case STAGE_FILL:  valve(true);  pump(false); setMotor(false);     break;
    case STAGE_WASH:  valve(false); pump(false); setMotor(true,true);  break;
    case STAGE_RINSE: valve(true);  pump(false); setMotor(true,true);  break;
    case STAGE_DRAIN: valve(false); pump(true);  setMotor(false);       break;
    default: break;
  }
  broadcastState(from);
}

void cmdStop(Src from){
  isPaused = false;
  if(curStage==STAGE_DRAIN && isStopping){
    curStage = STAGE_IDLE; isStopping = false;
    setMotor(false); valve(false); pump(false);
    broadcastState(from); return;
  }
  if(curStage!=STAGE_IDLE && curStage!=STAGE_DONE && curStage!=STAGE_ALARM){
    isStopping = true; enterStage(STAGE_DRAIN, from);
  } else if(curStage==STAGE_DONE || curStage==STAGE_ALARM){
    curStage = STAGE_IDLE; isStopping = false;
    setMotor(false); valve(false); pump(false); broadcastState(from);
  }
}

bool pendingReboot = false;
uint32_t rebootAtMs = 0;
void scheduleReboot(uint32_t delayMs){ pendingReboot = true; rebootAtMs = millis() + delayMs; }

void cmdSetWifi(const String& ssid, const String& pwd, Src from){
  if(ssid.length()==0){ bleSend("{\"wifi\":\"error\",\"msg\":\"ssid 为空\"}\n"); return; }
  prefs.begin("wifi", false); prefs.putString("ssid", ssid); prefs.putString("pwd", pwd); prefs.end();
  bleSend("{\"wifi\":\"saving\",\"ssid\":\""+ssid+"\"}\n");
  delay(300); scheduleReboot(800);
}

void cmdQueryWifi(Src from){
  String json = "{\"wifi\":";
  json += wifiConnected ? "connected" : "disconnected";
  json += "\",\"ssid\":\""+wifiSsid+"\"";
  json += ",\"ip\":\""+wifiIp+"\"";
  json += ",\"rssi\":"+String(WiFi.RSSI());
  json += "}\n";
  bleSend(json);
}

void cmdScanWifi(Src from){
  WiFi.mode(WIFI_STA);
  delay(100);
  int n = WiFi.scanNetworks(false, false, false, 300);
  String json = "{\"wifi_scan\":[";
  int count = 0;
  for(int i=0; i<n && count<10; i++){
    String ssid = WiFi.SSID(i);
    if(ssid.length()==0) continue;
    ssid.replace("\\", "\\\\"); ssid.replace("\"", "\\\"");
    if(count>0) json += ",";
    json += "{\"ssid\":\"" + ssid + "\"";
    json += ",\"rssi\":" + String(WiFi.RSSI(i));
    json += ",\"enc\":" + String(WiFi.encryptionType(i)) + "}";
    count++;
  }
  json += "],\"count\":" + String(count) + "}\n";
  WiFi.scanDelete();
  WiFi.mode(wifiConnected ? WIFI_STA : WIFI_OFF);
  bleSend(json);
}

BLEServer* bleServer = nullptr;
BLECharacteristic* bleChar = nullptr;
bool bleConnected = false;
String rxBuffer = "";

class BleServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; }
  void onDisconnect(BLEServer*) override { bleConnected = false; BLEDevice::startAdvertising(); }
};

class BleCharCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue().c_str();
    for(char ch : v){
      if(ch=='\n'){ parseBleCmd(rxBuffer); rxBuffer = ""; }
      else if(ch!='\r'){ rxBuffer += ch; }
    }
  }
};

String extractField(const String& s, const char* key){
  String pat = String("\"") + key + "\":\"";
  int i = s.indexOf(pat); if(i<0) return "";
  int start = i + pat.length(); int end = s.indexOf("\"", start);
  return s.substring(start, end);
}

void parseBleCmd(const String& line){
  String cmd = extractField(line, "cmd");
  String mode = extractField(line, "mode");
  if(cmd=="start")    cmdStart(mode, SRC_BLE);
  else if(cmd=="pause")  cmdPause(SRC_BLE);
  else if(cmd=="resume") cmdResume(SRC_BLE);
  else if(cmd=="stop")   cmdStop(SRC_BLE);
  else if(cmd=="status") broadcastState(SRC_BLE);
  else if(cmd=="version") bleSend(String("{\"ver\":\"") + FW_VERSION + "\"}\n");
  else if(cmd=="wifi"){ String s=extractField(line,"ssid"); String p=extractField(line,"pwd"); cmdSetWifi(s,p,SRC_BLE); }
  else if(cmd=="wifi_status") cmdQueryWifi(SRC_BLE);
  else if(cmd=="wifi_scan") cmdScanWifi(SRC_BLE);
  else if(cmd=="ota_begin"){
    uint32_t sz = extractField(line, "size").toInt();
    otaTotalSize = sz; otaWritten = 0; otaActive = true;
    if(Update.begin(sz)) bleSend(String("{\"ota\":\"begin\",\"ok\":1}\n"));
    else { otaActive = false; bleSend(String("{\"ota\":\"begin\",\"ok\":0}\n")); }
  }
  else if(cmd=="ota_data" && otaActive){
    String b64 = extractField(line, "data");
    size_t outLen = 0;
    uint8_t* buf = (uint8_t*)malloc(b64.length());
    if(mbedtls_base64_decode(buf, b64.length(), &outLen, (const uint8_t*)b64.c_str(), b64.length()) == 0){
      Update.write(buf, outLen); otaWritten += outLen;
    }
    free(buf);
    bleSend(String("{\"ota\":\"ack\",\"seq\":") + extractField(line,"seq") + "}\n");
  }
  else if(cmd=="ota_end" && otaActive){
    if(Update.end(true)){ otaActive = false; bleSend(String("{\"ota\":\"end\",\"ok\":1}\n")); }
    else { otaActive = false; bleSend(String("{\"ota\":\"end\",\"ok\":0}\n")); }
  }
  else if(cmd=="reboot"){ ESP.restart(); }
}

void bleSend(const String& s){
  if(bleConnected && bleChar){ bleChar->setValue(s.c_str()); bleChar->notify(); }
}

struct WasherFaucet : Service::Faucet {
  SpanCharacteristic *active, *statusFault, *remainingDur, *curTemp, *waterLevel;
  WasherFaucet() : Service::Faucet() {
    active = new Characteristic::Active(0);
    statusFault = new Characteristic::StatusFault(0);
    remainingDur = new Characteristic::RemainingDuration(0);
    curTemp = new Characteristic::CurrentTemperature(25);
    waterLevel = new Characteristic::WaterLevel(0);
    new Characteristic::Name("Rinse");
    new Characteristic::ConfiguredName("Rinse");
  }
  boolean update() override {
    if(active->updated()){
      bool value = active->getNewVal();
      if(value){ if(curStage==STAGE_IDLE||curStage==STAGE_DONE||curStage==STAGE_ALARM) cmdStart("",SRC_HAP); else if(isPaused) cmdResume(SRC_HAP); }
      else cmdStop(SRC_HAP);
    }
    return true;
  }
  void loop() override {
    if(active->timeVal() < 1000) return;
    statusFault->setVal(curStage==STAGE_ALARM ? 1 : 0);
    remainingDur->setVal(remainSeconds());
    float t = readTempC();
    curTemp->setVal(t < 0 ? -273 : t);
    waterLevel->setVal(waterLevelPct());
  }
};
WasherFaucet* g_faucet = nullptr;

struct ModeSwitch : Service::Switch {
  SpanCharacteristic *on; uint8_t modeIdx;
  bool pendingReset = false; uint32_t resetAtMs = 0;
  ModeSwitch(uint8_t idx) : Service::Switch() { modeIdx = idx; on = new Characteristic::On(false); new Characteristic::Name(MODES[idx].name); }
  boolean update() override {
    if(on->updated()){ bool value = on->getNewVal(); if(value){ cmdStart(MODES[modeIdx].name, SRC_HAP); if(g_faucet) g_faucet->active->setVal(1); pendingReset = true; resetAtMs = millis() + 500; } }
    return true;
  }
  void loop() override { if(pendingReset && millis() > resetAtMs){ on->setVal(false); pendingReset = false; } }
};

void broadcastState(Src from){
  String stage = STAGE_NAMES[curStage];
  uint32_t rem = remainSeconds();
  float t = readTempC();
  uint32_t stageTotalSec = (stageDurMs>0) ? stageDurMs/1000 : 0;
  String json = "{\"state\":\""+stage+"\",\"stage\":\""+stage+"\",\"remain\":"+String(rem)+",\"total\":"+String(stageTotalSec)+",\"temp\":"+(int)(t<0?-1:t);
  json += ",\"paused\":" + String(isPaused ? 1 : 0);
  json += ",\"ver\":\"" FW_VERSION "\"";
  if(curStage==STAGE_ALARM && alarmMsg.length()) json += ",\"code\":\"E1\",\"msg\":\""+alarmMsg+"\"";
  json += "}\n";
  bleSend(json);
  if(g_faucet){
    if(from != SRC_HAP) g_faucet->active->setVal(curStage==STAGE_IDLE||curStage==STAGE_DONE||curStage==STAGE_ALARM ? 0 : 1);
    g_faucet->statusFault->setVal(curStage==STAGE_ALARM?1:0);
    g_faucet->remainingDur->setVal(rem);
    g_faucet->curTemp->setVal(t);
    g_faucet->waterLevel->setVal(waterLevelPct());
  }
}

uint32_t lastWasherTickMs = 0;
uint32_t lastWifiTickMs = 0;
const uint32_t WASHER_TICK_INTERVAL = 500;
const uint32_t WIFI_TICK_INTERVAL = 2000;

void washerTick(){
  if(isPaused) return;
  if(curStage==STAGE_IDLE || curStage==STAGE_DONE || curStage==STAGE_ALARM) return;
  uint32_t elapsed = millis() - stageStartMs;
  switch(curStage){
    case STAGE_FILL: if(elapsed > stageDurMs){ valve(false); enterStage(STAGE_WASH, SRC_INTERNAL); } break;
    case STAGE_WASH:
      if((elapsed/5000)%2==0) setMotor(true, true); else setMotor(true, false);
      if(elapsed>stageDurMs) enterStage(STAGE_DRAIN, SRC_INTERNAL); break;
    case STAGE_RINSE:
      if((elapsed/5000)%2==0) setMotor(true, true); else setMotor(true, false);
      if(elapsed>stageDurMs){ rinseCount++; enterStage(STAGE_DRAIN, SRC_INTERNAL); } break;
    case STAGE_DRAIN:
      if(isStopping) break;
      if(elapsed > stageDurMs){
        if(curParam->rinseCycles>0 && rinseCount<curParam->rinseCycles) enterStage(STAGE_RINSE, SRC_INTERNAL);
        else enterStage(STAGE_DONE, SRC_INTERNAL);
      } break;
    default: break;
  }
}

void wifiTick(){
  bool nowConnected = (WiFi.status() == WL_CONNECTED);
  if(nowConnected != wifiConnected){
    wifiConnected = nowConnected;
    if(nowConnected){
      wifiIp = WiFi.localIP().toString();
      bleSend("{\"wifi\":\"connected\",\"ssid\":\""+String(WiFi.SSID().c_str())+"\",\"ip\":\""+wifiIp+"\",\"rssi\":"+String(WiFi.RSSI())+"}\n");
    } else {
      bleSend("{\"wifi\":\"disconnected\"}\n");
      WiFi.reconnect(); wifiConnStartMs = millis();
    }
  }
  if(!wifiConnected && millis() - wifiConnStartMs > 30000){
    bleSend("{\"wifi\":\"timeout\"}\n");
    WiFi.reconnect(); wifiConnStartMs = millis();
  }
}

void setup(){
  Serial.begin(115200); delay(200);
  Serial.println("\n[BOOT] ESP32-S3 Rinse启动中...");
  pinMode(PIN_MOTOR_IN1, OUTPUT); pinMode(PIN_MOTOR_IN2, OUTPUT);
  pinMode(PIN_VALVE, OUTPUT); pinMode(PIN_PUMP, OUTPUT);
  motorPwmInit(); valve(false); pump(false); setMotor(false);
  dallas.begin(); analogReadResolution(12);
  BLEDevice::init("Rinse-设备"); BLEDevice::setMTU(185);
  bleServer = BLEDevice::createServer(); bleServer->setCallbacks(new BleServerCb());
  BLEService* bleSvc = bleServer->createService(BLE_SERVICE_UUID);
  bleChar = bleSvc->createCharacteristic(BLE_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  bleChar->addDescriptor(new BLE2902()); bleChar->setCallbacks(new BleCharCb());
  bleSvc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID); BLEDevice::startAdvertising();
  prefs.begin("wifi", true);
  wifiSsid = prefs.getString("ssid", ""); wifiPwd = prefs.getString("pwd", ""); prefs.end();
  if(wifiSsid.length() > 0){
    WiFi.mode(WIFI_STA); WiFi.begin(wifiSsid.c_str(), wifiPwd.c_str()); wifiConnStartMs = millis();
  } else {
    bleSend("{\"wifi\":\"unprovisioned\"}\n");
  }
  homeSpan.setLogLevel(1); homeSpan.setPairingCode("46622668"); homeSpan.enableOTA();
  homeSpan.begin(Category::Faucets, "Rinse");
  new SpanAccessory();
  new Service::AccessoryInformation(); new Characteristic::Identify();
  new Characteristic::Manufacturer("DualTrack-DIY"); new Characteristic::SerialNumber("DT-WASHER-001");
  new Characteristic::Model("ESP32-Washer-Dual"); new Characteristic::FirmwareRevision("1.1.0");
  g_faucet = new WasherFaucet();
  for(uint8_t i=0;i<MODE_COUNT;i++) new ModeSwitch(i);
  lastWasherTickMs = millis(); lastWifiTickMs = millis();
  Serial.println("[Dual] DIY Washer ready. BLE + HomeKit both up.");
  Serial.println("[Dual] 配对码: 466-22-668");
  Serial.flush();
}

void loop(){
  homeSpan.poll();
  uint32_t now = millis();
  if(now - lastWasherTickMs >= WASHER_TICK_INTERVAL){ lastWasherTickMs = now; washerTick(); }
  if(now - lastWifiTickMs >= WIFI_TICK_INTERVAL){ lastWifiTickMs = now; wifiTick(); }
  if(pendingReboot && millis() > rebootAtMs){
    bleSend("{\"wifi\":\"reboot\"}\n"); delay(200); ESP.restart();
  }
}

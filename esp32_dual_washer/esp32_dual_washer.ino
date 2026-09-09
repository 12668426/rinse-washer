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
#define FW_VERSION "1.0.0"

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
  // 各模式时长（ms）：进水+洗涤+排水+(漂洗+排水)*次数 = 总时长
  // standard: 30+1800+60+(600+60)*2 = 3120s = 52:00 → 调整为 30min=1800s
  //   30+1380+60+(180+60)*2 = 30+1380+60+480 = 1950 → 改洗涤为1200s
  //   30+1200+60+(180+60)*2 = 30+1200+60+480 = 1770 → 改漂洗为165s
  //   30+1200+60+(165+60)*2 = 30+1200+60+450 = 1740 → 改洗涤为1230s
  //   30+1230+60+(165+60)*2 = 30+1230+60+450 = 1770 → 改为 30+1260+60+(150+60)*2 = 30+1260+60+420 = 1770 → 直接调
  // 实际方案：standard=30min, quick=15min, gentle=25min, rinse=10min
  // standard 1800s: 进水30+洗涤1200+排水60+(漂洗120+排水60)*2 = 30+1200+60+360 = 1650 → 洗涤改1290s
  //   30+1290+60+(120+60)*2 = 30+1290+60+360 = 1740 → 洗涤改1320
  //   30+1320+60+(120+60)*2 = 30+1320+60+360 = 1770 → 洗涤改1350
  //   30+1350+60+(120+60)*2 = 30+1350+60+360 = 1800 ✓
  {"standard", 30000, 1350000, 120000, 60000, 2},  // 30+1350+60+(120+60)*2=1800s=30:00
  // quick 900s: 进水20+洗涤X+排水60+(漂洗Y+排水60)*1 = 900
  //   20+X+60+Y+60 = 900 → X+Y=760, 取 X=500,Y=260
  //   20+500+60+(260+60)*1 = 20+500+60+320 = 900 ✓
  {"quick",     20000,  500000, 260000, 60000, 1},  // 20+500+60+(260+60)*1=900s=15:00
  // gentle 1500s: 进水30+洗涤X+排水60+(漂洗Y+排水60)*2 = 1500
  //   30+X+60+(Y+60)*2 = 1500 → X+2Y=1290, 取 X=1050,Y=120
  //   30+1050+60+(120+60)*2 = 30+1050+60+360 = 1500 ✓
  {"gentle",    30000, 1050000, 120000, 60000, 2},  // 30+1050+60+(120+60)*2=1500s=25:00
  // rinse 600s: 进水0+洗涤0+排水0+(漂洗240+排水60)*2 = 600 ✓
  {"rinse",         0,       0, 240000, 60000, 2},  // (240+60)*2=600s=10:00
};
const uint8_t MODE_COUNT = sizeof(MODES)/sizeof(MODES[0]);

// ===== 全局运行态（状态机单一源真理） =====
Stage curStage = STAGE_IDLE;
uint8_t  curModeIdx = 0;
uint32_t stageElapsedBeforePause = 0;  // 暂停前本阶段已运行毫秒，恢复时还原
const ModeParam* curParam = &MODES[0];
bool     isPaused = false;
bool     isStopping = false;   // 用户主动停止：排水完成后直接进 DONE，不进漂洗
// BLE OTA 状态
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
String wifiSsid = "";       // 当前使用的 SSID
String wifiPwd  = "";
bool   wifiConnected = false;  // 是否已连上 WiFi
uint32_t wifiConnStartMs = 0;  // 连接尝试起始时间
String  wifiIp = "";           // 分配到的 IP

// 指令来源标识，避免双向循环推送
enum Src : uint8_t { SRC_BLE, SRC_HAP, SRC_INTERNAL };
Src lastSrc = SRC_INTERNAL;

// ===== 硬件层 =====
// ESP32-S3 LEDC 新 API（Arduino core 3.x）：
//   旧：ledcSetup(channel, freq, res) + ledcAttachPin(pin, channel) + ledcWrite(channel, duty)
//   新：ledcAttach(pin, freq, res)    + ledcWrite(pin, duty)
// 兼容做法：通过宏检测核心版本
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // core 3.x：新 API
  #define MOTOR_PWM_FREQ    1000
  #define MOTOR_PWM_RES     13
  void motorPwmInit(){
    ledcAttach(PIN_MOTOR_PWM, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
  }
  void motorPwmWrite(uint32_t duty){
    ledcWrite(PIN_MOTOR_PWM, duty);
  }
#else
  // core 2.x：旧 API
  #define MOTOR_PWM_CH      0
  #define MOTOR_PWM_FREQ    1000
  #define MOTOR_PWM_RES     13
  void motorPwmInit(){
    ledcSetup(MOTOR_PWM_CH, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
    ledcAttachPin(PIN_MOTOR_PWM, MOTOR_PWM_CH);
  }
  void motorPwmWrite(uint32_t duty){
    ledcWrite(MOTOR_PWM_CH, duty);
  }
#endif

// BTS7960 驱动逻辑：
// - 正转：IN1=HIGH, IN2=LOW, ENA=PWM
// - 反转：IN1=LOW, IN2=HIGH, ENA=PWM
// - 停止：IN1=LOW, IN2=LOW, ENA=0（自由停转）
// - 刹车：IN1=HIGH, IN2=HIGH, ENA=0（短路制动，775 电机快速停转）
void setMotor(bool on, bool fwd=true){
  if(!on){
    // 停止：先断 PWM，再清方向，避免误转
    motorPwmWrite(0);
    digitalWrite(PIN_MOTOR_IN1, LOW);
    digitalWrite(PIN_MOTOR_IN2, LOW);
    return;
  }
  digitalWrite(PIN_MOTOR_IN1, fwd ? HIGH : LOW);
  digitalWrite(PIN_MOTOR_IN2, fwd ? LOW  : HIGH);
  motorPwmWrite(820);  // 13bit 分辨率，820 ≈ 10% duty（775 电机柔启动，避免堵转冲击）
}
void valve(bool on){ digitalWrite(PIN_VALVE, on?HIGH:LOW); }
void pump(bool on){ digitalWrite(PIN_PUMP, on?HIGH:LOW); }
// 浮球开关已移除：纯计时器驱动，不判断水位
// waterLevelPct 保留接口供 HAP WaterLevel 特征使用，返回固定 0
uint8_t waterLevelPct(){
  return 0;  // 无水位传感器
}


OneWire oneWire(PIN_TEMP);
DallasTemperature dallas(&oneWire);
float readTempC(){
  if(millis() - lastTempMs > 2000){
    dallas.requestTemperatures();
    float t = dallas.getTempCByIndex(0);
    // 获取不到（传感器未接或断线）→ -1 表示无效
    curTemp = (t == DEVICE_DISCONNECTED_C || t < -50 || t > 125) ? -1 : t;
    lastTempMs = millis();
  }
  return curTemp;
}
// 计算当前模式总时长（秒）= 进水 + 洗涤 + 首次排水 + (漂洗+排水)*rinseCycles
uint32_t totalSeconds(){
  if(!curParam) return 0;
  uint32_t total = 0;
  total += curParam->fillMs;
  total += curParam->washMs;
  const uint32_t drainMs = 60000;
  total += drainMs;  // WASH 后首次排水
  total += (curParam->rinseMs + drainMs) * curParam->rinseCycles;
  return total / 1000;
}

// 计算当前环节剩余时间（秒）—— 每个环节独立倒计时
// 用户要求：点启动后先倒计时进水 20s，结束进洗涤倒计时 500s，不是总和
uint32_t remainSeconds(){
  if(curStage==STAGE_IDLE || curStage==STAGE_DONE || curStage==STAGE_ALARM) return 0;
  if(stageDurMs==0) return 0;

  // 当前阶段已运行时间（暂停时用暂停前进度）
  uint32_t elapsed = isPaused ? stageElapsedBeforePause : (millis()-stageStartMs);
  // 防止 uint32_t 下溢
  if(elapsed >= stageDurMs) return 0;
  return (stageDurMs - elapsed)/1000;
}

// ===== 前向声明：双通道状态推送 =====
void broadcastState(Src from);  // BLE notify + HAP setVal

// ===== 状态切换 =====
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
  alarmMsg = msg;
  valve(false); pump(false); setMotor(false);
  curStage = STAGE_ALARM;
  broadcastState(from);
}

// ===== 指令入口（被 BLE / HAP 调用） =====
// 启动：mode="standard"/"quick"/... ; 若 mode 为空则用当前模式
void cmdStart(const String& mode, Src from){
  String m = mode.length()?mode:String(curParam->name);
  const ModeParam* p = nullptr;
  for(uint8_t i=0;i<MODE_COUNT;i++) if(String(MODES[i].name)==m){ p=&MODES[i]; curModeIdx=i; break; }
  if(!p){ raiseAlarm("unknown mode", from); return; }
  // F1: 重置计数器和报警状态，避免第二次启动跳过漂洗
  rinseCount = 0;
  alarmMsg = "";
  isStopping = false;  // 新启动：清除停止标志，走正常漂洗流程
  // F9: 如果正在运行，先停止排水再重新启动，避免溢水
  if(curStage!=STAGE_IDLE && curStage!=STAGE_DONE && curStage!=STAGE_ALARM){
    enterStage(STAGE_DRAIN, from);
    // 排水完成后会自动进 DONE，用户需再按启动；这里简化为直接重置
  }
  curParam = p;
  isPaused = false;
  // F3: rinse 模式（fillMs=0, washMs=0）直接进 RINSE，避免电机干转
  if(curParam->fillMs > 0)      enterStage(STAGE_FILL, from);
  else if(curParam->washMs > 0) enterStage(STAGE_WASH, from);
  else                          enterStage(STAGE_RINSE, from);
}

void cmdPause(Src from){
  if(curStage==STAGE_IDLE || curStage==STAGE_DONE || curStage==STAGE_ALARM) return;
  // 保存暂停前本阶段已运行时长，恢复时还原，避免"重新计时"
  stageElapsedBeforePause = millis() - stageStartMs;
  isPaused = true;
  setMotor(false); valve(false); pump(false);  // F4: 关闭所有执行器含排水泵
  broadcastState(from);
}

void cmdResume(Src from){
  if(!isPaused) return;
  isPaused = false;
  // 关键：还原暂停前的进度，而不是从 0 重新计时
  stageStartMs = millis() - stageElapsedBeforePause;
  stageElapsedBeforePause = 0;
  // 恢复本阶段硬件（不调 enterStage 避免重置 stageDurMs/broadcastState 两次）
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
  // 手动排水态：再点 stop = 结束排水，重置回 IDLE
  if(curStage==STAGE_DRAIN && isStopping){
    curStage = STAGE_IDLE;
    isStopping = false;
    setMotor(false); valve(false); pump(false);
    broadcastState(from);
    return;
  }
  // 运行中：停止运行，进入手动排水（不计时，等用户点"结束排水"）
  if(curStage!=STAGE_IDLE && curStage!=STAGE_DONE && curStage!=STAGE_ALARM){
    isStopping = true;
    enterStage(STAGE_DRAIN, from);  // enterStage 内部检测 isStopping，设 stageDurMs=0
  } else if(curStage==STAGE_DONE || curStage==STAGE_ALARM){
    // 完成态/报警态：重置回 IDLE
    curStage = STAGE_IDLE;
    isStopping = false;
    setMotor(false); valve(false); pump(false);
    broadcastState(from);
  }
}

// ===== WiFi 配网指令：保存到 NVS，准备重启自连 =====
// 延迟重启机制（替代旧版 Task::once）
bool pendingReboot = false;
uint32_t rebootAtMs = 0;
void scheduleReboot(uint32_t delayMs){
  pendingReboot = true;
  rebootAtMs = millis() + delayMs;
}

void cmdSetWifi(const String& ssid, const String& pwd, Src from){
  if(ssid.length()==0){
    bleSend("{\"wifi\":\"error\",\"msg\":\"ssid 为空\"}\n");
    return;
  }
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pwd",  pwd);
  prefs.end();
  // 通知小程序
  bleSend("{\"wifi\":\"saving\",\"ssid\":"+ssid+"\"}\n");
  delay(300);  // 确保发送出去
  // 标记 800ms 后重启
  scheduleReboot(800);
}

// 查询 WiFi 状态
void cmdQueryWifi(Src from){
  String json = "{\"wifi\":\"";
  json += wifiConnected ? "connected" : "disconnected";
  json += "\",\"ssid\":"+wifiSsid+"\"";
  json += ",\"ip\":"+wifiIp+"\"";
  json += ",\"rssi\":"+String(WiFi.RSSI());
  json += "}\n";
  bleSend(json);
}

// ============================================================
//  通道 A：蓝牙 BLE 适配层
// ============================================================
BLEServer* bleServer = nullptr;
BLECharacteristic* bleChar = nullptr;
bool bleConnected = false;
String rxBuffer = "";

class BleServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; }
  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    BLEDevice::startAdvertising();
  }
};

class BleCharCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    // ESP32 core 3.x: getValue() 返回 String，不是 std::string
    String v = c->getValue().c_str();
    for(char ch : v){
      if(ch=='\n'){
        parseBleCmd(rxBuffer);
        rxBuffer = "";
      } else if(ch!='\r'){
        rxBuffer += ch;
      }
    }
  }
};

// 极简 JSON 解析
String extractField(const String& s, const char* key){
  String pat = String("\"") + key + "\":\"";
  int i = s.indexOf(pat);
  if(i<0) return "";
  int start = i + pat.length();
  int end = s.indexOf("\"", start);
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
  else if(cmd=="wifi"){
    String ssid = extractField(line, "ssid");
    String pwd  = extractField(line, "pwd");
    cmdSetWifi(ssid, pwd, SRC_BLE);
  }
  else if(cmd=="wifi_status") cmdQueryWifi(SRC_BLE);
  // ===== BLE OTA 固件升级 =====
  else if(cmd=="ota_begin"){
    // 开始 OTA：{"cmd":"ota_begin","size":12345}
    uint32_t sz = extractField(line, "size").toInt();
    otaTotalSize = sz;
    otaWritten = 0;
    otaActive = true;
    if(Update.begin(sz)){
      bleSend(String("{\"ota\":\"begin\",\"ok\":1}\n"));
    } else {
      otaActive = false;
      bleSend(String("{\"ota\":\"begin\",\"ok\":0}\n"));
    }
  }
  else if(cmd=="ota_data" && otaActive){
    // 数据分片：{"cmd":"ota_data","seq":0,"data":"base64..."}
    String b64 = extractField(line, "data");
    // base64 解码
    size_t outLen = 0;
    uint8_t* buf = (uint8_t*)malloc(b64.length());
    if(mbedtls_base64_decode(buf, b64.length(), &outLen, (const uint8_t*)b64.c_str(), b64.length()) == 0){
      Update.write(buf, outLen);
      otaWritten += outLen;
    }
    free(buf);
    // 回 ACK
    bleSend(String("{\"ota\":\"ack\",\":") + extractField(line,"seq") + "}\n");
  }
  else if(cmd=="ota_end" && otaActive){
    // 结束：校验并应用
    if(Update.end(true)){
      otaActive = false;
      bleSend(String("{\"ota\":\"end\",\"ok\":1}\n"));
      // 等小程序发 reboot 命令再重启
    } else {
      otaActive = false;
      bleSend(String("{\"ota\":\"end\",\"ok\":0}\n"));
    }
  }
  else if(cmd=="reboot"){
    ESP.restart();
  }
}

void bleSend(const String& s){
  if(bleConnected && bleChar){
    bleChar->setValue(s.c_str());
    bleChar->notify();
  }
}

// ============================================================
//  通道 B：HomeKit HAP 适配层
// ============================================================
struct WasherFaucet : Service::Faucet {
  SpanCharacteristic *active;
  SpanCharacteristic *statusFault;
  SpanCharacteristic *remainingDur;
  SpanCharacteristic *curTemp;
  SpanCharacteristic *waterLevel;

  WasherFaucet() : Service::Faucet() {
    // HomeSpan 2.1.8：枚举值在 Characteristic::X 命名空间，或直接用数字
    active = new Characteristic::Active(0);           // 0=INACTIVE, 1=ACTIVE
    statusFault = new Characteristic::StatusFault(0);  // 0=NO_FAULT, 1=FAULT
    remainingDur = new Characteristic::RemainingDuration(0);
    curTemp = new Characteristic::CurrentTemperature(25);
    waterLevel = new Characteristic::WaterLevel(0);
    new Characteristic::Name("Rinse");
    new Characteristic::ConfiguredName("Rinse");
  }

  // HomeSpan 2.1.8：用 update() 替代 setActive()
  // 当 HomeKit 要修改 Active 特征时，update() 被调用
  boolean update() override {
    if(active->updated()){
      bool value = active->getNewVal();
      if(value){
        // 启动
        if(curStage==STAGE_IDLE || curStage==STAGE_DONE || curStage==STAGE_ALARM){
          cmdStart("", SRC_HAP);
        } else if(isPaused){
          cmdResume(SRC_HAP);
        }
      } else {
        // 停止
        cmdStop(SRC_HAP);
      }
    }
    return true;
  }

  // 定期更新特征值（替代旧版 loop + setTimeVal 节流）
  void loop() override {
    // timeVal() 返回自上次更新以来的毫秒数，用于节流
    if(active->timeVal() < 1000) return;

    statusFault->setVal(curStage==STAGE_ALARM ? 1 : 0);  // 1=FAULT, 0=NO_FAULT
    remainingDur->setVal(remainSeconds());
    float t = readTempC();
    // HAP spec: -273 表示无值；传感器有效才报实际温度
    curTemp->setVal(t < 0 ? -273 : t);
    waterLevel->setVal(waterLevelPct());
  }
};
WasherFaucet* g_faucet = nullptr;

struct ModeSwitch : Service::Switch {
  SpanCharacteristic *on;
  uint8_t modeIdx;
  // 延迟归零机制（替代 Task::once）
  bool pendingReset = false;
  uint32_t resetAtMs = 0;

  ModeSwitch(uint8_t idx) : Service::Switch() {
    modeIdx = idx;
    on = new Characteristic::On(false);
    new Characteristic::Name(MODES[idx].name);
  }

  // HomeSpan 2.1.8：用 update() 替代 setOn()
  boolean update() override {
    if(on->updated()){
      bool value = on->getNewVal();
      if(value){
        // 启动该模式
        cmdStart(MODES[modeIdx].name, SRC_HAP);
        if(g_faucet) g_faucet->active->setVal(1);  // 1=ACTIVE
        // 500ms 后自动归零（触发器语义）
        pendingReset = true;
        resetAtMs = millis() + 500;
      }
    }
    return true;
  }

  void loop() override {
    if(pendingReset && millis() > resetAtMs){
      on->setVal(false);
      pendingReset = false;
    }
  }
};

// ============================================================
//  双通道状态广播（避免循环推送）
// ============================================================
void broadcastState(Src from){
  String stage = STAGE_NAMES[curStage];
  uint32_t rem = remainSeconds();
  float t = readTempC();

  // BLE 推送（永远推，BLE 不会回环）
  // total = 当前环节总时长（秒），用于小程序显示当前环节倒计时
  uint32_t stageTotalSec = (stageDurMs>0) ? stageDurMs/1000 : 0;
  String json = "{\"state\":"+stage+",\"stage\":"+stage+",\"remain\":"+String(rem)+",\"total\":"+String(stageTotalSec)+",\"temp\":"+(int)(t<0?-1:t);
  json += ",\"paused\":" + String(isPaused ? 1 : 0);
  json += ",\"ver\":\"" FW_VERSION "\"";
  if(curStage==STAGE_ALARM && alarmMsg.length()) json += ",\"code\":\"E1\",\"msg\":"+alarmMsg+"\"";
  json += "}\n";
  bleSend(json);

  // HAP 推送（来自 HAP 的指令不回推 Active，避免 setVal 触发 update() 死循环）
  if(g_faucet){
    if(from != SRC_HAP){
      g_faucet->active->setVal(curStage==STAGE_IDLE||curStage==STAGE_DONE||curStage==STAGE_ALARM ? 0 : 1);  // 0=INACTIVE, 1=ACTIVE
    }
    g_faucet->statusFault->setVal(curStage==STAGE_ALARM?1:0);  // 1=FAULT, 0=NO_FAULT
    g_faucet->remainingDur->setVal(rem);
    g_faucet->curTemp->setVal(t);
    g_faucet->waterLevel->setVal(waterLevelPct());
  }
}

// ============================================================
//  状态机节拍（用 millis 替代旧版 Task::repeat）
// ============================================================
uint32_t lastWasherTickMs = 0;
uint32_t lastWifiTickMs = 0;
const uint32_t WASHER_TICK_INTERVAL = 500;   // 500ms
const uint32_t WIFI_TICK_INTERVAL = 2000;    // 2000ms

void washerTick(){
  if(isPaused) return;
  if(curStage==STAGE_IDLE || curStage==STAGE_DONE || curStage==STAGE_ALARM) return;

  uint32_t elapsed = millis() - stageStartMs;
  switch(curStage){
    case STAGE_FILL:
      // 纯计时器：到时间就进洗涤（无水位传感器）
      if(elapsed > stageDurMs){ valve(false); enterStage(STAGE_WASH, SRC_INTERNAL); }
      break;
    case STAGE_WASH:
      // BTS7960 正反交替：每 5s 切换方向，用 IN1/IN2
      if((elapsed/5000)%2==0) setMotor(true, true);   // 正转
      else                    setMotor(true, false);  // 反转
      if(elapsed>stageDurMs) enterStage(STAGE_DRAIN, SRC_INTERNAL);
      break;
    case STAGE_RINSE:
      // 漂洗也是正反交替
      if((elapsed/5000)%2==0) setMotor(true, true);
      else                    setMotor(true, false);
      if(elapsed>stageDurMs){
        rinseCount++;
        enterStage(STAGE_DRAIN, SRC_INTERNAL);
      }
      break;
    case STAGE_DRAIN:
      // 手动排水（isStopping）：不计时，等用户点"结束排水"
      if(isStopping) break;
      // 正常排水：到时间进下一阶段
      if(elapsed > stageDurMs){
        if(curParam->rinseCycles>0 && rinseCount<curParam->rinseCycles){
          enterStage(STAGE_RINSE, SRC_INTERNAL);
        } else {
          enterStage(STAGE_DONE, SRC_INTERNAL);
        }
      }
      break;
    default: break;
  }
}

// ===== WiFi 状态机节拍：监测连接状态，成功后广播 =====
void wifiTick(){
  static uint8_t broadcastOnce = 0;
  bool nowConnected = (WiFi.status() == WL_CONNECTED);

  // 状态变化
  if(nowConnected != wifiConnected){
    wifiConnected = nowConnected;
    if(nowConnected){
      wifiIp = WiFi.localIP().toString();
      Serial.printf("[WiFi] 连上 %s, IP=%s, RSSI=%d\n",
                    WiFi.SSID().c_str(), wifiIp.c_str(), WiFi.RSSI());
      // 通知小程序
      bleSend("{\"wifi\":\"connected\",\"ssid\":"+String(WiFi.SSID().c_str())+"\",\"ip\":"+wifiIp+"\",\"rssi\":"+String(WiFi.RSSI())+"}\n");
      broadcastOnce = 0;
    } else {
      Serial.println("[WiFi] 断开，尝试重连...");
      bleSend("{\"wifi\":\"disconnected\"}\n");
      WiFi.reconnect();
      wifiConnStartMs = millis();
    }
  }

  // 连接尝试超时（30s）—— F7: 超时后也主动重连，避免初始连接失败永远不重连
  if(!wifiConnected && millis() - wifiConnStartMs > 30000){
    bleSend("{\"wifi\":\"timeout\"}\n");
    WiFi.reconnect();  // F7: 超时后主动重连
    wifiConnStartMs = millis();  // 重置避免连续推送
  }
}

// ============================================================
//  setup / loop
// ============================================================
// 这块 ESP32-S3 开发板有两个 USB Type-C 口：
//   - USB-OTG 口：走原生 USB-Serial/JTAG（之前那个）
//   - CH343 UART 口：走 UART0（GPIO43/44），普通 Serial 即可输出（本口）
// 接在 CH343 UART 口时，标准 Serial.begin(115200) + Serial.println 就能直接看到日志。
void setup(){
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] ESP32-S3 Rinse启动中...");

  // 引脚
  pinMode(PIN_MOTOR_IN1, OUTPUT);
  pinMode(PIN_MOTOR_IN2, OUTPUT);
  pinMode(PIN_VALVE, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);
  // 浮球开关：常开型，内部上拉，触发时拉低
  // 浮球开关已移除，无需初始化
  motorPwmInit();   // 兼容 ESP32 / ESP32-S3 的 LEDC 初始化
  valve(false); pump(false); setMotor(false);
  dallas.begin();
  analogReadResolution(12);   // ESP32-S3 默认 12bit ADC，显式声明更稳

  // ===== 蓝牙初始化（小程序通道） =====
  BLEDevice::init("Rinse-设备");
  BLEDevice::setMTU(185);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCb());
  BLEService* bleSvc = bleServer->createService(BLE_SERVICE_UUID);
  bleChar = bleSvc->createCharacteristic(
    BLE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  bleChar->addDescriptor(new BLE2902());
  bleChar->setCallbacks(new BleCharCb());
  bleSvc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  BLEDevice::startAdvertising();

  // ===== 从 NVS 读取 WiFi 凭据，自动连接 =====
  prefs.begin("wifi", true);  // 只读
  wifiSsid = prefs.getString("ssid", "");
  wifiPwd  = prefs.getString("pwd",  "");
  prefs.end();

  if(wifiSsid.length() > 0){
    Serial.printf("[WiFi] 从 NVS 读取到 SSID=%s，开始连接...\n", wifiSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPwd.c_str());
    wifiConnStartMs = millis();
    // 非阻塞：wifiTick 会持续监测并广播
  } else {
    Serial.println("[WiFi] NVS 未保存 WiFi，等待小程序配网");
    bleSend("{\"wifi\":\"unprovisioned\"}\n");
  }

  // ===== HomeSpan 初始化（HomeKit 通道） =====
  homeSpan.setLogLevel(1);
  homeSpan.setPairingCode("46622668");  // 必须正好 8 位数字
  homeSpan.enableOTA();

  homeSpan.begin(Category::Faucets, "Rinse");
  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Manufacturer("DualTrack-DIY");
  new Characteristic::SerialNumber("DT-WASHER-001");
  new Characteristic::Model("ESP32-Washer-Dual");
  new Characteristic::FirmwareRevision("2.0.0");

  g_faucet = new WasherFaucet();
  for(uint8_t i=0;i<MODE_COUNT;i++) new ModeSwitch(i);

  // 状态机节拍改为 millis 调度（替代旧版 Task::repeat）
  lastWasherTickMs = millis();
  lastWifiTickMs = millis();

  Serial.println("[Dual] DIY Washer ready. BLE + HomeKit both up.");
  Serial.println("[Dual] 配对码: 466-22-668");
  Serial.println("[Dual] 蓝牙名: Rinse-设备");
  Serial.flush();  // 确保输出刷出去
}

void loop(){
  homeSpan.poll();   // HomeSpan 内部跑 HAP；BLE 由 ESP-IDF 后台处理

  // 状态机节拍 500ms
  uint32_t now = millis();
  if(now - lastWasherTickMs >= WASHER_TICK_INTERVAL){
    lastWasherTickMs = now;
    washerTick();
  }
  // WiFi 监测节拍 2000ms
  if(now - lastWifiTickMs >= WIFI_TICK_INTERVAL){
    lastWifiTickMs = now;
    wifiTick();
  }
  // 不做定时广播：只在状态变化时广播（enterStage/pause/resume/stop）
  // 小程序本地每秒匀速倒数，阶段结束时广播校准
  // 延迟重启检查（配网后 800ms 重启）
  if(pendingReboot && millis() > rebootAtMs){
    bleSend("{\"wifi\":\"reboot\"}\n");
    delay(200);
    ESP.restart();
  }
}

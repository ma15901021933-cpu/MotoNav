#pragma once
// v7.13 导航播报从圆屏喇叭出声：手机只传合成好的声音（大块数据类型 5），不再占用手机喇叭。
// 由 MotoNav.ino 按依赖顺序包含（在 MapState.h 之后、BulkReceiver.h 之前），仅编译一次。
#include <ESP_I2S.h>
#include "../../VoiceClip.h"

#define VOICE_BULK 5
#define ES8311_ADDR 0x18

I2SClass voiceI2s;
bool voiceReady = false;               // 编解码芯片和 I2S 都初始化成功才告诉手机 CAP,VOX,1
int voiceVolume = 80;                  // 0–100，存在 Preferences "voxvol"
String pendingVoiceAck;                // 单独一条，不和地图的 K 确认互相覆盖
volatile int voiceEndedId = -1;        // 播放任务写，主循环发 "VE,编号"
volatile bool voiceStopReq = false;    // 手机点了静音：停下正在放的，丢掉下一句
volatile bool voiceToneReq = false;    // "#VT" 喇叭自检：两声短“嘀”，不需要手机合成声音

// 收齐的一句放进 pending；正在放的那句在 playing。新的一句只替换还没开始放的那句。
static uint8_t *voicePending = nullptr, *voicePlaying = nullptr;
static uint32_t voicePendingSize = 0;
static int voicePendingId = -1;
static TaskHandle_t voiceTask = nullptr;
static portMUX_TYPE voiceMux = portMUX_INITIALIZER_UNLOCKED;

static bool es8311Write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}

// 只开放音（DAC）：MCLK 由引脚输入 4.096 MHz = 256 × 16 kHz，16 位 I2S 从模式。
// 寄存器取值按乐鑫 es8311 驱动（微雪 1.75C 07_ES8311 示例）对应这一组时钟算出。
static bool es8311Init() {
  static const uint8_t regs[][2] = {
    {0x00, 0x1F}, {0x00, 0x00}, {0x00, 0x80},      // 复位后上电，从模式
    {0x01, 0x3F}, {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10}, {0x05, 0x00},
    {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},      // 时钟分频：16 kHz，BCLK = MCLK / 4
    {0x09, 0x0C}, {0x0A, 0x0C},                    // 输入输出都是 16 位 I2S
    {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00}, {0x13, 0x10},   // 模拟部分、DAC、耳机驱动上电
    {0x1C, 0x6A}, {0x37, 0x08}, {0x31, 0x00}};
  for (auto &r : regs) if (!es8311Write(r[0], r[1])) return false;
  return true;
}

void voiceSetVolume(int v) {
  voiceVolume = constrain(v, 0, 100);
  if (voiceReady) es8311Write(0x32, voiceVolume ? voiceVolume * 256 / 100 - 1 : 0);
}

static void voiceWrite(const int16_t *stereo, size_t frames) {
  const uint8_t *p = (const uint8_t *)stereo;
  size_t left = frames * 4;
  while (left) {
    size_t w = voiceI2s.write(p, left);
    if (!w) break;
    p += w; left -= w;
  }
}

static void voiceTone(int16_t *stereo) {          // 880 Hz，200 ms × 2，中间停 120 ms
  uint32_t t = 0;
  for (int beep = 0; beep < 2; beep++) {
    for (int k = 0; k < 2 * VoiceClip::RATE / 10 / 256; k++) {
      for (int i = 0; i < 256; i++, t++) stereo[2 * i] = stereo[2 * i + 1] = (int16_t)(12000 * sinf(2 * PI * 880 * t / VoiceClip::RATE));
      voiceWrite(stereo, 256);
    }
    memset(stereo, 0, 512 * sizeof(int16_t));
    for (int k = 0; k < VoiceClip::RATE * 12 / 100 / 256; k++) voiceWrite(stereo, 256);
  }
}

static void voiceLoop(void *) {
  static int16_t mono[256], stereo[512];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool speaking = false;
    if (voiceToneReq) {
      voiceToneReq = false;
      digitalWrite(PA, HIGH); speaking = true; delay(20);
      Serial.println("VOICE tone test");
      voiceTone(stereo);
    }
    for (;;) {
      int id;
      portENTER_CRITICAL(&voiceMux);
      id = voicePendingId;
      uint32_t size = voicePendingSize;
      if (id >= 0) { uint8_t *t = voicePlaying; voicePlaying = voicePending; voicePending = t; voicePendingId = -1; }
      portEXIT_CRITICAL(&voiceMux);
      if (id < 0) break;
      voiceStopReq = false;
      VoiceClip clip;
      if (!clip.begin(voicePlaying, size)) { voiceEndedId = id; continue; }
      if (!speaking) { digitalWrite(PA, HIGH); speaking = true; delay(20); }   // 功放只在播报时打开，平时不出底噪
      Serial.printf("VOICE play id=%d samples=%lu\n", id, (unsigned long)clip.samples());
      size_t n;
      while (!voiceStopReq && (n = clip.decode(mono, 256)) > 0) {
        for (size_t i = 0; i < n; i++) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
        voiceWrite(stereo, n);
      }
      voiceEndedId = id;
    }
    if (speaking) {                                    // 等 DMA 里最后一点放完再关功放
      memset(stereo, 0, sizeof(stereo));
      for (int i = 0; i < 6; i++) voiceI2s.write((const uint8_t *)stereo, sizeof(stereo));
      digitalWrite(PA, LOW);
    }
  }
}

void voiceInit() {
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);
  {
    Preferences pr;
    pr.begin("motonav", true);
    voiceVolume = constrain(pr.getInt("voxvol", 80), 0, 100);
    pr.end();
  }
  Bulk &b = bulks[VOICE_BULK];
  b.cap = VoiceClip::MAX_BYTES; b.buf = (uint8_t *)ps_malloc(b.cap); b.id = -1; b.done = false;
  voicePending = (uint8_t *)ps_malloc(VoiceClip::MAX_BYTES);
  voicePlaying = (uint8_t *)ps_malloc(VoiceClip::MAX_BYTES);
  if (!b.buf || !voicePending || !voicePlaying) { b.buf = nullptr; Serial.println("VOICE no memory"); return; }
  voiceI2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!voiceI2s.begin(I2S_MODE_STD, VoiceClip::RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("VOICE i2s failed"); return;
  }
  if (!es8311Init()) { voiceI2s.end(); Serial.println("VOICE es8311 not found"); return; }
  voiceReady = true;
  voiceSetVolume(voiceVolume);
  // 核心 0、比刷屏和画底图高：大部分时间阻塞在 DMA 上，只占很少计算。
  if (xTaskCreatePinnedToCore(voiceLoop, "voice", 4096, nullptr, 5, &voiceTask, 0) != pdPASS) { voiceTask = nullptr; voiceReady = false; }
  Serial.printf("VOICE ready=%d volume=%d\n", voiceReady, voiceVolume);
}

// 一句收齐：先检查格式，再复制给播放任务。返回 false 时不回确认。
bool voiceQueue(const uint8_t *data, uint32_t size, int id) {
  VoiceClip check;
  if (!voiceReady || !check.begin(data, size)) return false;
  portENTER_CRITICAL(&voiceMux);
  const int replaced = voicePendingId;
  voicePendingId = -1;                                 // 先作废旧的，复制期间播放任务不会取到半截
  portEXIT_CRITICAL(&voiceMux);
  if (replaced >= 0) voiceEndedId = replaced;          // 被顶掉没放的那句也算结束
  memcpy(voicePending, data, size);
  portENTER_CRITICAL(&voiceMux);
  voicePendingSize = size; voicePendingId = id;
  portEXIT_CRITICAL(&voiceMux);
  xTaskNotifyGive(voiceTask);
  return true;
}

void handleVoiceText(const String &m) {
  if (m == "#VT") {                                    // 喇叭自检
    if (voiceReady) { voiceToneReq = true; xTaskNotifyGive(voiceTask); }
    Serial.printf("VOICE test ready=%d volume=%d\n", voiceReady, voiceVolume);
    return;
  }
  if (m == "#VX") {                                    // 静音：正在放的停下，没放的丢掉
    portENTER_CRITICAL(&voiceMux);
    const int dropped = voicePendingId; voicePendingId = -1;
    portEXIT_CRITICAL(&voiceMux);
    if (dropped >= 0) voiceEndedId = dropped;
    voiceStopReq = true;
    return;
  }
  if (m.startsWith("#VV,")) {                          // 音量
    int v = constrain(m.substring(4).toInt(), 0, 100);
    if (v != voiceVolume) {
      voiceSetVolume(v);
      Preferences pr; pr.begin("motonav", false); pr.putInt("voxvol", voiceVolume); pr.end();
    }
  }
}

// 调试：USB 串口也能发 "#VT" / "#VV,80"，不用手机就能测喇叭。
void voiceSerialPoll() {
  static char line[24]; static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      line[len] = 0;
      if (len) handleVoiceText(String(line));
      len = 0;
    } else if (len < sizeof(line) - 1) line[len++] = c;
  }
}

void voiceShutdown() { digitalWrite(PA, LOW); }

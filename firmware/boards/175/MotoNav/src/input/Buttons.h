#pragma once
// v7.13 圆屏两个实体键：PWR 短按音量 +10，BOOT 短按音量 -10（上北下南在手机设置里切换）。
// 由 MotoNav.ino 按依赖顺序包含（在 VoicePlayer.h、MapState.h 之后），仅编译一次。
// PWR 接在电源芯片上：长按仍是硬件关机，这里只读它的“短按”中断标志（AXP2101 0x41/0x49 第 3 位）。
#define BOOT_KEY 0

void buttonsInit() {
  pinMode(BOOT_KEY, INPUT_PULLUP);
  Preferences pr;
  pr.begin("motonav", true);
  northUp = pr.getBool("northup", true);
  pr.end();
  if (pmuOK) {
    pmuSetBit(0x41, 3);                                // 允许记录 PWR 短按
    Wire.beginTransmission(PMU_ADDR); Wire.write(0x49); Wire.write((uint8_t)0x08); Wire.endTransmission();   // 清掉开机那一下
  }
  Serial.printf("BUTTONS northUp=%d\n", northUp);
}

void buttonsPoll() {
  const uint32_t now = millis();
  static uint32_t pmuAt = 0;
  if (pmuOK && now - pmuAt >= 60) {
    pmuAt = now;
    const int st = pmuRead(0x49);
    if (st > 0 && (st & 0x08)) {
      Wire.beginTransmission(PMU_ADDR); Wire.write(0x49); Wire.write((uint8_t)0x08); Wire.endTransmission();   // 写 1 清除
      voiceStep(+10);
    }
  }
  static uint32_t downAt = 0;
  if (digitalRead(BOOT_KEY) == LOW) {
    if (!downAt) downAt = now | 1;
  } else if (downAt) {
    if (now - downAt >= 30) voiceStep(-10);            // 30 ms 以下当作抖动
    downAt = 0;
  }
}

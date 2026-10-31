#pragma once
// v7.13 路口放大图：手机把高德的放大图压成 JPEG（大块数据类型 6，'J' '1' + JPEG），
// 圆屏用芯片 ROM 里的 JPEG 解码器解开，盖在地图中间一块（只替换地图，车速、车道、底部转向距离和外圈都不挡）。
// "#J,0" 收起；导航结束、或 90 秒没有新图也收起。由 MotoNav.ino 按依赖顺序包含，仅编译一次。
#include "esp32s3/rom/tjpgd.h"

#define JCT_BULK 6
#define JCT_W 300                           // 高德放大图宽:高 = 25:16
#define JCT_H 192
#define JCT_Y 146                           // 上面是车道（到 140），下面是转向箭头和距离（344 起）
#define JCT_MAX_BYTES 65536

uint16_t *jctPix = nullptr;
bool jctOn = false;
unsigned long jctAt = 0;

struct JctSrc { const uint8_t *p; uint32_t left; int ox, oy; };

static UINT jctIn(JDEC *jd, BYTE *buf, UINT n) {
  JctSrc *s = (JctSrc *)jd->device;
  if (n > s->left) n = s->left;
  if (buf) memcpy(buf, s->p, n);
  s->p += n; s->left -= n;
  return n;
}

static UINT jctOut(JDEC *jd, void *bitmap, JRECT *r) {   // ROM 版输出 RGB888
  JctSrc *s = (JctSrc *)jd->device;
  const uint8_t *src = (const uint8_t *)bitmap;
  for (int y = r->top; y <= r->bottom; y++) {
    const int ty = y + s->oy;
    for (int x = r->left; x <= r->right; x++, src += 3) {
      const int tx = x + s->ox;
      if (tx < 0 || tx >= JCT_W || ty < 0 || ty >= JCT_H) continue;
      jctPix[ty * JCT_W + tx] = ((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3);
    }
  }
  return 1;
}

void jctInit() {
  Bulk &b = bulks[JCT_BULK];
  b.cap = JCT_MAX_BYTES; b.buf = (uint8_t *)ps_malloc(b.cap); b.id = -1; b.done = false;
  jctPix = (uint16_t *)ps_malloc(JCT_W * JCT_H * 2);
  if (!b.buf || !jctPix) { b.buf = nullptr; Serial.println("JUNCTION no memory"); }
}

// 收齐一张：解码成功才回确认
bool jctReceive(const uint8_t *d, uint32_t n) {
  if (!jctPix || n < 4 || d[0] != 'J' || d[1] != '1') return false;
  static uint8_t work[4096];
  JDEC jd;
  JctSrc s = {d + 2, n - 2, 0, 0};
  if (jd_prepare(&jd, jctIn, work, sizeof(work), &s) != JDR_OK) { Serial.println("JUNCTION bad jpeg"); return false; }
  s.ox = (JCT_W - (int)jd.width) / 2; s.oy = (JCT_H - (int)jd.height) / 2;   // 尺寸不同就居中
  memset(jctPix, 0, JCT_W * JCT_H * 2);
  const unsigned long t = millis();
  if (jd_decomp(&jd, jctOut, 0) != JDR_OK) { Serial.println("JUNCTION decode failed"); return false; }
  Serial.printf("JUNCTION %ux%u %lu bytes %lu ms\n", jd.width, jd.height, (unsigned long)n, millis() - t);
  jctOn = true; jctAt = millis();
  return true;
}

// 调试：USB 串口发 "#JB,字节数" 再发 'J' '1' + JPEG 原始字节，不用手机就能看放大图
bool jctSerialLoad(uint32_t len) {
  Bulk &b = bulks[JCT_BULK];
  if (!b.buf || len < 4 || len > b.cap) return false;
  uint32_t got = 0; const unsigned long t = millis();
  while (got < len && millis() - t < 5000) {
    int c = Serial.available();
    if (c > 0) got += Serial.readBytes(b.buf + got, min((uint32_t)c, len - got));
    else delay(1);
  }
  b.id = -1; b.done = false;                           // 手机再发同一编号也会重新收
  const bool ok = got == len && jctReceive(b.buf, len);
  Serial.printf("JUNCTION serial got=%lu ok=%d\n", (unsigned long)got, ok);
  if (ok) navDirty = true;
  return ok;
}

// 地图画完、车速等信息画之前调用：圆角矩形贴上去，加一圈黑边和地图分开
void drawJunction() {
  if (!jctOn || !jctPix) return;
  if (millis() - jctAt > 90000) { jctOn = false; return; }   // 手机没说收起也不一直挡着地图
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  const int x0 = CX - JCT_W / 2, R = 10;
  for (int y = -2; y < JCT_H + 2; y++) {
    const int fy = JCT_Y + y;
    if (fy < 0 || fy >= LCD_HEIGHT) continue;
    for (int x = -2; x < JCT_W + 2; x++) {
      const int dx = x < R ? R - x : (x >= JCT_W - R ? x - (JCT_W - R - 1) : 0);
      const int dy = y < R ? R - y : (y >= JCT_H - R ? y - (JCT_H - R - 1) : 0);
      const int d2 = dx * dx + dy * dy;
      if (d2 > (R + 2) * (R + 2)) continue;
      uint16_t &p = fb[fy * LCD_WIDTH + x0 + x];
      p = (x < 0 || y < 0 || x >= JCT_W || y >= JCT_H || d2 > R * R) ? C_BLACK : jctPix[y * JCT_W + x];
    }
  }
}

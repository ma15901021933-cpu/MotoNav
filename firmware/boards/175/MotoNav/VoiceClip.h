#pragma once
#include <stddef.h>
#include <stdint.h>

// 手机合成好的导航播报，经大块数据类型 5 整段传来，圆屏自己用喇叭放。
// 格式（小端）：'V' '1' 采样率(2) 采样数(4) 初始预测值(2，有符号) 初始步长序号(1) 保留(1) + IMA ADPCM
// 单声道、每字节两个采样，低 4 位在前。只收 16 kHz：喇叭的 I2S 时钟固定在这个频率。
class VoiceClip {
public:
  static constexpr uint32_t HEADER = 12;
  static constexpr uint32_t RATE = 16000;
  static constexpr uint32_t MAX_SAMPLES = RATE * 20;              // 一句播报最长 20 秒
  static constexpr uint32_t MAX_BYTES = HEADER + (MAX_SAMPLES + 1) / 2;

  // 检查整段数据；不合格就不播，也不回确认（手机会重发）。
  bool begin(const uint8_t *clip, uint32_t size) {
    data = nullptr; total = 0; pos = 0;
    if (!clip || size < HEADER || size > MAX_BYTES || clip[0] != 'V' || clip[1] != '1') return false;
    const uint32_t rate = clip[2] | (uint32_t)clip[3] << 8;
    const uint32_t count = clip[4] | (uint32_t)clip[5] << 8 | (uint32_t)clip[6] << 16 | (uint32_t)clip[7] << 24;
    const int index = clip[10];
    if (rate != RATE || count == 0 || count > MAX_SAMPLES || index > 88 || clip[11] != 0) return false;
    if (size != HEADER + (count + 1) / 2) return false;
    data = clip + HEADER; total = count;
    predictor = (int16_t)(clip[8] | clip[9] << 8); step = index;
    return true;
  }
  uint32_t samples() const { return total; }
  bool done() const { return pos >= total; }

  // 往下解出最多 max 个采样，返回实际个数；放完返回 0。
  size_t decode(int16_t *out, size_t max) {
    size_t n = 0;
    while (n < max && pos < total) {
      const uint8_t b = data[pos >> 1];
      out[n++] = next((pos & 1) ? b >> 4 : b & 15);
      pos++;
    }
    return n;
  }

private:
  const uint8_t *data = nullptr;
  uint32_t total = 0, pos = 0;
  int predictor = 0, step = 0;

  int16_t next(int code) {
    static const int16_t steps[89] = {
      7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
      107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
      876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
      5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
      27086, 29794, 32767};
    static const int8_t moves[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    const int s = steps[step];
    int diff = s >> 3;
    if (code & 4) diff += s;
    if (code & 2) diff += s >> 1;
    if (code & 1) diff += s >> 2;
    predictor += (code & 8) ? -diff : diff;
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    step += moves[code & 7];
    if (step < 0) step = 0;
    if (step > 88) step = 88;
    return (int16_t)predictor;
  }
};

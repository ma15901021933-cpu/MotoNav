#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../boards/175/MotoNav/VoiceClip.h"

// Same IMA ADPCM encoder as the iOS ScreenVoice packer.
static const int steps[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int moves[8] = {-1,-1,-1,-1,2,4,6,8};
std::vector<uint8_t> pack(const std::vector<int16_t> &pcm, uint32_t rate = 16000) {
  std::vector<uint8_t> out = {'V','1',(uint8_t)rate,(uint8_t)(rate>>8),(uint8_t)pcm.size(),(uint8_t)(pcm.size()>>8),(uint8_t)(pcm.size()>>16),(uint8_t)(pcm.size()>>24),0,0,0,0};
  int pred = 0, index = 0;
  for (size_t i = 0; i < pcm.size(); i++) {
    int step = steps[index], diff = pcm[i] - pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
    pred += (code & 8) ? -delta : delta;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    index += moves[code & 7]; if (index < 0) index = 0; if (index > 88) index = 88;
    if (i & 1) out.back() |= code << 4; else out.push_back(code);
  }
  return out;
}

int main() {
  // A 440 Hz tone survives the round trip closely once the step size has adapted.
  std::vector<int16_t> pcm(16001);
  for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(12000 * sin(2 * M_PI * 440 * i / 16000.0));
  auto clip = pack(pcm);
  assert(clip.size() == VoiceClip::HEADER + (pcm.size() + 1) / 2);
  VoiceClip v;
  assert(v.begin(clip.data(), clip.size()) && v.samples() == pcm.size() && !v.done());
  std::vector<int16_t> out; int16_t buf[256]; size_t n;
  while ((n = v.decode(buf, 256)) > 0) out.insert(out.end(), buf, buf + n);
  assert(out.size() == pcm.size() && v.done() && v.decode(buf, 256) == 0);
  double err = 0;
  for (size_t i = 200; i < pcm.size(); i++) err += fabs((double)out[i] - pcm[i]);
  assert(err / (pcm.size() - 200) < 400);

  // Rejected clips are never played (and the phone retransmits them).
  VoiceClip bad;
  assert(!bad.begin(nullptr, 0));
  assert(!bad.begin(clip.data(), clip.size() - 1));                 // truncated
  auto other = pack(pcm, 22050); assert(!bad.begin(other.data(), other.size()));
  auto magic = clip; magic[1] = '2'; assert(!bad.begin(magic.data(), magic.size()));
  auto index = clip; index[10] = 89; assert(!bad.begin(index.data(), index.size()));
  auto empty = pack({}); assert(!bad.begin(empty.data(), empty.size()));
  std::vector<int16_t> tooLong(VoiceClip::MAX_SAMPLES + 1);
  auto longClip = pack(tooLong); assert(!bad.begin(longClip.data(), longClip.size()));
  std::vector<int16_t> longest(VoiceClip::MAX_SAMPLES);
  auto maxClip = pack(longest); assert(maxClip.size() == VoiceClip::MAX_BYTES && bad.begin(maxClip.data(), maxClip.size()));

  // Odd sample counts: the final high nibble is padding.
  auto one = pack({1000}); VoiceClip single; assert(single.begin(one.data(), one.size()) && single.samples() == 1);
  assert(single.decode(buf, 256) == 1 && single.done());
  puts("PASS: ADPCM round trip, header/size validation, rate/length limits and odd sample counts");
}

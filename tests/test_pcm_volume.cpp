#include "../esp32/music_controller_ino/pcm_volume.h"

#include <cassert>
#include <cstdint>

int main() {
  uint8_t pcm[] = {
      0xff, 0x7f,  // +32767
      0x00, 0x80,  // -32768
      0xff, 0xff,  // -1
      0x00, 0x00,  // 0
  };
  assert(scale_pcm_s16le(pcm, sizeof(pcm), 50));
  const uint8_t half[] = {0xff, 0x3f, 0x00, 0xc0,
                          0x00, 0x00, 0x00, 0x00};
  for (size_t i = 0; i < sizeof(pcm); ++i) assert(pcm[i] == half[i]);

  assert(scale_pcm_s16le(pcm, sizeof(pcm), 100));
  for (size_t i = 0; i < sizeof(pcm); ++i) assert(pcm[i] == half[i]);

  assert(scale_pcm_s16le(pcm, sizeof(pcm), 0));
  for (uint8_t byte : pcm) assert(byte == 0);

  assert(!scale_pcm_s16le(pcm, sizeof(pcm), -1));
  assert(!scale_pcm_s16le(pcm, sizeof(pcm), 101));
  assert(!scale_pcm_s16le(pcm, sizeof(pcm) - 1, 50));
  assert(!scale_pcm_s16le(nullptr, sizeof(pcm), 50));
}

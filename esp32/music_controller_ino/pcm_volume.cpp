#include "pcm_volume.h"

#include <cstring>

bool scale_pcm_s16le(uint8_t *pcm, size_t byte_count, int volume_percent) {
  constexpr int kVolumeDiv = 100;
  if (pcm == nullptr || byte_count % 2 != 0 ||
      volume_percent < 0 || volume_percent > kVolumeDiv) return false;
  if (volume_percent == kVolumeDiv) return true;
  if (volume_percent == 0) {
    memset(pcm, 0, byte_count);
    return true;
  }

  for (size_t i = 0; i < byte_count; i += 2) {
    const uint16_t raw = static_cast<uint16_t>(pcm[i]) |
                         (static_cast<uint16_t>(pcm[i + 1]) << 8);
    const int32_t sample = raw < 0x8000u
                               ? static_cast<int32_t>(raw)
                               : static_cast<int32_t>(raw) - 65536;
    const int32_t scaled = sample * volume_percent / kVolumeDiv;
    const uint16_t bits = static_cast<uint16_t>(scaled);
    pcm[i] = static_cast<uint8_t>(bits);
    pcm[i + 1] = static_cast<uint8_t>(bits >> 8);
  }
  return true;
}

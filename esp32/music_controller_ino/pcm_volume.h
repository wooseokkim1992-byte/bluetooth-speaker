#pragma once

#include <stddef.h>
#include <stdint.h>

// PCM is signed 16-bit little-endian. 0%=mute, 100%=unchanged.
// Scaling the copied I2S buffer preserves the PCM queue.
bool scale_pcm_s16le(uint8_t *pcm, size_t byte_count, int volume_percent);

#pragma once

#include <stddef.h>
#include <stdint.h>

// Byte ring buffer. The caller synchronizes producer/consumer access and
// supplies complete PCM frames (4 bytes for 16-bit stereo) to read/write.
class PcmRingBuffer {
 public:
  PcmRingBuffer(uint8_t *storage, size_t capacity)
      : storage_(storage), capacity_(capacity) {}

  void clear();
  bool write(const uint8_t *data, size_t length);  // All or nothing.
  size_t read(uint8_t *out, size_t length);
  size_t size() const { return used_; }
  size_t free_space() const { return capacity_ - used_; }

 private:
  uint8_t *storage_;
  size_t capacity_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t used_ = 0;
};

#include "pcm_ring.h"

#include <cstring>

void PcmRingBuffer::clear() {
  head_ = 0;
  tail_ = 0;
  used_ = 0;
}

bool PcmRingBuffer::write(const uint8_t *data, size_t length) {
  if (storage_ == nullptr || capacity_ == 0 ||
      (length != 0 && data == nullptr) || length > free_space()) {
    return false;
  }
  const size_t first = length < capacity_ - tail_ ? length : capacity_ - tail_;
  if (first != 0) memcpy(storage_ + tail_, data, first);
  if (length > first) memcpy(storage_, data + first, length - first);
  tail_ = (tail_ + length) % capacity_;
  used_ += length;
  return true;
}

size_t PcmRingBuffer::read(uint8_t *out, size_t length) {
  if (storage_ == nullptr || capacity_ == 0 ||
      (length != 0 && out == nullptr)) {
    return 0;
  }
  if (length > used_) length = used_;
  const size_t first = length < capacity_ - head_ ? length : capacity_ - head_;
  if (first != 0) memcpy(out, storage_ + head_, first);
  if (length > first) memcpy(out + first, storage_, length - first);
  head_ = (head_ + length) % capacity_;
  used_ -= length;
  return length;
}

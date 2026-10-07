#include "../esp32/music_controller_ino/pcm_ring.h"

#include <cassert>
#include <cstdint>

int main() {
  uint8_t storage[8] = {};
  PcmRingBuffer ring(storage, sizeof(storage));
  const uint8_t first[6] = {1, 2, 3, 4, 5, 6};
  uint8_t out[8] = {};

  assert(ring.write(first, sizeof(first)));
  assert(ring.size() == 6);
  assert(!ring.write(first, 3));  // No partial packet on overflow.
  assert(ring.read(out, 4) == 4);
  assert(out[0] == 1 && out[3] == 4);

  const uint8_t second[6] = {7, 8, 9, 10, 11, 12};
  assert(ring.write(second, sizeof(second)));  // Wrap at the end.
  assert(ring.read(out, sizeof(out)) == sizeof(out));
  const uint8_t expected[8] = {5, 6, 7, 8, 9, 10, 11, 12};
  for (size_t i = 0; i < sizeof(out); ++i) assert(out[i] == expected[i]);

  assert(ring.size() == 0);
  assert(ring.write(first, sizeof(first)));
  ring.clear();
  assert(ring.size() == 0 && ring.free_space() == sizeof(storage));
}

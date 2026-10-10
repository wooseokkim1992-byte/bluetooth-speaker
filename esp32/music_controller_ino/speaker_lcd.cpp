#include "speaker_lcd.h"

#include <Arduino.h>
#include <Wire.h>

#include <cstring>

namespace {
constexpr int kSdaPin = 21;
constexpr int kSclPin = 19;  // GPIO22 is already the I2S data pin.
constexpr uint8_t kColumns = 16;
constexpr uint8_t kRs = 0x01;
constexpr uint8_t kEnable = 0x04;
constexpr uint8_t kBacklight = 0x08;

uint8_t lcd_address = 0;
bool lcd_ready = false;
char shown_lines[2][kColumns + 1] = {};

bool write_expander(uint8_t value) {
  Wire.beginTransmission(lcd_address);
  Wire.write(value | kBacklight);
  return Wire.endTransmission() == 0;
}

bool write_nibble(uint8_t nibble, bool data) {
  const uint8_t value = (nibble & 0xf0) | (data ? kRs : 0);
  const bool ok = write_expander(value) &&
                  write_expander(value | kEnable) &&
                  write_expander(value);
  delayMicroseconds(50);
  return ok;
}

bool send_byte(uint8_t value, bool data) {
  return write_nibble(value & 0xf0, data) &&
         write_nibble(static_cast<uint8_t>(value << 4), data);
}

bool command(uint8_t value) {
  return send_byte(value, false);
}

uint8_t find_backpack_address() {
  // PCF8574: 0x20-0x27; PCF8574A: 0x38-0x3f.
  for (uint8_t address = 0x20; address <= 0x3f; ++address) {
    if (address > 0x27 && address < 0x38) continue;
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) return address;
  }
  return 0;
}

void make_line(char *destination, const char *source) {
  memset(destination, ' ', kColumns);
  destination[kColumns] = '\0';
  if (source == nullptr) return;
  for (uint8_t column = 0; column < kColumns && source[column] != '\0'; ++column) {
    destination[column] = source[column];
  }
}

bool show_line(uint8_t row, const char *text) {
  char line[kColumns + 1];
  make_line(line, text);
  if (memcmp(shown_lines[row], line, sizeof(line)) == 0) return true;
  if (!command(row == 0 ? 0x80 : 0xc0)) return false;
  for (uint8_t column = 0; column < kColumns; ++column) {
    if (!send_byte(static_cast<uint8_t>(line[column]), true)) return false;
  }
  memcpy(shown_lines[row], line, sizeof(line));
  return true;
}
}  // namespace

bool speaker_lcd_begin() {
  if (!Wire.begin(kSdaPin, kSclPin)) {
    Serial.println("LCD I2C initialization failed");
    return false;
  }
  Wire.setClock(100000);
  lcd_address = find_backpack_address();
  if (lcd_address == 0) {
    Serial.println("LCD I2C backpack not found");
    return false;
  }

  delay(50);
  lcd_ready = write_nibble(0x30, false);
  delayMicroseconds(4500);
  lcd_ready = lcd_ready && write_nibble(0x30, false);
  delayMicroseconds(4500);
  lcd_ready = lcd_ready && write_nibble(0x30, false);
  delayMicroseconds(150);
  lcd_ready = lcd_ready && write_nibble(0x20, false) &&
              command(0x28) &&  // Four-bit bus, two lines.
              command(0x08) &&  // Display off during setup.
              command(0x01);    // Clear display.
  delay(2);
  lcd_ready = lcd_ready && command(0x06) && command(0x0c);
  Serial.printf("LCD I2C address=0x%02x, ready=%d\n", lcd_address, lcd_ready);
  return lcd_ready;
}

void speaker_lcd_show(const char *first_line, const char *second_line) {
  if (!lcd_ready) return;
  if (!show_line(0, first_line) || !show_line(1, second_line)) {
    lcd_ready = false;
    Serial.println("LCD I2C write failed");
  }
}

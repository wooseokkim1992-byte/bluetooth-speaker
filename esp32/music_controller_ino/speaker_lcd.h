#pragma once

// 16x2 HD44780 LCD with the common PCF8574 I2C backpack wiring.
bool speaker_lcd_begin();
void speaker_lcd_show(const char *first_line, const char *second_line);

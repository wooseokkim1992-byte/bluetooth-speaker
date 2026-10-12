#include <Wire.h> // I2C 기본 라이브러리
#include <LiquidCrystal_I2C.h>
#define SONG_LIST 5
// lcd라는 이름의 LCD 객체 생성
LiquidCrystal_I2C lcd(0x27, 16, 2);
// 함수 밖에 선언해야 loop()에서도 사용 가능

const char* messages[] = {
  "Hello ESP32!",
  "WiFi connected",
  "Music player",
  "Volume: 50%",
  "Ready to play"
};

int i = 0;
int lastBtnState = HIGH;
bool Move = true;

void SongList() {
  int next = (i+1) % SONG_LIST;
  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 0);
  lcd.print(">");
  lcd.setCursor(1, 0);
  lcd.print(messages[i]);

  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(1, 1);
  lcd.print(messages[next]);
}

void setup() {
  Wire.begin(21, 22);  // SDA=21, SCL=22
  lcd.init();
  lcd.backlight();

  pinMode(27, INPUT_PULLUP);
  Serial.begin(115200);

  SongList();
}

void loop() {
  int y = analogRead(33);
  int BtnState = digitalRead(27);

  if (lastBtnState == HIGH && BtnState == LOW) {
    delay(100);
    if (digitalRead(27) == LOW){
      Serial.print("selected : ");
      Serial.println(messages[i]);
    }
  }
  if (y > 1200 && y < 2000) {
    Move = true;
  }

  if (Move) {
    if (y > 4000) {
      i = (i + SONG_LIST - 1) % SONG_LIST;
      Move = false;
      SongList();
    }
    else if (y < 300) {
      i = (i + 1) % SONG_LIST;
      Move = false;
      SongList();
    }
  }

  delay(20);
}
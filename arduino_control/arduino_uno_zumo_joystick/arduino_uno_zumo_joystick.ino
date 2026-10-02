#include <SoftwareSerial.h>
#include <ZumoMotors.h>

// ==================================================
// M5StickC -> Arduino Uno UART
// ==================================================
// 配線:
//   M5StickC G32 -> Arduino Uno / Zumo A1
//   M5StickC GND -> Arduino Uno / Zumo GND
//
// A1はZumo Shieldのバッテリーモニタ用ジャンパを接続していないこと。
// SoftwareSerialのTX(D2)は今回は配線しない。

const uint8_t M5_RX_PIN = A1;
const uint8_t M5_TX_PIN = 2;
const long M5_BAUD = 9600;

SoftwareSerial m5Serial(M5_RX_PIN, M5_TX_PIN);


// ==================================================
// Zumo motors
// ==================================================

ZumoMotors motors;

// ZumoMotorsは -400 ～ 400
const int FORWARD_SPEED = 125;
const int TURN_SPEED = 110;

// 一定時間コマンドが来なければ安全のため停止
const unsigned long COMMAND_TIMEOUT_MS = 500;

unsigned long lastCommandTime = 0;
char activeCommand = 'S';


bool isDriveCommand(char command) {
  return
    command == 'F' ||
    command == 'B' ||
    command == 'L' ||
    command == 'R' ||
    command == 'S';
}


void applyDriveCommand(char command) {

  switch (command) {

    case 'F':
      // 前進
      motors.setSpeeds(FORWARD_SPEED, FORWARD_SPEED);
      break;

    case 'B':
      // 後退
      motors.setSpeeds(-FORWARD_SPEED, -FORWARD_SPEED);
      break;

    case 'L':
      // その場で左旋回
      motors.setSpeeds(-TURN_SPEED, TURN_SPEED);
      break;

    case 'R':
      // その場で右旋回
      motors.setSpeeds(TURN_SPEED, -TURN_SPEED);
      break;

    case 'S':
    default:
      motors.setSpeeds(0, 0);
      command = 'S';
      break;
  }

  if (command != activeCommand) {
    Serial.print("Drive: ");
    Serial.println(command);
  }

  activeCommand = command;
}


void setup() {

  // PCデバッグ用。D0/D1を使うが，M5Stickとの通信はA1なので競合しない。
  Serial.begin(115200);

  // M5StickCからのUART受信用
  m5Serial.begin(M5_BAUD);

  // 起動直後は停止
  applyDriveCommand('S');
  lastCommandTime = millis();

  Serial.println("Zumo UART controller started");
  Serial.println("Commands: F B L R S");
}


void loop() {

  // M5StickCから届いた文字をすべて処理
  while (m5Serial.available()) {

    char command = m5Serial.read();

    // 改行は無視
    if (command == '\r' || command == '\n') {
      continue;
    }

    // 念のため小文字にも対応
    if (command >= 'a' && command <= 'z') {
      command = command - 'a' + 'A';
    }

    if (!isDriveCommand(command)) {
      continue;
    }

    lastCommandTime = millis();
    applyDriveCommand(command);
  }

  // M5StickやBLE側が止まった場合のデッドマン停止
  if (
    activeCommand != 'S' &&
    millis() - lastCommandTime > COMMAND_TIMEOUT_MS
  ) {
    Serial.println("Command timeout -> STOP");
    applyDriveCommand('S');
  }
}

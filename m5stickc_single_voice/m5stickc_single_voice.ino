#include <M5StickC.h>
#include <math.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include "voice.h"
#include "hello.h"
#include "please_stay_here_16k.h"
#include "do_research_16k.h"
#include "stay_clean_16k.h"
#include "zunda_01.h"
#include "zunda_02.h"
#include "zunda_03.h"


// ==================================================
// Bluetooth (BLE)
// ==================================================

// M5Stack側のスケッチと同じ値にする
const char* BLE_DEVICE_NAME   = "M5StickC";
const char* SERVICE_UUID      = "3d436b81-f335-4abf-9169-7b95a4d4df16";
const char* COMMAND_CHAR_UUID = "07abb273-12ef-45ba-879c-2011ecf81e38";
const char* NOTIFY_CHAR_UUID  = "da3650aa-5da4-4cac-8ed5-fcdb0b15eb66";

// 状態・音声一覧を送るキャラクタリスティック
NimBLECharacteristic* notifyCharacteristic = nullptr;

// BLE受信（NimBLEのタスク）からloopへコマンドを渡すキュー
const int COMMAND_MAX_LENGTH = 50;
const int COMMAND_QUEUE_SIZE = 8;

QueueHandle_t commandQueue;

// Bluetoothで制御する動作状態
// true  : 持ち上げ検出・音声再生 ON
// false : OFF
bool operationEnabled = true;

// M5Stackから再生を指示された音声の番号
// -1 : 指示なし
int requestedVoice = -1;

// 再生中の音声の番号
// -1 : 再生していない
int playingVoice = -1;

// 音声一覧の送信要求
bool listRequested = false;


// ==================================================
// HAT-SPK
// ==================================================

const int SPK_ENABLE_PIN = 0;
const int SPK_AUDIO_PIN  = 26;

const int SAMPLE_RATE = 16000;


// ==================================================
// 音量
// ==================================================

// 0〜100 %。M5Stackから VOL n で変える
// 電源を切っても残るようにフラッシュ（NVS）に保存する
const int VOLUME_MAX = 100;

int volumePercent = VOLUME_MAX;

Preferences preferences;


// ==================================================
// 持ち上げ検出
// ==================================================

const float LIFT_THRESHOLD = 0.20;
const unsigned long COOLDOWN_TIME = 1500;

unsigned long lastTriggerTime = 0;

float accX = 0;
float accY = 0;
float accZ = 0;


// ==================================================
// 音声情報
// ==================================================

struct VoiceData {

  // M5Stackに表示する名前
  const char* name;

  const uint8_t* data;
  unsigned int length;

  // 持ち上げ検出によるランダム再生の対象
  bool randomTarget;

};


// 再生する音声
// 番号は上から 0, 1, 2, ...
VoiceData voices[] = {

  {
    "voice",
    voice_pcm,
    voice_pcm_len,
    true
  },

  {
    "hello",
    hello_pcm,
    hello_pcm_len,
    true
  },

  {
    "please stay here",
    please_stay_here_pcm,
    please_stay_here_pcm_len,
    true
  },

  {
    "do research",
    do_research_pcm,
    do_research_pcm_len,
    true
  },

  {
    "stay clean",
    stay_clean_pcm,
    stay_clean_pcm_len,
    true
  },

  {
    "人間に戻して",
    zunda_01_pcm,
    zunda_01_pcm_len,
    true
  },

  {
    "今日も会えて嬉しいよ",
    zunda_02_pcm,
    zunda_02_pcm_len,
    true
  },

  {
    "今日も頑張ろう！",
    zunda_03_pcm,
    zunda_03_pcm_len,
    true
  }

};


// 音声数
const int VOICE_COUNT =
    sizeof(voices) / sizeof(voices[0]);


// ==================================================
// 口
// ==================================================

void drawMouth(bool open) {

  // 口の領域を黒で消す
  M5.Lcd.fillRect(
    5,
    10,
    70,
    60,
    BLACK
  );


  if (open) {

    // ------------------------------
    // 開いた口
    // ------------------------------

    M5.Lcd.fillRoundRect(
      10,
      22,
      60,
      34,
      17,
      WHITE
    );

    M5.Lcd.fillRoundRect(
      14,
      26,
      52,
      26,
      13,
      BLACK
    );

  } else {

    // ------------------------------
    // 閉じた口
    // ------------------------------

    M5.Lcd.fillRoundRect(
      10,
      37,
      60,
      6,
      3,
      WHITE
    );

  }
}


// ==================================================
// BLE送信
// ==================================================

void notifyText(const String& text) {

  Serial.print("Notify: ");
  Serial.println(text);


  if (notifyCharacteristic == nullptr) {
    return;
  }


  // 読み出されたときに返す値
  notifyCharacteristic->setValue(
    text.c_str()
  );

  // 内容を渡して送る
  // （引数なしの notify() は送信時に値を読むので、
  //   続けて setValue() すると前の通知が後の内容で上書きされる）
  notifyCharacteristic->notify(
    reinterpret_cast<const uint8_t*>(text.c_str()),
    text.length()
  );
}


// 例: "STATE ON -1"  "STATE OFF -1"  "STATE ON 3"
void notifyState() {

  String text =
      operationEnabled ? "STATE ON " : "STATE OFF ";

  text += String(playingVoice);


  notifyText(text);
}


// 例: "VOL 80"
void notifyVolume() {

  notifyText(
    "VOL " + String(volumePercent)
  );
}


// "COUNT k" のあと "NAME i 名前" を1件ずつ送る
void notifyVoiceList() {

  notifyText(
    "COUNT " + String(VOICE_COUNT)
  );


  for (
    int i = 0;
    i < VOICE_COUNT;
    i++
  ) {

    // 通知を詰めて送ると取りこぼすので間隔をあける
    delay(20);

    notifyText(
      "NAME " + String(i) + " " + voices[i].name
    );

  }
}


// ==================================================
// BLE受信
// ==================================================

// 書き込まれた文字列をキューに入れるだけにする
// （コマンドの実行はloop側で行う）
class CommandCallbacks : public NimBLECharacteristicCallbacks {

  void onWrite(
    NimBLECharacteristic* characteristic,
    NimBLEConnInfo& connInfo
  ) override {

    NimBLEAttValue value =
        characteristic->getValue();


    char command[COMMAND_MAX_LENGTH + 1];

    size_t length =
        min(
          (size_t)value.length(),
          (size_t)COMMAND_MAX_LENGTH
        );

    memcpy(
      command,
      value.data(),
      length
    );

    command[length] = '\0';


    // キューが一杯なら捨てる
    xQueueSend(
      commandQueue,
      command,
      0
    );
  }
};


class ServerCallbacks : public NimBLEServerCallbacks {

  void onConnect(
    NimBLEServer* server,
    NimBLEConnInfo& connInfo
  ) override {

    Serial.println("BLE connected");
  }


  // 切断後の広告再開は advertiseOnDisconnect(true) で行う
  void onDisconnect(
    NimBLEServer* server,
    NimBLEConnInfo& connInfo,
    int reason
  ) override {

    Serial.print("BLE disconnected: ");
    Serial.println(reason);
  }
};


// ==================================================
// Bluetoothコマンド実行
// ==================================================

// "PLAY n" "VOL n" の n を取り出す
// 数字でなければ -1
int parseNumber(String text) {

  text.trim();


  if (text.length() == 0) {
    return -1;
  }


  for (
    unsigned int i = 0;
    i < text.length();
    i++
  ) {

    if (!isDigit(text[i])) {
      return -1;
    }

  }


  return text.toInt();
}


void executeBluetoothCommand(String command) {

  // 前後の空白を除去
  command.trim();

  // 大文字に変換
  command.toUpperCase();


  if (command.length() == 0) {
    return;
  }


  Serial.print("Bluetooth command: ");
  Serial.println(command);


  // ------------------------------
  // ON
  // ------------------------------

  if (
    command == "ON" ||
    command == "1"
  ) {

    operationEnabled = true;

    // ON直後に誤検出しないように
    lastTriggerTime = millis();


    Serial.println("Operation: ON");

    notifyState();

  }


  // ------------------------------
  // OFF
  // ------------------------------

  else if (
    command == "OFF" ||
    command == "0"
  ) {

    operationEnabled = false;

    // 受け付け済みの再生指示も取り消す
    requestedVoice = -1;


    Serial.println("Operation: OFF");

    // 再生中なら、再生を止めたときに playVoice が通知する
    if (playingVoice < 0) {
      notifyState();
    }

  }


  // ------------------------------
  // STATUS
  // ------------------------------

  else if (command == "STATUS") {

    notifyState();

    notifyVolume();

  }


  // ------------------------------
  // PLAY n（ポン出し）
  // ------------------------------

  else if (command.startsWith("PLAY ")) {

    int index =
        parseNumber(
          command.substring(5)
        );


    if (
      index < 0 ||
      index >= VOICE_COUNT
    ) {

      notifyText("ERR " + command);

    }

    // OFF中は再生しない
    else if (!operationEnabled) {

      Serial.println("Ignored: operation is OFF");

      notifyState();

    }

    else {

      // 再生中なら playPCM の中で止めてから切り替える
      requestedVoice = index;

    }

  }


  // ------------------------------
  // VOL n（音量 0〜100 %）
  // ------------------------------

  else if (command.startsWith("VOL ")) {

    int volume =
        parseNumber(
          command.substring(4)
        );


    if (
      volume < 0 ||
      volume > VOLUME_MAX
    ) {

      notifyText("ERR " + command);

    }

    else {

      // 再生中でも次のサンプルから反映される
      if (volume != volumePercent) {

        volumePercent = volume;

        preferences.putUChar(
          "volume",
          volumePercent
        );

      }


      Serial.print("Volume: ");
      Serial.println(volumePercent);

      notifyVolume();

    }

  }


  // ------------------------------
  // LIST
  // ------------------------------

  else if (command == "LIST") {

    // 再生中に送ると音が途切れるので loop で送る
    listRequested = true;

  }


  // ------------------------------
  // 不明なコマンド
  // ------------------------------

  else {

    notifyText("ERR " + command);

  }
}


// ==================================================
// Bluetooth受信
// ==================================================

void checkBluetooth() {

  char command[COMMAND_MAX_LENGTH + 1];


  while (
    xQueueReceive(
      commandQueue,
      command,
      0
    ) == pdTRUE
  ) {

    executeBluetoothCommand(
      String(command)
    );

  }
}


// ==================================================
// PCM音声再生
// ==================================================

void playPCM(
  const uint8_t* data,
  unsigned int length
) {

  // OFFなら再生しない
  if (!operationEnabled) {
    return;
  }


  // ------------------------------
  // スピーカーON
  // ------------------------------

  digitalWrite(
    SPK_ENABLE_PIN,
    HIGH
  );


  bool mouthOpen = true;

  drawMouth(true);


  // 16 kHz
  // 1サンプル = 62.5 us
  const unsigned int samplePeriod =
      1000000UL / SAMPLE_RATE;


  unsigned long nextSampleTime =
      micros();


  // ------------------------------
  // PCM再生
  // ------------------------------

  for (
    unsigned int i = 0;
    i < length;
    i++
  ) {

    // ------------------------------
    // Bluetoothを定期的に確認
    //
    // 256 sample ≒ 16 ms
    // ------------------------------

    if ((i % 256) == 0) {

      checkBluetooth();


      // OFFを受信したら
      // 音声を即座に停止
      if (!operationEnabled) {

        Serial.println(
          "Playback stopped by Bluetooth"
        );

        break;

      }


      // 別の音声の再生指示を受信したら
      // 今の音声を止めて切り替える
      if (requestedVoice >= 0) {

        Serial.println(
          "Playback switched by Bluetooth"
        );

        break;

      }
    }


    // ------------------------------
    // PCMデータ取得
    // ------------------------------

    uint8_t sample =
        pgm_read_byte(data + i);


    // ------------------------------
    // 音量を掛ける
    //
    // 128 が無音
    // volumePercent は 100 以下なので 0〜255 に収まる
    // ------------------------------

    int scaled =
        128 +
        ((int)sample - 128) * volumePercent / 100;


    // ------------------------------
    // DAC出力
    // ------------------------------

    dacWrite(
      SPK_AUDIO_PIN,
      scaled
    );


    // ------------------------------
    // 口のアニメーション
    //
    // 16 kHz × 0.12 sec
    // = 1920 sample
    // ------------------------------

    if (
      i > 0 &&
      i % 1920 == 0
    ) {

      mouthOpen = !mouthOpen;

      drawMouth(mouthOpen);

    }


    // ------------------------------
    // 次サンプルまで待つ
    // ------------------------------

    nextSampleTime += samplePeriod;


    while (
      (long)(
        micros() -
        nextSampleTime
      ) < 0
    ) {

      // wait

    }
  }


  // ------------------------------
  // DACを中央値に戻す
  // ------------------------------

  dacWrite(
    SPK_AUDIO_PIN,
    128
  );


  // ------------------------------
  // スピーカーOFF
  // ------------------------------

  digitalWrite(
    SPK_ENABLE_PIN,
    LOW
  );


  // ------------------------------
  // 口を閉じる
  // ------------------------------

  drawMouth(false);
}


// ==================================================
// 番号を指定して再生
// ==================================================

void playVoice(int index) {

  if (!operationEnabled) {
    return;
  }


  Serial.print("Play voice: ");
  Serial.println(index);


  playingVoice = index;

  notifyState();


  playPCM(
    voices[index].data,
    voices[index].length
  );


  playingVoice = -1;

  notifyState();
}


// ==================================================
// ランダム音声再生
// ==================================================

void playRandomVoice() {

  if (!operationEnabled) {
    return;
  }


  // ランダム再生の対象の数
  int targetCount = 0;

  for (
    int i = 0;
    i < VOICE_COUNT;
    i++
  ) {

    if (voices[i].randomTarget) {
      targetCount++;
    }

  }


  if (targetCount == 0) {
    return;
  }


  // 対象の中から pick 番目を再生
  int pick =
      random(targetCount);


  for (
    int i = 0;
    i < VOICE_COUNT;
    i++
  ) {

    if (!voices[i].randomTarget) {
      continue;
    }


    if (pick == 0) {

      playVoice(i);

      return;

    }


    pick--;

  }
}


// ==================================================
// setup
// ==================================================

void setup() {

  // ------------------------------
  // M5StickC
  // ------------------------------

  M5.begin();


  // ------------------------------
  // USB Serial
  // ------------------------------

  Serial.begin(115200);


  // ------------------------------
  // IMU
  // ------------------------------

  M5.IMU.Init();


  // ------------------------------
  // LCD
  // ------------------------------

  M5.Lcd.setRotation(0);

  M5.Lcd.fillScreen(BLACK);

  drawMouth(false);


  // ------------------------------
  // HAT-SPK
  // ------------------------------

  pinMode(
    SPK_ENABLE_PIN,
    OUTPUT
  );

  digitalWrite(
    SPK_ENABLE_PIN,
    LOW
  );

  dacWrite(
    SPK_AUDIO_PIN,
    128
  );


  // ------------------------------
  // 音量（前回の値を読み出す）
  // ------------------------------

  preferences.begin(
    "voice",
    false
  );

  volumePercent =
      preferences.getUChar(
        "volume",
        VOLUME_MAX
      );

  if (volumePercent > VOLUME_MAX) {
    volumePercent = VOLUME_MAX;
  }


  // ------------------------------
  // Bluetooth (BLE)
  // ------------------------------

  commandQueue =
      xQueueCreate(
        COMMAND_QUEUE_SIZE,
        COMMAND_MAX_LENGTH + 1
      );


  NimBLEDevice::init(
    BLE_DEVICE_NAME
  );


  NimBLEServer* server =
      NimBLEDevice::createServer();

  server->setCallbacks(
    new ServerCallbacks()
  );

  // 切断されたら広告を再開して接続を待つ
  server->advertiseOnDisconnect(true);


  NimBLEService* service =
      server->createService(
        SERVICE_UUID
      );


  NimBLECharacteristic* commandCharacteristic =
      service->createCharacteristic(
        COMMAND_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_NR
      );

  commandCharacteristic->setCallbacks(
    new CommandCallbacks()
  );


  notifyCharacteristic =
      service->createCharacteristic(
        NOTIFY_CHAR_UUID,
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::NOTIFY
      );


  service->start();

  notifyState();


  // サービスUUIDは広告に、名前はスキャン応答に入れる
  NimBLEAdvertising* advertising =
      NimBLEDevice::getAdvertising();

  advertising->enableScanResponse(true);

  advertising->setName(
    BLE_DEVICE_NAME
  );

  advertising->addServiceUUID(
    SERVICE_UUID
  );

  advertising->start();


  Serial.println();
  Serial.println(
    "=============================="
  );

  Serial.println(
    "M5StickC started"
  );

  Serial.println(
    "BLE device: M5StickC"
  );

  Serial.println(
    "Commands:"
  );

  Serial.println(
    "  ON"
  );

  Serial.println(
    "  OFF"
  );

  Serial.println(
    "  STATUS"
  );

  Serial.println(
    "  PLAY n"
  );

  Serial.println(
    "  LIST"
  );

  Serial.println(
    "  VOL n (0-100)"
  );

  Serial.print(
    "Volume: "
  );

  Serial.println(
    volumePercent
  );

  Serial.println(
    "=============================="
  );


  // ------------------------------
  // 乱数
  // ------------------------------

  // randomSeed() は呼ばない
  // （呼ぶと random() がハードウェア乱数から疑似乱数に切り替わり、
  //   起動直後の micros() は毎回近い値なので選ばれる順番が似る）


  delay(1000);
}


// ==================================================
// loop
// ==================================================

void loop() {

  unsigned long now =
      millis();


  // ==================================================
  // Bluetooth確認
  // ==================================================

  checkBluetooth();


  if (listRequested) {

    listRequested = false;

    notifyVoiceList();

  }


  // ==================================================
  // OFF状態
  // ==================================================

  if (!operationEnabled) {

    // Bluetoothだけは監視し続ける
    delay(10);

    return;
  }


  // ==================================================
  // M5Stackからの再生指示（ポン出し）
  // ==================================================

  if (requestedVoice >= 0) {

    int index = requestedVoice;

    requestedVoice = -1;


    playVoice(index);

    return;
  }


  // ==================================================
  // 加速度取得
  // ==================================================

  M5.IMU.getAccelData(
    &accX,
    &accY,
    &accZ
  );


  // 加速度ベクトルの大きさ
  float accMagnitude =
      sqrt(
        accX * accX +
        accY * accY +
        accZ * accZ
      );


  // 静止時 ≒ 1Gなので
  // 1Gとの差を動き量とする
  float movement =
      fabs(
        accMagnitude - 1.0
      );


  Serial.print("movement: ");
  Serial.println(
    movement,
    2
  );


  // ==================================================
  // 持ち上げ検出
  // ==================================================

  if (
    movement > LIFT_THRESHOLD &&
    now - lastTriggerTime >
      COOLDOWN_TIME
  ) {

    lastTriggerTime = now;

    playRandomVoice();

  }


  delay(10);
}

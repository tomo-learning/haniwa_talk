#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <Wire.h>


// ==================================================
// Bluetooth (BLE)
// ==================================================

// M5StickC側のスケッチと同じ値にする
const char* SERVICE_UUID      = "3d436b81-f335-4abf-9169-7b95a4d4df16";
const char* COMMAND_CHAR_UUID = "07abb273-12ef-45ba-879c-2011ecf81e38";
const char* NOTIFY_CHAR_UUID  = "da3650aa-5da4-4cac-8ed5-fcdb0b15eb66";

// 接続の状態
enum LinkState {
  LINK_SCANNING,
  LINK_CONNECTING,
  LINK_CONNECTED
};

LinkState linkState = LINK_SCANNING;

// スキャンで見つけたM5StickCのアドレス（NimBLEのタスクで書き込む）
// スキャン結果そのものは次のスキャンで消えるので、アドレスだけを複製して持つ
NimBLEAddress foundAddress;
volatile bool deviceFound = false;

// 1回のスキャンの長さ
// 止まっていたら loop で始め直す（NimBLEの内部のリセットでも止まるため）
const uint32_t SCAN_TIME_MS = 10000;

unsigned long lastScanStart = 0;

// 切断の知らせ（NimBLEのタスクで書き込む）
volatile bool disconnected = false;

NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* commandCharacteristic = nullptr;

// 通知（NimBLEのタスク）からloopへ文字列を渡すキュー
const int NOTIFY_MAX_LENGTH = 128;
const int NOTIFY_QUEUE_SIZE = 40;

QueueHandle_t notifyQueue;


// ==================================================
// Joystick Unit U024 (I2C)
// ==================================================

// M5Stack Port A: SDA=GPIO21, SCL=GPIO22
const uint8_t JOYSTICK_ADDR = 0x52;
const int JOYSTICK_CENTER = 128;
const int JOYSTICK_DEAD_ZONE = 35;

// ユニットの取り付け向きが違う場合は，ここで軸を合わせる
// 90度回転なら SWAP_XY と必要な INVERT を組み合わせる
const bool JOYSTICK_SWAP_XY = false;
const bool JOYSTICK_INVERT_X = false;
const bool JOYSTICK_INVERT_Y = false;

const uint32_t JOYSTICK_POLL_MS = 20;
const uint32_t DRIVE_HEARTBEAT_MS = 150;
const uint32_t JOYSTICK_LOST_STOP_MS = 300;

unsigned long lastJoystickPoll = 0;
unsigned long lastJoystickGood = 0;
unsigned long lastDriveSend = 0;

// F:前進, B:後退, L:左旋回, R:右旋回, S:停止
char currentDriveCommand = 'S';

// ジョイスティック押し込み（Z軸ボタン）でZumo走行ON/OFFを切り替える
// U024は押下時1，非押下時0。チャタリング対策として安定後に1回だけ反応する。
const uint32_t JOYSTICK_BUTTON_DEBOUNCE_MS = 40;
bool joystickButtonRaw = false;
bool joystickButtonStable = false;
unsigned long joystickButtonChangedAt = 0;

// ==================================================
// M5StickCの状態（通知で更新する）
// ==================================================

bool stateKnown = false;
bool stickEnabled = false;
int playingVoice = -1;

// M5StickCの音量（%）。-1 はまだ受け取っていない
int stickVolume = -1;

const int VOLUME_MAX = 100;
const int VOLUME_STEP = 10;

// 画面モード
enum UiScreen {
  SCREEN_MAIN,
  SCREEN_VOLUME
};

UiScreen uiScreen = SCREEN_MAIN;

// Zumo走行の有効/無効。従来挙動を維持するため初期値はON。
bool zumoDriveEnabled = true;

// A をこの時間押し続けると音量画面へ移る
const uint32_t VOLUME_HOLD_MS = 800;

const int MAX_VOICES = 32;

String voiceNames[MAX_VOICES];
int voiceCount = 0;

// Bで選んでいる音声の番号
int selectedVoice = 0;

// 状態・一覧を最後に要求した時刻（取りこぼしたときの再要求に使う）
unsigned long lastStatusRequest = 0;
unsigned long lastListRequest = 0;


// ==================================================
// 画面
// ==================================================

const int HEADER_Y = 0;
const int HEADER_H = 24;

const int STATE_Y = 26;
const int STATE_H = 40;

const int LIST_Y = 70;
const int LINE_H = 20;
const int LIST_LINES = 7;

const int LABEL_Y = 218;
const int LABEL_H = 22;

// 再生中の表示を置く位置（名前はこの左で切る）
const int PLAYING_MARK_X = 250;

bool headerDirty = true;
bool stateDirty = true;
bool listDirty = true;
bool labelsDirty = true;


void drawHeader() {

  M5.Display.fillRect(0, HEADER_Y, 320, HEADER_H, TFT_BLACK);

  M5.Display.setFont(&fonts::lgfxJapanGothicP_16);
  M5.Display.setTextDatum(textdatum_t::middle_left);


  if (linkState == LINK_CONNECTED) {

    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.drawString("M5StickC に接続済み", 8, HEADER_Y + HEADER_H / 2);

    M5.Display.setTextDatum(textdatum_t::middle_right);
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);

    if (uiScreen == SCREEN_MAIN) {
      M5.Display.drawString("A長押し: 音量", 312, HEADER_Y + HEADER_H / 2);
    } else {
      M5.Display.drawString("A: 戻る", 312, HEADER_Y + HEADER_H / 2);
    }

  } else if (linkState == LINK_CONNECTING) {

    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawString("M5StickC への接続を試みています", 8, HEADER_Y + HEADER_H / 2);

  } else {

    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawString("M5StickC を探しています", 8, HEADER_Y + HEADER_H / 2);

  }
}


void drawState() {

  M5.Display.fillRect(0, STATE_Y, 320, STATE_H, TFT_BLACK);

  bool connected = (linkState == LINK_CONNECTED);

  // 音声機能の状態
  M5.Display.setFont(&fonts::lgfxJapanGothicP_16);
  M5.Display.setTextDatum(textdatum_t::middle_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("音声", 8, STATE_Y + STATE_H / 2);

  String voiceState = "--";
  uint16_t voiceColor = TFT_DARKGREY;

  if (connected && stateKnown) {
    voiceState = stickEnabled ? "ON" : "OFF";
    voiceColor = stickEnabled ? TFT_GREEN : TFT_RED;
  }

  M5.Display.setTextColor(voiceColor, TFT_BLACK);
  M5.Display.drawString(voiceState, 48, STATE_Y + STATE_H / 2);

  // Zumo走行の状態（ジョイスティック押し込みで切り替え）
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("Zumo", 102, STATE_Y + STATE_H / 2);
  M5.Display.setTextColor(zumoDriveEnabled ? TFT_GREEN : TFT_RED, TFT_BLACK);
  M5.Display.drawString(zumoDriveEnabled ? "ON" : "OFF", 150, STATE_Y + STATE_H / 2);

  // 音量
  M5.Display.setTextDatum(textdatum_t::middle_right);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  String volumeText =
      (connected && stickVolume >= 0) ? String(stickVolume) + "%" : String("--");

  M5.Display.drawString("音量 " + volumeText, 312, STATE_Y + STATE_H / 2);
}


// 音量画面（一覧の場所に表示する）
void drawVolume() {

  M5.Display.fillRect(0, LIST_Y, 320, LINE_H * LIST_LINES, TFT_BLACK);


  M5.Display.setFont(&fonts::lgfxJapanGothicP_16);
  M5.Display.setTextDatum(textdatum_t::middle_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("音量", 20, LIST_Y + 12);


  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.drawString(stickVolume < 0 ? String("--") : String(stickVolume) + "%", 160, LIST_Y + 50);


  // 0〜100 % の棒
  const int barX = 20;
  const int barY = LIST_Y + 84;
  const int barW = 280;
  const int barH = 24;

  M5.Display.drawRect(barX, barY, barW, barH, TFT_WHITE);

  if (stickVolume > 0) {
    M5.Display.fillRect(barX + 2, barY + 2, (barW - 4) * stickVolume / VOLUME_MAX, barH - 4, TFT_GREEN);
  }
}


void drawList() {

  M5.Display.fillRect(0, LIST_Y, 320, LINE_H * LIST_LINES, TFT_BLACK);

  M5.Display.setFont(&fonts::lgfxJapanGothicP_16);


  if (voiceCount == 0) {

    if (linkState == LINK_CONNECTED) {

      M5.Display.setTextDatum(textdatum_t::middle_left);
      M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
      M5.Display.drawString("音声の一覧を受信しています", 8, LIST_Y + LINE_H / 2);

    }

    return;
  }


  // 選んでいる音声が見える位置から表示する
  int top = selectedVoice - LIST_LINES / 2;

  if (top > voiceCount - LIST_LINES) {
    top = voiceCount - LIST_LINES;
  }

  if (top < 0) {
    top = 0;
  }


  for (int row = 0; row < LIST_LINES; row++) {

    int index = top + row;

    if (index >= voiceCount) {
      break;
    }


    int y = LIST_Y + row * LINE_H;

    bool selected = (index == selectedVoice);

    uint16_t background = selected ? TFT_WHITE : TFT_BLACK;
    uint16_t foreground = selected ? TFT_BLACK : TFT_WHITE;

    M5.Display.fillRect(0, y, 320, LINE_H, background);


    // 番号と名前
    M5.Display.setClipRect(0, y, PLAYING_MARK_X - 4, LINE_H);

    M5.Display.setTextDatum(textdatum_t::middle_left);
    M5.Display.setTextColor(foreground, background);
    M5.Display.drawString(String(index) + "  " + voiceNames[index], 8, y + LINE_H / 2);

    M5.Display.clearClipRect();


    // 再生中の表示
    if (index == playingVoice) {

      M5.Display.setTextDatum(textdatum_t::middle_right);
      M5.Display.setTextColor(selected ? TFT_RED : TFT_GREEN, background);
      M5.Display.drawString("再生中", 312, y + LINE_H / 2);

    }
  }
}


void drawLabels() {

  M5.Display.fillRect(0, LABEL_Y, 320, LABEL_H, TFT_BLACK);

  M5.Display.setFont(&fonts::lgfxJapanGothicP_16);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);

  // ボタンA・B・Cの真上に置く
  if (uiScreen == SCREEN_VOLUME) {

    M5.Display.drawString("戻る", 68, LABEL_Y + LABEL_H / 2);
    M5.Display.drawString("下げる", 160, LABEL_Y + LABEL_H / 2);
    M5.Display.drawString("上げる", 252, LABEL_Y + LABEL_H / 2);

  } else {

    M5.Display.drawString("ON/OFF", 68, LABEL_Y + LABEL_H / 2);
    M5.Display.drawString("次へ", 160, LABEL_Y + LABEL_H / 2);
    M5.Display.drawString("再生", 252, LABEL_Y + LABEL_H / 2);

  }
}


void redrawIfNeeded() {

  if (headerDirty) {
    headerDirty = false;
    drawHeader();
  }

  if (stateDirty) {
    stateDirty = false;
    drawState();
  }

  if (listDirty) {
    listDirty = false;

    if (uiScreen == SCREEN_VOLUME) {
      drawVolume();
    } else {
      drawList();
    }
  }

  if (labelsDirty) {
    labelsDirty = false;
    drawLabels();
  }
}


// ==================================================
// BLE受信
// ==================================================

class ScanCallbacks : public NimBLEScanCallbacks {

  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {

    if (!deviceFound && advertisedDevice->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {

      // アドレスを複製してから知らせる
      foundAddress = advertisedDevice->getAddress();
      deviceFound = true;

      // 接続の前にスキャンを止める
      NimBLEDevice::getScan()->stop();

    }
  }
};


class ClientCallbacks : public NimBLEClientCallbacks {

  void onDisconnect(NimBLEClient* pClient, int reason) override {

    Serial.printf("Disconnected: %d\n", reason);

    // 探し直しはloopで行う
    disconnected = true;
  }
};


// 末尾で途中まで切れた UTF-8 の文字を除いた長さを返す
// （MTU や NOTIFY_MAX_LENGTH で通知が切れた場合に、文字化けした文字を描かないため）
size_t completeUtf8Length(const char* text, size_t length) {

  // 最後の文字の先頭バイトを探す
  size_t start = length;

  while (start > 0 && (text[start - 1] & 0xC0) == 0x80) {
    start--;
  }

  if (start == 0) {
    return 0;
  }


  uint8_t lead = text[start - 1];

  size_t need =
      (lead & 0x80) == 0x00 ? 1 :
      (lead & 0xE0) == 0xC0 ? 2 :
      (lead & 0xF0) == 0xE0 ? 3 :
      (lead & 0xF8) == 0xF0 ? 4 : 1;

  return (length - (start - 1) < need) ? start - 1 : length;
}


// 通知の文字列をキューに入れるだけにする
// （中身の処理はloop側で行う）
void onNotify(
  NimBLERemoteCharacteristic* characteristic,
  uint8_t* data,
  size_t length,
  bool isNotify
) {

  char text[NOTIFY_MAX_LENGTH + 1];

  size_t n = std::min(length, (size_t)NOTIFY_MAX_LENGTH);

  memcpy(text, data, n);

  n = completeUtf8Length(text, n);

  text[n] = '\0';


  // キューが一杯なら捨てる
  xQueueSend(notifyQueue, text, 0);
}


// "STATE ON -1"  "VOL 80"  "COUNT k"  "NAME i 名前"  "ERR 内容"
void handleNotify(const String& text) {

  Serial.print("Notify: ");
  Serial.println(text);


  if (text.startsWith("STATE ")) {

    int space = text.indexOf(' ', 6);

    if (space < 0) {
      return;
    }

    stickEnabled = (text.substring(6, space) == "ON");
    playingVoice = text.substring(space + 1).toInt();
    stateKnown = true;

    stateDirty = true;
    listDirty = true;

  } else if (text.startsWith("VOL ")) {

    stickVolume = text.substring(4).toInt();

    stateDirty = true;

    if (uiScreen == SCREEN_VOLUME) {
      listDirty = true;
    }

  } else if (text.startsWith("COUNT ")) {

    voiceCount = text.substring(6).toInt();

    if (voiceCount > MAX_VOICES) {
      voiceCount = MAX_VOICES;
    }

    for (int i = 0; i < MAX_VOICES; i++) {
      voiceNames[i] = "";
    }

    if (selectedVoice >= voiceCount) {
      selectedVoice = 0;
    }

    listDirty = true;

  } else if (text.startsWith("NAME ")) {

    int space = text.indexOf(' ', 5);

    if (space < 0) {
      return;
    }

    int index = text.substring(5, space).toInt();

    if (index >= 0 && index < voiceCount) {
      voiceNames[index] = text.substring(space + 1);
      listDirty = true;
    }

  }
}


// ==================================================
// BLE送信
// ==================================================

void sendCommand(const String& command) {

  if (linkState != LINK_CONNECTED || commandCharacteristic == nullptr) {
    return;
  }


  Serial.print("Send: ");
  Serial.println(command);

  commandCharacteristic->writeValue(command.c_str(), command.length(), true);
}



// ==================================================
// ジョイスティック読み取り・走行コマンド送信
// ==================================================

bool readJoystick(uint8_t& x, uint8_t& y, uint8_t& button) {

  Wire.requestFrom(JOYSTICK_ADDR, (uint8_t)3);

  if (Wire.available() < 3) {
    return false;
  }

  x = Wire.read();
  y = Wire.read();
  button = Wire.read();

  // 念のため余分なバイトがあれば捨てる
  while (Wire.available()) {
    Wire.read();
  }

  return true;
}


char joystickToDriveCommand(uint8_t x, uint8_t y) {

  int dx = (int)x - JOYSTICK_CENTER;

  // U024を普通の向きに置いた場合，Yが小さい側を「前」にする
  int dy = JOYSTICK_CENTER - (int)y;

  if (JOYSTICK_SWAP_XY) {
    int temp = dx;
    dx = dy;
    dy = temp;
  }

  if (JOYSTICK_INVERT_X) {
    dx = -dx;
  }

  if (JOYSTICK_INVERT_Y) {
    dy = -dy;
  }

  // 中央付近は停止
  if (
    abs(dx) < JOYSTICK_DEAD_ZONE &&
    abs(dy) < JOYSTICK_DEAD_ZONE
  ) {
    return 'S';
  }

  // 大きく倒している軸を優先して4方向に分類
  if (abs(dy) >= abs(dx)) {
    return (dy > 0) ? 'F' : 'B';
  }

  return (dx > 0) ? 'R' : 'L';
}


void sendDriveCommand(char direction) {

  if (
    linkState != LINK_CONNECTED ||
    commandCharacteristic == nullptr
  ) {
    return;
  }

  String command = "DRIVE ";
  command += direction;

  // 走行コマンドは150 msごとに繰り返すので，
  // WRITE_NR（応答なし）で軽く送る
  commandCharacteristic->writeValue(
    command.c_str(),
    command.length(),
    false
  );

  lastDriveSend = millis();
}


void setZumoDriveEnabled(bool enabled) {

  if (zumoDriveEnabled == enabled) {
    return;
  }

  zumoDriveEnabled = enabled;

  // OFFにした瞬間に必ず停止させる
  if (!zumoDriveEnabled) {
    currentDriveCommand = 'S';
    sendDriveCommand('S');
  }

  stateDirty = true;

  Serial.print("Zumo drive: ");
  Serial.println(zumoDriveEnabled ? "ON" : "OFF");
}


void updateJoystickButton(uint8_t button, unsigned long now) {

  bool pressed = (button != 0);

  // 生の入力が変化した時刻を記録
  if (pressed != joystickButtonRaw) {
    joystickButtonRaw = pressed;
    joystickButtonChangedAt = now;
  }

  // 一定時間同じ状態が続いたら確定
  if (
    now - joystickButtonChangedAt >= JOYSTICK_BUTTON_DEBOUNCE_MS &&
    joystickButtonStable != joystickButtonRaw
  ) {
    joystickButtonStable = joystickButtonRaw;

    // 押した瞬間だけON/OFFを反転する
    if (joystickButtonStable) {
      setZumoDriveEnabled(!zumoDriveEnabled);
    }
  }
}


void updateJoystick() {

  unsigned long now = millis();

  if (now - lastJoystickPoll < JOYSTICK_POLL_MS) {
    return;
  }

  lastJoystickPoll = now;

  uint8_t x = 0;
  uint8_t y = 0;
  uint8_t button = 0;

  if (!readJoystick(x, y, button)) {

    // ジョイスティックが抜けた/読めなくなった場合は停止
    if (
      now - lastJoystickGood > JOYSTICK_LOST_STOP_MS &&
      currentDriveCommand != 'S'
    ) {
      currentDriveCommand = 'S';
      sendDriveCommand('S');
      Serial.println("Joystick lost -> STOP");
    }

    return;
  }

  lastJoystickGood = now;

  // ZumoがOFFでも押し込みで再びONにできるよう，
  // 走行判定より先にボタンを常に処理する。
  updateJoystickButton(button, now);

  // Zumo走行がOFFの間は必ず停止コマンドだけを送る。
  if (!zumoDriveEnabled) {
    bool changed = (currentDriveCommand != 'S');
    bool heartbeat = (now - lastDriveSend >= DRIVE_HEARTBEAT_MS);

    currentDriveCommand = 'S';

    if (changed || heartbeat) {
      sendDriveCommand('S');
    }

    return;
  }

  char nextCommand = joystickToDriveCommand(x, y);

  bool changed = (nextCommand != currentDriveCommand);
  bool heartbeat = (now - lastDriveSend >= DRIVE_HEARTBEAT_MS);

  if (changed) {
    currentDriveCommand = nextCommand;

    Serial.printf(
      "Joystick x=%u y=%u button=%u -> %c\n",
      x,
      y,
      button,
      currentDriveCommand
    );
  }

  if (changed || heartbeat) {
    sendDriveCommand(currentDriveCommand);
  }
}


void requestStatus() {

  lastStatusRequest = millis();

  sendCommand("STATUS");
}


void requestList() {

  lastListRequest = millis();

  sendCommand("LIST");
}


// 音量を VOLUME_STEP ずつ変える
// 画面はすぐに書き換え、M5StickCからの VOL の通知で確定する
void changeVolume(int delta) {

  if (stickVolume < 0) {
    requestStatus();
    return;
  }


  int volume = constrain(stickVolume + delta, 0, VOLUME_MAX);

  if (volume == stickVolume) {
    return;
  }


  stickVolume = volume;

  stateDirty = true;
  listDirty = true;

  sendCommand("VOL " + String(volume));
}


// 一覧の名前がすべてそろっているか
bool listComplete() {

  if (voiceCount == 0) {
    return false;
  }

  for (int i = 0; i < voiceCount; i++) {
    if (voiceNames[i].length() == 0) {
      return false;
    }
  }

  return true;
}


// ==================================================
// 接続
// ==================================================

// isContinue = false で前回のスキャン結果を消してから始める
void startScan() {

  lastScanStart = millis();

  NimBLEDevice::getScan()->start(SCAN_TIME_MS, false, true);
}


bool connectToStick() {

  if (client == nullptr) {

    client = NimBLEDevice::createClient();

    client->setClientCallbacks(new ClientCallbacks(), true);

    client->setConnectTimeout(5000);

  }


  if (!client->connect(foundAddress)) {
    Serial.println("Connect failed");
    return false;
  }


  NimBLERemoteService* service = client->getService(SERVICE_UUID);

  if (service == nullptr) {
    Serial.println("Service not found");
    client->disconnect();
    return false;
  }


  commandCharacteristic = service->getCharacteristic(COMMAND_CHAR_UUID);

  NimBLERemoteCharacteristic* notifyCharacteristic =
      service->getCharacteristic(NOTIFY_CHAR_UUID);

  if (commandCharacteristic == nullptr || notifyCharacteristic == nullptr) {
    Serial.println("Characteristic not found");
    client->disconnect();
    return false;
  }


  if (!notifyCharacteristic->subscribe(true, onNotify)) {
    Serial.println("Subscribe failed");
    client->disconnect();
    return false;
  }


  Serial.println("Connected");

  return true;
}


// ==================================================
// setup
// ==================================================

void setup() {

  auto cfg = M5.config();

  M5.begin(cfg);


  // Joystick Unit U024 on Port A (I2C)
  Wire.begin(21, 22, 400000);
  lastJoystickGood = millis();


  notifyQueue = xQueueCreate(NOTIFY_QUEUE_SIZE, NOTIFY_MAX_LENGTH + 1);


  // A の短押し（音声ON/OFF）と長押し（音量画面）を分ける時間
  M5.BtnA.setHoldThresh(VOLUME_HOLD_MS);


  M5.Display.fillScreen(TFT_BLACK);

  redrawIfNeeded();


  NimBLEDevice::init("M5Stack");

  NimBLEScan* scan = NimBLEDevice::getScan();

  scan->setScanCallbacks(new ScanCallbacks(), false);

  scan->setActiveScan(true);

  startScan();


  Serial.println("M5Stack remote started");
}


// ==================================================
// loop
// ==================================================

void loop() {

  M5.update();


  // ジョイスティック入力をBLEでM5StickCへ送る
  updateJoystick();


  // ==================================================
  // 通知の処理
  // ==================================================

  char text[NOTIFY_MAX_LENGTH + 1];

  while (xQueueReceive(notifyQueue, text, 0) == pdTRUE) {
    handleNotify(String(text));
  }


  // ==================================================
  // 切断されたら探し直す
  // ==================================================

  // 接続に失敗したときの切断の知らせが後から届くことがあるので
  // 実際に切れているかを確かめる
  if (disconnected && (client == nullptr || !client->isConnected())) {

    disconnected = false;

    linkState = LINK_SCANNING;
    commandCharacteristic = nullptr;
    stateKnown = false;
    playingVoice = -1;
    stickVolume = -1;

    // 設定画面は接続中だけ使う
    uiScreen = SCREEN_MAIN;

    // スキャンは下の判定で始め直す
    deviceFound = false;

    headerDirty = true;
    stateDirty = true;
    listDirty = true;
    labelsDirty = true;
  }


  // ==================================================
  // スキャンが止まっていたら始め直す
  // ==================================================

  if (
    linkState == LINK_SCANNING &&
    !deviceFound &&
    !NimBLEDevice::getScan()->isScanning() &&
    millis() - lastScanStart > 1000
  ) {
    startScan();
  }


  // ==================================================
  // 見つけたら接続する
  // ==================================================

  if (linkState == LINK_SCANNING && deviceFound) {

    linkState = LINK_CONNECTING;

    headerDirty = true;
    redrawIfNeeded();


    if (connectToStick()) {

      linkState = LINK_CONNECTED;

      // 音声一覧と状態を受け取る
      requestList();
      requestStatus();

    } else {

      linkState = LINK_SCANNING;

      // スキャンは上の判定で始め直す
      deviceFound = false;

    }

    headerDirty = true;
    stateDirty = true;
    listDirty = true;
  }


  // ==================================================
  // 最初の通知を取りこぼしたときは要求し直す
  // ==================================================

  if (linkState == LINK_CONNECTED) {

    // STATUS には状態と音量の両方が返る
    if ((!stateKnown || stickVolume < 0) && millis() - lastStatusRequest > 1000) {
      requestStatus();
    }

    // 一覧の送信は再生が終わるまで待たされるので、間隔を長めにとる
    if (!listComplete() && millis() - lastListRequest > 3000) {
      requestList();
    }
  }


  // ==================================================
  // ボタン
  // ==================================================

  if (uiScreen == SCREEN_VOLUME) {

    // ------------------------------
    // 音量画面
    // ------------------------------

    // A短押し: 元の画面に戻る
    if (M5.BtnA.wasClicked()) {

      uiScreen = SCREEN_MAIN;

      headerDirty = true;
      listDirty = true;
      labelsDirty = true;

    }

    // B: 下げる
    if (M5.BtnB.wasPressed()) {
      changeVolume(-VOLUME_STEP);
    }

    // C: 上げる
    if (M5.BtnC.wasPressed()) {
      changeVolume(VOLUME_STEP);
    }

  } else {

    // ------------------------------
    // 通常の画面
    // ------------------------------

    // A 短押し: 音声機能ON/OFF の切り替え
    // 長押しと区別するため、離したときに切り替える
    // 状態が分からないときは、切り替えずに状態を要求する
    if (M5.BtnA.wasClicked()) {

      if (stateKnown) {
        sendCommand(stickEnabled ? "OFF" : "ON");
      } else {
        requestStatus();
      }

    }

    // A 長押し: 音量画面を開く
    if (M5.BtnA.wasHold() && linkState == LINK_CONNECTED) {

      uiScreen = SCREEN_VOLUME;

      headerDirty = true;
      listDirty = true;
      labelsDirty = true;

    }

    // B: 次の音声を選ぶ
    if (M5.BtnB.wasPressed() && voiceCount > 0) {
      selectedVoice = (selectedVoice + 1) % voiceCount;
      listDirty = true;
    }

    // C: 選んでいる音声を再生する（ポン出し）
    if (M5.BtnC.wasPressed() && voiceCount > 0) {
      sendCommand("PLAY " + String(selectedVoice));
    }

  }

  redrawIfNeeded();

  delay(10);
}

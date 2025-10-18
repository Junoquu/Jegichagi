// ====== M5StickC Plus2 TCP Client (C-server; OK 후 BtnA로 세션 시작) ======
#include <M5Unified.h>
#include <WiFi.h>
#include <math.h>
#include "secrets.h"

// ===== 사용자 환경 (secrets.h에서 매크로를 변수로 바꿔 저장) =====
static const char*   WIFI_SSID   = WIFI_SSID_STR;
static const char*   WIFI_PSK    = WIFI_PASSWORD_STR;
static const char*   SERVER_IP   = SERVER_HOST_STR;
static const uint16_t SERVER_PORT = SERVER_PORT_NUM;

static constexpr int PIN_HOLD = 4;
static const int     MAX_GAMES = 3;

// ===== 메시지 큐 & 메시지 타입 =====
enum class MsgType : uint8_t { SEND_TEXT };
struct Msg {
  MsgType type;
  char buf[128];
};
QueueHandle_t g_msgq = nullptr;

// ===== 전역 상태 =====
WiFiClient g_client;
volatile uint32_t g_kickCount = 0;  // 제기차기 카운트(킥 감지 태스크가 올림)

// C 서버 프로토콜 상태
static bool serverReady   = false;   // 서버에서 OK 받았는지
static bool sessionActive = false;   // 사용자가 BtnA로 '연결 시작' 눌렀는지
static int  gameRound     = 0;       // 진행된 게임 회차 [0..3)

static bool inGame = false;          // START/FINAL 토글
static unsigned long gameStart = 0;  // START 시각(ms)

// ===== 유틸 =====
void drawCount(uint32_t cnt, const char* sub = nullptr) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(3);
  M5.Display.printf("JEGI: %lu\n", (unsigned long)cnt);
  if (sub && *sub) {
    M5.Display.setTextSize(2);
    M5.Display.println(sub);
  }
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PSK);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

// C 서버: "id mac name\n" 전송 → "OK\n" 수신하면 serverReady=true
bool connectAndHandshake() {
  if (!ensureWifi()) return false;

  if (!g_client.connect(SERVER_IP, SERVER_PORT)) {
    return false;
  }

  String mac = WiFi.macAddress();          // 예: "8C:8D:28:73:F8:CB"
  Serial.printf("ESP32 MAC: %s\n", mac.c_str());

  String hello = String(STUDENT_ID_STR) + " " + mac + " " + STUDENT_NAME_STR + "\n";
  g_client.print(hello);

  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < 3000) {           // 3초 대기
    while (g_client.available()) {
      char c = (char)g_client.read();
      if (c == '\n') {
        line.trim();
        Serial.printf("Server reply: [%s]\n", line.c_str());
        if (line == "OK") {
          serverReady = true;
          return true;
        } else {
          return false;                     // NOT FOUND / ERROR
        }
      } else {
        line += c;
      }
    }
    delay(10);
  }
  return false; // 타임아웃
}

// ===== 소켓 송신 태스크 =====
// - TCP 연결이 있고(serverReady==true) 사용자가 BtnA로 세션을 시작했을 때만 송신
void SocketTask(void* parameter) {
  for (;;) {
    if (!g_client.connected()) {
      serverReady = false;
      sessionActive = false;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!serverReady || !sessionActive) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    Msg msg;
    if (xQueueReceive(g_msgq, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
      g_client.print(msg.buf);
      g_client.print("\n");
      Serial.printf("TX: %s\n", msg.buf);
    }

    // 서버가 주는 "n ROUND DONE" 같은 응답 비우기
    while (g_client.connected() && g_client.available()) { (void)g_client.read(); }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ===== 디스플레이 주기 갱신 태스크 =====
void DisplayTask(void* parameter) {
  for (;;) {
    const char* sub = nullptr;
    if (WiFi.status() != WL_CONNECTED)      sub = "Wi-Fi 연결 중...";
    else if (!g_client.connected())         sub = "서버 미접속 ▶ A=접속";
    else if (!serverReady)                  sub = "OK 대기 중...";
    else if (!sessionActive)                sub = "OK 수신됨 ▶ A=연결 시작";
    else                                    sub = "연결됨 ▶ B=START/FINAL";

    drawCount(g_kickCount, sub);
    vTaskDelay(pdMS_TO_TICKS(150)); // ~6-7fps
  }
}

// ===== IMU 기반 킥 감지 태스크 =====
void KickDetectTask(void* parameter) {
  const float W_HI = 2.5f;
  const float A_HI = 2.2f;
  const float A_LO = 1.2f;
  const uint32_t T_SWING_MIN   = 60;
  const uint32_t T_SWING_MAX   = 450;
  const uint32_t T_IMPACT_WIN  = 180;
  const uint32_t T_RECOVER     = 120;
  const uint32_t T_COOLDOWN    = 220;
  const uint32_t T_MIN_INTERVAL= 250;

  enum Axis { X=0, Y=1, Z=2 };
  Axis swingAxis = Y;
  Axis instepAxis = X;

  auto ema = [](float prev, float x, float alpha){ return alpha*x + (1.f-alpha)*prev; };
  float axf=0, ayf=0, azf=0, gx_f=0, gy_f=0, gz_f=0;
  const float A_ALPHA = 0.25f;
  const float G_ALPHA = 0.20f;

  enum State { IDLE, SWING, IMPACT_WAIT, COOLDOWN };
  State st = IDLE;

  uint32_t t_enter = 0;
  uint32_t lastCountMs = 0;
  bool swingArmed = false;

  for (;;) {
    float ax, ay, az, gx, gy, gz;
    M5.Imu.getAccel(&ax, &ay, &az);  // g
    M5.Imu.getGyro(&gx, &gy, &gz);   // rad/s

    axf = ema(axf, ax, A_ALPHA);
    ayf = ema(ayf, ay, A_ALPHA);
    azf = ema(azf, az, A_ALPHA);
    gx_f = ema(gx_f, gx, G_ALPHA);
    gy_f = ema(gy_f, gy, G_ALPHA);
    gz_f = ema(gz_f, gz, G_ALPHA);

    float gSwing = (swingAxis==X)?gx_f: (swingAxis==Y)?gy_f:gz_f;
    float aInstep= (instepAxis==X)?axf: (instepAxis==Y)?ayf:azf;
    float aMag   = sqrtf(axf*axf + ayf*ayf + azf*azf);

    uint32_t now = millis();

    switch (st) {
      case IDLE: {
        if (now - lastCountMs < T_MIN_INTERVAL) break;
        if (fabsf(gSwing) > W_HI) { st = SWING; t_enter = now; swingArmed = true; }
        break;
      }
      case SWING: {
        uint32_t dt = now - t_enter;
        if (dt > T_SWING_MAX) { st = IDLE; swingArmed = false; break; }
        if (fabsf(gSwing) < (W_HI*0.6f)) {
          if (dt < T_SWING_MIN) { st = IDLE; swingArmed = false; }
          else { st = IMPACT_WAIT; t_enter = now; }
        }
        break;
      }
      case IMPACT_WAIT: {
        uint32_t dt = now - t_enter;
        if (aMag > A_HI) {
          bool directionOK = (aInstep > (A_HI - 0.2f));
          if (directionOK && swingArmed) {
            g_kickCount++;
            lastCountMs = now;
            st = COOLDOWN; t_enter = now;
          } else {
            st = IDLE;
          }
        } else if (dt > T_IMPACT_WIN) {
          st = IDLE;
        }
        break;
      }
      case COOLDOWN: {
        if ((aMag < A_LO) && (now - t_enter > T_RECOVER) && (now - lastCountMs > T_COOLDOWN)) {
          st = IDLE; swingArmed = false;
        }
        break;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

void setup() {
  // 전원 유지
  pinMode(PIN_HOLD, OUTPUT);
  digitalWrite(PIN_HOLD, HIGH);

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Imu.begin();                 // IMU 초기화
  M5.Display.setRotation(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  Serial.begin(115200);

  drawCount(g_kickCount, "Wi-Fi 연결 중...");
  ensureWifi();

  // 메시지 큐
  g_msgq = xQueueCreate(16, sizeof(Msg));

  // 태스크 시작
  xTaskCreatePinnedToCore(SocketTask,   "SocketTask",   4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(DisplayTask,  "DisplayTask",  2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(KickDetectTask,"KickDetect",  3072, nullptr, 1, nullptr, 1);

  drawCount(g_kickCount, "A=서버 접속/OK, 다시 A=연결 시작, B=START/FINAL");
}

void loop() {
  M5.update();

  // BtnA: 1) 서버 접속/OK 수신  2) OK 받은 뒤 '연결 시작'
  if (M5.BtnA.wasPressed()) {
    if (!g_client.connected() || !serverReady) {
      drawCount(g_kickCount, "서버 접속/OK 대기...");
      if (connectAndHandshake()) {
        drawCount(g_kickCount, "OK 수신 ▶ 다시 A=연결 시작");
      } else {
        drawCount(g_kickCount, "접속 실패 또는 NOT FOUND");
      }
    } else {
      sessionActive = true;
      drawCount(g_kickCount, "연결 시작됨 ▶ B=START/FINAL");
    }
  }

  // BtnB: 세션 활성화된 상태에서 START/FINAL 토글 전송
  if (M5.BtnB.wasPressed()) {
    if (!sessionActive) {
      drawCount(g_kickCount, "먼저 A로 '연결 시작'!");
    } else {
      Msg m{}; m.type = MsgType::SEND_TEXT;
      if (!inGame) {
        gameStart = millis();
        inGame = true;
        snprintf(m.buf, sizeof(m.buf), "START %lu %lu", gameStart, (unsigned long)g_kickCount);
      } else {
        unsigned long tEnd = millis();
        inGame = false;
        gameRound++;
        snprintf(m.buf, sizeof(m.buf), "FINAL %lu %lu", tEnd, (unsigned long)g_kickCount);
        if (gameRound >= MAX_GAMES) {
          // 3회 끝나면 자동 종료/리셋하고 싶다면 아래 주석 해제
          // sessionActive = false;
          // gameRound = 0;
          // drawCount(g_kickCount, "3회 종료 ▶ 다시 A로 연결 가능");
        }
      }
      xQueueSend(g_msgq, &m, 0);
    }
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}

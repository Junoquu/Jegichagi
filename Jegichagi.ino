// ====== M5StickC Plus2 TCP Client with FreeRTOS Queue ======
#include <M5Unified.h>
#include <WiFi.h>

// ===== 사용자 환경 =====
static const char* WIFI_SSID   = "SK_WiFiGIGA1D02_2.4G";     // 수정
static const char* WIFI_PSK    = "1805000638";     // 수정
static const char* SERVER_IP   = "192.168.35.189";  // 노트북 IP로 수정
static const uint16_t SERVER_PORT = 5000;

static constexpr int PIN_HOLD = 4;

// ===== 메시지 큐 & 메시지 타입 =====
enum class MsgType : uint8_t { SEND_TEXT };
struct Msg {
  MsgType type;
  char buf[128];
};
QueueHandle_t g_msgq = nullptr;

// ===== 전역 상태 =====
WiFiClient g_client;
volatile uint32_t g_kickCount = 0;  // 제기차기 카운트

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

bool ensureSocket() {
  if (g_client.connected()) return true;
  if (!ensureWifi()) return false;

  if (!g_client.connect(SERVER_IP, SERVER_PORT)) return false;

  // (옵션) 최초 1회: MAC 전송 + OK 확인을 이미 끝내셨다면 아래는 생략 가능
  String mac = WiFi.macAddress();
  g_client.print(mac); g_client.print("\n");
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < 3000) {
    while (g_client.available()) {
      char c = (char)g_client.read();
      if (c=='\n') {
        line.trim();
        if (line == "OK") return true;
        return false;
      } else line += c;
    }
    delay(10);
  }
  return false;
}

// ===== 소켓 송신 태스크 =====
void SocketTask(void* parameter) {
  for (;;) {
    if (!ensureSocket()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

    // 큐에서 메시지 꺼내어 한 줄로 전송
    Msg msg;
    if (xQueueReceive(g_msgq, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
      g_client.print(msg.buf);
      g_client.print("\n");
    }

    // 서버에서 오는 데이터 처리(옵션)
    while (g_client.connected() && g_client.available()) {
      (void)g_client.read(); // 이번 요구사항에선 무시
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ===== 디스플레이 주기 갱신 태스크 =====
void DisplayTask(void* parameter) {
  for (;;) {
    uint32_t c = g_kickCount;
    drawCount(c, "BtnA=Send, BtnB=+1");
    vTaskDelay(pdMS_TO_TICKS(120)); // ~8fps
  }
}

// ===== 간단 IMU 기반 킥 감지 태스크 =====
// 가속도 magnitude가 thresh_hi를 넘었다가 다시 thresh_lo 아래로 떨어질 때 1회로 인식
void KickDetectTask(void* parameter) {
  const float thresh_hi = 2.0f; // 중력 1g 기준, 스파이크 임계치(필요 시 튜닝)
  const float thresh_lo = 1.2f; // 히스테리시스 하한
  bool armed = true;
  uint32_t lastTick = 0;

  // IMU 준비
  //if (!M5.Imu.isEnabled()) M5.Imu.setEnabled(true);

  for (;;) {
    float ax=0, ay=0, az=0;
    M5.Imu.getAccel(&ax, &ay, &az);
    float mag = sqrtf(ax*ax + ay*ay + az*az); // g 단위

    uint32_t now = millis();

    // 너무 빠른 중복 방지(예: 250ms 이내 중복 무시)
    bool cooldown = (now - lastTick < 250);

    if (armed && !cooldown && mag > thresh_hi) {
      // 피크 감지
      g_kickCount++;
      lastTick = now;
      armed = false;
    }
    if (!armed && mag < thresh_lo) {
      // 원위치 → 재무장
      armed = true;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  // 전원 유지
  pinMode(PIN_HOLD, OUTPUT);
  digitalWrite(PIN_HOLD, HIGH);

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  Serial.begin(115200);

  // Wi-Fi 시작(초기 연결)
  ensureWifi();

  // 메시지 큐
  g_msgq = xQueueCreate(16, sizeof(Msg));

  // 태스크 시작
  xTaskCreatePinnedToCore(SocketTask,   "SocketTask",   4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(DisplayTask,  "DisplayTask",  2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(KickDetectTask,"KickDetect",  2048, nullptr, 1, nullptr, 1);

  // 첫 화면
  drawCount(g_kickCount, "BtnA=Send, BtnB=+1");
}

void loop() {
  M5.update();

  // 테스트: BtnB 누르면 카운트 +1
  if (M5.BtnB.wasPressed()) {
    g_kickCount++;
  }

  // 요구사항: BtnA 누르면 현재 카운트를 서버로 전송
  if (M5.BtnA.wasPressed()) {
    Msg m{};
    m.type = MsgType::SEND_TEXT;
    // 전송 형식: JEGI:<count>
    snprintf(m.buf, sizeof(m.buf), "JEGI:%lu", (unsigned long)g_kickCount);
    xQueueSend(g_msgq, &m, 0);
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
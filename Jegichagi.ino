// ===== M5StickC Plus2 TCP Client (Professor-like Flow, A=START/FINAL, B=Count Manual/IMU) =====
#include <M5Unified.h>
#include <WiFi.h>
#include <math.h>
#include <string.h>
#include "secrets.h"

// ---------------------------- 사용자 설정 스위치 ----------------------------
// 1: 수동(버튼 B로 카운트++) / 0: IMU 자동 카운트
#define COUNT_MODE_MANUAL 1

// getGyro() 반환 단위: 보통 deg/s (M5Unified 기본)
#define GYRO_RETURNS_DEG_PER_SEC 1

// 버튼 디바운스/최소 라운드 시간(즉시 FINAL 방지)
static const unsigned long MIN_ROUND_MS = 800;  // 0.8s
static const unsigned long BTN_DEBOUNCE_MS = 220;

// ---------------------------- 서버/와이파이 ----------------------------
static const char*   WIFI_SSID    = WIFI_SSID_STR;
static const char*   WIFI_PSK     = WIFI_PASSWORD_STR;
static const char*   SERVER_IP    = SERVER_HOST_STR;
static const uint16_t SERVER_PORT = SERVER_PORT_NUM;

// ---------------------------- 하드웨어/라운드 ----------------------------
static constexpr int PIN_HOLD = 4;
static const int     MAX_GAMES = 3;

// ---------------------------- 메시지 큐 ----------------------------
enum class MsgType : uint8_t { SEND_TEXT };
struct Msg { MsgType type; char buf[128]; };
QueueHandle_t g_msgq = nullptr;

// ---------------------------- 전역 상태 ----------------------------
WiFiClient g_client;

volatile uint32_t g_kickCount = 0;   // 전체 카운트(수동/IMU 공용)
volatile uint32_t g_baseCount = 0;   // 라운드 시작시 스냅샷(증분 계산용)

static bool serverReady   = false;    // OK 수신 여부
static bool sessionActive = false;    // 세션 활성(OK 이후)
static bool inGame        = false;    // 라운드 진행 중
static int  gameRound     = 0;        // 0..(MAX_GAMES)
static unsigned long ttime = 0;   // START 시각(ms)

static volatile bool isHandshaking = false;

// ---------------------------- 유틸/표시 ----------------------------
void drawCount(uint32_t cnt, const char* sub=nullptr) {
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
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

// ---------------------------- 패킷 조립 (3필드, \n 종단) ----------------------------
void queue_start_packet() {
  Msg m{}; m.type = MsgType::SEND_TEXT;
  g_baseCount = g_kickCount;
  ttime = millis();
  inGame = true;
  // START <ttime(ms)> <dtime=0> <value=0>
  snprintf(m.buf, sizeof(m.buf), "START %lu %u %u",
           (unsigned long)ttime, 0u, 0u);
  xQueueSend(g_msgq, &m, 0);
  Serial.printf("[TX-QUEUE] %s\\n\n", m.buf);
}

void queue_final_packet() {
  if (!inGame) return;
  Msg m{}; m.type = MsgType::SEND_TEXT;
  unsigned long tEnd = millis();
  unsigned long dt = tEnd - ttime;
  uint32_t jegiCount = g_kickCount - g_baseCount;
  inGame = false;
  gameRound++;
  // FINAL <duration(ms)> <dtime=0> <jegiCount>
  snprintf(m.buf, sizeof(m.buf), "FINAL %lu %u %lu",
           (unsigned long)dt, 0u, (unsigned long)jegiCount);
  xQueueSend(g_msgq, &m, 0);
  Serial.printf("[TX-QUEUE] %s\\n\n", m.buf);
}

// ---------------------------- 핸드셰이크 ----------------------------
bool connectAndHandshake() {
  isHandshaking = true;                 // ★ 시작 플래그

  if (!ensureWifi()) { 
    Serial.println("[NET] Wi-Fi not connected");
    isHandshaking = false;
    return false;
  }

  if (g_client.connected()) g_client.stop();         // 이전 소켓 깨끗이
  Serial.printf("[TCP] CONNECT to %s:%u ...\n", SERVER_IP, (unsigned)SERVER_PORT);
  if (!g_client.connect(SERVER_IP, SERVER_PORT)) {
    Serial.println("[TCP] CONNECT FAIL");
    g_client.stop();
    isHandshaking = false;
    return false;
  }
  Serial.println("[TCP] CONNECT OK");

  g_client.setNoDelay(true);
  g_client.setTimeout(1000);

  String id  = STUDENT_ID_STR;
  String mac = WiFi.macAddress();
  String name = STUDENT_NAME_STR;

  char hello[160];
  snprintf(hello, sizeof(hello), "%s %s %s\r\n",
           id.c_str(), mac.c_str(), name.c_str());
  g_client.write((const uint8_t*)hello, strlen(hello));
  Serial.printf("[HELLO] TX HELLO: %s", hello);

  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    if (g_client.available()) {
      String line = g_client.readStringUntil('\n');
      line.trim();
      Serial.printf("[HELLO ]RX HELLO: [%s]\n", line.c_str());
      if (line.startsWith("OK")) {
        serverReady   = true;
        sessionActive = true;
        inGame        = false;
        gameRound     = 0;
        g_baseCount   = g_kickCount;

        while (g_client.available()) g_client.read(); // 잔여 바이트 비움
        drawCount(g_kickCount, "OK > A=START");
        isHandshaking = false;                        // ★ 정상 종료
        return true;
      } else {
        Serial.println("[HELLO] not OK → stop");
        g_client.stop();
        isHandshaking = false;       // ★ 실패 종료
        return false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  Serial.println("[HELLO] timeout → stop");
  g_client.stop();
  isHandshaking = false;             // ★ 타임아웃 종료
  return false;
}

// ---------------------------- 소켓 송신 태스크 ----------------------------
void SocketTask(void* parameter) {
  uint32_t dc_since = 0;
  for (;;) {
    if (!g_client.connected()) {
      if (dc_since == 0) dc_since = millis();
      if (millis() - dc_since > 400) {      // 0.4s 이상 지속 시만 세션 리셋
        serverReady = sessionActive = inGame = false;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    } else {
      dc_since = 0;
    }

    if (!serverReady || !sessionActive) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    Msg msg;
    if (xQueueReceive(g_msgq, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
      size_t len = strnlen(msg.buf, sizeof(msg.buf));
      g_client.write((const uint8_t*)msg.buf, len);
      g_client.write((const uint8_t*)"\n", 1);
      Serial.printf("TX: %s\\n\n", msg.buf);

      if (strncmp(msg.buf, "FINAL", 5) == 0) {
        String resp; uint32_t t0 = millis();
        while (millis() - t0 < 1200) {
          if (g_client.available()) { resp = g_client.readStringUntil('\n'); break; }
          vTaskDelay(pdMS_TO_TICKS(10));
        }
        resp.trim();
        if (resp.length()) Serial.printf("RX: [%s]\n", resp.c_str());
        if (resp.length() && (resp[0]=='1' || resp[0]=='2' || resp[0]=='3')) {
          if (gameRound < MAX_GAMES) drawCount(g_kickCount, "Next round ready > A=START");
          else { drawCount(g_kickCount, "3 rounds finished > A=Reconnect"); sessionActive = false; }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------------------- 디스플레이 태스크 ----------------------------
void DisplayTask(void* parameter) {
  for (;;) {
    const char* sub = nullptr;
    if (WiFi.status() != WL_CONNECTED)      sub = "Connecting to Wi-Fi...";
    else if (!g_client.connected())         sub = "Server not connected > A=Connect";
    else if (!serverReady)                  sub = "Waiting for OK...";
    else if (!sessionActive)                sub = "Session inactive > A=Connect";
    else if (inGame)                        sub = COUNT_MODE_MANUAL ? "In progress > A=FINAL / B=+1" : "In progress > A=FINAL";
    else                                    sub = "Ready > A=START";

    uint32_t shown = inGame ? (g_kickCount - g_baseCount) : g_kickCount;
    drawCount(shown, sub);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

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

// // ---------------------------- IMU 킥 감지(옵션) ----------------------------
// // Δg 스파이크 + 자이로(deg/s) 스윙 임계
// static const float W_HI_DEG = 150.0f;   // 스윙 감지 임계(deg/s)
// static const float A_SPIKE  = 0.6f;     // Δg 충격 임계
// static const float A_RECOV  = 0.15f;    // Δg 복귀 임계
// static const uint32_t T_SWING_MIN   = 60;
// static const uint32_t T_SWING_MAX   = 600;
// static const uint32_t T_IMPACT_WIN  = 320;
// static const uint32_t T_RECOVER     = 160;
// static const uint32_t T_COOLDOWN    = 260;
// static const uint32_t T_MIN_INTERVAL= 400;

// void KickDetectTask(void* parameter) {
// #if COUNT_MODE_MANUAL
//   vTaskDelete(nullptr); // 수동 모드면 태스크 종료
// #else
//   auto ema = [](float prev, float x, float a){ return a*x + (1.f-a)*prev; };
//   const float A_ALPHA = 0.25f, G_ALPHA = 0.20f, G0_ALPHA=0.02f;

//   enum Axis { X=0, Y=1, Z=2 };
//   Axis swingAxis = Y, instepAxis = X;
//   enum State { IDLE, SWING, IMPACT_WAIT, COOLDOWN };
//   State st = IDLE;

//   float axf=0, ayf=0, azf=0, gx_f=0, gy_f=0, gz_f=0, g0=1.0f;
//   uint32_t t_enter=0, lastCountMs=0; bool swingArmed=false;

//   for (;;) {
//     float ax, ay, az, gx, gy, gz;
//     M5.Imu.getAccel(&ax, &ay, &az);
//     M5.Imu.getGyro(&gx, &gy, &gz); // 보통 deg/s

//     axf = ema(axf, ax, A_ALPHA); ayf = ema(ayf, ay, A_ALPHA); azf = ema(azf, az, A_ALPHA);
//     gx_f = ema(gx_f, gx, G_ALPHA); gy_f = ema(gy_f, gy, G_ALPHA); gz_f = ema(gz_f, gz, G_ALPHA);

//     float gSwingDeg = (swingAxis==X)?gx_f: (swingAxis==Y)?gy_f:gz_f;
//     float absSwing  = fabsf(gSwingDeg);
//     float amag = sqrtf(axf*axf + ayf*ayf + azf*azf);
//     g0 = ema(g0, amag, G0_ALPHA);
//     float aspike = amag - g0;

//     float aInstep = (instepAxis==X)?axf: (instepAxis==Y)?ayf:azf;
//     bool directionOK = fabsf(aInstep) > 0.15f;

//     uint32_t now = millis();
//     switch (st) {
//       case IDLE:
//         if (now - lastCountMs >= T_MIN_INTERVAL && absSwing > W_HI_DEG) { st=SWING; t_enter=now; swingArmed=true; }
//         break;
//       case SWING: {
//         uint32_t dt = now - t_enter;
//         if (dt > T_SWING_MAX) { st=IDLE; swingArmed=false; break; }
//         if (absSwing < (W_HI_DEG*0.5f)) {
//           if (dt < T_SWING_MIN) { st=IDLE; swingArmed=false; }
//           else { st=IMPACT_WAIT; t_enter=now; }
//         }
//         break;
//       }
//       case IMPACT_WAIT: {
//         uint32_t dt = now - t_enter;
//         if (aspike > A_SPIKE && directionOK && swingArmed) {
//           if (inGame) g_kickCount++; // 라운드 중에만 카운트
//           lastCountMs = now; st = COOLDOWN; t_enter=now;
//         } else if (dt > T_IMPACT_WIN) st=IDLE;
//         break;
//       }
//       case COOLDOWN:
//         if ((aspike < A_RECOV) && (now - t_enter > T_RECOVER)) { st=IDLE; swingArmed=false; }
//         break;
//     }
//     vTaskDelay(pdMS_TO_TICKS(8));
//   }
// #endif
// }

// ---------------------------- Setup / Loop ----------------------------
void setup() {
  pinMode(PIN_HOLD, OUTPUT);
  digitalWrite(PIN_HOLD, HIGH);

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Imu.begin();
  M5.Display.setRotation(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  Serial.begin(115200);

  drawCount(g_kickCount, "Wi-Fi Connecting...");
  ensureWifi();

  g_msgq = xQueueCreate(16, sizeof(Msg));

  xTaskCreatePinnedToCore(SocketTask,   "SocketTask",  4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(DisplayTask,  "DisplayTask", 2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(KickDetectTask,"KickDetect", 3072, nullptr, 1, nullptr, 1);

  drawCount(g_kickCount, "A=CONNECTION/OK → A=START / A=FINAL, B=+1(Manually)");
}

void loop() {
  M5.update();
  static unsigned long lastA=0, lastB=0;
  unsigned long now = millis();

  if (!isHandshaking && g_client.connected() && !serverReady) {
    g_client.stop();
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // A 버튼: 접속/OK → START/FINAL 토글 (이하는 기존 그대로)
  if (M5.BtnA.wasPressed() && now - lastA > BTN_DEBOUNCE_MS) {
    Serial.println("[BTN] A pressed");
    lastA = now;
    if (!g_client.connected() || !serverReady) {
      drawCount(g_kickCount, "Waiting for server connection/OK...");
      if (connectAndHandshake()) drawCount(g_kickCount, "OK > A=START");
      else                      drawCount(g_kickCount, "NOT FOUND");
    } else {
      if (!inGame && gameRound < MAX_GAMES) {
        queue_start_packet();
        drawCount(g_kickCount - g_baseCount, "Round in progress > A=FINAL");
      } else if (inGame) {
        if (millis() - ttime < MIN_ROUND_MS) {
          drawCount(g_kickCount - g_baseCount, "Keep going a bit more > A=FINAL");
        } else {
          queue_final_packet();  // 응답은 SocketTask
        }
      } else {
        drawCount(g_kickCount, "3 rounds finished > A=Reconnect");
      }
    }
  }

  // B 버튼: 수동 카운트(라운드 중일 때만)
  if (M5.BtnB.wasPressed() && now - lastB > BTN_DEBOUNCE_MS) {
    Serial.println("[BTN] B pressed");
    lastB = now;
#if COUNT_MODE_MANUAL
    if (inGame) {
      g_kickCount++;
      drawCount(g_kickCount - g_baseCount, "Manual +1 (A=FINAL)");
    } else {
      drawCount(g_kickCount, "A=START First!");
    }
#else
    drawCount(g_kickCount - g_baseCount, "IMU mode (B=diabled)");
#endif
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
# Jegichagi on M5StickC Plus2

- ESP32 기반 M5StickC Plus2로 제기차기(킥) 횟수를 인식·표시하고, 노트북 TCP 서버로 전송하는 프로젝트입니다.

- M5StickC Plus2: 카운트 표시 + 버튼 전송 + IMU(가속도/자이로) 기반 킥 인식

- 노트북: TCP 서버로 수신/로깅

### 주요 기능

- 디스플레이: 현재 제기 카운트 상시 표시

- 버튼 전송: 정면 BtnA를 누르면 JEGI:count 를 서버로 전송

- IMU 킥 인식: 스윙(자이로) → 임팩트(가속도) → 상태기계(FSM)로 오검출(벽/바닥 충돌 등) 최소화

- 네트워크: Wi-Fi STA + TCP 클라이언트, 메시지 큐 기반 송신 태스크


### 프로젝트 구조
```
Jegichagi/
├─ Jegichagi.ino           # 메인 코드
├─ secrets.h               # 비밀값
├─ tcp_server.py           # 노트북 측 Python TCP 서버
└─ .gitignore              # secrets.h 등 비공개 처리
```

### 준비물

- M5StickC Plus2

- Arduino IDE

- 라이브러리: M5Unified, WiFi

- 노트북(Windows/macOS/Linux) + Python 3.x

### 설치 & 빌드
1) Arduino 보드/라이브러리

- Arduino IDE → 보드 매니저에서 ESP32 설치

- 라이브러리 매니저에서 M5Unified 설치

2) 비밀값 분리 (secrets.h)

- secrets.h.template를 복사해 secrets.h 생성 후 값 채우기

```cpp
// secrets.h
#pragma once
static constexpr char WIFI_SSID[]     = "ssid_name";
static constexpr char WIFI_PASSWORD[] = "wifi_password";
static constexpr char SERVER_HOST[]   = "my-pc.local";
static constexpr uint16_t SERVER_PORT = 5000;
```

3) 스케치 설정 포인트

- 전원 유지(HOLD): Plus2는 부팅 후 GPIO4(HOLD)=HIGH 필요


### 노트북 서버 실행

```
python tcp_server.py
```

- 기본 포트: 5000

- 최초 접속 시 ESP32가 MAC을 전송 → 서버가 OK를 회신(인증)

- 이후 count 라인이 버튼A 입력 시마다 도착


### 동작 방법

- 전원 온 → Stick 화면에 JEGI: n 표시

- 제기를 차면 카운트가 증가

- BtnA(정면 버튼)을 누르면 현재 카운트를 서버로 전송 (JEGI:n)
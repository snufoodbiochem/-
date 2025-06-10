#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <SoftwareSerial.h>

const char* ssid = "jabara";
const char* password = "12345678";
const IPAddress sensor_ip(192, 168, 4, 2);  // 센서 ESP IP

ESP8266WebServer server(80);

const int pumpPin = 0;
bool pumpOn = false;
float clo2 = 0.0;

unsigned long lastRead = 0;

// 수동 모드
bool manualPumpMode = false;
unsigned long manualPumpStart = 0;
unsigned long manualPumpDuration = 0;

// 자동 모드
bool autoMode = false;
float targetClO2 = 0.0;
unsigned long autoPumpStart = 0;

bool inPausePhase = false;
unsigned long pauseStartTime = 0;

bool inMeasuringPhase = false;
int measureCount = 0;
float measureSum = 0;
unsigned long lastMeasureTime = 0;

bool waitingToTurnOn = false;
unsigned long lowStartTime = 0;

void turnOnPump() {
  digitalWrite(pumpPin, LOW);
  pumpOn = true;
}

void turnOffPump() {
  digitalWrite(pumpPin, HIGH);
  pumpOn = false;
}

const int maxSensorRetries = 5;
int sensorRetries = 0;

void fetchClO2FromSensor() {
  if (sensorRetries >= maxSensorRetries) {
    return;  // 이미 5번 시도한 경우, 더 이상 시도하지 않음
  }

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + sensor_ip.toString() + "/clo2";
  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    clo2 = payload.toFloat();
    sensorRetries = 0;  // 센서가 응답하면 시도 횟수 초기화
  } else {
    clo2 = -1.0;  // 센서 응답 실패 시 -1로 설정
    sensorRetries++;  // 시도 횟수 증가
    Serial.println("Failed to get ClO2 value from sensor.");
  }

  http.end();
}

void handlePumpCommand() {
  String uri = server.uri();
  if (uri.startsWith("/pump_")) {
    int seconds = uri.substring(6).toInt();
    if (seconds > 0) {
      manualPumpMode = true;
      autoMode = false;
      manualPumpStart = millis();
      manualPumpDuration = seconds * 1000;
      turnOnPump();
      server.send(200, "text/plain", "Pump on for " + String(seconds) + " seconds");
      return;
    }
  }
  server.send(400, "text/plain", "Invalid pump command");
}

void handleClo2Command() {
  // 센서가 없는 경우, /clo2 명령어를 무시하도록 설정
  if (clo2 == -1.0) {
    server.send(200, "text/plain", String(clo2, 2));
    //server.send(400, "text/plain", "Sensor not available");
    return;
  }

  String uri = server.uri();
  if (uri.startsWith("/clo2_")) {
    float val = uri.substring(6).toFloat();
    if (val > 0) {
      targetClO2 = val;

      fetchClO2FromSensor();

      autoMode = true;
      manualPumpMode = false;
      inPausePhase = false;
      inMeasuringPhase = false;
      measureCount = 0;
      measureSum = 0;
      waitingToTurnOn = false;

      if (clo2 < targetClO2) {
        turnOnPump();
        autoPumpStart = millis();
      } else {
        turnOffPump();  // 목표보다 높으면 대기 상태로 시작
      }

      server.send(200, "text/plain", "Target ClO2 set: " + String(val));
      return;
    }
  }
  server.send(400, "text/plain", "Invalid clo2 command");
}

void handleStop() {
  autoMode = false;
  manualPumpMode = false;
  inPausePhase = false;
  inMeasuringPhase = false;
  waitingToTurnOn = false;
  turnOffPump();
  server.send(200, "text/plain", "Pump stopped, all modes cancelled");
}

void handleClo2Value() {
  // 센서가 없는 경우 /clo2 요청에 대해 400 응답
  if (clo2 == -1.0) {
   server.send(200, "text/plain", String(clo2, 2));
   //server.send(400, "text/plain", "Sensor not available");
    return;
  }

  fetchClO2FromSensor();
  server.send(200, "text/plain", String(clo2, 2));
}

void setup() {
  pinMode(pumpPin, OUTPUT);
  turnOffPump();

  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid, password);

  server.on("/stop", handleStop);
  server.on("/clo2", handleClo2Value);
  server.onNotFound([]() {
    String uri = server.uri();
    if (uri.startsWith("/pump_")) handlePumpCommand();
    else if (uri.startsWith("/clo2_")) handleClo2Command();
    else server.send(404, "text/plain", "Unknown command");
  });

  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // 주기적 ClO2 값 요청 (clo2 값이 -1.0일 때 5초마다 요청)
  if (now - lastRead >= (clo2 == -1.0 ? 5000 : 1000)) {  // clo2가 -1.0이면 5초마다 요청
    lastRead = now;
    fetchClO2FromSensor();
  }

  // 수동 모드 타이머
  if (manualPumpMode && now - manualPumpStart >= manualPumpDuration) {
    manualPumpMode = false;
    turnOffPump();
  }

  // 자동 모드
  if (autoMode) {
    // 펌프가 ON된 상태 → 10초 후 OFF + 5초 휴지기 진입
    if (pumpOn && !inPausePhase && now - autoPumpStart >= 10000) {
      turnOffPump();
      inPausePhase = true;
      pauseStartTime = now;
    }

    // 5초 휴지기 → 끝나면 측정 시작
    if (inPausePhase && !inMeasuringPhase && now - pauseStartTime >= 5000) {
      inPausePhase = false;
      inMeasuringPhase = true;
      measureCount = 0;
      measureSum = 0;
      lastMeasureTime = now;
    }

    // 측정 단계: 5초간 평균 측정
    if (inMeasuringPhase && now - lastMeasureTime >= 1000 && measureCount < 5) {
      lastMeasureTime = now;
      measureSum += clo2;
      measureCount++;

      if (measureCount == 5) {
        float avg = measureSum / 5.0;
        if (avg < targetClO2) {
          autoPumpStart = millis();
          turnOnPump();
          inMeasuringPhase = false;
        } else {
          inMeasuringPhase = false;
          waitingToTurnOn = false;
        }
      }
    }

    // 측정 대기 상태에서 ClO2가 5초 연속 낮으면 재작동
    if (!pumpOn && !inMeasuringPhase && !inPausePhase) {
      if (clo2 < targetClO2) {
        if (!waitingToTurnOn) {
          waitingToTurnOn = true;
          lowStartTime = now;
        } else {
          if (now - lowStartTime >= 5000) {
            autoPumpStart = now;
            turnOnPump();
            waitingToTurnOn = false;
          }
        }
      } else {
        waitingToTurnOn = false;
        lowStartTime = 0;
      }
    }
  }
}

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

const char* ssid = "jabara";         // Access Point 이름
const char* password = "12345678";   // AP 비밀번호

ESP8266WebServer server(80);

// NC 에 연결함

const int pumpPin = 0;   // 펌프(릴레이) 제어 핀 (회로에 따라 조정)
bool pumpOn = false;
unsigned long pumpOnStart = 0;    // 펌프 ON 시작 시각 (millis)
unsigned long pumpDuration = 0;   // 펌프를 ON 해야 하는 시간 (밀리초)

// 펌프를 끄는 함수 reversed
void turnOffPump() {
  digitalWrite(pumpPin, HIGH);  // 릴레이 OFF (회로에 따라 LLL가 OFF일 수 있음)
  pumpOn = false;
}

// /stop 요청 처리: 펌프  즉시 정지
void handleStop() {
  turnOffPump();
  server.send(200, "text/plain", "Pump stopped");
}

// NotFound 핸들러에서 "pump_XX" 명령 처리
void handlePumpCommand() {
  String uri = server.uri();   // 예: "/pump_10"
  if (uri.startsWith("/pump_")) {
    String timeStr = uri.substring(6);  // "/pump_" 이후의 문자열 추출
    int seconds = timeStr.toInt();        // 문자열을 정수로 변환
    if (seconds > 0) {
      digitalWrite(pumpPin, LOW);   // 펌프 ON (릴레이 활성화)
      pumpOn = true;
      pumpOnStart = millis();
      pumpDuration = seconds * 1000; // 초를 밀리초로 변환
      server.send(200, "text/plain", "Pump on for " + String(seconds) + " seconds");
      return;
    }
  }
  // 유효하지 않은 명령이면 404 응답
  server.send(404, "text/plain", "Invalid command");
}

void setup() {
  Serial.begin(115200);
  pinMode(pumpPin, OUTPUT);
 
 digitalWrite(pumpPin, HIGH);

  WiFi.softAP(ssid, password);
  Serial.println("Access Point started");
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/stop", handleStop);
  server.onNotFound(handlePumpCommand);
  server.begin();
  server.send(404, "text/plain", "Pump connected");
  
  
}

void loop() {
  server.handleClient();
  // 펌프가 켜진 상태에서 지정 시간이 지나면 자동 OFF
  if (pumpOn && (millis() - pumpOnStart >= pumpDuration)) {
    turnOffPump();
  }
}

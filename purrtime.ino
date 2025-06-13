#include <Arduino_LSM6DS3.h>
#include <WiFiNINA.h>
#include <FlashStorage.h>
#include <WiFiUdp.h>
#include <TimeLib.h>

char ap_ssid[] = "Nano_Config";
char ap_pass[] = "12345678";
WiFiServer server(80);

bool shouldConnect = false;
String targetSSID = "";
String targetPASS = "";

typedef struct {
  char ssid[64];
  char pass[64];
} WiFiConfig;

FlashStorage(wifi_storage, WiFiConfig);


WiFiUDP udp;
const char* ntpServer = "pool.ntp.org";
const int timeZoneOffset = 0;  // UTC

unsigned long lastNtpSync = 0;
unsigned long ntpSyncInterval = 3600000;  // 每小时同步一次
unsigned long lastPostTime = 0;
unsigned long postInterval = 60000;  // 每分钟发送一次

time_t currentUtcTime = 0;

const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];

void syncTimeWithNTP() {
  udp.begin(2390);
  IPAddress timeServerIP;
  WiFi.hostByName(ntpServer, timeServerIP);

  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;  // LI, Version, Mode
  packetBuffer[1] = 0;           // Stratum
  packetBuffer[2] = 6;           // Polling Interval
  packetBuffer[3] = 0xEC;        // Peer Clock Precision

  udp.beginPacket(timeServerIP, 123);
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();

  delay(1000);
  if (udp.parsePacket()) {
    udp.read(packetBuffer, NTP_PACKET_SIZE);
    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
    unsigned long secsSince1900 = (highWord << 16) | lowWord;
    const unsigned long seventyYears = 2208988800UL;
    currentUtcTime = secsSince1900 - seventyYears + timeZoneOffset * 3600;
    setTime(currentUtcTime);
    Serial.println("🕓 NTP Time Synced.");
  } else {
    Serial.println("⚠️ NTP sync failed");
  }
}


void handlePeriodicPost() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();

  // 同步 NTP 时间（每小时）
  if (now - lastNtpSync > ntpSyncInterval || currentUtcTime == 0) {
    syncTimeWithNTP();
    lastNtpSync = now;
  }

  // 每分钟发送一次
  if (now - lastPostTime >= postInterval) {
    lastPostTime = now;

    char timestampBuffer[30];
    sprintf(timestampBuffer, "%04d-%02d-%02dT%02d:%02d:%02dZ",
            year(), month(), day(), hour(), minute(), second());

    postActivity(
      "4d8bc5ed-1b60-4181-89ae-becdf0cddedf",  // catId
      "Resting",
      timestampBuffer);
  }
}


void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;

  if (tryConnectFromFlash()) {
    return;  // 成功连接就不启动 AP
  }

  // 启动热点模式
  if (WiFi.beginAP(ap_ssid, ap_pass) != WL_AP_LISTENING) {
    Serial.println("Failed to start AP");
    while (true)
      ;
  }

  Serial.print("AP started. Connect to: ");
  Serial.println(WiFi.localIP());

  server.begin();
}

void loop() {
  handleHttpRequest();
  handleWiFiConnection();
  handlePeriodicPost();
}

void saveWiFiConfig(String ssid, String pass) {
  WiFiConfig config;
  ssid.toCharArray(config.ssid, sizeof(config.ssid));
  pass.toCharArray(config.pass, sizeof(config.pass));
  wifi_storage.write(config);
}

bool tryConnectFromFlash() {
  WiFiConfig config = wifi_storage.read();
  if (strlen(config.ssid) == 0 || strlen(config.pass) == 0) return false;

  Serial.print("Trying saved WiFi: ");
  Serial.println(config.ssid);

  WiFi.begin(config.ssid, config.pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Auto-reconnected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\n❌ Failed to connect.");
    Serial.print("Status code: ");
    Serial.println(WiFi.status());
    return false;
  }
}

void handleHttpRequest() {
  WiFiClient client = server.available();
  if (!client) return;

  Serial.println("Client connected");

  String request = "";
  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      request += c;
      if (request.endsWith("\r\n\r\n")) break;
    }
  }

  if (request.indexOf("GET / ") >= 0) {
    sendForm(client);
  } else if (request.indexOf("POST /connect") >= 0) {
    String body = "";
    while (client.available()) {
      char c = client.read();
      body += c;
    }

    int ssidIndex = body.indexOf("ssid=");
    int passIndex = body.indexOf("&pass=");
    if (ssidIndex >= 0 && passIndex >= 0) {
      targetSSID = urlDecode(body.substring(ssidIndex + 5, passIndex));
      targetPASS = urlDecode(body.substring(passIndex + 6));
      shouldConnect = true;

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/html\r\n");
      client.println("<html><body>");
      client.println("<h3>Trying to connect to WiFi...</h3>");
      client.println("</body></html>");
    }
  }

  delay(1);
  client.stop();
  Serial.println("Client disconnected");
}

void handleWiFiConnection() {
  if (!shouldConnect) return;

  Serial.print("Connecting to: ");
  Serial.println(targetSSID);

  WiFi.end();  // 退出 AP
  delay(1000);
  WiFi.begin(targetSSID.c_str(), targetPASS.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connected to home WiFi!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    // saveWiFiConfig(targetSSID, targetPASS);
  } else {
    Serial.println("\n❌ Failed to connect. Restarting AP...");
    WiFi.end();
    WiFi.beginAP(ap_ssid, ap_pass);
  }

  saveWiFiConfig(targetSSID, targetPASS);

  shouldConnect = false;
}

void sendForm(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html\r\n");

  client.println("<html><body>");
  client.println("<h2>Configure WiFi</h2>");
  client.println("<form method='POST' action='/connect'>");
  client.println("SSID: <input name='ssid'><br>");
  client.println("Password: <input name='pass' type='password'><br>");
  client.println("<input type='submit' value='Connect'>");
  client.println("</form>");
  client.println("</body></html>");
}

String urlDecode(String input) {
  String decoded = "";
  char temp[] = "0x00";
  unsigned int len = input.length();
  unsigned int i = 0;

  while (i < len) {
    char c = input[i];
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%') {
      if (i + 2 < len) {
        temp[2] = input[i + 1];
        temp[3] = input[i + 2];
        decoded += (char)strtol(temp, NULL, 16);
        i += 2;
      }
    } else {
      decoded += c;
    }
    i++;
  }
  return decoded;
}

void initIMU() {
  if (!IMU.begin()) {
    Serial.println("❌ 无法初始化加速度计（IMU）！");
    while (true)
      ;  // 卡住
  }

  Serial.println("✅ 加速度计初始化成功！");
}

void getAcceleration() {
  float x, y, z;

  if (IMU.accelerationAvailable()) {
    IMU.readAcceleration(x, y, z);

    float threshold = 0.2;  // 灵敏度阈值
    if (abs(x) > threshold || abs(y) > threshold || abs(z - 1.0) > threshold) {
      Serial.println("🐾 Cat is moving");
    } else {
      Serial.println("😴 Cat is resting");
    }

  } else {
    Serial.println("等待数据...");
  }

  delay(300);
}

void postActivity(const char* catId, const char* status, const char* timestamp) {
  WiFiSSLClient client;
  const char* host = "purrtimebackend.onrender.com";
  int port = 443;

  String url = "/activities/" + String(catId);

  // 构造 JSON 请求体
  String jsonBody = "{\"status\":\"" + String(status) + "\",\"timeStamp\":\"" + String(timestamp) + "\"}";

  // 尝试连接服务器
  if (client.connect(host, port)) {
    Serial.println("✅ Connected to server");

    client.println("POST " + url + " HTTP/1.1");
    client.println("Host: " + String(host));
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(jsonBody.length());
    client.println("Connection: close");
    client.println();          // 结束 Header
    client.println(jsonBody);  // 发送正文

    // 可选：读取响应
    while (client.connected()) {
      if (client.available()) {
        String line = client.readStringUntil('\n');
        Serial.println(line);
      }
    }

    client.stop();
    Serial.println("✅ Request sent and connection closed");
  } else {
    Serial.println("❌ Connection to server failed");
  }
}

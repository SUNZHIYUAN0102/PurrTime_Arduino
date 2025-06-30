#include <Arduino_LSM6DS3.h>
#include <WiFiNINA.h>
#include <FlashStorage.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
char ap_ssid[] = "Cat_Device_AP";
char ap_pass[] = "12345678";
WiFiServer server(80);

bool shouldConnect = false;
String targetSSID = "";
String targetPASS = "";
String targetCatId = ""; // 新增 CatID 字段

typedef struct
{
  char ssid[64];
  char pass[64];
  char deviceId[20];
  char catId[40];
  bool registered;
} WiFiConfig;

FlashStorage(wifi_storage, WiFiConfig);

#define BUTTON_PIN 7

unsigned long pressStart = 0;
bool isPressed = false;

void handleButtonEvent()
{
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  if (pressed && !isPressed)
  {
    pressStart = millis();
    isPressed = true;
  }
  else if (!pressed && isPressed)
  {
    unsigned long duration = millis() - pressStart;
    isPressed = false;

    if (duration > 2000)
    {
      Serial.println("🧹 Long press detected, resetting config...");

      // 清空配置并重启
      WiFiConfig emptyConfig = {};
      wifi_storage.write(emptyConfig);
      delay(1000);
      NVIC_SystemReset(); // 软件重启
    }
    else
    {
      Serial.println("✅ Short press detected");
      // 你可以在这里加上：手动执行一次 postActivity()
      // 或者 print 当前连接状态等
    }
  }
}

WiFiUDP udp;
const char *ntpServer = "pool.ntp.org";
const int timeZoneOffset = 0; // UTC
unsigned long lastNtpSync = 0;
unsigned long ntpSyncInterval = 3600000; // 每小时同步一次
unsigned long lastPostTime = 0;
unsigned long postInterval = 60000; // 每分钟发送一次
time_t currentUtcTime = 0;
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];

void syncTimeWithNTP()
{
  udp.begin(2390);
  IPAddress timeServerIP;
  WiFi.hostByName(ntpServer, timeServerIP);

  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011; // LI, Version, Mode
  packetBuffer[1] = 0;          // Stratum
  packetBuffer[2] = 6;          // Polling Interval
  packetBuffer[3] = 0xEC;       // Peer Clock Precision

  udp.beginPacket(timeServerIP, 123);
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();

  delay(1000);
  if (udp.parsePacket())
  {
    udp.read(packetBuffer, NTP_PACKET_SIZE);
    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
    unsigned long secsSince1900 = (highWord << 16) | lowWord;
    const unsigned long seventyYears = 2208988800UL;
    currentUtcTime = secsSince1900 - seventyYears + timeZoneOffset * 3600;
    setTime(currentUtcTime);
    Serial.println("🕓 NTP Time Synced.");
  }
  else
  {
    Serial.println("⚠️ NTP sync failed");
  }
}

void handlePeriodicPost()
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  unsigned long now = millis();

  // 同步 NTP 时间（每小时）
  if (now - lastNtpSync > ntpSyncInterval || currentUtcTime == 0)
  {
    syncTimeWithNTP();
    lastNtpSync = now;
  }

  // 每分钟发送一次
  if (now - lastPostTime >= postInterval)
  {
    lastPostTime = now;

    char timestampBuffer[30];
    sprintf(timestampBuffer, "%04d-%02d-%02dT%02d:%02d:%02dZ",
            year(), month(), day(), hour(), minute(), second());

    postActivity(
        "4d8bc5ed-1b60-4181-89ae-becdf0cddedf", // catId
        "Resting",
        timestampBuffer);
  }
}

void setup()
{
  Serial.begin(115200);
  while (!Serial)
    ;

  if (tryConnectFromFlash())
  {
    return; // 成功连接就不启动 AP
  }

  // 启动热点模式
  if (WiFi.beginAP(ap_ssid, ap_pass) != WL_AP_LISTENING)
  {
    Serial.println("Failed to start AP");
    while (true)
      ;
  }

  Serial.print("AP started. Connect to: ");
  Serial.println(WiFi.localIP());

  server.begin();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void loop()
{
  handleHttpRequest();
  handleWiFiConnection();
  // handlePeriodicPost();
  handleButtonEvent();
}

void saveWiFiConfig(String ssid, String pass, String catId)
{
  WiFiConfig config;
  ssid.toCharArray(config.ssid, sizeof(config.ssid));
  pass.toCharArray(config.pass, sizeof(config.pass));
  catId.toCharArray(config.catId, sizeof(config.catId));

  String deviceId = getDeviceId();
  deviceId.toCharArray(config.deviceId, sizeof(config.deviceId));

  wifi_storage.write(config);
  Serial.print("💾 Saved Device ID to flash: ");
  Serial.println(config.deviceId);
  Serial.print("🐱 Cat ID: ");
  Serial.println(config.catId);
}

String getDeviceId()
{
  byte mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  sprintf(macStr, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

bool tryConnectFromFlash()
{
  WiFiConfig config = wifi_storage.read();
  if (strlen(config.ssid) == 0 || strlen(config.pass) == 0)
    return false;

  Serial.print("Trying saved WiFi: ");
  Serial.println(config.ssid);

  WiFi.begin(config.ssid, config.pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\n✅ Auto-reconnected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  else
  {
    Serial.println("\n❌ Failed to connect.");
    Serial.print("Status code: ");
    Serial.println(WiFi.status());
    return false;
  }
}

void handleHttpRequest()
{
  WiFiClient client = server.available();
  if (!client)
    return;

  Serial.println("Client connected");

  String request = "";
  while (client.connected())
  {
    if (client.available())
    {
      char c = client.read();
      request += c;
      if (request.endsWith("\r\n\r\n"))
        break;
    }
  }

  if (request.indexOf("GET / ") >= 0)
  {
    sendForm(client);
  }
  else if (request.indexOf("POST /connect") >= 0)
  {
    String body = "";
    while (client.available())
    {
      char c = client.read();
      body += c;
    }

    int ssidIndex = body.indexOf("ssid=");
    int passIndex = body.indexOf("&pass=");
    int catIdIndex = body.indexOf("&catId=");
    if (ssidIndex >= 0 && passIndex >= 0 && catIdIndex >= 0)
    {
      targetSSID = urlDecode(body.substring(ssidIndex + 5, passIndex));
      targetPASS = urlDecode(body.substring(passIndex + 6, catIdIndex));
      targetCatId = urlDecode(body.substring(catIdIndex + 7));
      Serial.println("Received WiFi credentials:");
      Serial.print("SSID: ");
      Serial.println(targetSSID);
      Serial.print("Password: ");
      Serial.println(targetPASS);
      Serial.print("CatID: ");
      Serial.println(targetCatId);

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

void handleWiFiConnection()
{
  if (!shouldConnect)
    return;

  Serial.print("Connecting to: ");
  Serial.println(targetSSID);

  WiFi.end(); // 退出 AP
  delay(1000);
  WiFi.begin(targetSSID.c_str(), targetPASS.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\n✅ Connected to home WiFi!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    saveWiFiConfig(targetSSID, targetPASS, targetCatId);
    syncTimeWithNTP(); // 同步 NTP 时间
    addDeviceToServer(); // 注册设备到服务器
  }
  else
  {
    Serial.println("\n❌ Failed to connect. Restarting AP...");
    WiFi.end();

    delay(1000);

    if (WiFi.beginAP(ap_ssid, ap_pass) == WL_AP_LISTENING)
    {
      Serial.println("📡 AP restarted at: ");
      Serial.println(WiFi.localIP());

      server.begin(); // ✅ 重新启动 HTTP server
    }
    else
    {
      Serial.println("❌ Failed to restart AP mode");
    }
  }
  shouldConnect = false;
}

void sendForm(WiFiClient &client)
{
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html\r\n");

  client.println("<html><body>");
  client.println("<h2>Configure WiFi</h2>");
  client.println("<form method='POST' action='/connect'>");
  client.println("SSID: <input name='ssid' required><br>");
  client.println("Password: <input name='pass' type='password' required><br>");
  client.println("CatID: <input name='catId' required><br>");
  client.println("<input type='submit' value='Connect'>");
  client.println("</form>");
  client.println("</body></html>");
}

String urlDecode(String input)
{
  String decoded = "";
  char temp[] = "0x00";
  unsigned int len = input.length();
  unsigned int i = 0;

  while (i < len)
  {
    char c = input[i];
    if (c == '+')
    {
      decoded += ' ';
    }
    else if (c == '%')
    {
      if (i + 2 < len)
      {
        temp[2] = input[i + 1];
        temp[3] = input[i + 2];
        decoded += (char)strtol(temp, NULL, 16);
        i += 2;
      }
    }
    else
    {
      decoded += c;
    }
    i++;
  }
  return decoded;
}

void initIMU()
{
  if (!IMU.begin())
  {
    Serial.println("❌ 无法初始化加速度计（IMU）！");
    while (true)
      ; // 卡住
  }

  Serial.println("✅ 加速度计初始化成功！");
}

void getAcceleration()
{
  float x, y, z;

  if (IMU.accelerationAvailable())
  {
    IMU.readAcceleration(x, y, z);

    float threshold = 0.2; // 灵敏度阈值
    if (abs(x) > threshold || abs(y) > threshold || abs(z - 1.0) > threshold)
    {
      Serial.println("🐾 Cat is moving");
    }
    else
    {
      Serial.println("😴 Cat is resting");
    }
  }
  else
  {
    Serial.println("等待数据...");
  }

  delay(300);
}

void postActivity(const char *catId, const char *status, const char *timestamp)
{
  WiFiSSLClient client;
  const char *host = "purrtimebackend.onrender.com";
  int port = 443;

  String url = "/activities/" + String(catId);

  // 构造 JSON 请求体
  String jsonBody = "{\"status\":\"" + String(status) + "\",\"timeStamp\":\"" + String(timestamp) + "\"}";

  // 尝试连接服务器
  if (client.connect(host, port))
  {
    Serial.println("✅ Connected to server");

    client.println("POST " + url + " HTTP/1.1");
    client.println("Host: " + String(host));
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(jsonBody.length());
    client.println("Connection: close");
    client.println();         // 结束 Header
    client.println(jsonBody); // 发送正文

    // 可选：读取响应
    while (client.connected())
    {
      if (client.available())
      {
        String line = client.readStringUntil('\n');
        Serial.println(line);
      }
    }

    client.stop();
    Serial.println("✅ Request sent and connection closed");
  }
  else
  {
    Serial.println("❌ Connection to server failed");
  }
}

void addDeviceToServer()
{
  WiFiSSLClient client;
  const char *host = "purrtimebackend.onrender.com";
  int port = 443;

  WiFiConfig config = wifi_storage.read();

  // 生成 ISO 时间字符串作为 lastSeen
  char timestampBuffer[30];
  sprintf(timestampBuffer, "%04d-%02d-%02dT%02d:%02d:%02dZ",
          year(), month(), day(), hour(), minute(), second());

  // 构造 JSON body
  String jsonBody = "{\"deviceId\":\"" + String(config.deviceId) +
                    "\",\"lastSeen\":\"" + String(timestampBuffer) + "\"}";

  // 构造请求路径
  String url = "/devices/" + String(config.catId);

  Serial.println("📡 Connecting to backend to register device...");

  if (client.connect(host, port))
  {
    Serial.println("✅ Connected to backend");

    client.println("POST " + url + " HTTP/1.1");
    client.println("Host: " + String(host));
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(jsonBody.length());
    client.println("Connection: close");
    client.println();         // 结束 headers
    client.println(jsonBody); // 发送正文

    String responseLine = "";
    while (client.connected())
    {
      if (client.available())
      {
        String line = client.readStringUntil('\n');
        Serial.println(line);

        if (responseLine == "" && line.startsWith("HTTP/1.1"))
        {
          responseLine = line;
        }
      }
    }

    // 检查是否为 201 Created 或 200 OK
    if (responseLine.startsWith("HTTP/1.1 200") || responseLine.startsWith("HTTP/1.1 201"))
    {
      Serial.println("✅ Device registration successful");

      // 更新 registered = true
      config.registered = true;
      wifi_storage.write(config);
    }
    else
    {
      Serial.println("❌ Server responded with error.");
    }

    client.stop();
  }
  else
  {
    Serial.println("❌ Failed to connect to backend");
  }
}

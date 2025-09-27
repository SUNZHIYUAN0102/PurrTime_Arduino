#include <Arduino_LSM6DS3.h>
#include <WiFiNINA.h>
#include <FlashStorage.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <ArduinoBLE.h>
#include <Wire.h>

#define ENABLE_BLE_STREAM 0
#define BLE_SERVICE_UUID "12345678-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_TX_UUID "12345678-1234-5678-1234-56789abcdef1" // notify

BLEService dataService(BLE_SERVICE_UUID);
BLECharacteristic txChar(BLE_CHAR_TX_UUID, BLERead | BLENotify, 512);

bool initBLE()
{
  if (!BLE.begin())
  {
    Serial.println("❌ BLE init failed");
    return false;
  }
  BLE.setLocalName("Nano33IoT");
  BLE.setDeviceName("Nano33IoT");
  BLE.setAdvertisedService(dataService);
  dataService.addCharacteristic(txChar);
  BLE.addService(dataService);

  uint8_t zero = 0;
  txChar.writeValue(&zero, 1);

  BLE.advertise();
  Serial.println("📡 BLE advertising...");
  return true;
}

void bleSendLongLine(const char *line)
{
  const size_t MTU_SAFE = 180; // 保守安全片长（适配 Win 常见上限）
  size_t n = strlen(line);
  for (size_t i = 0; i < n; i += MTU_SAFE)
  {
    size_t chunkLen = (i + MTU_SAFE <= n) ? MTU_SAFE : (n - i);
    txChar.writeValue((const uint8_t *)(line + i), chunkLen);
    delay(5); // 给栈留点时间，防止黏包/拥塞
  }
}

void setAccelRange8g()
{
  // 先读 CTRL1_XL
  Wire.beginTransmission(0x6A); // LSM6DS3 I2C 地址（默认 0x6A）
  Wire.write(0x10);             // CTRL1_XL
  Wire.endTransmission(false);
  Wire.requestFrom(0x6A, 1);
  uint8_t ctrl1 = Wire.read();

  // 清掉 FS bits (bit[3:2])，写成 10 (8g)
  ctrl1 &= ~(0b11 << 2);
  ctrl1 |= (0b10 << 2);

  // 写回去
  Wire.beginTransmission(0x6A);
  Wire.write(0x10);
  Wire.write(ctrl1);
  Wire.endTransmission();
}

char ap_ssid[] = "Cat_Device_AP";
char ap_pass[] = "12345678";
WiFiServer server(80);

enum NetMode
{
  MODE_AP,
  MODE_STA
};
NetMode netMode = MODE_AP;

unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000;

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

void setup()
{
  Serial.begin(115200);
  // while (!Serial)
  //   ;

  initIMU();

  setAccelRange8g();
  Serial.println("Accel range set to ±8g");

  // 蓝牙模式
  if (ENABLE_BLE_STREAM == 1)
  {
    if (!initBLE())
      while (1)
        ;
  }
  // WiFi模式
  else
  {
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
  }
}

void loop()
{
  handleHttpRequest();
  handleWiFiConnection();

  if (ENABLE_BLE_STREAM == 1)
  {
    BLEDevice central = BLE.central();
    if (central)
    {
      Serial.print("🔗 BLE connected: ");
      Serial.println(central.address());

      unsigned long last = 0;
      while (central.connected())
      {
        BLE.poll();
        // if (millis() - last >= 1000)
        // { // 每秒一次
        //   last = millis();
        //   collectFeatures(); // ← 这行会计算+打印+BLE发送
        // }
        collectFeatures();
      }
      Serial.println("🔌 BLE disconnected");
    }
  }
  else
  {
    WiFiConfig config = wifi_storage.read();
    if (config.catId && config.registered && WiFi.status() == WL_CONNECTED)
    {
      collectFeatures();
    }

    if (millis() - lastWiFiCheck >= WIFI_CHECK_INTERVAL)
    {
      lastWiFiCheck = millis();

      if (WiFi.status() != WL_CONNECTED)
      {
        // 不在线 ⇒ 若当前不是 AP，则切回 AP（避免重复 beginAP）
        if (netMode != MODE_AP)
        {
          Serial.println("⚠️ WiFi disconnected → switch to AP");

          WiFi.end();
          delay(1000);

          if (WiFi.beginAP(ap_ssid, ap_pass) != WL_AP_LISTENING)
          {
            Serial.println("Failed to start AP");
            while (true)
              ;
          }

          Serial.print("AP started. Connect to: ");
          Serial.println(WiFi.localIP());

          netMode = MODE_AP;
          server.begin();
        }
      }
      else
      {
        // 已在线 ⇒ 标记为 STA（避免 AP/STA 反复切换）
        if (netMode != MODE_STA)
        {
          netMode = MODE_STA;
          Serial.println("✅ WiFi back online (STA)");
        }
      }
    }
  }
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
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
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
    syncTimeWithNTP(); // 同步 NTP 时间
    netMode = MODE_STA;
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

    netMode = MODE_STA;

    saveWiFiConfig(targetSSID, targetPASS, targetCatId);
    syncTimeWithNTP();   // 同步 NTP 时间
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
      netMode = MODE_AP;

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

const int SAMPLE_RATE = 30;
const int SAMPLE_INTERVAL = 1000 / SAMPLE_RATE;

float xSamples[SAMPLE_RATE];
float ySamples[SAMPLE_RATE];
float zSamples[SAMPLE_RATE];

void collectFeatures()
{
  // 1. 收集 1 秒 30 个点
  for (int i = 0; i < SAMPLE_RATE; i++)
  {
    while (!IMU.accelerationAvailable())
      ;
    IMU.readAcceleration(xSamples[i], ySamples[i], zSamples[i]);
    delay(SAMPLE_INTERVAL);
  }

  // Z轴取反, X轴取反
  for (int i = 0; i < SAMPLE_RATE; i++)
  {
    zSamples[i] = -zSamples[i];
    xSamples[i] = -xSamples[i];
  }

  // --- X 轴 ---
  float xMean = mean(xSamples, SAMPLE_RATE);
  float xMin = minValue(xSamples, SAMPLE_RATE);
  float xMax = maxValue(xSamples, SAMPLE_RATE);
  float xSum = sum(xSamples, SAMPLE_RATE);

  // --- Y 轴 ---
  float yMean = mean(ySamples, SAMPLE_RATE);
  float yMin = minValue(ySamples, SAMPLE_RATE);
  float yMax = maxValue(ySamples, SAMPLE_RATE);
  float ySum = sum(ySamples, SAMPLE_RATE);

  // --- Z 轴 ---
  float zMean = mean(zSamples, SAMPLE_RATE);
  float zMin = minValue(zSamples, SAMPLE_RATE);
  float zMax = maxValue(zSamples, SAMPLE_RATE);
  float zSum = sum(zSamples, SAMPLE_RATE);

  // 2. 直接打印一行
  static char line[512];
  int len = snprintf(line, sizeof(line),
                     "%.6f,%.6f,%.6f,%.6f,"   // X: mean,min,max,sum,sd,skew,kurt
                     "%.6f,%.6f,%.6f,%.6f,"   // Y
                     "%.6f,%.6f,%.6f,%.6f\n", // Z
                     xMean, xMin, xMax, xSum,
                     yMean, yMin, yMax, ySum,
                     zMean, zMin, zMax, zSum);

  // 串口打印（保持你原有行为）
  Serial.print(line);

  // 通过 BLE 通知发出去（中心设备需先订阅）
  // 注意：BLE 与 WiFiNINA 无法同时工作；此版本用于 BLE 模式。
  if (len > 0)
  {
    if (ENABLE_BLE_STREAM == 1)
    {
      bleSendLongLine(line);
    }
    else
    {
      float feats[12];
      int k = 0;
      feats[k++] = xMean;
      feats[k++] = xMin;
      feats[k++] = xMax;
      feats[k++] = xSum;
      feats[k++] = yMean;
      feats[k++] = yMin;
      feats[k++] = yMax;
      feats[k++] = ySum;
      feats[k++] = zMean;
      feats[k++] = zMin;
      feats[k++] = zMax;
      feats[k++] = zSum;

      char ts[30];
      sprintf(ts, "%04d-%02d-%02dT%02d:%02d:%02dZ", year(), month(), day(), hour(), minute(), second());
      postBehaviours(ts, feats, k);
    }
  }
}

const char *UDP_HOST = "100.65.24.237"; // ← 改成你电脑的 局域网IP（别用 Render 域名）
const int UDP_PORT = 41234;            // 后端监听的端口

// setup() 里加：固定本地端口，便于将来收 ACK
// udp.begin(9000);

void postBehaviours(const char *timestampISO, const float *feats, size_t n)
{
  WiFiConfig cfg = wifi_storage.read(); // 读取已保存的 catId
  String catId = String(cfg.catId);

  // 1) 组 JSON：带上 catId
  String body;
  body.reserve(512);
  body += "{\"catId\":\"";
  body += catId;
  body += "\",\"timestamp\":\"";
  body += timestampISO;
  body += "\",\"rawData\":[";
  for (size_t i = 0; i < n; ++i)
  {
    if (i)
      body += ",";
    body += String(feats[i], 6);
  }
  body += "]}";

  // 2) 解析目标 IP（也可以直接写 IPAddress dst(192,168,1,50);）
  IPAddress dst;
  if (WiFi.hostByName(UDP_HOST, dst) != 1)
  {
    Serial.println("❌ UDP: DNS failed");
    return;
  }

  // 3) 发送（使用全局 udpTx；setup 里已经 begin 过，如果没 begin 就容错一次）
  static bool begun = false;
  if (!begun)
  {
    if (udp.begin(9000) == 0)
    {
      Serial.println("❌ UDP: begin failed");
      return;
    }
    begun = true;
  }

  if (!udp.beginPacket(dst, UDP_PORT))
  {
    Serial.println("❌ UDP: beginPacket failed");
    return;
  }
  udp.write((const uint8_t *)body.c_str(), body.length());
  if (!udp.endPacket())
  {
    Serial.println("❌ UDP: endPacket failed");
    return;
  }

  Serial.println("✅ UDP sent");
}

/** ======= 工具函数 ======= */
float sum(float *data, int n)
{
  float s = 0;
  for (int i = 0; i < n; i++)
    s += data[i];
  return s;
}

float mean(float *data, int n)
{
  return sum(data, n) / n;
}

float minValue(float *data, int n)
{
  float m = data[0];
  for (int i = 1; i < n; i++)
    if (data[i] < m)
      m = data[i];
  return m;
}

float maxValue(float *data, int n)
{
  float m = data[0];
  for (int i = 1; i < n; i++)
    if (data[i] > m)
      m = data[i];
  return m;
}
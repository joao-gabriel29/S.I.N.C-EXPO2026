/*
  S.I.N.C. - Smartwatch ESP32 (protótipo)
  Placa: ESP32-WROOM / ESP32 Dev Module

  Funções:
  - Leitura analógica do Pulse Sensor
  - Estimativa de BPM por detecção de cruzamento de limiar adaptativo
  - Exibição em OLED SSD1306 128x64 via I2C
  - Alarme local por buzzer ativo, acionado por transistor
  - Envio periódico de registros ao backend por HTTP POST (JSON)

  IMPORTANTE:
  1) O algoritmo é experimental e precisa ser calibrado com o sensor real.
  2) BPM_LIMIT é um limite de demonstração, não um limite médico universal.
  3) API_URL e o JSON precisam corresponder ao contrato real do backend S.I.N.C.
  4) Para HTTPS, o código usa setInsecure() SOMENTE para teste inicial. Antes de
     uso real, substitua por client.setCACert(CERTIFICADO_RAIZ_DO_SERVIDOR).
*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ============================================================
// 1. CONFIGURAÇÕES DO PROJETO - PREENCHER ANTES DE COMPILAR
// ============================================================

// Rede Wi-Fi 2.4 GHz
const char* WIFI_SSID     = "NOME_DA_REDE_WIFI";
const char* WIFI_PASSWORD = "SENHA_DA_REDE_WIFI";

// Endereço completo da rota que RECEBE as medições no backend.
const char* API_URL = "https://SEU_BACKEND/SEU_ENDPOINT";

const char* API_TOKEN = "token_de_autenticacao_do_backend";
const char* DEVICE_ID  = "SINC-ESP32-001";


const int PULSE_PIN  = 34;
const int BUZZER_PIN = 25;
const int OLED_SDA   = 21;
const int OLED_SCL   = 22;

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS  0x3C
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledAvailable = false;


const uint32_t SAMPLE_INTERVAL_US = 4000; 
const uint32_t DISPLAY_INTERVAL_MS = 200;
const uint32_t UPLOAD_INTERVAL_MS = 5000;
const uint32_t WIFI_RETRY_MS = 10000;

const int BPM_LIMIT = 120;                 
const uint32_t ALERT_CONFIRM_MS = 8000;    
const uint32_t BPM_STALE_MS = 2500;         

const float MIN_SIGNAL_AMPLITUDE = 35.0f;  
const uint32_t MIN_BEAT_INTERVAL_MS = 300; 
const uint32_t MAX_BEAT_INTERVAL_MS = 2000;
const int BPM_AVERAGE_SIZE = 5;

const long GMT_OFFSET_SEC = -3 * 60 * 60;
const int DAYLIGHT_OFFSET_SEC = 0;


portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;
volatile int sharedBpm = 0;
volatile bool sharedBpmValid = false;
volatile bool sharedAlarm = false;
volatile int sharedRawSignal = 0;


float filteredSignal = 0.0f;
float signalMin = 0.0f;
float signalMax = 0.0f;
bool signalInitialized = false;
bool wasAboveThreshold = false;

uint32_t lastBeatMs = 0;
uint32_t highBpmSinceMs = 0;
uint32_t lastDisplayMs = 0;
uint32_t lastSampleUs = 0;

int bpmHistory[BPM_AVERAGE_SIZE] = {0};
int bpmHistoryIndex = 0;
int bpmHistoryCount = 0;
int currentBpm = 0;
bool currentBpmValid = false;
bool alarmActive = false;
int currentRawSignal = 0;


void updateSharedData() {
  portENTER_CRITICAL(&dataMux);
  sharedBpm = currentBpm;
  sharedBpmValid = currentBpmValid;
  sharedAlarm = alarmActive;
  sharedRawSignal = currentRawSignal;
  portEXIT_CRITICAL(&dataMux);
}

void addBpmSample(int bpm) {
  bpmHistory[bpmHistoryIndex] = bpm;
  bpmHistoryIndex = (bpmHistoryIndex + 1) % BPM_AVERAGE_SIZE;
  if (bpmHistoryCount < BPM_AVERAGE_SIZE) bpmHistoryCount++;

  long sum = 0;
  for (int i = 0; i < bpmHistoryCount; i++) sum += bpmHistory[i];
  currentBpm = (int)(sum / bpmHistoryCount);
  currentBpmValid = true;
}

void processPulseSample() {
  const uint32_t nowMs = millis();
  const int raw = analogRead(PULSE_PIN);
  currentRawSignal = raw;

  if (!signalInitialized) {
    filteredSignal = (float)raw;
    signalMin = filteredSignal;
    signalMax = filteredSignal + 50.0f;
    signalInitialized = true;
  }

  filteredSignal += 0.25f * ((float)raw - filteredSignal);

  const float envelopeRate = 0.0025f;
  if (filteredSignal < signalMin) signalMin = filteredSignal;
  else signalMin += envelopeRate * (filteredSignal - signalMin);

  if (filteredSignal > signalMax) signalMax = filteredSignal;
  else signalMax += envelopeRate * (filteredSignal - signalMax);

  const float amplitude = signalMax - signalMin;
  const float threshold = signalMin + (0.58f * amplitude);
  const bool aboveThreshold = (filteredSignal > threshold);

  if (amplitude >= MIN_SIGNAL_AMPLITUDE && aboveThreshold && !wasAboveThreshold) {
    if (lastBeatMs == 0 || (nowMs - lastBeatMs) >= MIN_BEAT_INTERVAL_MS) {
      if (lastBeatMs != 0) {
        const uint32_t interval = nowMs - lastBeatMs;
        if (interval >= MIN_BEAT_INTERVAL_MS && interval <= MAX_BEAT_INTERVAL_MS) {
          const int instantBpm = (int)(60000UL / interval);
          if (instantBpm >= 30 && instantBpm <= 200) addBpmSample(instantBpm);
        }
      }
      lastBeatMs = nowMs;
    }
  }
  wasAboveThreshold = aboveThreshold;

  if (lastBeatMs == 0 || (nowMs - lastBeatMs) > BPM_STALE_MS) {
    currentBpmValid = false;
    currentBpm = 0;
    bpmHistoryCount = 0;
    bpmHistoryIndex = 0;
  }

  if (currentBpmValid && currentBpm > BPM_LIMIT) {
    if (highBpmSinceMs == 0) highBpmSinceMs = nowMs;
    if ((nowMs - highBpmSinceMs) >= ALERT_CONFIRM_MS) alarmActive = true;
  } else {
    highBpmSinceMs = 0;
    alarmActive = false;
  }

  updateSharedData();
}

void updateBuzzer() {
  if (!alarmActive) {
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  const bool buzzerOn = ((millis() / 250U) % 2U) == 0U;
  digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
}

void updateDisplay() {
  if (!oledAvailable) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("S.I.N.C. | MONITOR CARDIACO"));
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 17);
  display.setTextSize(1);
  display.print(F("FREQUENCIA CARDIACA"));

  display.setCursor(0, 27);
  display.setTextSize(3);
  if (currentBpmValid) display.print(currentBpm);
  else display.print(F("---"));

  display.setTextSize(1);
  display.setCursor(88, 39);
  display.print(F("BPM"));

  display.drawLine(0, 53, 127, 53, SSD1306_WHITE);
  display.setCursor(0, 56);
  if (alarmActive) {
    display.print(F("ALERTA: BPM ELEVADO"));
  } else if (!currentBpmValid) {
    display.print(F("Aguardando sinal do sensor"));
  } else {
    display.print(WiFi.status() == WL_CONNECTED ? F("Wi-Fi conectado") : F("Wi-Fi desconectado"));
  }

  display.display();
}

String getIsoTimestamp() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 50)) return String("");

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S-03:00", &timeInfo);
  return String(buffer);
}


void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println(F("[WiFi] Tentando conectar..."));
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 15000UL) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] Conectado. IP: "));
    Serial.println(WiFi.localIP());
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.google.com");
  } else {
    Serial.println(F("[WiFi] Nao foi possivel conectar. O monitor local continua funcionando."));
  }
}

String makeJsonPayload(int bpm, bool bpmValid, bool alarm, int rawSignal) {
  String timestamp = getIsoTimestamp();
  String payload;
  payload.reserve(260);

  payload += F("{\"device_id\":\"");
  payload += DEVICE_ID;
  payload += F("\",\"bpm\":");
  if (bpmValid) payload += String(bpm);
  else payload += F("null");
  payload += F(",\"bpm_valid\":");
  payload += bpmValid ? F("true") : F("false");
  payload += F(",\"signal_raw\":");
  payload += String(rawSignal);
  payload += F(",\"alarm_active\":");
  payload += alarm ? F("true") : F("false");
  payload += F(",\"uptime_ms\":");
  payload += String(millis());
  payload += F(",\"timestamp\":");
  if (timestamp.length() > 0) {
    payload += F("\"");
    payload += timestamp;
    payload += F("\"");
  } else {
    payload += F("null");
  }
  payload += F("}");
  return payload;
}

void addApiHeaders(HTTPClient& http) {
  http.addHeader("Content-Type", "application/json");
  if (strlen(API_TOKEN) > 0) {
    http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
  }
}

int postJson(const String& payload) {
  HTTPClient http;
  http.setTimeout(4000);

  int statusCode = -1;
  const bool isHttps = strncmp(API_URL, "https://", 8) == 0;

  if (isHttps) {
    WiFiClientSecure secureClient;
    
    secureClient.setInsecure();
    if (!http.begin(secureClient, API_URL)) return -1;
    addApiHeaders(http);
    statusCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    if (statusCode > 0) {
      Serial.printf("[API] HTTP %d\n", statusCode);
      Serial.println(http.getString());
    } else {
      Serial.printf("[API] Falha no POST: %s\n", http.errorToString(statusCode).c_str());
    }
    http.end();
  } else {
    WiFiClient plainClient;
    if (!http.begin(plainClient, API_URL)) return -1;
    addApiHeaders(http);
    statusCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    if (statusCode > 0) {
      Serial.printf("[API] HTTP %d\n", statusCode);
      Serial.println(http.getString());
    } else {
      Serial.printf("[API] Falha no POST: %s\n", http.errorToString(statusCode).c_str());
    }
    http.end();
  }

  return statusCode;
}

void cloudTask(void* parameter) {
  (void)parameter;
  for (;;) {
    ensureWiFiConnected();

    if (WiFi.status() == WL_CONNECTED && strcmp(API_URL, "https://SEU_BACKEND/SEU_ENDPOINT") != 0) {
      int bpmCopy;
      bool validCopy;
      bool alarmCopy;
      int signalCopy;

      portENTER_CRITICAL(&dataMux);
      bpmCopy = sharedBpm;
      validCopy = sharedBpmValid;
      alarmCopy = sharedAlarm;
      signalCopy = sharedRawSignal;
      portEXIT_CRITICAL(&dataMux);

      String payload = makeJsonPayload(bpmCopy, validCopy, alarmCopy, signalCopy);
      Serial.print(F("[API] Enviando: "));
      Serial.println(payload);
      postJson(payload);
    } else if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("[API] Configure API_URL antes de enviar dados."));
    }

    vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
  }
}


void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12); 
  analogSetPinAttenuation(PULSE_PIN, ADC_11db);

  Wire.begin(OLED_SDA, OLED_SCL);
  oledAvailable = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (oledAvailable) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("S.I.N.C. SMARTWATCH"));
    display.println(F("Inicializando..."));
    display.display();
  } else {
    Serial.println(F("[OLED] Nao encontrado. Verifique SDA/SCL e endereco 0x3C/0x3D."));
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);


  xTaskCreatePinnedToCore(
    cloudTask,
    "CloudTask",
    8192,
    nullptr,
    1,
    nullptr,
    0
  );

  Serial.println(F("S.I.N.C. iniciado. Sensor sendo amostrado a 250 Hz."));
}

void loop() {
  const uint32_t nowUs = micros();
  if ((uint32_t)(nowUs - lastSampleUs) >= SAMPLE_INTERVAL_US) {
    lastSampleUs = nowUs;
    processPulseSample();
  }

  updateBuzzer();

  if ((millis() - lastDisplayMs) >= DISPLAY_INTERVAL_MS) {
    lastDisplayMs = millis();
    updateDisplay();
  }
}

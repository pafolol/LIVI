#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "ESP_I2S.h"
#include "FS.h"
#include "SD.h"
#include "esp_camera.h"
#include "secrets.h"

// =====================
// Backend paths
// =====================
const char *PATH_TRANSCRIPT = "/livi/transcript";
const char *PATH_AUDIO = "/livi/audio";
const char *PATH_COMMANDSNEXT = "/livi/commands/next";
const char *PATH_IMAGE = "/livi/image";

const char *QUICK_DESCRIBE_PROMPT =
    "Describe rapidamente y de forma breve lo que ves en esta imagen. Responde en espanol.";

const char *TRANSCRIBE_MODEL = "whisper-1";
const char *RESPONSE_FORMAT = "text";
const char *AUDIO_LANGUAGE = "es";

// =====================
// Hardware
// =====================
constexpr int PDM_CLK_PIN = 42;
constexpr int PDM_DATA_PIN = 41;
constexpr int BUTTON_PIN = 3;

constexpr uint32_t SAMPLE_RATE_HZ = 16000;
constexpr uint32_t RECORD_SECONDS = 5;

constexpr int SD_CS_PIN = 21;
const char *REC_PATH = "/arduinor_rec.wav";

I2SClass i2s;

// =====================
// WiFi
// =====================
void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.printf("Connecting to %s", WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected.");
    Serial.println(WiFi.localIP());
}

// =====================
// Audio Record
// =====================
bool record_and_save()
{
    size_t wav_size = 0;
    uint8_t *wav_buffer = i2s.recordWAV(RECORD_SECONDS, &wav_size);

    if (!wav_buffer || wav_size == 0)
        return false;

    if (SD.exists(REC_PATH))
        SD.remove(REC_PATH);

    File file = SD.open(REC_PATH, FILE_WRITE);
    if (!file)
        return false;

    file.write(wav_buffer, wav_size);
    file.close();
    free(wav_buffer);

    return true;
}

// =====================
// HTTPS helper
// =====================
String httpRequest(const String &req, int &status)
{
    status = -1;

    WiFiClientSecure client;
    client.setInsecure();

    if (!client.connect(BACKEND_HOST, BACKEND_PORT))
        return "";

    client.print(req);

    String statusLine = client.readStringUntil('\n');
    int s1 = statusLine.indexOf(' ');
    int s2 = statusLine.indexOf(' ', s1 + 1);
    if (s1 > 0 && s2 > s1)
        status = statusLine.substring(s1 + 1, s2).toInt();

    while (true)
    {
        String h = client.readStringUntil('\n');
        if (h == "\r" || h.length() == 0)
            break;
    }

    String body;
    while (client.available())
        body += (char)client.read();

    return body;
}

// =====================
// Backend: transcript
// =====================
bool backendPostTranscript(const String &transcript)
{
    String payload = String("{\"device_id\":\"") + DEVICE_ID + "\",\"transcript\":\"" + transcript + "\"}";

    String req;
    req += String("POST ") + PATH_TRANSCRIPT + " HTTP/1.1\r\n";
    req += String("Host: ") + BACKEND_HOST + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += String("Content-Length: ") + payload.length() + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += payload;

    int status;
    String body = httpRequest(req, status);

    Serial.println("Response:");
    Serial.println(body);

    return status == 200;
}

// =====================
// Setup
// =====================
void setup()
{
    Serial.begin(115200);
    delay(1000);

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    if (!SD.begin(SD_CS_PIN))
    {
        Serial.println("SD failed.");
        while (1);
    }

    i2s.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);
    if (!i2s.begin(I2S_MODE_PDM_RX, SAMPLE_RATE_HZ,
                   I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_MONO))
    {
        Serial.println("I2S failed.");
        while (1);
    }

    connectWiFi();
    Serial.println("Ready.");
}

// =====================
// Loop
// =====================
void loop()
{
    if (digitalRead(BUTTON_PIN) == LOW)
    {
        delay(300);

        Serial.println("Recording...");
        if (record_and_save())
        {
            Serial.println("Uploading...");
            backendPostTranscript("Audio sent");
        }
        else
        {
            Serial.println("Record failed.");
        }

        while (digitalRead(BUTTON_PIN) == LOW)
            delay(10);
    }
}

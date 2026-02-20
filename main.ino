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

// =====================
// OpenAI settings (si decides usar transcripción directa desde ESP)
// =====================
const char *TRANSCRIBE_MODEL = "whisper-1";
const char *RESPONSE_FORMAT = "text";
const char *AUDIO_LANGUAGE = "es";

// =====================
// Hardware pins / settings
// =====================
constexpr int PDM_CLK_PIN = 42;
constexpr int PDM_DATA_PIN = 41;
constexpr int BUTTON_PIN = 3;

constexpr uint32_t SAMPLE_RATE_HZ = 16000;
constexpr uint32_t RECORD_SECONDS = 5;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr uint32_t BUTTON_DOUBLE_CLICK_MS = 350;

constexpr int SD_CS_PIN = 21;
const char *REC_PATH = "/arduinor_rec.wav";

// =====================
// Globals
// =====================
I2SClass i2s;

enum ButtonCommand
{
    BUTTON_CMD_NONE = 0,
    BUTTON_CMD_SINGLE_CLICK = 1,
    BUTTON_CMD_DOUBLE_CLICK = 2,
};

static int g_buttonRawState = HIGH;
static int g_buttonStableState = HIGH;
static unsigned long g_lastDebounceAt = 0;
static uint8_t g_clickCount = 0;
static unsigned long g_firstClickAt = 0;

// ====== XIAO ESP32S3 Sense - Camera pins (Sense expansion) ======
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1

#define XCLK_GPIO_NUM 10
#define SIOD_GPIO_NUM 40 // CAM_SDA
#define SIOC_GPIO_NUM 39 // CAM_SCL

#define Y9_GPIO_NUM 48
#define Y8_GPIO_NUM 11
#define Y7_GPIO_NUM 12
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 16
#define Y4_GPIO_NUM 18
#define Y3_GPIO_NUM 17
#define Y2_GPIO_NUM 15

#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM 47
#define PCLK_GPIO_NUM 13

// =====================
// Camera helpers
// =====================
static bool cameraInitXiaoSense(framesize_t frameSize = FRAMESIZE_VGA, int jpegQuality = 12, int fbCount = 2)
{
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;

    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;

    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    config.frame_size = frameSize;
    config.jpeg_quality = jpegQuality; // 0-63 (más alto = más compresión)
    config.fb_count = fbCount;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.printf("ERROR: esp_camera_init failed: 0x%X\n", err);
        return false;
    }
    return true;
}

static camera_fb_t *cameraCaptureJpeg() { return esp_camera_fb_get(); }
static void cameraRelease(camera_fb_t *fb)
{
    if (fb)
        esp_camera_fb_return(fb);
}

// ====== Photo save settings ======
const char *PHOTO_DIR = "/photos";
static uint32_t photoCounter = 0;

// =====================
// WiFi
// =====================
void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.printf("Connecting to WiFi SSID '%s'", WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 30000)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("WiFi connected. IP: ");
        Serial.println(WiFi.localIP());

        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    }
    else
    {
        Serial.println("WiFi FAILED. Check SSID/PASS or signal.");
    }
}

// =====================
// WAV record to SD
// =====================
bool record_and_save()
{
    Serial.printf("Recording %u seconds of audio data...\n", (unsigned)RECORD_SECONDS);

    size_t wav_size = 0;
    uint8_t *wav_buffer = i2s.recordWAV(RECORD_SECONDS, &wav_size);

    if (!wav_buffer || wav_size == 0)
    {
        Serial.println("ERROR: recordWAV returned no data.");
        return false;
    }

    if (SD.exists(REC_PATH))
        SD.remove(REC_PATH);

    File file = SD.open(REC_PATH, FILE_WRITE);
    if (!file)
    {
        Serial.println("ERROR: Failed to open file for writing on SD.");
        free(wav_buffer);
        return false;
    }

    Serial.printf("Writing %u bytes to %s...\n", (unsigned)wav_size, REC_PATH);
    size_t written = file.write(wav_buffer, wav_size);
    file.flush();
    file.close();

    free(wav_buffer);

    if (written != wav_size)
    {
        Serial.println("ERROR: Failed to write full WAV data to SD.");
        return false;
    }

    Serial.println("Saved WAV to SD.");
    return true;
}

static void ensurePhotoDir()
{
    if (!SD.exists(PHOTO_DIR))
    {
        SD.mkdir(PHOTO_DIR);
    }
}

// Genera nombre: /photos/000123_2141.jpg (contador + bytes)
static String makePhotoPath(size_t jpgLen)
{
    char name[64];
    snprintf(name, sizeof(name), "%s/%06lu_%u.jpg", PHOTO_DIR, (unsigned long)photoCounter++, (unsigned)jpgLen);
    return String(name);
}

// Guarda el JPEG del fb en SD
static bool saveJpegToSD(const uint8_t *jpg, size_t jpgLen, const String &path)
{
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f)
    {
        Serial.println("ERROR: No pude abrir archivo para escribir JPG en SD.");
        return false;
    }

    size_t written = f.write(jpg, jpgLen);
    f.flush();
    f.close();

    if (written != jpgLen)
    {
        Serial.printf("ERROR: JPG write incompleto: %u/%u\n", (unsigned)written, (unsigned)jpgLen);
        return false;
    }
    return true;
}

// =====================
// Basic HTTP read (until close)
// =====================
static String readBodyToClose(WiFiClientSecure &client)
{
    String body;
    body.reserve(4096);
    while (client.connected() || client.available())
    {
        if (client.available())
            body += (char)client.read();
        else
            delay(1);
    }
    return body;
}

static String httpRequest(
    const char *host, uint16_t port,
    const String &req,
    int &outStatus)
{
    outStatus = -1;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(35000);
    client.setNoDelay(true);

    if (!client.connect(host, port))
        return "";

    client.print(req);

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();

    // parse HTTP code
    int s1 = statusLine.indexOf(' ');
    int s2 = statusLine.indexOf(' ', s1 + 1);
    if (s1 > 0 && s2 > s1)
        outStatus = statusLine.substring(s1 + 1, s2).toInt();

    // consume headers
    while (true)
    {
        String h = client.readStringUntil('\n');
        if (h.length() == 0 || h == "\r")
            break;
    }

    return readBodyToClose(client);
}

// =====================
// Tiny JSON helpers (naive but works for small responses)
// =====================
static bool jsonGetBool(const String &json, const char *key, bool defaultVal = false)
{
    String k = String("\"") + key + "\"";
    int i = json.indexOf(k);
    if (i < 0)
        return defaultVal;
    i = json.indexOf(':', i);
    if (i < 0)
        return defaultVal;
    i++;
    while (i < (int)json.length() && isspace((unsigned char)json[i]))
        i++;
    if (i >= (int)json.length())
        return defaultVal;
    if (json.startsWith("true", i))
        return true;
    if (json.startsWith("false", i))
        return false;
    return defaultVal;
}

static String jsonGetString(const String &json, const char *key, const String &def = "")
{
    String k = String("\"") + key + "\"";
    int i = json.indexOf(k);
    if (i < 0)
        return def;
    i = json.indexOf(':', i);
    if (i < 0)
        return def;
    i++;
    while (i < (int)json.length() && isspace((unsigned char)json[i]))
        i++;
    if (i >= (int)json.length() || json[i] != '"')
        return def;
    i++;
    int j = i;
    bool esc = false;
    String out;
    while (j < (int)json.length())
    {
        char c = json[j++];
        if (esc)
        {
            out += c;
            esc = false;
            continue;
        }
        if (c == '\\')
        {
            esc = true;
            continue;
        }
        if (c == '"')
            break;
        out += c;
    }
    return out;
}

// =====================
// 1) OpenAI transcribe (como lo tenías) - OPCIONAL
// =====================
String readHttpBody(WiFiClientSecure &client, bool chunked, int contentLength)
{
    String body;
    body.reserve(4096);

    if (chunked)
    {
        while (true)
        {
            String sizeLine = client.readStringUntil('\n');
            sizeLine.trim();
            if (sizeLine.length() == 0)
                continue;

            int chunkSize = (int)strtol(sizeLine.c_str(), nullptr, 16);
            if (chunkSize <= 0)
            {
                client.readStringUntil('\n');
                break;
            }

            for (int i = 0; i < chunkSize; i++)
            {
                int c;
                while ((c = client.read()) < 0)
                {
                    if (!client.connected())
                        break;
                    delay(1);
                }
                if (c < 0)
                    break;
                body += (char)c;
            }
            client.read();
            client.read();
        }
        return body;
    }

    if (contentLength >= 0)
    {
        body.reserve(contentLength + 1);
        while ((int)body.length() < contentLength)
        {
            if (client.available())
                body += (char)client.read();
            else if (!client.connected())
                break;
            else
                delay(1);
        }
        return body;
    }

    while (client.connected() || client.available())
    {
        if (client.available())
            body += (char)client.read();
        else
            delay(1);
    }
    return body;
}

String extractTextFromJson(const String &json)
{
    int k = json.indexOf("\"text\"");
    if (k < 0)
        return json;
    int colon = json.indexOf(':', k);
    if (colon < 0)
        return json;
    int i = colon + 1;
    while (i < (int)json.length() && isspace((unsigned char)json[i]))
        i++;
    if (i >= (int)json.length() || json[i] != '"')
        return json;
    i++;

    String out;
    bool esc = false;
    for (; i < (int)json.length(); i++)
    {
        char c = json[i];
        if (esc)
        {
            out += c;
            esc = false;
        }
        else
        {
            if (c == '\\')
                esc = true;
            else if (c == '"')
                break;
            else
                out += c;
        }
    }
    return out;
}

bool openai_transcribe_file(const char *wavPath, String &outText)
{
    outText = "";

    // Si no hay key, no intentes
    if (!OPENAI_API_KEY || strlen(OPENAI_API_KEY) < 10)
    {
        Serial.println("OpenAI key missing. Skipping openai_transcribe_file.");
        return false;
    }

    File audio = SD.open(wavPath, FILE_READ);
    if (!audio)
    {
        Serial.println("ERROR: Could not open WAV file for reading.");
        return false;
    }

    size_t fileSize = audio.size();
    Serial.printf("Uploading %u bytes to OpenAI...\n", (unsigned)fileSize);

    const String boundary = "----esp32Boundary7MA4YWxkTrZu0gW";

    String partModel =
        "--" + boundary + "\r\n"
                          "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
        String(TRANSCRIBE_MODEL) + "\r\n";

    String partResp =
        "--" + boundary + "\r\n"
                          "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n" +
        String(RESPONSE_FORMAT) + "\r\n";

    String partLang = "";
    if (AUDIO_LANGUAGE && strlen(AUDIO_LANGUAGE) > 0)
    {
        partLang =
            "--" + boundary + "\r\n"
                              "Content-Disposition: form-data; name=\"language\"\r\n\r\n" +
            String(AUDIO_LANGUAGE) + "\r\n";
    }

    String partFileHeader =
        "--" + boundary + "\r\n"
                          "Content-Disposition: form-data; name=\"file\"; filename=\"arduinor_rec.wav\"\r\n"
                          "Content-Type: audio/wav\r\n\r\n";

    String partEnd =
        "\r\n--" + boundary + "--\r\n";

    size_t contentLength =
        partModel.length() + partResp.length() + partLang.length() +
        partFileHeader.length() + fileSize + partEnd.length();

    WiFiClientSecure client;
    client.setTimeout(120000);
    client.setInsecure();
    client.setNoDelay(true);

    if (!client.connect(OPENAI_HOST, OPENAI_PORT))
    {
        Serial.println("ERROR: TLS connect to api.openai.com failed.");
        audio.close();
        return false;
    }

    client.printf("POST %s HTTP/1.1\r\n", OPENAI_PATH);
    client.printf("Host: %s\r\n", OPENAI_HOST);
    client.printf("Authorization: Bearer %s\r\n", OPENAI_API_KEY);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
    client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
    client.printf("Connection: close\r\n\r\n");

    client.print(partModel);
    client.print(partResp);
    if (partLang.length() > 0)
        client.print(partLang);
    client.print(partFileHeader);

    static uint8_t buf[8192];
    while (audio.available())
    {
        int n = audio.read(buf, sizeof(buf));
        if (n > 0)
        {
            int off = 0;
            while (off < n)
            {
                int w = client.write(buf + off, n - off);
                if (w <= 0)
                    break;
                off += w;
            }
            delay(0);
        }
    }
    audio.close();

    client.print(partEnd);

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    Serial.println(statusLine);

    int statusCode = -1;
    int s1 = statusLine.indexOf(' ');
    int s2 = statusLine.indexOf(' ', s1 + 1);
    if (s1 > 0 && s2 > s1)
        statusCode = statusLine.substring(s1 + 1, s2).toInt();

    bool chunked = false;
    int contentLenResp = -1;
    while (true)
    {
        String h = client.readStringUntil('\n');
        if (h.length() == 0 || h == "\r")
            break;
        String ht = h;
        ht.trim();
        String lower = ht;
        lower.toLowerCase();
        if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0)
            chunked = true;
        else if (lower.startsWith("content-length:"))
            contentLenResp = lower.substring(strlen("content-length:")).toInt();
    }

    String body = readHttpBody(client, chunked, contentLenResp);
    if (statusCode != 200)
    {
        Serial.println("OpenAI error response:");
        Serial.println(body);
        return false;
    }

    body.trim();
    outText = body.startsWith("{") ? extractTextFromJson(body) : body;
    return true;
}

// =====================
// 2) Backend: POST /transcript
// =====================
bool backendPostTranscript(const String &transcript, bool &needImage, String &requestId, String &assistantResponse, String &imageRequest)
{
    needImage = false;
    requestId = "";
    assistantResponse = "";
    imageRequest = "";

    // NOTE: no escapamos comillas dentro de transcript (igual que tu original).
    // Si quieres, te lo hago safe con escape JSON.
    String payload = String("{\"device_id\":\"") + DEVICE_ID + "\",\"transcript\":" + "\"" + transcript + "\"}";

    String req;
    req += String("POST ") + PATH_TRANSCRIPT + " HTTP/1.1\r\n";
    req += String("Host: ") + BACKEND_HOST + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += String("Content-Length: ") + payload.length() + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += payload;

    int status = -1;
    String body = httpRequest(BACKEND_HOST, BACKEND_PORT, req, status);
    if (status != 200)
    {
        Serial.printf("backend /transcript HTTP %d\n", status);
        Serial.println(body);
        return false;
    }

    needImage = jsonGetBool(body, "need_image", false);
    requestId = jsonGetString(body, "request_id", "");
    assistantResponse = jsonGetString(body, "assistant_response", "");
    imageRequest = jsonGetString(body, "image_request", "");
    return true;
}

bool backendPostAudio(const char *wavPath, bool &needImage, String &requestId, String &imageRequest)
{
    needImage = false;
    requestId = "";
    imageRequest = "";

    File audio = SD.open(wavPath, FILE_READ);
    if (!audio)
    {
        Serial.println("ERROR: Could not open WAV file for backend upload.");
        return false;
    }

    size_t fileSize = audio.size();
    Serial.printf("Uploading %u bytes of audio to backend...\n", (unsigned)fileSize);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(120000);
    client.setNoDelay(true);

    if (!client.connect(BACKEND_HOST, BACKEND_PORT))
    {
        Serial.println("ERROR: connect backend failed");
        audio.close();
        return false;
    }

    String path = String(PATH_AUDIO) + "?device_id=" + DEVICE_ID;
    client.printf("POST %s HTTP/1.1\r\n", path.c_str());
    client.printf("Host: %s\r\n", BACKEND_HOST);
    client.print("Content-Type: audio/wav\r\n");
    client.printf("Content-Length: %u\r\n", (unsigned)fileSize);
    client.print("Connection: close\r\n\r\n");

    static uint8_t buf[8192];
    while (audio.available())
    {
        int n = audio.read(buf, sizeof(buf));
        if (n <= 0)
            break;

        int off = 0;
        while (off < n)
        {
            int w = client.write(buf + off, n - off);
            if (w <= 0)
                break;
            off += w;
        }
        delay(0);
    }
    audio.close();

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();

    int status = -1;
    int s1 = statusLine.indexOf(' ');
    int s2 = statusLine.indexOf(' ', s1 + 1);
    if (s1 > 0 && s2 > s1)
        status = statusLine.substring(s1 + 1, s2).toInt();

    while (true)
    {
        String h = client.readStringUntil('\n');
        if (h.length() == 0 || h == "\r")
            break;
    }

    String body = readBodyToClose(client);
    if (status != 200)
    {
        Serial.printf("backend /audio HTTP %d\n", status);
        Serial.println(body);
        return false;
    }

    needImage = jsonGetBool(body, "need_image", false);
    requestId = jsonGetString(body, "request_id", "");
    imageRequest = jsonGetString(body, "image_request", "");

    String transcript = jsonGetString(body, "transcript", "");
    if (transcript.length() > 0)
    {
        Serial.println("\n=== TRANSCRIPT (SERVER) ===");
        Serial.println(transcript);
        Serial.println("===========================");
    }

    return true;
}

// =====================
// 3) Polling: GET /commands/next?device_id=...
// =====================
String pollUntil200CommandsNext(uint32_t intervalMs = 500)
{
    for (;;)
    {
        String path = String(PATH_COMMANDSNEXT) + "?device_id=" + DEVICE_ID;

        String req;
        req += String("GET ") + path + " HTTP/1.1\r\n";
        req += String("Host: ") + BACKEND_HOST + "\r\n";
        req += "Connection: close\r\n\r\n";

        int status = -1;
        String body = httpRequest(BACKEND_HOST, BACKEND_PORT, req, status);

        if (status == 200)
            return body;
        delay(intervalMs);
    }
}

// =====================
// 4) POST /image (JPEG crudo)
// =====================
bool backendPostImageRawJpeg(const String &requestId, const uint8_t *jpg, size_t jpgLen, const char *detail, String &outResponse)
{
    outResponse = "";

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30000);
    client.setNoDelay(true);

    if (!client.connect(BACKEND_HOST, BACKEND_PORT))
    {
        Serial.println("ERROR: connect backend failed");
        return false;
    }

    String path = String(PATH_IMAGE) + "?device_id=" + DEVICE_ID + "&request_id=" + requestId + "&detail=" + detail;

    client.printf("POST %s HTTP/1.1\r\n", path.c_str());
    client.printf("Host: %s\r\n", BACKEND_HOST);
    client.printf("Content-Type: image/jpeg\r\n");
    client.printf("Content-Length: %u\r\n", (unsigned)jpgLen);
    client.print("Connection: close\r\n\r\n");

    size_t sent = 0;
    while (sent < jpgLen)
    {
        size_t chunk = 8192;
        if (chunk > (jpgLen - sent))
            chunk = jpgLen - sent;
        int w = client.write(jpg + sent, chunk);
        if (w <= 0)
            break;
        sent += (size_t)w;
        delay(0);
    }

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();

    int status = -1;
    int s1 = statusLine.indexOf(' ');
    int s2 = statusLine.indexOf(' ', s1 + 1);
    if (s1 > 0 && s2 > s1)
        status = statusLine.substring(s1 + 1, s2).toInt();

    while (true)
    {
        String h = client.readStringUntil('\n');
        if (h.length() == 0 || h == "\r")
            break;
    }

    String body = readBodyToClose(client);

    if (status != 200)
    {
        Serial.printf("backend /image HTTP %d\n", status);
        Serial.println(body);
        return false;
    }

    outResponse = jsonGetString(body, "response", body);
    return true;
}

bool completeImageFlow(String requestId, const String &imageRequest, const char *detail, bool pollCommands)
{
    Serial.println("\nBackend requested image:");
    Serial.println(imageRequest);
    Serial.printf("request_id=%s\n", requestId.c_str());

    if (pollCommands)
    {
        Serial.println("Polling /commands/next until HTTP 200...");
        String cmdJson = pollUntil200CommandsNext(500);
        String rid2 = jsonGetString(cmdJson, "request_id", requestId);
        if (rid2.length() > 0)
            requestId = rid2;

        Serial.println("Command received:");
        Serial.println(cmdJson);
    }
    else
    {
        Serial.println("Quick mode: skipping /commands/next polling.");
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s)
    {
        s->set_framesize(s, FRAMESIZE_VGA);
        s->set_quality(s, 12);
    }

    for (int i = 0; i < 2; i++)
    {
        camera_fb_t *tmp = cameraCaptureJpeg();
        if (tmp)
            cameraRelease(tmp);
        delay(50);
    }

    camera_fb_t *fb = cameraCaptureJpeg();
    if (!fb)
    {
        Serial.println("Capture failed (fb null).");
        return false;
    }

    Serial.printf("Captured JPEG: %u bytes\n", (unsigned)fb->len);
    String photoPath = makePhotoPath(fb->len);
    if (saveJpegToSD(fb->buf, fb->len, photoPath))
    {
        Serial.print("Saved photo to SD: ");
        Serial.println(photoPath);
    }
    else
    {
        Serial.println("WARNING: Failed saving photo to SD.");
    }

    String visionAnswer;
    bool ok = backendPostImageRawJpeg(requestId, fb->buf, fb->len, detail, visionAnswer);
    cameraRelease(fb);

    if (!ok)
    {
        Serial.println("Upload image failed.");
        return false;
    }

    Serial.println("\n=== VISION RESPONSE ===");
    Serial.println(visionAnswer);
    Serial.println("=======================");
    return true;
}

bool runVisionFlow(const String &promptText, const char *detail = "low", bool pollCommands = true)
{
    bool needImage = false;
    String requestId, assistantResponse, imageRequest;

    if (!backendPostTranscript(promptText, needImage, requestId, assistantResponse, imageRequest))
    {
        Serial.println("Backend /transcript failed.");
        return false;
    }

    if (!needImage)
    {
        Serial.println("\n=== BACKEND RESPONSE (NO IMAGE) ===");
        Serial.println(assistantResponse);
        Serial.println("===================================");
        return true;
    }

    return completeImageFlow(requestId, imageRequest, detail, pollCommands);
}

static void resetButtonState()
{
    g_buttonRawState = digitalRead(BUTTON_PIN);
    g_buttonStableState = g_buttonRawState;
    g_lastDebounceAt = millis();
    g_clickCount = 0;
    g_firstClickAt = 0;
}

static ButtonCommand pollButtonCommand()
{
    unsigned long now = millis();
    int raw = digitalRead(BUTTON_PIN);

    if (raw != g_buttonRawState)
    {
        g_buttonRawState = raw;
        g_lastDebounceAt = now;
    }

    if ((now - g_lastDebounceAt) >= BUTTON_DEBOUNCE_MS && raw != g_buttonStableState)
    {
        g_buttonStableState = raw;
        if (g_buttonStableState == LOW)
        {
            if (g_clickCount == 0)
            {
                g_clickCount = 1;
                g_firstClickAt = now;
            }
            else if (g_clickCount == 1 && (now - g_firstClickAt) <= BUTTON_DOUBLE_CLICK_MS)
            {
                g_clickCount = 0;
                return BUTTON_CMD_DOUBLE_CLICK;
            }
            else
            {
                g_clickCount = 1;
                g_firstClickAt = now;
            }
        }
    }

    if (g_clickCount == 1 && (now - g_firstClickAt) > BUTTON_DOUBLE_CLICK_MS)
    {
        g_clickCount = 0;
        return BUTTON_CMD_SINGLE_CLICK;
    }

    return BUTTON_CMD_NONE;
}

// =====================
// MAIN
// =====================
void setup()
{
    Serial.begin(115200);
    delay(200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    resetButtonState();

    Serial.println("Initializing I2S bus...");
    i2s.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);
    if (!i2s.begin(I2S_MODE_PDM_RX, SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO))
    {
        Serial.println("Failed to initialize I2S!");
        while (1)
            delay(100);
    }
    Serial.println("I2S initialized.");

    Serial.println("Initializing SD card...");
    if (!SD.begin(SD_CS_PIN))
    {
        Serial.println("Failed to mount SD Card!");
        while (1)
            delay(100);
    }
    Serial.println("SD initialized.");
    ensurePhotoDir();

    connectWiFi();

    Serial.println("Initializing Camera...");
    if (!psramFound())
        Serial.println("WARNING: PSRAM not found (lower resolution if needed)");
    if (!cameraInitXiaoSense(FRAMESIZE_VGA, 12, 2))
    {
        Serial.println("Camera init failed (check ribbon/pins/PSRAM).");
    }
    else
    {
        Serial.println("Camera OK.");
    }

    Serial.println();
    Serial.printf("Ready. Button on GPIO %d:\n", BUTTON_PIN);
    Serial.println("- 1 click: quick photo description");
    Serial.println("- 2 clicks: record audio + server flow");

    sensor_t *s = esp_camera_sensor_get();
    if (s)
    {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
        s->set_brightness(s, 1);
        s->set_contrast(s, 1);
        s->set_saturation(s, 0);
    }
}

void loop()
{
    ButtonCommand cmd = pollButtonCommand();
    if (cmd == BUTTON_CMD_NONE)
    {
        delay(10);
        return;
    }

    if (cmd == BUTTON_CMD_DOUBLE_CLICK)
    {
        Serial.println("\n--- START (DOUBLE CLICK) ---");

        bool ok = true;
        if (!record_and_save())
        {
            Serial.println("Record failed.");
            ok = false;
        }

        if (ok)
        {
            bool needImage = false;
            String requestId, imageRequest;
            if (!backendPostAudio(REC_PATH, needImage, requestId, imageRequest))
            {
                Serial.println("Backend /audio failed.");
                ok = false;
            }
            else if (!needImage)
            {
                Serial.println("Backend says no image needed.");
            }
            else if (!completeImageFlow(requestId, imageRequest, "low", false))
            {
                ok = false;
            }
        }

        if (ok)
            Serial.println("--- DONE ---\n");
        else
            Serial.println("--- FAILED ---\n");

        Serial.println("Ready. 1 click=quick, 2 clicks=record.");
        resetButtonState();
    }
    else if (cmd == BUTTON_CMD_SINGLE_CLICK)
    {
        Serial.println("\n--- QUICK PHOTO START (SINGLE CLICK) ---");
        Serial.println("Prompt:");
        Serial.println(QUICK_DESCRIBE_PROMPT);

        if (!runVisionFlow(String(QUICK_DESCRIBE_PROMPT), "low", false))
        {
            Serial.println("--- QUICK PHOTO FAILED ---\n");
        }
        else
        {
            Serial.println("--- QUICK PHOTO DONE ---\n");
        }

        Serial.println("Ready. 1 click=quick, 2 clicks=record.");
        resetButtonState();
    }

    delay(10);
}

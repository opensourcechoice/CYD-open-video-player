// ============================================================================
//  Open CYD Player — open-source MJPEG + MP3 video player for the
//  ESP32-2432S028R "Cheap Yellow Display" (2.8" resistive touch)
//
//  Media layout on FAT32 SD card:
//    /videos/movie.mjpeg   concatenated JPEG frames (320x240)
//    /videos/movie.mp3     matching audio track (same basename)
//    /videos/movie.idx     frame index (see tools/convert.py) — enables seeking
//
//  License: MIT
// ============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <JPEGDEC.h>
#include <XPT2046_Touchscreen.h>
#include "Audio.h"          // ESP32-audioI2S
#include "config.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
TFT_eSPI tft;
JPEGDEC jpeg;
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
Audio audio;

// ---- IDX file format (little-endian) ----
// char magic[4] = "CYD1"
// uint32 fps_milli      (fps * 1000, e.g. 30000 = 30.000 fps)
// uint32 frame_count
// uint32 offsets[frame_count + 1]   (last entry = file size)
struct VideoIndex {
  float    fps = 30.0f;
  uint32_t frames = 0;
  uint32_t *offsets = nullptr;   // frames+1 entries, heap (PSRAM-less boards: ~4B/frame)
  bool     valid = false;
};

enum class Screen { BROWSER, PLAYER };

struct PlayerState {
  Screen   screen        = Screen::BROWSER;
  String   basePath;                 // "/videos/movie" (no extension)
  File     vfile;
  VideoIndex idx;
  uint32_t frame         = 0;
  bool     playing       = false;
  bool     hasAudio      = false;
  uint32_t startMs       = 0;        // wall-clock anchor for frame pacing
  uint32_t pausedAtMs    = 0;
  bool     osdVisible    = true;
  uint32_t osdShownMs    = 0;
  uint8_t  volume        = DEFAULT_VOLUME;
  uint8_t  brightness    = DEFAULT_BRIGHT;
  bool     dirty         = true;     // browser needs redraw
  int      browserPage   = 0;
} st;

static uint8_t *frameBuf = nullptr;
static size_t   frameBufSize = 0;

std::vector<String> videoList;      // basenames without extension

// Audio runs on its own task/core so video decode never starves it
TaskHandle_t audioTaskHandle = nullptr;
SemaphoreHandle_t audioMutex;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static void setBrightness(uint8_t v) {
  st.brightness = v;
  ledcWrite(BL_CHANNEL, v);
}

static void showOsd() {
  st.osdVisible = true;
  st.osdShownMs = millis();
}

static String fmtTime(uint32_t sec) {
  char b[16];
  snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
  return String(b);
}

// ---------------------------------------------------------------------------
// JPEG draw callback -> push decoded MCU block straight to the display
// ---------------------------------------------------------------------------
static int jpegDraw(JPEGDRAW *p) {
  tft.pushImage(p->x, p->y, p->iWidth, p->iHeight, (uint16_t *)p->pPixels);
  return 1;
}

// ---------------------------------------------------------------------------
// Index loading
// ---------------------------------------------------------------------------
static bool loadIndex(const String &base, VideoIndex &ix) {
  ix.valid = false;
  if (ix.offsets) { free(ix.offsets); ix.offsets = nullptr; }

  File f = SD.open(base + ".idx", FILE_READ);
  if (!f) return false;

  char magic[4];
  if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "CYD1", 4) != 0) { f.close(); return false; }

  uint32_t fpsMilli = 0;
  f.read((uint8_t *)&fpsMilli, 4);
  f.read((uint8_t *)&ix.frames, 4);
  if (fpsMilli < 1000 || fpsMilli > 60000 || ix.frames == 0 || ix.frames > 2000000) { f.close(); return false; }

  ix.offsets = (uint32_t *)malloc((ix.frames + 1) * sizeof(uint32_t));
  if (!ix.offsets) { f.close(); return false; }

  size_t want = (ix.frames + 1) * sizeof(uint32_t);
  if (f.read((uint8_t *)ix.offsets, want) != (int)want) {
    free(ix.offsets); ix.offsets = nullptr; f.close(); return false;
  }
  f.close();
  ix.fps = fpsMilli / 1000.0f;
  ix.valid = true;
  return true;
}

// ---------------------------------------------------------------------------
// Audio task (core 0). audio.loop() must be called constantly while playing.
// ---------------------------------------------------------------------------
static void audioTask(void *) {
  for (;;) {
    if (xSemaphoreTake(audioMutex, portMAX_DELAY) == pdTRUE) {
      audio.loop();
      xSemaphoreGive(audioMutex);
    }
    vTaskDelay(1);
  }
}

static void audioCmd(std::function<void()> fn) {
  if (xSemaphoreTake(audioMutex, portMAX_DELAY) == pdTRUE) {
    fn();
    xSemaphoreGive(audioMutex);
  }
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------
static void syncClockToFrame() {
  // Re-anchor wall clock so `frame` is "now"
  st.startMs = millis() - (uint32_t)(st.frame * 1000.0f / st.idx.fps);
}

static void seekToFrame(uint32_t frame) {
  if (!st.idx.valid) return;
  if (frame >= st.idx.frames) frame = st.idx.frames - 1;
  st.frame = frame;
  st.vfile.seek(st.idx.offsets[frame]);
  syncClockToFrame();
  if (st.hasAudio) {
    uint32_t sec = (uint32_t)(frame / st.idx.fps);
    audioCmd([sec] { audio.setAudioPlayPosition(sec); });
  }
}

static void setPlaying(bool p) {
  if (p == st.playing) return;
  st.playing = p;
  if (p) {
    // resume: shift anchor by pause duration
    st.startMs += millis() - st.pausedAtMs;
    if (st.hasAudio) audioCmd([] { audio.pauseResume(); });
  } else {
    st.pausedAtMs = millis();
    if (st.hasAudio) audioCmd([] { audio.pauseResume(); });
  }
  showOsd();
}

static void stopPlayback() {
  if (st.hasAudio) audioCmd([] { audio.stopSong(); });
  if (st.vfile) st.vfile.close();
  if (st.idx.offsets) { free(st.idx.offsets); st.idx.offsets = nullptr; st.idx.valid = false; }
  st.screen = Screen::BROWSER;
  st.dirty = true;
}

static bool startPlayback(const String &base) {
  stopPlayback();

  st.vfile = SD.open(base + ".mjpeg", FILE_READ);
  if (!st.vfile) return false;

  if (!loadIndex(base, st.idx)) {
    // No index: fall back to a fake single-chunk index at 30fps (no seeking)
    st.idx.fps = 30.0f;
    st.idx.frames = 0;               // frames==0 => stream-parse mode
    st.idx.valid = false;
  }

  st.hasAudio = SD.exists(base + ".mp3");
  if (st.hasAudio) {
    String mp3 = base + ".mp3";
    audioCmd([mp3] {
      audio.setVolume(st.volume);
      audio.connecttoFS(SD, mp3.c_str());
    });
  }

  st.basePath = base;
  st.frame = 0;
  st.playing = true;
  st.startMs = millis();
  st.screen = Screen::PLAYER;
  tft.fillScreen(TFT_BLACK);
  showOsd();
  return true;
}

// ---------------------------------------------------------------------------
// Frame reading
//   Indexed mode: exact offsets from .idx
//   Stream mode (no .idx): scan for JPEG SOI/EOI markers (slower, no seek)
// ---------------------------------------------------------------------------
static bool ensureBuf(size_t n) {
  if (n <= frameBufSize) return true;
  uint8_t *nb = (uint8_t *)realloc(frameBuf, n);
  if (!nb) return false;
  frameBuf = nb;
  frameBufSize = n;
  return true;
}

static int readNextFrame() {          // returns frame byte length, 0 = EOF, -1 = error
  if (st.idx.valid) {
    if (st.frame >= st.idx.frames) return 0;
    uint32_t off = st.idx.offsets[st.frame];
    uint32_t len = st.idx.offsets[st.frame + 1] - off;
    if (!ensureBuf(len)) return -1;
    st.vfile.seek(off);
    if (st.vfile.read(frameBuf, len) != (int)len) return -1;
    return (int)len;
  }

  // ---- stream-parse fallback ----
  if (!ensureBuf(64 * 1024)) return -1;
  // find SOI 0xFFD8
  int b, prev = -1;
  while ((b = st.vfile.read()) >= 0) {
    if (prev == 0xFF && b == 0xD8) break;
    prev = b;
  }
  if (b < 0) return 0;
  size_t n = 0;
  frameBuf[n++] = 0xFF; frameBuf[n++] = 0xD8;
  prev = -1;
  while ((b = st.vfile.read()) >= 0) {
    if (n >= frameBufSize && !ensureBuf(frameBufSize + 32 * 1024)) return -1;
    frameBuf[n++] = (uint8_t)b;
    if (prev == 0xFF && b == 0xD9) return (int)n;
    prev = b;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// OSD (on-screen controls)
// ---------------------------------------------------------------------------
static void drawOsd() {
  const int W = tft.width(), H = tft.height();

  // top bar: filename + time
  tft.fillRect(0, 0, W, 22, tft.color565(0, 0, 0));
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  String name = st.basePath.substring(st.basePath.lastIndexOf('/') + 1);
  tft.drawString(name, 4, 4, 2);

  uint32_t curSec = (uint32_t)(st.frame / st.idx.fps);
  uint32_t totSec = st.idx.valid ? (uint32_t)(st.idx.frames / st.idx.fps) : 0;
  tft.setTextDatum(TR_DATUM);
  tft.drawString(fmtTime(curSec) + " / " + fmtTime(totSec), W - 4, 4, 2);

  // bottom bar: progress + buttons
  int barY = H - 46;
  tft.fillRect(0, barY, W, 46, TFT_BLACK);

  // progress
  tft.drawRect(8, barY + 4, W - 16, 8, TFT_DARKGREY);
  if (st.idx.valid && st.idx.frames) {
    int fill = (int)((uint64_t)(W - 18) * st.frame / st.idx.frames);
    tft.fillRect(9, barY + 5, fill, 6, TFT_YELLOW);
  }

  // buttons:  [back]  [-1m]  [play/pause]  [+1m]
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("BACK", 34, barY + 30, 2);
  tft.drawString("<< 1m", W / 2 - 70, barY + 30, 2);
  tft.drawString(st.playing ? "| |" : ">", W / 2, barY + 30, 4);
  tft.drawString("1m >>", W / 2 + 70, barY + 30, 2);
  tft.drawString("VOL " + String(st.volume), W - 34, barY + 30, 2);
}

// ---------------------------------------------------------------------------
// Touch mapping (rotation 1: landscape 320x240)
// ---------------------------------------------------------------------------
static bool readTouch(int &x, int &y) {
  if (!touch.touched()) return false;
  TS_Point p = touch.getPoint();
  // map raw -> screen, rotated for landscape
  x = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, tft.width());
  y = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, tft.height(), 0);
  x = constrain(x, 0, tft.width() - 1);
  y = constrain(y, 0, tft.height() - 1);
  return true;
}

// Edge-drag gestures: left edge = brightness, right edge = volume
static void handlePlayerTouch() {
  static bool wasDown = false;
  static int downX = 0, downY = 0;
  static uint32_t downMs = 0;
  static bool dragging = false;

  int x, y;
  bool down = readTouch(x, y);
  const int W = tft.width(), H = tft.height();

  if (down && !wasDown) {            // press
    downX = x; downY = y; downMs = millis(); dragging = false;
  }

  if (down && wasDown) {             // drag
    int dy = downY - y;              // up = positive
    if (!dragging && abs(dy) > 18 && (downX < 48 || downX > W - 48)) dragging = true;
    if (dragging) {
      if (downX < 48) {              // brightness
        int nb = constrain(st.brightness + dy / 2, 8, 255);
        setBrightness(nb);
      } else {                       // volume
        int nv = constrain(st.volume + dy / 24, 0, 21);
        if (nv != st.volume) {
          st.volume = nv;
          audioCmd([] { audio.setVolume(st.volume); });
        }
      }
      downY = y;                     // incremental
      showOsd();
    }
  }

  if (!down && wasDown && !dragging) {   // tap released
    uint32_t heldMs = millis() - downMs;
    if (heldMs < 600) {
      int barY = H - 46;
      if (!st.osdVisible) {
        showOsd();
      } else if (downY >= barY) {
        // progress bar tap -> seek
        if (downY < barY + 16 && st.idx.valid) {
          uint32_t f = (uint64_t)st.idx.frames * constrain(downX - 8, 0, W - 16) / (W - 16);
          seekToFrame(f);
        } else if (downX < 68) {
          stopPlayback();
        } else if (downX < W / 2 - 35) {
          long f = (long)st.frame - (long)(SEEK_STEP_SEC * st.idx.fps);
          seekToFrame(f < 0 ? 0 : (uint32_t)f);
        } else if (downX < W / 2 + 35) {
          setPlaying(!st.playing);
        } else if (downX < W - 68) {
          seekToFrame(st.frame + (uint32_t)(SEEK_STEP_SEC * st.idx.fps));
        }
        showOsd();
      } else if (downY > 22) {
        setPlaying(!st.playing);     // tap video area = play/pause
      }
    }
  }
  wasDown = down;
}

// ---------------------------------------------------------------------------
// File browser
// ---------------------------------------------------------------------------
static void scanVideos() {
  videoList.clear();
  File dir = SD.open(VIDEO_DIR);
  if (!dir) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    if (n.endsWith(".mjpeg")) {
      String base = n.substring(0, n.length() - 6);
      if (base.startsWith("/")) videoList.push_back(base);
      else videoList.push_back(String(VIDEO_DIR) + "/" + base);
    }
  }
  dir.close();
}

static const int ROWS_PER_PAGE = 6;

static void drawBrowser() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Open CYD Player", 8, 6, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("/videos on SD card", 8, 32, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int start = st.browserPage * ROWS_PER_PAGE;
  for (int i = 0; i < ROWS_PER_PAGE; i++) {
    int gi = start + i;
    if (gi >= (int)videoList.size()) break;
    int y = 52 + i * 28;
    tft.fillRoundRect(6, y, tft.width() - 12, 24, 4, tft.color565(24, 24, 24));
    String name = videoList[gi].substring(videoList[gi].lastIndexOf('/') + 1);
    tft.drawString(name, 14, y + 4, 2);
  }

  if (videoList.empty()) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("No .mjpeg files found", tft.width() / 2, 120, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Put movie.mjpeg + movie.mp3 + movie.idx", tft.width() / 2, 145, 2);
    tft.drawString("in /videos (use tools/convert.py)", tft.width() / 2, 162, 2);
  }

  // paging arrows
  int pages = (videoList.size() + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
  if (pages > 1) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("<", 20, tft.height() - 12, 4);
    tft.drawString(">", tft.width() - 20, tft.height() - 12, 4);
    tft.drawString(String(st.browserPage + 1) + "/" + String(pages), tft.width() / 2, tft.height() - 12, 2);
  }
  st.dirty = false;
}

static void handleBrowserTouch() {
  static bool wasDown = false;
  int x, y;
  bool down = readTouch(x, y);
  if (!down && wasDown) {
    int pages = (videoList.size() + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (y > tft.height() - 26 && pages > 1) {
      if (x < 60 && st.browserPage > 0) { st.browserPage--; st.dirty = true; }
      else if (x > tft.width() - 60 && st.browserPage < pages - 1) { st.browserPage++; st.dirty = true; }
    } else {
      int row = (y - 52) / 28;
      int gi = st.browserPage * ROWS_PER_PAGE + row;
      if (row >= 0 && row < ROWS_PER_PAGE && gi < (int)videoList.size()) {
        if (!startPlayback(videoList[gi])) st.dirty = true;
      }
    }
  }
  wasDown = down;
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Backlight PWM
  ledcSetup(BL_CHANNEL, BL_FREQ, BL_RES_BITS);
  ledcAttachPin(BL_PIN, BL_CHANNEL);
  setBrightness(DEFAULT_BRIGHT);

  // Display
  tft.init();
  tft.setRotation(1);                // landscape 320x240
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);

  // Touch (dedicated SPI bus)
  touchSPI.begin(TOUCH_CLK, TOUCH_DO, TOUCH_DIN, TOUCH_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);

  // SD card
  if (!SD.begin(SD_CS)) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("SD card not found!", tft.width() / 2, tft.height() / 2, 4);
    while (!SD.begin(SD_CS)) delay(500);
    tft.fillScreen(TFT_BLACK);
  }

  // Audio out
#if USE_EXTERNAL_I2S_DAC
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
#else
  audio.setInternalDAC(true);        // GPIO26 -> onboard amp
#endif
  audio.setVolume(DEFAULT_VOLUME);

  audioMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 2, &audioTaskHandle, 0);

  scanVideos();
  st.dirty = true;
}

void loop() {
  if (st.screen == Screen::BROWSER) {
    if (st.dirty) drawBrowser();
    handleBrowserTouch();
    delay(10);
    return;
  }

  // -------- PLAYER --------
  handlePlayerTouch();

  // auto-hide OSD
  if (st.osdVisible && st.playing && millis() - st.osdShownMs > OSD_TIMEOUT_MS) {
    st.osdVisible = false;
    // OSD regions get repainted by subsequent frames; force clear for pause case
  }

  if (st.playing) {
    // frame pacing against wall clock
    uint32_t due = st.startMs + (uint32_t)(st.frame * 1000.0f / st.idx.fps);
    uint32_t now = millis();
    if ((int32_t)(now - due) >= 0) {
      int len = readNextFrame();
      if (len <= 0) {                // end of video (or error)
        stopPlayback();
        return;
      }
      // if we're badly behind (slow SD / big frame), drop frames to catch up
      if (jpeg.openRAM(frameBuf, len, jpegDraw)) {
        jpeg.setPixelType(RGB565_BIG_ENDIAN);
        int32_t behind = (int32_t)(millis() - due);
        if (behind < (int32_t)(1000.0f / st.idx.fps) * 2) {
          jpeg.decode(0, 0, 0);
        }                            // else: skip decode, just advance
        jpeg.close();
      }
      st.frame++;
    }
  }

  if (st.osdVisible) drawOsd();
}

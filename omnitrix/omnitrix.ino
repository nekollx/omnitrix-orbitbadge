#include <LittleFS.h>
#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <CST816S.h>
#include "driver/i2s_std.h"
i2s_chan_handle_t tx_chan = NULL;

// ESP8266Audio MP3 & Dual-Channel Mixer Headers
#include <AudioFileSourceLittleFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioOutputMixer.h>
enum DialColorMode { MODE_ACTIVE, MODE_RECHARGE, MODE_SCANNER, MODE_CALIBRATION, MODE_EON };
enum OmnitrixState { IDLE_HOURGLASS, SELECT_ALIEN, SCANNER_BROWSE, TRANSFORMED, TIMEOUT_WARNING, RECHARGE, COLOR_PICKER, SETTINGS_MENU, CLOCK_SET, WATCH_MODE, SLEEP_MODE };
bool masterMuted = false;
float masterVolume = 0.85f; // 0.10 to 1.00
int presetRosterIndex = 0;  // 0:Classic S1, 1:Classic End, 2:Alien Force, 3:Omniverse, 4:Reboot
bool hideCardinalCues = false;
bool watchModeActive = false;
bool lowPowerLockout = false;
int settingsPage = 0;       // 0 = Audio/Colors, 1 = Watch Mode/Clock/Cues/Playlist

// Software Clock Base (editable in Settings -> Set Clock)
int clockBaseHour = 9;     // 1..12
int clockBaseMin  = 53;     // 0..59
bool clockBasePM  = false;
unsigned long clockSyncMillis = 0;
int clockEditField = 0;     // 0 = Hour, 1 = Minute, 2 = AM/PM
unsigned long lastWatchDraw = 0;

// Auto-initializes the watch clock from the uploading computer's local compile time (__TIME__)
void initClockFromCompileTime() {
  String compileTime = String(__TIME__); // Format: "HH:MM:SS" (24-hour)
  if (compileTime.length() >= 5) {
    int h24 = compileTime.substring(0, 2).toInt();
    int m   = compileTime.substring(3, 5).toInt();

    if (h24 >= 0 && h24 <= 23 && m >= 0 && m <= 59) {
      clockBasePM = (h24 >= 12);
      int h12 = h24 % 12;
      if (h12 == 0) h12 = 12;
      clockBaseHour = h12;
      clockBaseMin  = m;
    }
  }
  clockSyncMillis = millis();
}


// ==========================================
// DYNAMIC DISPLAY & HARDWARE PINOUT (OrbitBadge)
// ==========================================
int SCREEN_SIZE = 240;
const unsigned long SLEEP_TIMEOUT_MS = 30000;

CST816S touch(43, 44, 46, 38); // Touch + BQ27220 Fuel Gauge I2C (SDA=43, SCL=44)
#define SIDE_BTN_PIN 0         // Action / Bezel button

// ES8311 Audio Codec & I2S Pins
#define AUDIO_I2C_SDA 6
#define AUDIO_I2C_SCL 7
#define ES8311_ADDR   0x18

#define I2S_MCLK_PIN  45
#define I2S_BCLK_PIN  39
#define I2S_WS_PIN    41
#define I2S_DOUT_PIN  42
#define I2S_DIN_PIN   40

// BQ27220 Battery Gauge Address (on Wire / pins 43, 44)
#define BQ27220_ADDR  0x55

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 10;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 14;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 13;
      cfg.pin_rst          = 11;
      cfg.panel_width      = 240;
      cfg.panel_height     = 240;
      cfg.invert           = true;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 16;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX tft;
LGFX_Sprite canvas(&tft);
LGFX_Sprite modeSprite(&tft);
Preferences prefs;
const uint16_t DIAL_SIDE_GREY = tft.color565(117, 117, 117);

// ==========================================
// DUAL-CHANNEL MP3 AUDIO ENGINE (Non-Blocking Single-Core DMA = Zero Screech & Zero Freeze)
// ==========================================
String clickPoolPacked = "|";
int clickPoolCount = 0;
int lastClickIndex = -1;

class I2SBridgeOutput : public AudioOutput {
public:
  I2SBridgeOutput() {}
  virtual bool begin() override { return true; }
  virtual bool SetRate(int hz) override {
    hertz = hz;
    return true;
  }
  virtual uint16_t ConsumeSamples(int16_t* samples, uint16_t count) override {
    if (!tx_chan || !samples || count == 0) return 0;
    size_t bytesToWrite = count * 2 * sizeof(int16_t);
    size_t bytesWritten = 0;
    // Non-blocking write (0ms timeout): returns immediately if DMA queue is full so UI never freezes!
    i2s_channel_write(tx_chan, samples, bytesToWrite, &bytesWritten, 0);
    return bytesWritten / (2 * sizeof(int16_t));
  }
  virtual bool ConsumeSample(int16_t* sample) override {
    return ConsumeSamples(sample, 1) == 1;
  }
  virtual bool stop() override {
    flushSilence();
    return true;
  }
  void flushSilence() {
    if (!tx_chan) return;
    int16_t* zeros = (int16_t*)calloc(256, sizeof(int16_t));
    if (zeros) {
      size_t written = 0;
      for (int i = 0; i < 32; i++) {
        i2s_channel_write(tx_chan, zeros, 256 * sizeof(int16_t), &written, 20);
      }
      free(zeros);
    }
  }
};

I2SBridgeOutput* audioOut = nullptr;
AudioOutputMixer* audioMixer = nullptr;
AudioOutputMixerStub* sfxChannel = nullptr;
AudioOutputMixerStub* bgmChannel = nullptr;

AudioGeneratorMP3* mp3Sfx = nullptr;
AudioFileSourceLittleFS* fileSfx = nullptr;

AudioGeneratorMP3* mp3Bgm = nullptr;
AudioFileSourceLittleFS* fileBgm = nullptr;

bool audioHardwareReady = false;
bool themeMusicEnabled = false;
bool idleHumDesired = false;
String activeBgmPath = "";
unsigned long lastBatteryCheck = 0;

void discoverClickSounds() {
  clickPoolPacked = "|";
  clickPoolCount = 0;
  File dir = LittleFS.open("/sounds");
  if (!dir || !dir.isDirectory()) return;

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String fname = String(f.name());
      int slashIdx = fname.lastIndexOf('/');
      if (slashIdx >= 0) fname = fname.substring(slashIdx + 1);
      String lower = fname;
      lower.toLowerCase();
      if (lower.startsWith("dial_click") && lower.endsWith(".mp3")) {
        clickPoolPacked += "/sounds/" + fname + "|";
        clickPoolCount++;
      }
    }
    f = dir.openNextFile();
  }
}

String getClickPathByIndex(int targetIdx) {
  int idx = 0;
  int startPos = 1;
  while (startPos < (int)clickPoolPacked.length()) {
    int endPos = clickPoolPacked.indexOf('|', startPos);
    if (endPos < 0) break;
    if (idx == targetIdx) return clickPoolPacked.substring(startPos, endPos);
    idx++;
    startPos = endPos + 1;
  }
  return "/sounds/dial_click01.mp3";
}

void es8311WriteReg(uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(ES8311_ADDR);
  Wire1.write(reg);
  Wire1.write(val);
  Wire1.endTransmission();
}

bool initES8311Codec() {
  Wire1.begin(AUDIO_I2C_SDA, AUDIO_I2C_SCL);
  Wire1.beginTransmission(ES8311_ADDR);
  if (Wire1.endTransmission() != 0) return false;

  es8311WriteReg(0x00, 0x1F);
  delay(20);
  es8311WriteReg(0x00, 0x80);
  es8311WriteReg(0x01, 0x3F);
  es8311WriteReg(0x02, 0x00);
  es8311WriteReg(0x03, 0x10);
  es8311WriteReg(0x04, 0x10);
  es8311WriteReg(0x05, 0x00);
  es8311WriteReg(0x06, 0x03);
  es8311WriteReg(0x07, 0x00);
  es8311WriteReg(0x08, 0xFF);
  es8311WriteReg(0x09, 0x0C);
  es8311WriteReg(0x0A, 0x0C);
  es8311WriteReg(0x0B, 0x00);
  es8311WriteReg(0x0C, 0x00);
  es8311WriteReg(0x10, 0x1F);
  es8311WriteReg(0x11, 0x7F);
  es8311WriteReg(0x0D, 0x01);
  es8311WriteReg(0x0E, 0x02);
  es8311WriteReg(0x12, 0x00);
  es8311WriteReg(0x13, 0x10);
  es8311WriteReg(0x14, 0x1A);
  es8311WriteReg(0x15, 0x40);
  es8311WriteReg(0x16, 0x24);
  es8311WriteReg(0x17, 0xC8);
  es8311WriteReg(0x1C, 0x6A);
  es8311WriteReg(0x31, 0x00);
  es8311WriteReg(0x37, 0x08);
  es8311WriteReg(0x32, 0xE8);
  es8311WriteReg(0x44, 0x03);
  delay(20);
  return true;
}

void startBgmStream(const char* path) {
  if (mp3Bgm) {
    if (mp3Bgm->isRunning()) mp3Bgm->stop();
    delete mp3Bgm;
    mp3Bgm = nullptr;
  }
  if (fileBgm) {
    delete fileBgm;
    fileBgm = nullptr;
  }
  activeBgmPath = String(path);
  if (activeBgmPath.length() > 0 && LittleFS.exists(activeBgmPath)) {
    fileBgm = new AudioFileSourceLittleFS(activeBgmPath.c_str());
    mp3Bgm = new AudioGeneratorMP3();
    mp3Bgm->begin(fileBgm, bgmChannel);
  } else if (audioOut && (!mp3Sfx || !mp3Sfx->isRunning())) {
    audioOut->flushSilence();
  }
}

void refreshBgmChannel() {
  if (!audioHardwareReady) return;
  if (masterMuted || watchModeActive) {
    startBgmStream("");
    return;
  }
  if (themeMusicEnabled) {
    bgmChannel->SetGain(0.45f * masterVolume);
    startBgmStream("/sounds/theme.mp3");
  } else if (idleHumDesired) {
    bgmChannel->SetGain(0.35f * masterVolume);
    startBgmStream("/sounds/idle_hum.mp3");
  } else {
    startBgmStream("");
  }
}

void playSFX(const char* path) {
if (!audioHardwareReady || masterMuted || watchModeActive || !LittleFS.exists(path)) return;
  if (mp3Sfx) {
    if (mp3Sfx->isRunning()) mp3Sfx->stop();
    delete mp3Sfx;
    mp3Sfx = nullptr;
  }
  if (fileSfx) {
    delete fileSfx;
    fileSfx = nullptr;
  }
  if (audioOut && !themeMusicEnabled && !idleHumDesired) {
    audioOut->flushSilence();
  }
  fileSfx = new AudioFileSourceLittleFS(path);
  mp3Sfx = new AudioGeneratorMP3();
  sfxChannel->SetGain(masterVolume);
  mp3Sfx->begin(fileSfx, sfxChannel);
}


void playRandomDialClick() {
  if (clickPoolCount <= 0) {
    playSFX("/sounds/dial_click01.mp3");
    return;
  }
  if (clickPoolCount == 1) {
    lastClickIndex = 0;
    playSFX(getClickPathByIndex(0).c_str());
    return;
  }
  int nextIdx = lastClickIndex;
  while (nextIdx == lastClickIndex) {
    nextIdx = random(0, clickPoolCount);
  }
  lastClickIndex = nextIdx;
  playSFX(getClickPathByIndex(nextIdx).c_str());
}

void setIdleHumActive(bool active) {
  if (idleHumDesired != active) {
    idleHumDesired = active;
    refreshBgmChannel();
  }
}

void toggleThemeMusic() {
  themeMusicEnabled = !themeMusicEnabled;
  refreshBgmChannel();
}

void serviceAudioMixer() {
  if (!audioHardwareReady) return;

  if (mp3Sfx && mp3Sfx->isRunning()) {
    if (!mp3Sfx->loop()) {
      mp3Sfx->stop();
      if (audioOut) {
        audioOut->flushSilence(); // Always wipe the DMA ring buffer at the end of every SFX!
      }
    }
  }

  if ((themeMusicEnabled || idleHumDesired) && mp3Bgm) {
    if (mp3Bgm->isRunning()) {
      bgmChannel->SetGain((themeMusicEnabled ? 0.45f : 0.35f) * masterVolume);
      if (!mp3Bgm->loop()) {
        startBgmStream(activeBgmPath.c_str());
      }
    }
  }
}

void pumpAudioDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    serviceAudioMixer();
    delay(2);
  }
}

void initAudioSystem() {
  discoverClickSounds();
  gpio_reset_pin((gpio_num_t)I2S_MCLK_PIN);

  // Larger DMA buffer (8 descriptors x 512 frames) prevents audio stutter while UI renders!
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 512;
  if (i2s_new_channel(&chan_cfg, &tx_chan, NULL) != ESP_OK) return;

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = (gpio_num_t)I2S_MCLK_PIN,
      .bclk = (gpio_num_t)I2S_BCLK_PIN,
      .ws   = (gpio_num_t)I2S_WS_PIN,
      .dout = (gpio_num_t)I2S_DOUT_PIN,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  if (i2s_channel_init_std_mode(tx_chan, &std_cfg) != ESP_OK) return;
  if (i2s_channel_enable(tx_chan) != ESP_OK) return;

  audioHardwareReady = initES8311Codec();
  if (!audioHardwareReady) return;
    // Unmute AW8010AFCR amp with LOW -> HIGH rising edge
  pinMode(1, OUTPUT);  digitalWrite(1, LOW);
  pinMode(2, OUTPUT);  digitalWrite(2, LOW);
  pinMode(3, OUTPUT);  digitalWrite(3, LOW);
  pinMode(4, OUTPUT);  digitalWrite(4, LOW);
  pinMode(5, OUTPUT);  digitalWrite(5, LOW);
  pinMode(8, OUTPUT);  digitalWrite(8, LOW);
  pinMode(9, OUTPUT);  digitalWrite(9, LOW);
  pinMode(15, OUTPUT); digitalWrite(15, LOW);
  pinMode(17, OUTPUT); digitalWrite(17, LOW);
  pinMode(18, OUTPUT); digitalWrite(18, LOW);
  pinMode(21, OUTPUT); digitalWrite(21, LOW);
  pinMode(47, OUTPUT); digitalWrite(47, LOW);
  pinMode(48, OUTPUT); digitalWrite(48, LOW);
  delay(20);
  digitalWrite(1, HIGH);
  digitalWrite(2, HIGH);
  digitalWrite(3, HIGH);
  digitalWrite(4, HIGH);
  digitalWrite(5, HIGH);
  digitalWrite(8, HIGH);
  digitalWrite(9, HIGH);
  digitalWrite(15, HIGH);
  digitalWrite(17, HIGH);
  digitalWrite(18, HIGH);
  digitalWrite(21, HIGH);
  digitalWrite(47, HIGH);
  digitalWrite(48, HIGH);
  delay(20);

  audioOut = new I2SBridgeOutput();
  audioOut->SetRate(44100);
  audioOut->SetGain(0.95f);

  audioMixer = new AudioOutputMixer(2048, audioOut);
  sfxChannel = audioMixer->NewInput();
  sfxChannel->SetGain(1.0f);

  bgmChannel = audioMixer->NewInput();
  bgmChannel->SetGain(0.35f);
}

int readBatteryPercent() {
  Wire.beginTransmission(BQ27220_ADDR);
  Wire.write(0x2C);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(BQ27220_ADDR, 2) == 2) {
    uint8_t lsb = Wire.read();
    uint8_t msb = Wire.read();
    int soc = (msb << 8) | lsb;
    if (soc >= 0 && soc <= 100) return soc;
  }
  return -1;
}


// ==========================================
// MODES, CUSTOM COLORS & 64-ALIEN ROSTER
// ==========================================
DialColorMode idleColorMode = MODE_ACTIVE;
OmnitrixState currentState = IDLE_HOURGLASS;
uint8_t modeR0 = 185, modeG0 = 255, modeB0 = 55;  // Active
uint8_t modeR1 = 255, modeG1 = 0,   modeB1 = 0;   // Recharge
uint8_t modeR2 = 255, modeG2 = 230, modeB2 = 0;   // Scanner
uint8_t modeR3 = 0,   modeG3 = 190, modeB3 = 255; // Calibration
uint8_t modeR4 = 180, modeG4 = 0,   modeB4 = 255; // Eon/Magic
uint8_t modeR5 = 255, modeG5 = 255, modeB5 = 255; // Transformed Hourglass (Default Pure White)


bool isDefaultActiveGreen = true;
int editSlot = 0;
float pickerHue = 0.33f;

const int BUILTIN_STANDARD_COUNT = 42;
const int CUSTOM_START_INDEX = 42;
const int MAX_CUSTOM_SLOTS = 21;
const int EON_INDEX = 63;
const int MAX_ROSTER_SLOTS = 64;

int discoveredCustomCount = 0;
String customSlugsPacked = "";

const uint64_t DEFAULT_ROSTER_MASK = 0x00000000000003FFULL;
uint64_t unlockedAlienMask = DEFAULT_ROSTER_MASK;
int currentAlien = 0;
int scannerAlien = 10;
bool hologramMode = false;

unsigned long btnDownTime = 0;
bool btnHeldHandled = false;
unsigned long lastBtnPress = 0;
unsigned long lastBtnRelease = 0;
unsigned long stateTimer = 0;
unsigned long lastActivityTime = 0;

float modeAngle = 0.0f;
unsigned long lastAnimFrame = 0;



// ==========================================
// BUILT-IN & AUTO-DISCOVERED ALIEN NAMES
// ==========================================
String getBuiltinSlug(int index) {
  switch (index) {
    case 0:  return "heatblast";
    case 1:  return "fourarms";
    case 2:  return "xlr8";
    case 3:  return "diamondhead";
    case 4:  return "upgrade";
    case 5:  return "greymatter";
    case 6:  return "ripjaws";
    case 7:  return "stinkfly";
    case 8:  return "ghostfreak";
    case 9:  return "wildmutt";
    case 10: return "cannonbolt";
    case 11: return "wildvine";
    case 12: return "blitzwolfer";
    case 13: return "snareoh";
    case 14: return "frankenstrike";
    case 15: return "upchuck";
    case 16: return "ditto";
    case 17: return "eyeguy";
    case 18: return "waybig";
    case 19: return "spitter";
    case 20: return "buzzshock";
    case 21: return "arctiguana";
    case 22: return "swampfire";
    case 23: return "echoecho";
    case 24: return "humungousaur";
    case 25: return "jetray";
    case 26: return "bigchill";
    case 27: return "chromastone";
    case 28: return "brainstorm";
    case 29: return "spidermonkey";
    case 30: return "goop";
    case 31: return "alienx";
    case 32: return "rath";
    case 33: return "lodestar";
    case 34: return "nanomech";
    case 35: return "waterhazard";
    case 36: return "ampfibian";
    case 37: return "armodrillo";
    case 38: return "terraspin";
    case 39: return "nrg";
    case 40: return "clockwork";
    case 41: return "feedback";
    case 63: return "eon";
    default: return "";
  }
}

bool isBuiltinSlug(String slug) {
  if (slug == "eon") return true;
  for (int i = 0; i < BUILTIN_STANDARD_COUNT; i++) {

if (getBuiltinSlug(i) == slug) return true;
}
return false;
}
String getCustomSlugByOffset(int offset) {
if (offset < 0 || offset >= discoveredCustomCount) return "";
int currentIdx = 0;
int startPos = 1;
while (startPos < (int)customSlugsPacked.length()) {
int endPos = customSlugsPacked.indexOf('|', startPos);
if (endPos < 0) break;
if (currentIdx == offset) {
return customSlugsPacked.substring(startPos, endPos);
}
currentIdx++;
startPos = endPos + 1;
}
return "";
}
void discoverCustomAliensInLittleFS() {
customSlugsPacked = "|";
discoveredCustomCount = 0;
File root = LittleFS.open("/");
if (!root || !root.isDirectory()) return;
File file = root.openNextFile();
while (file && discoveredCustomCount < MAX_CUSTOM_SLOTS) {
if (!file.isDirectory()) {
String fname = String(file.name());
if (fname.startsWith("/")) fname = fname.substring(1);
String lower = fname;
lower.toLowerCase();
if (lower.endsWith(".png") && !lower.startsWith("launchanim")) {
String slug = fname.substring(0, fname.length() - 4);
String lowerSlug = lower.substring(0, lower.length() - 4);
if (lowerSlug.endsWith("_holo")) {
slug = slug.substring(0, slug.length() - 5);
lowerSlug = lowerSlug.substring(0, lowerSlug.length() - 5);
}
if (!isBuiltinSlug(lowerSlug) && customSlugsPacked.indexOf("|" + slug + "|") < 0) {
customSlugsPacked += slug + "|";
discoveredCustomCount++;
}
}
}
file = root.openNextFile();
}
}
bool isValidStandardSlot(int index) {
if (index >= 0 && index < BUILTIN_STANDARD_COUNT) return true;
if (index >= CUSTOM_START_INDEX && index < (CUSTOM_START_INDEX + discoveredCustomCount)) return true;
return false;
}
String getAlienSlug(int index) {
if (index == EON_INDEX) return "eon";
if (index >= 0 && index < BUILTIN_STANDARD_COUNT) return getBuiltinSlug(index);
if (index >= CUSTOM_START_INDEX && index < (CUSTOM_START_INDEX + discoveredCustomCount)) {
return getCustomSlugByOffset(index - CUSTOM_START_INDEX);
}
return "unknown";
}
String getAlienDisplayName(int index) {
switch (index) {
case 0:  return "HEATBLAST";
case 1:  return "FOUR ARMS";
case 2:  return "XLR8";
case 3:  return "DIAMONDHEAD";
case 4:  return "UPGRADE";
case 5:  return "GREY MATTER";
case 6:  return "RIPJAWS";
case 7:  return "STINKFLY";
case 8:  return "GHOSTFREAK";
case 9:  return "WILDMUTT";
case 10: return "CANNONBOLT";
case 11: return "WILDVINE";
case 12: return "BLITZWOLFER";
case 13: return "SNARE-OH";
case 14: return "FRANKENSTRIKE";
case 15: return "UPCHUCK";
case 16: return "DITTO";
case 17: return "EYE GUY";
case 18: return "WAY BIG";
case 19: return "SPITTER";
case 20: return "BUZZSHOCK";
case 21: return "ARCTIGUANA";
case 22: return "SWAMPFIRE";
case 23: return "ECHO ECHO";
case 24: return "HUMUNGOUSAUR";
case 25: return "JETRAY";
case 26: return "BIG CHILL";
case 27: return "CHROMASTONE";
case 28: return "BRAINSTORM";
case 29: return "SPIDERMONKEY";
case 30: return "GOOP";
case 31: return "ALIEN X";
case 32: return "RATH";
case 33: return "LODESTAR";
case 34: return "NANOMECH";
case 35: return "WATER HAZARD";
case 36: return "AMPFIBIAN";
case 37: return "ARMODRILLO";
case 38: return "TERRASPIN";
case 39: return "NRG";
case 40: return "CLOCKWORK";
case 41: return "FEEDBACK";
case 63: return "EON";
default: {
String customName = getAlienSlug(index);
customName.toUpperCase();
customName.replace("_", " ");
return customName;
}
}
}

float getAlienScale(int index) {
switch (index) {
case 0: return 0.64f; // Heatblast
case 1: return 0.58f; // Four Arms
case 2: return 0.52f; // XLR8
case 3: return 0.56f; // Diamondhead
case 4: return 0.56f; // Upgrade
case 5: return 0.62f; // Grey Matter
case 6: return 0.54f; // Ripjaws
case 7: return 0.56f; // Stinkfly
case 8: return 0.56f; // Ghostfreak
case 9: return 0.52f; // Wildmutt
default: return 0.60f;
}
}
// ==========================================
// 64-BIT PLAYLIST & PERSISTENCE HELPERS
// ==========================================
bool isAlienUnlocked(int index) {
if (index < 0 || index >= MAX_ROSTER_SLOTS) return false;
return (unlockedAlienMask & (1ULL << index)) != 0;
}
int countUnlockedAliens() {
int count = 0;
for (int i = 0; i < MAX_ROSTER_SLOTS; i++) {
if (isAlienUnlocked(i)) count++;
}
return count;
}
int countScannableInactiveAliens() {
int count = 0;
for (int i = 0; i < EON_INDEX; i++) {
if (isValidStandardSlot(i) && !isAlienUnlocked(i)) count++;
}
return count;
}
int getNextUnlockedAlien(int startIdx, int stepDir) {
int idx = startIdx;
for (int i = 0; i < MAX_ROSTER_SLOTS; i++) {
idx = (idx + stepDir + MAX_ROSTER_SLOTS) % MAX_ROSTER_SLOTS;
if (isAlienUnlocked(idx)) return idx;
}
return startIdx;
}
int getNextScannableAlien(int startIdx, int stepDir) {
int idx = startIdx;
for (int i = 0; i < EON_INDEX; i++) {
idx = (idx + stepDir + EON_INDEX) % EON_INDEX;
if (isValidStandardSlot(idx) && !isAlienUnlocked(idx)) return idx;
}
return -1;
}
void saveSettingsToFlash() {
prefs.begin("omnitrix", false);
prefs.putUChar("r0", modeR0); prefs.putUChar("g0", modeG0); prefs.putUChar("b0", modeB0);
prefs.putUChar("r1", modeR1); prefs.putUChar("g1", modeG1); prefs.putUChar("b1", modeB1);
prefs.putUChar("r2", modeR2); prefs.putUChar("g2", modeG2); prefs.putUChar("b2", modeB2);
prefs.putUChar("r3", modeR3); prefs.putUChar("g3", modeG3); prefs.putUChar("b3", modeB3);
prefs.putUChar("r4", modeR4); prefs.putUChar("g4", modeG4); prefs.putUChar("b4", modeB4);
prefs.putUChar("r5", modeR5); prefs.putUChar("g5", modeG5); prefs.putUChar("b5", modeB5);

prefs.putBool("defG", isDefaultActiveGreen);
prefs.putULong64("roster64", unlockedAlienMask);
prefs.putBool("hideCues", hideCardinalCues);
prefs.putFloat("vol", masterVolume);
prefs.end();
}
void loadSettingsFromFlash() {
prefs.begin("omnitrix", true);
modeR0 = prefs.getUChar("r0", 185); modeG0 = prefs.getUChar("g0", 255); modeB0 = prefs.getUChar("b0", 55);
modeR1 = prefs.getUChar("r1", 255); modeG1 = prefs.getUChar("g1", 0);   modeB1 = prefs.getUChar("b1", 0);
modeR2 = prefs.getUChar("r2", 255); modeG2 = prefs.getUChar("g2", 230); modeB2 = prefs.getUChar("b2", 0);
modeR3 = prefs.getUChar("r3", 0);   modeG3 = prefs.getUChar("g3", 190); modeB3 = prefs.getUChar("b3", 255);
modeR4 = prefs.getUChar("r4", 180); modeG4 = prefs.getUChar("g4", 0);   modeB4 = prefs.getUChar("b4", 255);
modeR5 = prefs.getUChar("r5", 255); modeG5 = prefs.getUChar("g5", 255); modeB5 = prefs.getUChar("b5", 255);
isDefaultActiveGreen = prefs.getBool("defG", true);
unlockedAlienMask = prefs.getULong64("roster64", DEFAULT_ROSTER_MASK);
if (unlockedAlienMask == 0) unlockedAlienMask = DEFAULT_ROSTER_MASK;
hideCardinalCues = prefs.getBool("hideCues", false);
masterVolume = prefs.getFloat("vol", 0.85f);
prefs.end();
if (!isAlienUnlocked(currentAlien)) {
currentAlien = getNextUnlockedAlien(0, 1);
}
}
void resetDefaultsAndRoster() {
presetRosterIndex = 0;
modeR0 = 185; modeG0 = 255; modeB0 = 55;
modeR1 = 255; modeG1 = 0;   modeB1 = 0;
modeR2 = 255; modeG2 = 230; modeB2 = 0;
modeR3 = 0;   modeG3 = 190; modeB3 = 255;
modeR4 = 180; modeG4 = 0;   modeB4 = 255;
modeR5 = 255; modeG5 = 255; modeB5 = 255;
isDefaultActiveGreen = true;
unlockedAlienMask = DEFAULT_ROSTER_MASK;
currentAlien = 0;
hideCardinalCues = false;
saveSettingsToFlash();
}
// ==========================================
// COLOR & CARDINAL CUE GRAPHICS ENGINE
// ==========================================
void getSlotColor(int slot, uint8_t &r, uint8_t &g, uint8_t &b) {
  switch (slot) {
    case 0: r = modeR0; g = modeG0; b = modeB0; break;
    case 1: r = modeR1; g = modeG1; b = modeB1; break;
    case 2: r = modeR2; g = modeG2; b = modeB2; break;
    case 3: r = modeR3; g = modeG3; b = modeB3; break;
    case 4: r = modeR4; g = modeG4; b = modeB4; break;
    default: r = modeR5; g = modeG5; b = modeB5; break;
  }
}

void setSlotColor(int slot, uint8_t r, uint8_t g, uint8_t b) {
  switch (slot) {
    case 0: modeR0 = r; modeG0 = g; modeB0 = b; isDefaultActiveGreen = false; break;
    case 1: modeR1 = r; modeG1 = g; modeB1 = b; break;
    case 2: modeR2 = r; modeG2 = g; modeB2 = b; break;
    case 3: modeR3 = r; modeG3 = g; modeB3 = b; break;
    case 4: modeR4 = r; modeG4 = g; modeB4 = b; break;
    case 5: modeR5 = r; modeG5 = g; modeB5 = b; break;
  }
}

void hsvToRgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
int i = int(h * 6);
float f = h * 6 - i;
float p = v * (1 - s);
float q = v * (1 - f * s);
float t = v * (1 - (1 - f) * s);
float rf, gf, bf;
switch (i % 6) {
case 0: rf = v; gf = t; bf = p; break;
case 1: rf = q; gf = v; bf = p; break;
case 2: rf = p; gf = v; bf = t; break;
case 3: rf = p; gf = q; bf = v; break;
case 4: rf = t; gf = p; bf = v; break;
default: rf = v; gf = p; bf = q; break;
}
r = (uint8_t)(rf * 255);
g = (uint8_t)(gf * 255);
b = (uint8_t)(bf * 255);
}
void drawCardinalCues(bool showAllFourCues) {
  if (hideCardinalCues) return;
int cx = SCREEN_SIZE / 2;
int cy = SCREEN_SIZE / 2;
int edgePad = max(4, (SCREEN_SIZE * 5) / 240);
int sz = max(4, (SCREEN_SIZE * 5) / 240);
// BOTTOM (6 o'clock): Yellow Triangle
uint16_t scannerCol = tft.color565(modeR2, modeG2, modeB2);
int botY = SCREEN_SIZE - edgePad;
canvas.fillTriangle(cx - sz, botY, cx + sz, botY, cx, botY - sz - 1, scannerCol);
canvas.drawTriangle(cx - sz - 1, botY + 1, cx + sz + 1, botY + 1, cx, botY - sz - 2, TFT_BLACK);
if (!showAllFourCues) return;
// TOP (12 o'clock): Purple Dot
uint16_t eonCol = tft.color565(modeR4, modeG4, modeB4);
int topY = edgePad + sz;
canvas.fillCircle(cx, topY, sz - 1, eonCol);
canvas.drawCircle(cx, topY, sz, TFT_BLACK);
// RIGHT (3 o'clock): Blue Diamond
uint16_t calCol = tft.color565(modeR3, modeG3, modeB3);
int rightX = SCREEN_SIZE - edgePad - sz;
canvas.fillTriangle(rightX - sz, cy, rightX, cy - sz, rightX, cy + sz, calCol);
canvas.fillTriangle(rightX + sz, cy, rightX, cy - sz, rightX, cy + sz, calCol);
canvas.drawLine(rightX - sz - 1, cy, rightX, cy - sz - 1, TFT_BLACK);
canvas.drawLine(rightX, cy - sz - 1, rightX + sz + 1, cy, TFT_BLACK);
canvas.drawLine(rightX + sz + 1, cy, rightX, cy + sz + 1, TFT_BLACK);
canvas.drawLine(rightX, cy + sz + 1, rightX - sz - 1, cy, TFT_BLACK);
// LEFT (9 o'clock): Red Square
uint16_t resetCol = tft.color565(modeR1, modeG1, modeB1);
int leftX = edgePad;
int box = sz * 2;
canvas.fillRect(leftX, cy - sz, box, box, resetCol);
canvas.drawRect(leftX - 1, cy - sz - 1, box + 2, box + 2, TFT_BLACK);
}
void drawPngToTarget(LovyanGFX* target, const char* path, float targetFillRatio = 1.0f) {
File file = LittleFS.open(path, "r");
if (!file || file.size() < 24) {
if (file) file.close();
return;
}
uint8_t b0 = file.read();
uint8_t b1 = file.read();
if (b0 == 0x89 && b1 == 'P') {
file.seek(16);
uint32_t imgW = ((uint32_t)file.read() << 24) | ((uint32_t)file.read() << 16) | ((uint32_t)file.read() << 8) | (uint32_t)file.read();
uint32_t imgH = ((uint32_t)file.read() << 24) | ((uint32_t)file.read() << 16) | ((uint32_t)file.read() << 8) | (uint32_t)file.read();
file.close();
float scale = min(((float)SCREEN_SIZE * targetFillRatio) / (float)imgW,
((float)SCREEN_SIZE * targetFillRatio) / (float)imgH);
int offsetX = (SCREEN_SIZE - (int)(imgW * scale)) / 2;
int offsetY = (SCREEN_SIZE - (int)(imgH * scale)) / 2;
target->drawPngFile(LittleFS, path, offsetX, offsetY, SCREEN_SIZE, SCREEN_SIZE, 0, 0, scale, scale);
} else {
file.close();
}
}
void renderKeyedAnimFrameToModeSprite(const char* path, DialColorMode colorMode) {
uint16_t transparentKey = __builtin_bswap16(TFT_MAGENTA);
modeSprite.fillSprite(TFT_MAGENTA);
drawPngToTarget(&modeSprite, path, 1.0f);
bool shiftGreen = !(colorMode == MODE_ACTIVE && isDefaultActiveGreen);
uint8_t targetR = 0, targetG = 0, targetB = 0;
if (shiftGreen) {
getSlotColor((int)colorMode, targetR, targetG, targetB);
}
uint16_t* buf = (uint16_t*)modeSprite.getBuffer();
int totalPixels = SCREEN_SIZE * SCREEN_SIZE;
for (int i = 0; i < totalPixels; i++) {
uint16_t raw = *(buf + i);
if (raw == transparentKey) continue;
uint16_t c = __builtin_bswap16(raw);
uint8_t r = (c >> 11) & 0x1F;
uint8_t g = (c >> 5) & 0x3F;
uint8_t b = c & 0x1F;
uint8_t g5 = g >> 1;
if (abs((int)r - (int)b) <= 2 && abs((int)r - (int)g5) <= 2 && r > 7 && r < 27) {
*(buf + i) = transparentKey;
}
else if (shiftGreen && g5 > b + 4 && (g5 + 4) >= r) {
float lum = (float)max(r, g5) / 31.0f;
uint16_t shifted = (((uint8_t)(targetR * lum) >> 3) << 11) |
(((uint8_t)(targetG * lum) >> 2) << 5) |
((uint8_t)(targetB * lum) >> 3);
*(buf + i) = __builtin_bswap16(shifted);
}
}
}
void drawAlienPlaceholder(LGFX_Sprite* target, int index, uint16_t fgColor) {
int cx = SCREEN_SIZE / 2;
int cy = SCREEN_SIZE / 2;
int r = (SCREEN_SIZE * 22) / 240;
target->drawCircle(cx, cy - (r / 2), r, fgColor);
target->drawCircle(cx, cy - (r / 2), r - 2, fgColor);
target->fillTriangle(cx - r, cy + r, cx + r, cy + r, cx, cy - (r / 3), fgColor);
target->fillCircle(cx, cy - (r / 2), r / 3, fgColor);
target->setTextDatum(MC_DATUM);
target->setTextColor(fgColor);
target->setTextSize(max(1, SCREEN_SIZE / 240));
target->drawString(getAlienDisplayName(index), cx, cy + r + ((SCREEN_SIZE * 14) / 240));
}

String getAlienPath(int index) {
  String holoPath = "/" + getAlienSlug(index) + "_holo.png";
  if (LittleFS.exists(holoPath)) return holoPath;
  return "/" + getAlienSlug(index) + ".png";
}

void renderAlienGraphicLayer(int index, bool useHolo, DialColorMode bgColorMode) {
  String alienFile = getAlienPath(index);
  float alienScale = getAlienScale(index);
  uint16_t transparentKey = __builtin_bswap16(TFT_MAGENTA);
  uint16_t blackPixel = __builtin_bswap16(TFT_BLACK);
  int totalPixels = SCREEN_SIZE * SCREEN_SIZE;

  if (useHolo) {
    canvas.fillSprite(TFT_BLACK);
    if (LittleFS.exists(alienFile)) {
      modeSprite.fillSprite(TFT_BLACK);
      drawPngToTarget(&modeSprite, alienFile.c_str(), alienScale);

      uint8_t ar, ag, ab;
      getSlotColor((int)bgColorMode, ar, ag, ab);

      uint16_t* holoBuf = (uint16_t*)modeSprite.getBuffer();
      for (int i = 0; i < totalPixels; i++) {
        uint16_t c = __builtin_bswap16(*(holoBuf + i));
        uint8_t r = (c >> 11) & 0x1F;
        uint8_t g = (c >> 5) & 0x3F;
        uint8_t b = c & 0x1F;
        uint8_t g5 = g >> 1;

        if (g5 > b + 4 && (g5 + 4) >= r) {
          float lum = (float)max(r, g5) / 31.0f;
          uint16_t shifted = (((uint8_t)(ar * lum) >> 3) << 11) |
                             (((uint8_t)(ag * lum) >> 2) << 5) |
                             ((uint8_t)(ab * lum) >> 3);
          *(holoBuf + i) = __builtin_bswap16(shifted);
        }
      }
      modeSprite.pushSprite(&canvas, 0, 0);
    } else {
      uint8_t ar, ag, ab;
      getSlotColor((int)bgColorMode, ar, ag, ab);
      drawAlienPlaceholder(&canvas, index, tft.color565(ar, ag, ab));
    }
    return;
  }

  renderKeyedAnimFrameToModeSprite("/launchAnim0014.png", bgColorMode);
  canvas.fillSprite(DIAL_SIDE_GREY);
  modeSprite.pushSprite(&canvas, 0, 0, TFT_MAGENTA);

  modeSprite.fillSprite(TFT_BLACK);
  if (LittleFS.exists(alienFile)) {
    drawPngToTarget(&modeSprite, alienFile.c_str(), alienScale);

    uint16_t* alienBuf = (uint16_t*)modeSprite.getBuffer();
    for (int i = 0; i < totalPixels; i++) {
      uint16_t c = __builtin_bswap16(*(alienBuf + i));
      uint8_t r = (c >> 11) & 0x1F;
      uint8_t g = (c >> 5) & 0x3F;
      uint8_t b = c & 0x1F;
      uint8_t g5 = g >> 1;

      if (r < 5 && g5 < 7 && b < 5) {
        *(alienBuf + i) = transparentKey;
      } else {
        *(alienBuf + i) = blackPixel;
      }
    }
    modeSprite.pushRotateZoom(&canvas, SCREEN_SIZE / 2.0f, SCREEN_SIZE / 2.0f, 180.0f, 1.0f, 1.0f, TFT_MAGENTA);
  } else {
    drawAlienPlaceholder(&canvas, index, TFT_BLACK);
  }

  modeSprite.fillSprite(TFT_MAGENTA);
  drawPngToTarget(&modeSprite, "/launchAnim0014.png", 1.0f);

  uint16_t* maskBuf = (uint16_t*)modeSprite.getBuffer();
  for (int i = 0; i < totalPixels; i++) {
    uint16_t raw = *(maskBuf + i);
    if (raw == transparentKey) continue;

    uint16_t c = __builtin_bswap16(raw);
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5) & 0x3F;
    uint8_t b = c & 0x1F;
    uint8_t g5 = g >> 1;

    bool isGrey = (abs((int)r - (int)b) <= 2 && abs((int)r - (int)g5) <= 2 && r > 7 && r < 27);
    bool isGreen = (g5 > b + 4 && (g5 + 4) >= r);

    if (isGrey || isGreen) {
      *(maskBuf + i) = transparentKey;
    }
  }
  modeSprite.pushSprite(&canvas, 0, 0, TFT_MAGENTA);
}


// ==========================================
// SCREEN STATES & SPECIAL MODE ACTIONS
// ==========================================
void prepareIdleModeSprite(DialColorMode colorMode) {
modeAngle = 0.0f;
renderKeyedAnimFrameToModeSprite("/launchAnim0001.png", colorMode);
canvas.fillSprite(DIAL_SIDE_GREY);
modeSprite.pushSprite(&canvas, 0, 0, TFT_MAGENTA);
drawCardinalCues(true);
canvas.pushSprite(0, 0);
}
// Renders the idle hourglass tinted in pure White (for timeout warning pulses)
void prepareTransformedHourglass() {
  uint8_t tr, tg, tb;
  getSlotColor(5, tr, tg, tb);
  uint16_t transCol = tft.color565(tr, tg, tb);

  uint16_t transparentKey = __builtin_bswap16(TFT_MAGENTA);
  modeSprite.fillSprite(TFT_MAGENTA);
  drawPngToTarget(&modeSprite, "/launchAnim0001.png", 1.0f);

  uint16_t* buf = (uint16_t*)modeSprite.getBuffer();
  int totalPixels = SCREEN_SIZE * SCREEN_SIZE;

  for (int i = 0; i < totalPixels; i++) {
    uint16_t raw = *(buf + i);
    if (raw == transparentKey) continue;

    uint16_t c = __builtin_bswap16(raw);
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5) & 0x3F;
    uint8_t b = c & 0x1F;
    uint8_t g5 = g >> 1;

    if (abs((int)r - (int)b) <= 2 && abs((int)r - (int)g5) <= 2 && r > 7 && r < 27) {
      *(buf + i) = transparentKey;
    } else if (g5 > b + 4 && (g5 + 4) >= r) {
      float lum = (float)max(r, g5) / 31.0f;
      uint16_t shifted = (((uint8_t)(tr * lum) >> 3) << 11) |
                         (((uint8_t)(tg * lum) >> 2) << 5) |
                         ((uint8_t)(tb * lum) >> 3);
      *(buf + i) = __builtin_bswap16(shifted);
    }
  }

  canvas.fillSprite(DIAL_SIDE_GREY);
  modeSprite.pushSprite(&canvas, 0, 0, TFT_MAGENTA);
  drawCardinalCues(false);
  canvas.pushSprite(0, 0);
}

void updateSpecialModeAnimation() {
if (idleColorMode == MODE_ACTIVE || idleColorMode == MODE_RECHARGE) return;
if (millis() - lastAnimFrame < 25) return;
lastAnimFrame = millis();
float drawAngle = modeAngle;
if (idleColorMode == MODE_SCANNER) {
modeAngle += 3.0f;
if (modeAngle >= 360.0f) modeAngle -= 360.0f;
drawAngle = modeAngle;
}
else if (idleColorMode == MODE_CALIBRATION) {
modeAngle -= 9.0f;
if (modeAngle < 0.0f) modeAngle += 360.0f;
drawAngle = modeAngle;
}
else if (idleColorMode == MODE_EON) {
modeAngle += 0.25f;
drawAngle = sin(modeAngle) * 28.0f + random(-4, 5);
}
canvas.fillSprite(DIAL_SIDE_GREY);
modeSprite.pushRotateZoom(&canvas, SCREEN_SIZE / 2.0f, SCREEN_SIZE / 2.0f, drawAngle, 1.0f, 1.0f, TFT_MAGENTA);
drawCardinalCues(true);
canvas.pushSprite(0, 0);
}

void playOpeningAnimation() {
  uint8_t ar, ag, ab;
  getSlotColor(0, ar, ag, ab);
  uint16_t activeCol = tft.color565(ar, ag, ab);

  int cx = SCREEN_SIZE / 2;
  int cy = SCREEN_SIZE / 2;

  // Exact geometry matching Frame 1 (hourglass) -> Frame 14 (full diamond):
  // Chevron thickness = 18px, slope = 74px horizontal across half-height (120px)
  int thick       = max(4, (SCREEN_SIZE * 18) / 240);
  int slopeX      = (SCREEN_SIZE * 74) / 240;
  int startOffset = (SCREEN_SIZE * 34) / 240; // Hourglass waist half-width at Frame 1
  int endOffset   = (SCREEN_SIZE * 94) / 240; // Diamond horizontal half-width at Frame 14
  int totalTravel = startOffset + endOffset;
  int totalFrames = 24;

  for (int step = 0; step <= totalFrames; step++) {
    serviceAudioMixer();

    float t = (float)step / (float)totalFrames;
    float easedT = 1.0f - pow(1.0f - t, 1.7f);

    // Chevron A starts on LEFT (pointing right '>') and moves RIGHT past center
    int tipA = (cx - startOffset) + (int)(easedT * totalTravel);
    // Chevron B starts on RIGHT (pointing left '<') and moves LEFT past center
    int tipB = (cx + startOffset) - (int)(easedT * totalTravel);

    // 1. Base fill is Active Green (top/bottom triangles + expanding center diamond!)
    canvas.fillSprite(activeCol);

    // 2. Fill the OUTER grey wings (always bounded by the outer-facing V of each chevron):
    // Left grey wing sits to the left of whichever chevron is currently on the left (min(tipA, tipB))
    int leftApex  = min(tipA, tipB);
    int rightApex = max(tipA, tipB);

    canvas.fillTriangle(leftApex - slopeX - thick, 0,
                        leftApex - thick, cy,
                        leftApex - slopeX - thick, SCREEN_SIZE, DIAL_SIDE_GREY);
    if (leftApex - slopeX - thick > 0) {
      canvas.fillRect(0, 0, leftApex - slopeX - thick, SCREEN_SIZE, DIAL_SIDE_GREY);
    }

    canvas.fillTriangle(rightApex + slopeX + thick, 0,
                        rightApex + thick, cy,
                        rightApex + slopeX + thick, SCREEN_SIZE, DIAL_SIDE_GREY);
    if (rightApex + slopeX + thick < SCREEN_SIZE) {
      canvas.fillRect(rightApex + slopeX + thick, 0, SCREEN_SIZE - (rightApex + slopeX + thick), SCREEN_SIZE, DIAL_SIDE_GREY);
    }

    // 3. Draw the two solid BLACK V-Chevrons ('>' at tipA, '<' at tipB) cleanly over the top!
    // Chevron A ('>' pointing right):
    canvas.fillTriangle(tipA - slopeX - thick, 0, tipA - slopeX, 0, tipA, cy, TFT_BLACK);
    canvas.fillTriangle(tipA - slopeX, 0, tipA, cy, tipA + thick, cy, TFT_BLACK);
    canvas.fillTriangle(tipA - slopeX - thick, SCREEN_SIZE, tipA - slopeX, SCREEN_SIZE, tipA, cy, TFT_BLACK);
    canvas.fillTriangle(tipA - slopeX, SCREEN_SIZE, tipA, cy, tipA + thick, cy, TFT_BLACK);

    // Chevron B ('<' pointing left):
    canvas.fillTriangle(tipB + slopeX + thick, 0, tipB + slopeX, 0, tipB, cy, TFT_BLACK);
    canvas.fillTriangle(tipB + slopeX, 0, tipB, cy, tipB - thick, cy, TFT_BLACK);
    canvas.fillTriangle(tipB + slopeX + thick, SCREEN_SIZE, tipB + slopeX, SCREEN_SIZE, tipB, cy, TFT_BLACK);
    canvas.fillTriangle(tipB + slopeX, SCREEN_SIZE, tipB, cy, tipB - thick, cy, TFT_BLACK);

    drawCardinalCues(false);
    canvas.pushSprite(0, 0);
    delay(5);
  }
}


void showCurrentAlien() {
if (!isAlienUnlocked(currentAlien)) {
currentAlien = getNextUnlockedAlien(currentAlien, 1);
}
renderAlienGraphicLayer(currentAlien, hologramMode, MODE_ACTIVE);
drawCardinalCues(false);
canvas.pushSprite(0, 0);
}

void showScannerAlien() {
if (countScannableInactiveAliens() == 0) {
canvas.fillSprite(TFT_BLACK);
canvas.setTextDatum(MC_DATUM);
canvas.setTextColor(TFT_YELLOW);
canvas.setTextSize(max(1, SCREEN_SIZE / 240));
canvas.drawString("ALL DNA CAPTURED", SCREEN_SIZE / 2, SCREEN_SIZE / 2);
drawCardinalCues(false);
canvas.pushSprite(0, 0);
delay(1000);
currentState = IDLE_HOURGLASS;
idleColorMode = MODE_ACTIVE;
prepareIdleModeSprite(MODE_ACTIVE);
return;
}
if (isAlienUnlocked(scannerAlien) || !isValidStandardSlot(scannerAlien)) {
scannerAlien = getNextScannableAlien(0, 1);
}
renderAlienGraphicLayer(scannerAlien, false, MODE_SCANNER);
int cx = SCREEN_SIZE / 2;
int txtScale = max(1, SCREEN_SIZE / 240);
canvas.setTextDatum(MC_DATUM);
canvas.setTextSize(txtScale);
canvas.setTextColor(TFT_WHITE, TFT_BLACK);
canvas.drawString("DNA SCAN", cx, (SCREEN_SIZE * 18) / 240);
canvas.drawString("SWIPE DOWN: ADD", cx, SCREEN_SIZE - ((SCREEN_SIZE * 20) / 240));
drawCardinalCues(false);
canvas.pushSprite(0, 0);
}
void openScannerBrowser() {
if (countScannableInactiveAliens() == 0) {
uint8_t sr, sg, sb;
getSlotColor(2, sr, sg, sb);
tft.fillScreen(tft.color565(sr, sg, sb));
delay(150);
prepareIdleModeSprite(MODE_SCANNER);
return;
}
scannerAlien = getNextScannableAlien(0, 1);
currentState = SCANNER_BROWSE;
showScannerAlien();
}
void captureScannedAlien() {
if (!isValidStandardSlot(scannerAlien)) return;
unlockedAlienMask |= (1ULL << scannerAlien);
currentAlien = scannerAlien;
saveSettingsToFlash();
uint8_t sr, sg, sb;
getSlotColor(2, sr, sg, sb);
tft.fillScreen(tft.color565(sr, sg, sb));
delay(120);
tft.fillScreen(TFT_WHITE);
delay(120);
if (countScannableInactiveAliens() > 0) {
scannerAlien = getNextScannableAlien(scannerAlien, 1);
showScannerAlien();
} else {
currentState = IDLE_HOURGLASS;
idleColorMode = MODE_ACTIVE;
prepareIdleModeSprite(MODE_ACTIVE);
}
}
void triggerRecalibrationShuffle() {
uint8_t cr, cg, cb;
getSlotColor(3, cr, cg, cb);
uint16_t calCol = tft.color565(cr, cg, cb);
tft.fillScreen(calCol);
delay(150);
tft.fillScreen(TFT_WHITE);
delay(180);
tft.fillScreen(calCol);
delay(150);
// Pick 10 random aliens across ALL valid standard & custom slots (0..41 + discovered customs, never Eon 63!)
int totalStandardPool = BUILTIN_STANDARD_COUNT + discoveredCustomCount;
uint64_t newMask = 0;
int picked = 0;
randomSeed(millis() ^ micros());
while (picked < 10 && picked < totalStandardPool) {
int candidate = random(0, totalStandardPool);
if ((newMask & (1ULL << candidate)) == 0) {
newMask |= (1ULL << candidate);
picked++;
}
}
unlockedAlienMask = newMask;
currentAlien = getNextUnlockedAlien(0, 1);
saveSettingsToFlash();
idleColorMode = MODE_ACTIVE;
prepareIdleModeSprite(MODE_ACTIVE);
}
void triggerEonUnlock() {
uint8_t er, eg, eb;
getSlotColor(4, er, eg, eb);
uint16_t eonCol = tft.color565(er, eg, eb);
tft.fillScreen(eonCol);
delay(150);
tft.fillScreen(TFT_WHITE);
delay(180);
tft.fillScreen(eonCol);
delay(150);
unlockedAlienMask |= (1ULL << EON_INDEX);
currentAlien = EON_INDEX;
saveSettingsToFlash();
idleColorMode = MODE_ACTIVE;
prepareIdleModeSprite(MODE_ACTIVE);
}
void removeCurrentAlienFromPlaylist() {
if (countUnlockedAliens() <= 1) {
tft.fillScreen(TFT_YELLOW);
delay(90);
//playSFX("/sounds/idle_hum.mp3");
showCurrentAlien();
return;
}
unlockedAlienMask &= ~(1ULL << currentAlien);
saveSettingsToFlash();
tft.fillScreen(TFT_RED);
delay(110);
currentAlien = getNextUnlockedAlien(currentAlien, 1);
showCurrentAlien();
}
// ==========================================
// COLOR WHEEL & SLEEP HELPERS
// ==========================================
void drawColorWheelUI() {
canvas.fillSprite(TFT_BLACK);
int cx = SCREEN_SIZE / 2;
int cy = SCREEN_SIZE / 2;
int outerR = (SCREEN_SIZE / 2) - max(3, (SCREEN_SIZE * 4) / 240);
int innerR = outerR - ((SCREEN_SIZE * 30) / 240);
int txtScale = max(1, SCREEN_SIZE / 240);
for (int deg = 0; deg < 360; deg++) {
float rad = deg * DEG_TO_RAD;
uint8_t r, g, b;
hsvToRgb(deg / 360.0f, 1.0f, 1.0f, r, g, b);
uint16_t col = tft.color565(r, g, b);
int x1 = cx + cos(rad) * innerR;
int y1 = cy + sin(rad) * innerR;
int x2 = cx + cos(rad) * outerR;
int y2 = cy + sin(rad) * outerR;
canvas.drawLine(x1, y1, x2, y2, col);
canvas.drawLine(x1 + 1, y1, x2 + 1, y2, col);
}
uint8_t pr, pg, pb;
getSlotColor(editSlot, pr, pg, pb);
int previewR = (SCREEN_SIZE * 20) / 240;
int previewY = cy - ((SCREEN_SIZE * 34) / 240);
canvas.fillCircle(cx, previewY, previewR, tft.color565(pr, pg, pb));
canvas.drawCircle(cx, previewY, previewR + 1, TFT_WHITE);
String slotName = "1: ACTIVE";
if (editSlot == 1) slotName = "2: RECHARGE";
else if (editSlot == 2) slotName = "3: SCANNER";
else if (editSlot == 3) slotName = "4: CALIBRATE";
else if (editSlot == 4) slotName = "5: EON/MAGIC";
else if (editSlot == 5) slotName = "6: TRANSFORM";
canvas.setTextDatum(MC_DATUM);
canvas.setTextColor(TFT_WHITE, TFT_BLACK);
canvas.setTextSize(txtScale);
canvas.drawString(slotName, cx, cy - ((SCREEN_SIZE * 4) / 240));
int barW = (SCREEN_SIZE * 130) / 240;
int barH = max(12, (SCREEN_SIZE * 16) / 240);
int barX = cx - (barW / 2);
int barY = cy + ((SCREEN_SIZE * 8) / 240);
for (int x = 0; x < barW; x++) {
uint8_t shade = 255 - ((x * 255) / (barW - 1));
canvas.drawFastVLine(barX + x, barY, barH, tft.color565(shade, shade, shade));
}
canvas.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, TFT_WHITE);
int btnW = (SCREEN_SIZE * 68) / 240;
int btnH = (SCREEN_SIZE * 22) / 240;
int btnY = cy + ((SCREEN_SIZE * 34) / 240);
canvas.fillRoundRect(cx - (btnW / 2), btnY, btnW, btnH, max(4, (SCREEN_SIZE * 6) / 240), TFT_DARKGREEN);
canvas.setTextColor(TFT_WHITE);
canvas.drawString("SAVE", cx, btnY + (btnH / 2));
drawCardinalCues(false);
canvas.pushSprite(0, 0);
}

// ==========================================
// WATCH MODE (CLOCK + BATTERY) & LARGE PAGED SETTINGS
// ==========================================
void drawWatchModeScreen(bool forceFullRedraw = false) {
  if (!forceFullRedraw && (millis() - lastWatchDraw < 1000)) return;
  lastWatchDraw = millis();

  // Render clean base launchAnim0001 hourglass (respects your custom Active color!)
renderKeyedAnimFrameToModeSprite("/launchAnim0001.png", lowPowerLockout ? MODE_RECHARGE : MODE_ACTIVE);
  canvas.fillSprite(DIAL_SIDE_GREY);
  modeSprite.pushSprite(&canvas, 0, 0, TFT_MAGENTA);

  int cx = SCREEN_SIZE / 2;
  int cy = SCREEN_SIZE / 2;
  int unit = max(1, SCREEN_SIZE / 240);

  // Calculate current clock time from sync point
  unsigned long elapsedSec = (millis() - clockSyncMillis) / 1000;
  unsigned long totalMins = (clockBaseHour % 12) * 60 + clockBaseMin + (clockBasePM ? 720 : 0) + (elapsedSec / 60);
  totalMins %= 1440;
  int curH24 = totalMins / 60;
  int curM   = totalMins % 60;
  bool isPM  = (curH24 >= 12);
  int curH12 = curH24 % 12;
  if (curH12 == 0) curH12 = 12;

  String hourStr = String(curH12);
  String minStr  = (curM < 10) ? ("0" + String(curM)) : String(curM);

  canvas.setTextDatum(MC_DATUM);

  // 1. LEFT GREY WING: Large Hour Digits (with subtle black outline for crisp contrast)
  int leftX = (SCREEN_SIZE * 44) / 240;
  canvas.setTextSize(unit * 3);
  canvas.setTextColor(TFT_BLACK);
  canvas.drawString(hourStr, leftX + 2, cy + 2);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(hourStr, leftX, cy);

  // 2. RIGHT GREY WING: Large Minute Digits
  int rightX = SCREEN_SIZE - leftX;
  canvas.setTextColor(TFT_BLACK);
  canvas.drawString(minStr, rightX + 2, cy + 2);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(minStr, rightX, cy);

  // 3. TOP GREEN TRIANGLE: AM / PM
  int topY = (SCREEN_SIZE * 32) / 240;
  canvas.setTextSize(unit * 2);
  canvas.setTextColor(TFT_BLACK);
  canvas.drawString(isPM ? "PM" : "AM", cx, topY);

  // 4. BOTTOM GREEN TRIANGLE: Battery % (Black text inside glowing green triangle)
  int soc = readBatteryPercent();
  String battText = (soc >= 0) ? (String(soc) + "%") : "USB";
  int botY = SCREEN_SIZE - ((SCREEN_SIZE * 30) / 240);
  canvas.setTextSize(unit);
  canvas.setTextColor(TFT_BLACK);
  canvas.drawString(battText, cx, botY);

  canvas.pushSprite(0, 0);
}


void drawClockSetUI() {
  canvas.fillSprite(TFT_BLACK);
  int cx = SCREEN_SIZE / 2;
  int cy = SCREEN_SIZE / 2;
  int txtScale = max(1, SCREEN_SIZE / 240);

  uint8_t ar, ag, ab;
  getSlotColor(0, ar, ag, ab);
  uint16_t accent = tft.color565(ar, ag, ab);

  canvas.setTextDatum(MC_DATUM);
  canvas.setTextSize(txtScale);
  canvas.setTextColor(accent, TFT_BLACK);
  canvas.drawString("SET WATCH TIME", cx, (SCREEN_SIZE * 32) / 240);

  // Highlight active field (0=Hour, 1=Min, 2=AM/PM)
  char hBuf[6], mBuf[6];
  snprintf(hBuf, sizeof(hBuf), "%d", clockBaseHour);
  snprintf(mBuf, sizeof(mBuf), "%02d", clockBaseMin);

  int boxW = (SCREEN_SIZE * 46) / 240;
  int boxH = (SCREEN_SIZE * 36) / 240;
  int rowY = cy - (boxH / 2);

  canvas.fillRoundRect(cx - 76, rowY, boxW, boxH, 6, clockEditField == 0 ? TFT_DARKGREEN : 0x2104);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(String(hBuf), cx - 76 + boxW / 2, cy);

  canvas.drawString(":", cx - 23, cy);

  canvas.fillRoundRect(cx - 16, rowY, boxW, boxH, 6, clockEditField == 1 ? TFT_DARKGREEN : 0x2104);
  canvas.drawString(String(mBuf), cx - 16 + boxW / 2, cy);

  canvas.fillRoundRect(cx + 36, rowY, boxW, boxH, 6, clockEditField == 2 ? TFT_DARKGREEN : 0x2104);
  canvas.drawString(clockBasePM ? "PM" : "AM", cx + 36 + boxW / 2, cy);

  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("SWIPE UP/DOWN: ADJ", cx, cy + (SCREEN_SIZE * 34) / 240);

  int saveW = (SCREEN_SIZE * 84) / 240;
  int saveH = (SCREEN_SIZE * 28) / 240;
  int saveY = SCREEN_SIZE - (SCREEN_SIZE * 46) / 240;
  canvas.fillRoundRect(cx - saveW / 2, saveY, saveW, saveH, 6, TFT_DARKGREEN);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString("DONE", cx, saveY + saveH / 2);

  canvas.pushSprite(0, 0);
}

void drawSettingsMenuUI() {
  canvas.fillSprite(TFT_BLACK);
  int cx = SCREEN_SIZE / 2;
  int txtScale = max(1, SCREEN_SIZE / 240);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextSize(txtScale);

  uint8_t ar, ag, ab;
  getSlotColor(0, ar, ag, ab);
  uint16_t accent = tft.color565(ar, ag, ab);

  canvas.setTextColor(accent, TFT_BLACK);
  canvas.drawString(settingsPage == 0 ? "SETTINGS (1/2)" : "SETTINGS (2/2)", cx, (SCREEN_SIZE * 24) / 240);

  // Large, easy-to-tap rows (38px tall each!)
  int rowH = (SCREEN_SIZE * 38) / 240;
  int boxW = (SCREEN_SIZE * 184) / 240;
  int startY = (SCREEN_SIZE * 44) / 240;

  if (settingsPage == 0) {
    // Row 0: Color Customizer
    canvas.fillRoundRect(cx - boxW / 2, startY, boxW, rowH - 5, 8, 0x2104);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("COLORS >", cx, startY + (rowH / 2) - 2);

    // Row 1: BGM Theme Toggle
    int y1 = startY + rowH;
    canvas.fillRoundRect(cx - boxW / 2, y1, boxW, rowH - 5, 8, themeMusicEnabled ? TFT_DARKGREEN : 0x2104);
    canvas.drawString(themeMusicEnabled ? "BGM: ON" : "BGM: OFF", cx, y1 + (rowH / 2) - 2);

    // Row 2: Volume & Mute (< VOL 85% > | Tap Mute)
    int y2 = startY + (rowH * 2);
    int volPct = (int)(masterVolume * 100.0f + 0.5f);
    canvas.fillRoundRect(cx - boxW / 2, y2, boxW, rowH - 5, 8, masterMuted ? TFT_MAROON : 0x2104);
    canvas.drawString(masterMuted ? "MUTED (TAP)" : ("< VOL: " + String(volPct) + "% >"), cx, y2 + (rowH / 2) - 2);

    // Row 3: Series Playlist Presets (< CLASSIC S1 >)
    int y3 = startY + (rowH * 3);
    canvas.fillRoundRect(cx - boxW / 2, y3, boxW, rowH - 5, 8, 0x2104);
    canvas.setTextColor(accent);
    canvas.drawString("< " + getPresetPlaylistName(presetRosterIndex) + " >", cx, y3 + (rowH / 2) - 2);
  } else {
    // PAGE 2
    // Row 0: Watch Mode (Pocket Lock + Clock/Battery)
    canvas.fillRoundRect(cx - boxW / 2, startY, boxW, rowH - 5, 8, TFT_NAVY);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("WATCH MODE >", cx, startY + (rowH / 2) - 2);

    // Row 1: Set Watch Clock
    int y1 = startY + rowH;
    canvas.fillRoundRect(cx - boxW / 2, y1, boxW, rowH - 5, 8, 0x2104);
    canvas.drawString("SET CLOCK >", cx, y1 + (rowH / 2) - 2);

    // Row 2: Cardinal Mode Dots (SHOW / HIDE)
    int y2 = startY + (rowH * 2);
    canvas.fillRoundRect(cx - boxW / 2, y2, boxW, rowH - 5, 8, 0x2104);
    canvas.drawString(hideCardinalCues ? "MODE DOTS: HIDE" : "MODE DOTS: SHOW", cx, y2 + (rowH / 2) - 2);

    // Row 3: Quick Mute Toggle
    int y3 = startY + (rowH * 3);
    canvas.fillRoundRect(cx - boxW / 2, y3, boxW, rowH - 5, 8, masterMuted ? TFT_MAROON : 0x2104);
    canvas.drawString(masterMuted ? "SOUND: MUTED" : "SOUND: ACTIVE", cx, y3 + (rowH / 2) - 2);
  }

  // Bottom Navigation Bar: Page Switcher + Close
  int navY = startY + (rowH * 4) + 2;
  int navH = (SCREEN_SIZE * 28) / 240;
  canvas.fillRoundRect(cx - 78, navY, 72, navH, 6, 0x3186);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(settingsPage == 0 ? "NEXT >" : "< PREV", cx - 42, navY + navH / 2);

  canvas.fillRoundRect(cx + 6, navY, 72, navH, 6, TFT_DARKGREY);
  canvas.drawString("CLOSE", cx + 42, navY + navH / 2);

  canvas.pushSprite(0, 0);
}



String getPresetPlaylistName(int idx) {
  switch (idx) {
    case 0: return "CLASSIC S1";
    case 1: return "CLASSIC END";
    case 2: return "ALIEN FORCE";
    case 3: return "OMNIVERSE";
    case 4: return "REBOOT 10";
    default: return "CLASSIC S1";
  }
}

void applyPresetPlaylist(int idx) {
  presetRosterIndex = (idx + 5) % 5;
  uint64_t mask = 0;

  if (presetRosterIndex == 0) {
    // Classic S1: Original 10 (bits 0..9)
    mask = 0x00000000000003FFULL;
  } 
  else if (presetRosterIndex == 1) {
    // Classic Season 4 End: Original 10 MINUS Ghostfreak (bit 8) PLUS Cannonbolt..Way Big (bits 10..18)
    mask = (0x00000000000003FFULL & ~(1ULL << 8)) | 0x000000000007FC00ULL;
  } 
  else if (presetRosterIndex == 2) {
    // Alien Force 10: Swampfire..Alien X (bits 22..31)
    for (int b = 22; b <= 31; b++) mask |= (1ULL << b);
  } 
  else if (presetRosterIndex == 3) {
    // Omniverse / UA 10: Rath..Feedback (bits 32..41)
    for (int b = 32; b <= 41; b++) mask |= (1ULL << b);
  } 
  else if (presetRosterIndex == 4) {
    // Reboot 10: Heatblast(0), FourArms(1), XLR8(2), Diamondhead(3), Upgrade(4), GreyMatter(5), Stinkfly(7), Cannonbolt(10), Wildvine(11), WaterHazard(35)
    mask = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3) | (1ULL << 4) |
           (1ULL << 5) | (1ULL << 7) | (1ULL << 10) | (1ULL << 11) | (1ULL << 35);
  }

  unlockedAlienMask = mask;
  currentAlien = getNextUnlockedAlien(0, 1);
  saveSettingsToFlash();
}

void enterSleepMode() {
currentState = SLEEP_MODE;
for (int b = 220; b >= 0; b -= 20) {
tft.setBrightness(b);
delay(15);
}
tft.fillScreen(TFT_BLACK);
tft.setBrightness(0);
}
void wakeFromSleep() {
currentState = IDLE_HOURGLASS;
idleColorMode = MODE_ACTIVE;
lastActivityTime = millis();
prepareIdleModeSprite(MODE_ACTIVE);
tft.setBrightness(220);
}
   void activateDial() {
     currentState = SELECT_ALIEN;
     playSFX("/sounds/ready.mp3");
     playOpeningAnimation();
     setIdleHumActive(true);
     showCurrentAlien();
   }

void advanceManualTransformCycle() {
  if (currentState == TRANSFORMED) {
    currentState = TIMEOUT_WARNING;
    stateTimer = millis();
    playSFX("/sounds/warning_beep.mp3");
    prepareTransformedHourglass();
  } 
  else if (currentState == TIMEOUT_WARNING) {
    uint8_t rr, rg, rb;
    getSlotColor(1, rr, rg, rb);
    playSFX("/sounds/power_down.mp3");

    tft.fillScreen(TFT_WHITE);
    pumpAudioDelay(90);
    tft.fillScreen(tft.color565(rr, rg, rb));
    pumpAudioDelay(120);

    currentState = RECHARGE;
    prepareIdleModeSprite(MODE_RECHARGE);
  } 
  else if (currentState == RECHARGE) {
    currentState = IDLE_HOURGLASS;
    idleColorMode = MODE_ACTIVE;
    playSFX("/sounds/recharged.mp3");
    lastActivityTime = millis();
    prepareIdleModeSprite(MODE_ACTIVE);
  }
}


void triggerTransform() {
  currentState = TRANSFORMED;
  setIdleHumActive(false);
  stateTimer = millis();
  uint8_t ar, ag, ab;
  getSlotColor(0, ar, ag, ab);
  uint16_t activeCol = tft.color565(ar, ag, ab);

  playSFX("/sounds/transform.mp3");
  tft.fillScreen(activeCol);
  pumpAudioDelay(120);
  tft.fillScreen(TFT_WHITE);
  pumpAudioDelay(250);

  // Settle on the White-center hourglass (launchAnim0001) until tapped!
  prepareTransformedHourglass();
}

// ==========================================
// MAIN SETUP & LOOP
// ==========================================
void setup() {
Serial.begin(115200);
pinMode(SIDE_BTN_PIN, INPUT_PULLUP);
tft.init();
tft.setBrightness(220);
SCREEN_SIZE = min(tft.width(), tft.height());
canvas.setPsram(true);
canvas.setColorDepth(16);
canvas.createSprite(SCREEN_SIZE, SCREEN_SIZE);
modeSprite.setPsram(true);
modeSprite.setColorDepth(16);
modeSprite.createSprite(SCREEN_SIZE, SCREEN_SIZE);
if (!LittleFS.begin(false)) {
tft.fillScreen(TFT_BLACK);
tft.setTextColor(TFT_RED);
tft.setTextDatum(MC_DATUM);
tft.drawString("LittleFS Mount Failed", SCREEN_SIZE / 2, SCREEN_SIZE / 2);
while (1) delay(100);
}
// Auto-discover any custom alien PNGs added to LittleFS before loading playlist!
discoverCustomAliensInLittleFS();
loadSettingsFromFlash();
initClockFromCompileTime();
touch.begin();
lastActivityTime = millis();
initAudioSystem();
lastBatteryCheck = millis() - 55000; // First battery check runs 5s after boot


  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  playSFX("/sounds/ready.mp3");
  unsigned long bootAudioTimer = millis();
  while (millis() - bootAudioTimer < 1500) {
    serviceAudioMixer();
    delay(2);
  }




prepareIdleModeSprite(MODE_ACTIVE);
}

bool lowBattWarned = false;

void loop() {
  serviceAudioMixer();
  // Check BQ27220 Fuel Gauge every 60s (5s after boot) for Low-Power Recharge Lockout (<= 15%)
  if (millis() - lastBatteryCheck > 60000) {
    lastBatteryCheck = millis();
    int soc = readBatteryPercent();

    if (soc > 0 && soc <= 15) {
      bool justTriggered = !lowPowerLockout;
      lowPowerLockout = true;

      if (justTriggered) {
        setIdleHumActive(false);
        if (currentState == SLEEP_MODE) wakeFromSleep();
        if (currentState == WATCH_MODE) {
          drawWatchModeScreen(true);
        } else if (currentState != SETTINGS_MENU && currentState != CLOCK_SET && currentState != COLOR_PICKER) {
          currentState = IDLE_HOURGLASS;
          idleColorMode = MODE_RECHARGE;
          prepareIdleModeSprite(MODE_RECHARGE);
        }
      }

      if (!watchModeActive && !masterMuted) {
        playSFX("/sounds/low_battery.mp3");
      }
    } 
    else if (soc >= 18 && lowPowerLockout) {
      // Recovered above 18% on charger -> Unlock Omnitrix & play recharged!
      lowPowerLockout = false;
      if (currentState == WATCH_MODE) {
        drawWatchModeScreen(true);
      } else if (currentState == IDLE_HOURGLASS) {
        idleColorMode = MODE_ACTIVE;
        prepareIdleModeSprite(MODE_ACTIVE);
        playSFX("/sounds/recharged.mp3");
      }
    }
  }

// 1. Action Button: Single purpose -> Open / Close Settings Menu anytime!
bool btnPressed = (digitalRead(SIDE_BTN_PIN) == LOW);
if (btnPressed) {
  lastActivityTime = millis();
  if (currentState == SLEEP_MODE) {
    wakeFromSleep();
    lastBtnPress = millis();
    return;
  }
  if (btnDownTime == 0 && (millis() - lastBtnPress > 250)) {
    btnDownTime = millis();
    lastBtnPress = millis();
    if (currentState == WATCH_MODE) {
      watchModeActive = false;
      refreshBgmChannel();
      currentState = IDLE_HOURGLASS;
      idleColorMode = lowPowerLockout ? MODE_RECHARGE : MODE_ACTIVE;
      prepareIdleModeSprite(idleColorMode);
    } else if (currentState == SETTINGS_MENU || currentState == COLOR_PICKER || currentState == CLOCK_SET) {
      saveSettingsToFlash();
      currentState = IDLE_HOURGLASS;
      idleColorMode = MODE_ACTIVE;
      prepareIdleModeSprite(MODE_ACTIVE);
    } else {
      setIdleHumActive(false);
      settingsPage = 0;
      currentState = SETTINGS_MENU;
      drawSettingsMenuUI();
    }
  }

} else {
  btnDownTime = 0;
}

// 2. Capacitive Touch Handling
if (touch.available()) {
if (currentState == WATCH_MODE) return;

lastActivityTime = millis();
if (currentState == SLEEP_MODE) {
wakeFromSleep();
return;
}
String gesture = touch.gesture();
int tx = touch.data.x;
int ty = touch.data.y;

if (currentState == SETTINGS_MENU) {
  int cx = SCREEN_SIZE / 2;
  int rowH = (SCREEN_SIZE * 38) / 240;
  int startY = (SCREEN_SIZE * 44) / 240;
  int row = (ty - startY) / rowH;

  // Swipe Left/Right anywhere in Settings switches pages (unless swiping on Volume or Preset rows)
  if (row != 2 && row != 3 && (gesture == "SWIPE LEFT" || gesture == "SWIPE RIGHT")) {
    settingsPage = (settingsPage == 0) ? 1 : 0;
    drawSettingsMenuUI();
  }
  else if (ty >= startY && row >= 0 && row <= 3) {
    if (settingsPage == 0) {
      if (row == 0 && gesture == "SINGLE CLICK") {
        currentState = COLOR_PICKER;
        editSlot = 0;
        drawColorWheelUI();
      } else if (row == 1 && gesture == "SINGLE CLICK") {
        toggleThemeMusic();
        drawSettingsMenuUI();
      } else if (row == 2) {
        if (gesture == "SWIPE LEFT" || (gesture == "SINGLE CLICK" && tx < cx - 35)) {
          masterMuted = false;
          masterVolume = max(0.10f, masterVolume - 0.10f);
        } else if (gesture == "SWIPE RIGHT" || (gesture == "SINGLE CLICK" && tx > cx + 35)) {
          masterMuted = false;
          masterVolume = min(1.00f, masterVolume + 0.10f);
        } else if (gesture == "SINGLE CLICK") {
          masterMuted = !masterMuted;
        }
        refreshBgmChannel();
        saveSettingsToFlash();
        if (!masterMuted) playRandomDialClick();
        drawSettingsMenuUI();
      } else if (row == 3) {
        if (gesture == "SWIPE LEFT" || (gesture == "SINGLE CLICK" && tx < cx)) {
          applyPresetPlaylist(presetRosterIndex - 1);
        } else {
          applyPresetPlaylist(presetRosterIndex + 1);
        }
        playRandomDialClick();
        drawSettingsMenuUI();
      }
    } else {
      // PAGE 2
      if (row == 0 && gesture == "SINGLE CLICK") {
        // Activate Pocket-Safe Watch Mode!
        watchModeActive = true;
        refreshBgmChannel();
        currentState = WATCH_MODE;
        drawWatchModeScreen(true);
      } else if (row == 1 && gesture == "SINGLE CLICK") {
        clockEditField = 0;
        currentState = CLOCK_SET;
        drawClockSetUI();
      } else if (row == 2 && gesture == "SINGLE CLICK") {
        hideCardinalCues = !hideCardinalCues;
        saveSettingsToFlash();
        drawSettingsMenuUI();
      } else if (row == 3 && gesture == "SINGLE CLICK") {
        masterMuted = !masterMuted;
        refreshBgmChannel();
        saveSettingsToFlash();
        drawSettingsMenuUI();
      }
    }
  } 
  else if (row >= 4 && gesture == "SINGLE CLICK") {
    if (tx < cx) {
      settingsPage = (settingsPage == 0) ? 1 : 0;
      drawSettingsMenuUI();
    } else {
      saveSettingsToFlash();
      currentState = IDLE_HOURGLASS;
      idleColorMode = MODE_ACTIVE;
      prepareIdleModeSprite(MODE_ACTIVE);
    }
  }
}
else if (currentState == CLOCK_SET) {
  int cx = SCREEN_SIZE / 2;
  int cy = SCREEN_SIZE / 2;
  if (gesture == "SWIPE UP") {
    if (clockEditField == 0) clockBaseHour = (clockBaseHour % 12) + 1;
    else if (clockEditField == 1) clockBaseMin = (clockBaseMin + 1) % 60;
    else clockBasePM = !clockBasePM;
    clockSyncMillis = millis();
    drawClockSetUI();
  } else if (gesture == "SWIPE DOWN") {
    if (clockEditField == 0) clockBaseHour = (clockBaseHour == 1) ? 12 : (clockBaseHour - 1);
    else if (clockEditField == 1) clockBaseMin = (clockBaseMin == 0) ? 59 : (clockBaseMin - 1);
    else clockBasePM = !clockBasePM;
    clockSyncMillis = millis();
    drawClockSetUI();
  } else if (gesture == "SINGLE CLICK") {
    if (ty > SCREEN_SIZE - (SCREEN_SIZE * 55) / 240) {
      // DONE button tapped
      clockSyncMillis = millis();
      currentState = SETTINGS_MENU;
      drawSettingsMenuUI();
    } else if (tx < cx - 25) {
      clockEditField = 0;
      drawClockSetUI();
    } else if (tx < cx + 30) {
      clockEditField = 1;
      drawClockSetUI();
    } else {
      clockEditField = 2;
      drawClockSetUI();
    }
  }
}

else if (currentState == COLOR_PICKER) {
int cx = SCREEN_SIZE / 2;
int cy = SCREEN_SIZE / 2;
int dx = tx - cx;
int dy = ty - cy;
float dist = sqrt(dx * dx + dy * dy);
int outerR = (SCREEN_SIZE / 2) - max(3, (SCREEN_SIZE * 4) / 240);
int innerR = outerR - ((SCREEN_SIZE * 30) / 240);
int barW = (SCREEN_SIZE * 130) / 240;
int barHalfW = barW / 2;
int barTop = cy + ((SCREEN_SIZE * 4) / 240);
int barBot = cy + ((SCREEN_SIZE * 28) / 240);
int saveTop = cy + ((SCREEN_SIZE * 32) / 240);
int saveBot = cy + ((SCREEN_SIZE * 62) / 240);
int saveHalfW = (SCREEN_SIZE * 38) / 240;
if (dist >= innerR - 5 && dist <= outerR + 5) {
float angle = atan2(dy, dx) * RAD_TO_DEG;
if (angle < 0) angle += 360.0f;
pickerHue = angle / 360.0f;
uint8_t r, g, b;
hsvToRgb(pickerHue, 1.0f, 1.0f, r, g, b);
setSlotColor(editSlot, r, g, b);
drawColorWheelUI();
}
else if (ty >= barTop && ty <= barBot && tx >= cx - barHalfW - 3 && tx <= cx + barHalfW + 3) {
int clampedX = constrain(tx, cx - barHalfW, cx + barHalfW);
uint8_t shade = 255 - (((clampedX - (cx - barHalfW)) * 255) / barW);
setSlotColor(editSlot, shade, shade, shade);
drawColorWheelUI();
}
else if (ty >= saveTop && ty <= saveBot && abs(dx) <= saveHalfW) {
saveSettingsToFlash();
currentState = IDLE_HOURGLASS;
idleColorMode = MODE_ACTIVE;
prepareIdleModeSprite(MODE_ACTIVE);
}
else if (ty < barTop && dist < innerR - 6 && gesture == "SINGLE CLICK") {
editSlot = (editSlot + 1) % 6;
drawColorWheelUI();
}
}
else if (currentState == IDLE_HOURGLASS) {
  if (lowPowerLockout) {
    // Toy functions disabled below 15% battery—tap repeats low battery warning!
    if (gesture == "SINGLE CLICK") {
      playSFX("/sounds/low_battery.mp3");
    }
    return;
  }

if (gesture == "SINGLE CLICK") {
if (idleColorMode == MODE_ACTIVE) {
activateDial();
} else if (idleColorMode == MODE_SCANNER) {
openScannerBrowser();
} else if (idleColorMode == MODE_CALIBRATION) {
triggerRecalibrationShuffle();
} else if (idleColorMode == MODE_EON) {
triggerEonUnlock();
}
}
else if (gesture == "DOUBLE CLICK") {
currentState = COLOR_PICKER;
editSlot = 0;
drawColorWheelUI();
}
else if (gesture == "SWIPE UP") {
idleColorMode = MODE_SCANNER;
playSFX("/sounds/recalibrated.mp3");
prepareIdleModeSprite(MODE_SCANNER);
}
else if (gesture == "SWIPE LEFT") {
idleColorMode = MODE_CALIBRATION;
playSFX("/sounds/recalibrated.mp3");
prepareIdleModeSprite(MODE_CALIBRATION);
}
else if (gesture == "SWIPE DOWN") {
idleColorMode = MODE_EON;
playSFX("/sounds/recalibrated.mp3");
prepareIdleModeSprite(MODE_EON);
}
else if (gesture == "SWIPE RIGHT") {
resetDefaultsAndRoster();
tft.fillScreen(TFT_WHITE);
delay(80);
idleColorMode = MODE_ACTIVE;
prepareIdleModeSprite(MODE_ACTIVE);
}
}
else if (currentState == SCANNER_BROWSE) {  
if (gesture == "SWIPE LEFT") {
scannerAlien = getNextScannableAlien(scannerAlien, 1);
showScannerAlien();
} else if (gesture == "SWIPE RIGHT") {
scannerAlien = getNextScannableAlien(scannerAlien, -1);
showScannerAlien();
} else if (gesture == "SWIPE DOWN") {
captureScannedAlien();
} else if (gesture == "SWIPE UP" || gesture == "LONG PRESS") {
currentState = IDLE_HOURGLASS;
idleColorMode = MODE_ACTIVE;
prepareIdleModeSprite(MODE_ACTIVE);
}
}
else if (currentState == SELECT_ALIEN) {
if (gesture == "SWIPE LEFT") {
playRandomDialClick();
currentAlien = getNextUnlockedAlien(currentAlien, 1);
showCurrentAlien();
} else if (gesture == "SWIPE RIGHT") {
playRandomDialClick();
currentAlien = getNextUnlockedAlien(currentAlien, -1);
showCurrentAlien();
} else if (gesture == "SWIPE UP") {
removeCurrentAlienFromPlaylist();
} else if (gesture == "LONG PRESS") {
hologramMode = !hologramMode;
showCurrentAlien();
} else if (gesture == "SINGLE CLICK") {
triggerTransform();
}
}
  else if (currentState == TRANSFORMED || currentState == TIMEOUT_WARNING || currentState == RECHARGE) {
    if (gesture == "SINGLE CLICK") {
      advanceManualTransformCycle();
    }
  }

}
// 3. Run Continuous Animations & Sleep Timer
if (currentState == IDLE_HOURGLASS) {
updateSpecialModeAnimation();
if (millis() - lastActivityTime > SLEEP_TIMEOUT_MS) {
enterSleepMode();
}
}
else if (currentState == WATCH_MODE) { drawWatchModeScreen(false); }

  // 4. Timeout Warning Pulse & 2.0-Second Audio Beep Loop (Stalls until tapped!)
  static unsigned long lastPulseToggle = 0;
  static bool warningWhitePhase = true;

  if (currentState == TIMEOUT_WARNING) {
    // Restart power_down.mp3 every 2000ms so only the first 2 seconds of warning beeps loop!
    if (millis() - stateTimer >= 2000) {
      stateTimer = millis();
      playSFX("/sounds/warning_beep.mp3");
    }

    // Pulse hourglass between White and Recharge color every 250ms
    if (millis() - lastPulseToggle >= 250) {
      lastPulseToggle = millis();
      warningWhitePhase = !warningWhitePhase;
      if (warningWhitePhase) {
        prepareTransformedHourglass();
      } else {
        prepareIdleModeSprite(MODE_RECHARGE);
      }
    }
  }
} // <-- Add this final closing brace for void loop()

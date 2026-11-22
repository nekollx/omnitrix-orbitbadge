# Omnitrix OS for OrbitBadge (ESP32-S3)

A screen-accurate, interactive Omnitrix firmware for the **OrbitBadge** (ESP32-S3 + 1.28" 240×240 GC9A01 round touch display + ES8311/AW8010 audio). Includes dual-channel MP3 sound effects and background music, a 64-alien expandable roster, canon series presets, a 6-slot RGB color wheel, manual timeout stalling, and a pocket-safe digital Watch Mode.

---

## 1-Click Browser Install (Recommended)

You do **not** need Arduino IDE or any programming tools to install the default firmware:

1. Open the **[Omnitrix Web Installer](https://nekollx.github.io/omnitrix-orbitbadge/)** in **Google Chrome** or **Microsoft Edge** (Windows or macOS).
2. Switch the physical **Power Slide Switch ON** on your OrbitBadge (required to power the speaker amplifier) and plug it into your computer with a data USB-C cable.
3. Click **INSTALL OMNITRIX**, choose the **ESP32-S3 / USB JTAG** serial port, and wait ~30 seconds for flashing to finish.

---

## Controls & Gestures

### Idle Hourglass Screen
* **Single Tap:** Launch the Omnitrix dial (plays `ready.mp3` + chevron pass-through animation).
* **Swipe Up (Bottom Yellow Triangle):** Enter **DNA Scanner Mode** (spins yellow; tap to browse and capture locked aliens).
* **Swipe Left (Right Blue Diamond):** Enter **Recalibration Mode** (spins blue; tap to randomly shuffle 10 new aliens into your active playlist).
* **Swipe Down (Top Purple Dot):** Enter **Eon Mode** (wobbles purple; tap to unlock secret Eon).
* **Swipe Right (Left Red Square):** **Factory Reset** (restores default colors and the Classic Season 1 playlist).

### Alien Selection Dial
* **Swipe Left / Right:** Cycle through unlocked aliens with alternating dial click effects and ambient idle hum.
* **Swipe Up:** Remove the current alien from your active playlist.
* **Long Press:** Toggle **Hologram Mode** (pure black background + glowing tinted silhouette for Pepper's Ghost prisms).
* **Single Tap:** **Transform!**

### Manual Transformation & Timeout Stalling
* **After Transforming:** Displays the transformed hourglass (default pure white; customizable in the Color Wheel) until you tap.
* **Tap 1 (Stall Warning):** Enters **Timeout Warning**—alternates the hourglass between your Transformed color and Recharge color while looping warning beeps for as long as you stall.
* **Tap 2 (Power Down):** Plays the power-down sound and locks into **Recharge Mode**.
* **Tap 3 (Recharged):** Plays the recharge chime and snaps back to **Active Ready Mode**.

### Physical Action Button (`GPIO 0`) — Settings Menu
Press the side Action Button at any time to open or close the 2-page **Settings Menu**:
* **Page 1:**
  * **COLORS:** Customize all 6 mode colors (`1: Active`, `2: Recharge`, `3: Scanner`, `4: Calibrate`, `5: Eon/Magic`, `6: Transform`) using the hue wheel or white-to-black brightness bar.
  * **BGM ON/OFF:** Toggle the background theme music alongside sound effects.
  * **VOL / MUTE:** Adjust master volume (`10%–100%`) or mute all audio.
  * **PLAYLIST PRESETS:** Switch between **Classic S1**, **Classic End** (Season 4 roster minus Ghostfreak), **Alien Force**, **Omniverse**, and **Reboot 10**.
* **Page 2:**
  * **WATCH MODE:** Locks out accidental pocket touches, silences toy SFX, and displays a live **Hour | Minute | AM/PM | Battery %** watch face on the hourglass. Press the physical Action Button to exit Watch Mode.
  * **SET CLOCK:** Manually adjust the watch hour, minute, and AM/PM *(automatically syncs to your PC's clock when compiled from source)*.
  * **MODE DOTS:** Show or hide the 4 colored cardinal edge cues on the idle screen.
  * **QUICK MUTE:** Toggle silent mode.

*(Note: When battery charge drops to **≤ 15%**, the Omnitrix automatically enters Low-Power Recharge Lockout until charged back above 18%.)*

---

## Adding Custom Aliens (No Coding Required)

The firmware automatically scans flash storage on boot and supports up to **21 custom aliens** in addition to the 43 built-in characters:

1. Create a square PNG sprite (`240×240` recommended) with a **glowing green silhouette on a pure black background**, oriented **upside-down** (so it projects upright in both standard silhouette mode and Pepper's Ghost prism mode).
2. Name the file `<alienname>_holo.png` (lowercase, underscores for spaces—for example, `shockrock_holo.png`).
3. Drop it into the `omnitrix/data/` folder and re-upload LittleFS—it will automatically appear in your **DNA Scanner** and **Recalibration** pools!

---

## Building From Source (Arduino IDE)

If you want to customize the code or add your own `.png` / `.mp3` files:

### Required Libraries
* **LovyanGFX**
* **CST816S**
* **ESP8266Audio** (by Earle F. Philhower)
* **arduino-littlefs-upload** plugin for Arduino IDE 2.x

### Arduino IDE Board Settings (`ESP32S3 Dev Module`)
* **USB CDC On Boot:** `Enabled`
* **Flash Size:** `16MB (128Mb)`
* **PSRAM:** `OPI PSRAM`
* **Partition Scheme:** `Custom` *(uses the included `partitions.csv` for 12.7 MB of LittleFS storage)*

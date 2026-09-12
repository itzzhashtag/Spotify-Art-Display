<div align="center">

# 🕒🎵 Spotify HUB75 — The Smart Music Wall Clock

**by [Aniket Chowdhury](mailto:micro.aniket@gmail.com) (aka `#Hashtag`)**

<img src="https://img.shields.io/badge/Status-Working-brightgreen?style=for-the-badge&logo=arduino" />
<img src="https://img.shields.io/badge/Built%20with-Raspberry%20Pi%20Pico%202%20W-blue?style=for-the-badge&logo=raspberrypi" />
<img src="https://img.shields.io/badge/Display-HUB75%2064x64-black?style=for-the-badge" />
<img src="https://img.shields.io/badge/Features-Clock%20%7C%20Spotify-purple?style=for-the-badge" />

</div>

---

## 🎬 Project Overview

**Spotify HUB75** is a **smart animated wall clock** built using a Raspberry Pi Pico 2 W and a 64x64 HUB75 RGB LED matrix.

At its core, it is a **digital wall clock** that displays the current time with animated visuals.

When music is playing, the clock transforms into a **Spotify music display**, showing album artwork, track information, playback progress, and an animated rotating CD.

So instead of having a separate clock and music display, this project combines both into one always-on visual device.

---

## ✨ Highlights

* 🕒 **Real-time digital wall clock**
* 🎵 Spotify currently playing integration
* 🌐 WiFi connectivity
* ⏱️ Automatic time synchronization
* 💿 Animated rotating CD
* 🖼️ Dynamic Spotify album artwork
* 🎨 Smooth visual transitions
* 📊 Playback progress display
* 🟢 Spotify-inspired interface
* 💾 LittleFS token storage
* 🔐 Spotify OAuth authentication
* ⚡ Dual-core Pico architecture
* 🖥️ 64x64 HUB75 RGB matrix
* 🔄 Automatic WiFi / Spotify recovery
* 💤 Clock remains useful even when no music is playing

---

## 🕒 The Wall Clock

The primary purpose of the project is to function as a **wall clock**.

When Spotify is idle or no track is playing, the display provides a clean clock interface.

```text
┌────────────────────────────────┐
│                                │
│             12:42              │
│                                │
│        Wednesday, 11 Sep       │
│                                │
└────────────────────────────────┘
```

The clock is designed to remain useful throughout the day rather than becoming just another Spotify accessory.

---

## 🎵 Spotify Mode

When music starts playing, the clock becomes an animated Spotify display.

```text
┌────────────────────────────────┐
│       ┌──────────────┐         │
│       │   ALBUM ART  │         │
│       │      💿      │         │
│       └──────────────┘         │
└────────────────────────────────┘
```

The display can show:

* Album artwork
* Track name
* Artist
* Playback state
* Playback progress
* Animated CD
* Spotify visual elements

---

## 🔄 Smart Display Modes

The display automatically changes depending on what is happening.

```text
              ┌──────────────┐
              │  Power ON    │
              └──────┬───────┘
                     ↓
              ┌──────────────┐
              │   WiFi +     │
              │ Time Sync    │
              └──────┬───────┘
                     ↓
              ┌──────────────┐
              │  WALL CLOCK  │
              └──────┬───────┘
                     │
              Spotify playing?
                 ↙         ↘
               NO           YES
               ↓             ↓
        ┌────────────┐ ┌────────────┐
        │    CLOCK   │ │  SPOTIFY   │
        │    MODE    │ │    MODE    │
        └────────────┘ └────────────┘
```

The clock remains the default purpose of the device, while Spotify becomes an interactive visual mode.

---

## ⚙️ System Architecture

```text
                    Internet
                       │
                       ↓
                ┌─────────────┐
                │    WiFi     │
                └──────┬──────┘
                       │
          ┌────────────┴────────────┐
          ↓                         ↓
   ┌───────────────┐        ┌───────────────┐
   │     NTP       │        │    Spotify    │
   │   Time Sync   │        │    Web API    │
   └───────┬───────┘        └───────┬───────┘
           │                        │
           ↓                        ↓
      Current Time             Track Data
           │                  Album Artwork
           │                        │
           └──────────┬─────────────┘
                      ↓
              ┌───────────────┐
              │  Pico 2 W     │
              │ Display Logic │
              └───────┬───────┘
                      ↓
               HUB75 64x64
                      ↓
                Wall Display
```

---

## 🧠 Dual-Core Architecture

The Pico 2 W separates networking and display rendering between its two cores.

```text
┌─────────────────────────────────────┐
│          Raspberry Pi Pico 2 W      │
├──────────────────┬──────────────────┤
│      CORE 0      │      CORE 1      │
├──────────────────┼──────────────────┤
│ WiFi             │ HUB75 Rendering  │
│ NTP              │ Clock Rendering  │
│ Spotify API      │ Animations       │
│ OAuth            │ CD Rotation      │
│ Token Refresh    │ Album Artwork    │
│ Artwork Download │ Transitions      │
│ JPEG Decode      │ Frame Updates    │
└──────────────────┴──────────────────┘
```

This architecture keeps network operations from unnecessarily interrupting the visual rendering.

---

## 💿 Animated Spotify Experience

When a song is playing, the album artwork becomes the visual centerpiece.

The display combines:

* 🖼️ Album artwork
* 💿 Rotating CD
* 🎵 Track information
* 📊 Progress indicator
* ✨ Smooth transitions
* 🟢 Spotify visual elements

The goal is to make the transition from **clock → music → clock** feel like part of the design rather than a simple screen change.

---

## 🌐 Connectivity

The Pico 2 W connects to a **2.4 GHz WiFi network** to provide:

* NTP time synchronization
* Spotify API access
* OAuth authentication
* Album artwork downloads
* Automatic reconnection

The clock can continue displaying time even when Spotify or the internet is temporarily unavailable.

---

## 🔐 Spotify Authentication

The project uses the Spotify Web API with OAuth authentication.

Required scope:

```text
user-read-currently-playing
```

Authentication data is stored using:

```text
LittleFS
```

After the initial authorization, saved authentication information can be reused on subsequent boots.

---

## 🔧 Hardware

* Raspberry Pi Pico 2 W
* 64x64 HUB75 RGB LED Matrix
* 5V external power supply
* HUB75 wiring
* USB cable

### Display

```text
Resolution : 64 × 64
Interface  : HUB75E
Scan       : 1/32
```

---

## 🔌 HUB75 Pin Configuration

```text
R1  → GPIO 0
G1  → GPIO 1
B1  → GPIO 2

R2  → GPIO 3
G2  → GPIO 4
B2  → GPIO 5

A   → GPIO 6
B   → GPIO 7
C   → GPIO 8
D   → GPIO 9
E   → GPIO 10

CLK → GPIO 11
LAT → GPIO 12
OE  → GPIO 13
```

---

## 📚 Software & Libraries

* Arduino IDE
* Arduino-Pico
* Adafruit Protomatter
* WiFi
* WiFiClientSecure
* HTTPClient
* LittleFS
* TJpg_Decoder
* Crypto
* Spotify Web API

---

## ⚡ Performance

The display is designed for smooth animated rendering while simultaneously handling network communication.

Important parameters include:

```cpp
DISPLAY_FPS
TRANSITION_MS
DISC_SPEED
POLL_INTERVAL
```

> ⚠️ The Pico 2 W should run at an appropriate CPU frequency. Extremely low CPU frequencies can cause WiFi connectivity problems.

---

## 🛠️ Setup

### 1. Install Arduino IDE

Install Arduino IDE and configure the Arduino-Pico core.

### 2. Select the Board

```text
Raspberry Pi Pico 2 W
```

### 3. Configure WiFi

```cpp
const char* WIFI_SSID     = "YOUR_WIFI";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";
```

### 4. Configure Spotify

Create a Spotify Developer application and configure the required credentials and redirect URI.

### 5. Upload

Upload the sketch to the Pico 2 W.

### 6. Authorize

Complete Spotify authorization during the initial setup.

Once authentication is complete, the device can operate as a standalone wall clock and Spotify display.

---

## ⚠️ Important Notes

* ⚡ HUB75 panels require an appropriate external 5V power supply.
* 🔗 Pico and matrix must share a common ground.
* 🌐 Use a compatible 2.4 GHz WiFi network.
* 🚫 Do not power the HUB75 panel directly from the Pico.
* 🔐 Never commit WiFi passwords or Spotify secrets to a public repository.
* ⚙️ Very low Pico CPU frequencies can interfere with reliable WiFi operation.

---

## 📸 Showcase

> Images and videos coming soon.

---

## 🚀 Future Ideas

* 🌙 Automatic night mode
* 🔆 Automatic brightness adjustment
* 🎚️ Spotify playback controls
* 🎵 More music visualization modes
* 🌈 Audio-reactive animations
* 📱 Web-based configuration
* 🕒 Multiple clock designs
* 🌍 Additional time zones
* 🎨 Custom display themes

---
 
## 👤 Author & Contact

👨 **Name:** Aniket Chowdhury (aka Hashtag)  
📧 **Email:** [micro.aniket@gmail.com](mailto:micro.aniket@gmail.com)  
💼 **LinkedIn:** [itzz-hashtag](https://www.linkedin.com/in/itzz-hashtag/)  
🐙 **GitHub:** [itzzhashtag](https://github.com/itzzhashtag)  
📸 **Instagram:** [@itzz_hashtag](https://instagram.com/itzz_hashtag)

---

## 📜 License

This project is released under a Modified MIT License.
It is intended for personal and non-commercial use only.

🚫 Commercial use or distribution for profit is not permitted without prior written permission.
🤝 For collaboration, reuse, or licensing inquiries, please contact the author.

📄 View Full License <br>
[![License: MIT–NC](https://img.shields.io/badge/license-MIT--NC-blue.svg)](./LICENSE)

---

## ⭐ Support

If you like this project:

* ⭐ Star the repository
* 🍴 Fork it
* 🛠️ Build your own version
* 💡 Share your improvements

---

## ❤️ Acknowledgements

This is a solo passion project built through countless iterations of coding, hardware testing, debugging, and visual tuning.

From the clock engine and time synchronization to Spotify authentication, album artwork decoding, WiFi recovery, and HUB75 rendering, every part has been developed and refined to turn a small microcontroller and LED matrix into a functional **smart wall clock and music display**.

---

## 🔥 Final Thought

> **It's a clock when you need the time.**
>
> **It's a music display when you play a song.**
>
> **It's both, all the time.** 🕒🎵💿

</div>

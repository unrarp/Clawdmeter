# Waveshare ESP32 AMOLED board capabilities — research notes

> **Purpose.** Hardware-capability reference compiled while scoping a new project
> (initial use case: an overnight audio recorder, e.g. snore capture + light
> on-device sound processing). Covers audio, power/sleep, RTC/scheduled-wake,
> storage, and remote (OTA) firmware update across the Waveshare AMOLED boards.
>
> **Scope note.** Findings are split into **hardware** (what the silicon /
> schematic actually has) vs. **current firmware** (what the Clawdmeter
> usage-monitor firmware happens to drive). For a new project only the hardware
> facts matter; the firmware column is included so you know what is *not* a
> solved problem yet.
>
> **Sources.** Manufacturer wikis, the official **schematic PDFs**, and chip
> datasheets (links at the bottom). Schematic nets/designators are quoted where
> they settle a question. GPIO assignments below are read from the schematics
> and should be re-verified against the specific board revision you buy.

---

## 0. Boards at a glance

| | **AMOLED-1.8 (S3)** | **AMOLED-2.16 (S3)** | **AMOLED-2.16-C6** |
|---|---|---|---|
| MCU | ESP32-S3R8 (dual-core, PSRAM) | ESP32-S3R8 (dual-core, PSRAM) | ESP32-C6 (single-core RISC-V, **no PSRAM**, BLE 5.3 only) |
| Display | 1.8" SH8601, 368×448 | 2.16" CO5300, 480×480 | 2.16" CO5300, 480×480 |
| Touch | FT3168 | CST9220 | CST9220 |
| Flash | 16 MB (W25Q128) | 16 MB (XM25QH128) | 16 MB |
| PMU | AXP2101 @ 0x34 | AXP2101 @ 0x34 | AXP2101 @ 0x34 |
| IMU | QMI8658 @ 0x6B | QMI8658 @ 0x6B | QMI8658 @ 0x6B |
| RTC | PCF85063 | PCF85063 | PCF85063 (assumed; not separately verified) |
| microSD / TF slot | Yes | Yes | Yes |
| **Audio** | **1 analog mic, mono codec** | **2-mic far-field array + dedicated ADC** | amp present, not detailed |
| IO expander | TCA9554/XCA9554 | none | TCA9554 (audio-only) |

**Takeaway for a new audio project:** the **2.16 (S3)** is the strongest choice —
best mic subsystem, and the only board with a clean scheduled-wake path (see §3).
The C6 variant trades PSRAM and a core for lower cost; avoid it if you need
PSRAM buffering or heavier DSP.

---

## 1. Audio subsystems (schematic-verified)

### AMOLED-1.8 — mono in / mono out

- **`U9` = ES8311** — single mono audio codec (one ADC, one DAC).
- **One analog MEMS microphone** (`MIC1`, nets `MIC_P`/`MIC_N`) into the ES8311's
  single mic input (`MIC1P`/`MIC1N`, pins 18/17). No mic array.
- **`U10` = NS4150B** Class-D amp driving an **onboard speaker `H2`** (silk spec
  "12×10mm 8Ω 1W").
- I2S: `MCLK=GPIO16`, `SCLK=GPIO9`, `LRCK=GPIO45`, speaker `DSDIN=GPIO8`,
  mic `ASDOUT=GPIO10`. Amp enable (`PA_CTRL`) on a GPIO (≈GPIO46 via R38 in the
  revision read; some revisions route amp-enable through the IO expander —
  verify).
- Marketed for offline speech recognition / AI voice (near-field).

### AMOLED-2.16 — true 2-mic far-field array (two codec chips)

- **`U6` = ES8311** — mono codec (speaker DAC path + one ADC + AEC reference).
- **`U7` = ES7210** — a **dedicated 4-channel audio ADC** (I²C addr `0x40`),
  the part that makes this a real far-field mic array with hardware AEC.
- **Two analog MEMS mics** (`MIC1`, `MIC2`) feeding the ES7210
  (`MIC1_P/N`, `MIC2_P/N`); the speaker output is fed back as an AEC reference
  channel.
- **`U8` = NS4150B** Class-D amp driving the speaker connector `P10` (`SPK +/-`).
- I2S (shared by both codecs): `MCLK=GPIO42`, `SCLK=GPIO9`, `LRCK=GPIO45`,
  speaker `DSDIN=GPIO8`, mic `ASDOUT=GPIO10`; amp enable `PA_CTRL` (≈GPIO46 via R16).
- Explicitly marketed for **near-field/far-field wake** + noise reduction.

### AMOLED-2.16-C6

- Wiki mentions an audio amp serviced by a TCA9554; the Clawdmeter firmware flags
  it `BOARD_HAS_IO_EXPANDER 0` (not driven). Audio path not separately verified
  against a schematic here — **check its schematic before relying on audio.**

### Does "mono" matter?

For recording/detecting non-directional sound (snoring, ambient events), **no** —
a single channel is sufficient and halves storage. What matters is **near-field
vs. far-field sensitivity**. The 1.8 is one near-field mic; the 2.16 is a two-mic
far-field array on an ES7210, which is the better hardware for picking up a
distant source across a room. Note the ES7210's built-in noise-reduction/AEC is
tuned for *speech* and may suppress/distort non-speech sounds — for snoring you'd
likely **bypass the AFE and capture raw mic channels**, keeping the far-field
sensitivity and the second channel for redundancy/beamforming.

### Current firmware status (all boards)

No audio is implemented. The ES8311/ES7210 are never initialized; no I2S stream
exists. On the 1.8 the amp-enable line is toggled on but no samples ever flow.
Any audio project starts from Waveshare's ES8311/ES7210 + SD examples, not from
the Clawdmeter code.

---

## 2. Power management & sleep

- All boards use an **AXP2101 PMU** (I²C `0x34`) for battery, charging, VBUS
  detection, and the power button.
- "Power off" in the Clawdmeter firmware = `pmu.shutdown()`: cuts every rail
  except VRTC — a genuine ~µA hardware off, **not** a sleep. Waking it requires a
  **physical power-button press or USB (VBUS) insertion**.
- The S3/C6 silicon supports `esp_sleep_*` light/deep sleep with timer/GPIO
  wakeup, but the Clawdmeter project deliberately **rejected ESP32 deep sleep**
  on the S3 boards (see its decision log): deep sleep powers down only the ESP
  (~10 µA) while the AXP + panel rails stay on (tens of mA), GPIO0 is a BOOT
  strapping pin (wake → possible USB-download mode), and GPIO0 can't re-enable
  the AXP. **Note this GPIO0 concern is board/wiring-specific — see §3, where the
  2.16's RTC uses GPIO13, sidestepping it.**
- On the 2.16-C6, the LCD/touch are powered from AXP ALDO rails, so the PMU must
  be brought up *before* display init.

**For a battery + scheduled-wake project the practical low-power model is ESP32
deep sleep with the panel rails disabled via the AXP over I²C, then re-enabled on
wake — not the AXP full-off, which cannot self-wake.** See §3.

---

## 3. RTC & scheduled self-wake — the key board difference

All boards carry a **PCF85063** RTC (battery-backed, 32.768 kHz crystal, backup
pad to survive main-battery swaps). The decisive difference is **where the RTC
alarm interrupt is wired**:

| | **AMOLED-2.16 (S3)** | **AMOLED-1.8 (S3)** |
|---|---|---|
| RTC alarm `INT` routed to | **ESP32 `GPIO13`** (RTC-capable GPIO) | **TCA9554 I/O expander, EXIO3** (behind I²C) |
| Schematic net | `RTC_INT → U1 pin18 (GPIO13)` + `U4 pin4` | `RTC_INT/EXIO3 → U7 P3 (pin7)` + `U6 pin4` |
| Wake ESP from **deep sleep** via RTC alarm | **Yes** — `GPIO13` usable as `ext0`/`ext1` wake source | **No** — alarm only readable by polling the expander while awake |
| Wake from **full AXP-off** via RTC | No (INT not on AXP `PWRON`) | No (same) |

**Consequences**

- **2.16:** supports a clean "sleep, then auto-wake at time T" flow: set the
  PCF85063 alarm, drop the panel ALDO rails via AXP, arm `GPIO13` as the wake
  source, `esp_deep_sleep`; at T the alarm pulls `GPIO13` low → ESP wakes →
  re-enables panel rails over I²C → resumes. The GPIO0 strapping hazard from §2
  does **not** apply (RTC is on GPIO13, not GPIO0).
- **1.8:** no hardware deep-sleep wake from the RTC — the alarm lands on the I²C
  IO expander (which also carries AXP_IRQ, IMU INT, and even SD `CS`), so the ESP
  must be awake/polling to notice it. Scheduled wake from a low-power state is not
  cleanly supported.
- **Neither** board can self-wake from a true AXP `shutdown()` (RTC INT isn't
  wired to the AXP `PWRON`); only button/USB revives that state.

**AMOLED-1.8 TCA9554 EXIO map (from schematic), for reference:**
EXIO0=LCD_RESET, EXIO1=DSI_PWR_EN, EXIO2=TP_RESET, **EXIO3=RTC_INT**,
EXIO4=SYS_OUT (power-button sense), EXIO5=AXP_IRQ, EXIO6=QMI_INT1 (IMU),
EXIO7=SD_CS.

**Recommendation:** if scheduled wake matters, pick the **2.16 (S3)**. If the
device will always be on USB power, the RTC-wake difference is moot.

---

## 4. Storage & audio-recording feasibility

- All boards have a **microSD/TF slot** — essential, since internal flash
  (16 MB, mostly app partitions) can't hold recordings.
- The ESP32-S3 (240 MHz dual-core + PSRAM) easily runs continuous I2S capture +
  SD writes on one core and light DSP (RMS/bandpass/peak/event detection, even a
  small ML model) on the other. The C6 (single-core, no PSRAM) is more limited.

**Storage math — 8 hours (28,800 s), uncompressed PCM:**

| Format | 8-hour size | Notes |
|---|---|---|
| 8 kHz / 16-bit mono | ~460 MB | snoring lives <1 kHz; plenty |
| 16 kHz / 16-bit mono | ~920 MB | comfortable "voice" headroom |
| 16 kHz / 16-bit **stereo** (2 mics) | ~1.8 GB | only if you want both channels |
| 44.1 kHz / 16-bit mono | ~2.5 GB | overkill |

Reductions: **ADPCM** (~4:1) → ~230 MB; **Opus/MP3 at ~24 kbps** (ESP-ADF
encoders) → ~85 MB/night; **event-gated capture** (only write when an
RMS/bandpass threshold trips) often cuts a night to tens of minutes of data.

A cheap microSD card holds many nights even uncompressed.

**Power for an all-night session:** the AMOLED is the hog — turn the display off.
With it off, ESP + codec + periodic SD writes average ~60–100 mA, i.e.
~500–800 mAh over 8 h, marginal for a small LiPo. **Run it on USB overnight**
(also keeps the AXP from auto-shutting-down, since VBUS present blocks power-off).

---

## 5. Remote firmware update (OTA over WiFi)

- **Supported by hardware + flash layout, not yet implemented.** ESP32-S3/C6 do
  native WiFi OTA. The Clawdmeter envs already use 16 MB flash with the
  `default_16MB.csv` partition table → **dual app slots (`ota_0`/`ota_1`, ~6.5 MB
  each) + `otadata`**, the standard A/B layout OTA needs. No partition changes
  required.
- **No OTA code exists today** (no `esp_https_ota`/`esp_ota_*`/`ArduinoOTA`/
  `Update.*`). As shipped, updates are USB-only.
- **First flash is always USB** — OTA only works once an OTA-capable build is
  already running; thereafter updates can be wireless.

**Approaches to enable it:**
1. **Pull-based HTTPS OTA** (best fit): device periodically checks a version
   manifest and downloads a signed/checksummed `.bin` over TLS into the inactive
   slot, then reboots. The device already does TLS and talks to a host helper
   over the LAN, so a small server/host endpoint is the natural place to publish
   firmware + version.
2. **Push-based (`espota`/ArduinoOTA):** device runs an OTA listener; push from
   PlatformIO with `upload_protocol = espota`, `--upload-port <device-ip>`. Good
   for the dev loop on a LAN.
3. **Browser upload (ElegantOTA-style):** device serves a page to drop a `.bin`.

**Safety:** prefer OTA on USB / not-low-battery; the dual-partition layout gives
**rollback** if the new image fails to validate/boot. Add image
signing/checksum (and optionally secure boot) since you control the update path.

---

## 6. Recommendations for the new project

- **Board:** start on **AMOLED-2.16 (S3)** — best mic (2-mic far-field ES7210
  array + ES8311 + NS4150B speaker), PSRAM, and the only clean RTC→GPIO13
  scheduled-wake path. Keep the **1.8** as a smaller/cheaper near-field option
  (single mono mic; no deep-sleep RTC wake). Treat the **C6** as cost-reduced
  (no PSRAM/second core).
- **Audio:** build from Waveshare's ES8311/ES7210 + SD examples; for snore
  capture, bypass the speech AFE and record raw channels; consider event-gated
  recording to shrink storage.
- **Storage:** microSD; 16 kHz/16-bit mono ≈ 0.9 GB/night raw, far less
  compressed/gated.
- **Power:** display off + USB power for overnight runs. If you need battery +
  timed wake, use ESP deep sleep with panel rails dropped via AXP and the
  PCF85063 alarm on GPIO13 (2.16 only).
- **Updates:** plan OTA in from the start (pull-based HTTPS, signed image,
  served by your host); the 16 MB/dual-slot layout already supports it. First
  flash over USB.

---

## 7. Sources

**Wikis**
- AMOLED-1.8: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8
- AMOLED-2.16: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16
- Resource pages: `.../ESP32-S3-Touch-AMOLED-1.8/Resources-And-Documents`,
  `.../ESP32-S3-Touch-AMOLED-2.16/Resources-And-Documents`

**Schematics (authoritative for the wiring above)**
- 1.8: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf
- 2.16: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.16/ESP32-S3-Touch-AMOLED-2.16-Schematic.pdf

**Datasheets**
- ES8311 codec: https://files.waveshare.com/wiki/common/ES8311.DS.pdf
- AXP2101 PMU: https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf
- PCF85063 RTC: https://files.waveshare.com/wiki/common/PCF85063A.pdf
- QMI8658 IMU: https://files.waveshare.com/wiki/common/QMI8658C.pdf
- (ES7210 4-ch ADC: identified from the 2.16 schematic `U7`; fetch ESS's
  ES7210 datasheet separately — not on the Waveshare common-datasheet list.)

**Example code**
- 1.8: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8
- 2.16: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16/tree/main/examples

> Caveat carried from the research: Waveshare does not publish mic sensitivity/SNR
> or capture range. Validate real-world pickup distance with a quick recording
> test before committing to a board.

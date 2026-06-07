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
| Flash | 16 MB (W25Q128) | 16 MB (XM25QH128) | 16 MB (XM25QH128) |
| PSRAM | Yes (OPI) | Yes (OPI) | **No** (512 KB HP + 16 KB LP SRAM) |
| PMU | AXP2101 @ 0x34 | AXP2101 @ 0x34 | AXP2101 @ 0x34 |
| IMU | QMI8658 @ 0x6B | QMI8658 @ 0x6B | QMI8658 @ 0x6B |
| RTC | PCF85063 | PCF85063 | PCF85063 |
| microSD / TF slot | Yes | Yes | Yes |
| **Audio** | **1 analog mic, mono codec** | **2-mic far-field array + dedicated ADC** | **2-mic far-field array + dedicated ADC** (same as 2.16-S3) |
| IO expander | TCA9554/XCA9554 | none | none (schematic) |
| RTC alarm `INT` → | TCA9554 expander | **ESP32 GPIO13** | RTC + test point only (not MCU) |

**Takeaway for a new audio project:** the **2.16 (S3)** is the strongest overall —
the best mic subsystem (tied with the C6) **and** the only board with a clean
scheduled-wake path (see §3). The **2.16-C6** has the *same* dual-mic audio
hardware but **no PSRAM** (tighter buffers / heavier DSP), a single RISC-V core,
and no usable RTC-alarm wake — pick it only if cost/BLE matter more than those.
The **1.8** is the smaller/cheaper near-field option (single mono mic, no
deep-sleep RTC wake).

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

_Confirmed from:_ [1.8 schematic][sch-18] (designators/nets/I2S GPIOs),
[1.8 wiki][wiki-18] + [1.8 resource page][res-18] (mic/speaker/codec wording,
ES8311-only datasheet list), [ES8311 datasheet][ds-es8311].

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

_Confirmed from:_ [2.16 schematic][sch-216] (the `U6` ES8311, `U7` ES7210,
`U8` NS4150B, dual `MIC1`/`MIC2`, AEC reference, nets/I2S GPIOs),
[2.16 wiki][wiki-216] + [2.16 resource page][res-216] (dual-mic array /
echo-cancellation wording, ES8311 datasheet list — ES7210 identified from the
schematic, not the datasheet list), [ES8311 datasheet][ds-es8311].

### AMOLED-2.16-C6 — same dual-mic far-field array as the 2.16-S3

Schematic-verified, and it is the **same audio architecture as the 2.16-S3**
(this corrects an earlier assumption that the C6 only had a bare amp):

- **`U9` = ES8311** mono codec + **`U7` = ES7210** 4-channel ADC (I²C `0x40`) +
  **`U8` = NS4150B** Class-D amp.
- **Two analog MEMS mics** (`MIC1`, `MIC2`) into the ES7210; speaker output fed
  back as the AEC reference; speaker on the `P10` pads (`SPK`, "2-pin speaker").
- I2S (on C6 GPIOs): `MCLK=GPIO19`, `SCLK=GPIO20`, mic `ASDOUT=GPIO21`,
  `LRCK=GPIO22`, speaker `DSDIN=GPIO23`. **No TCA9554** in the schematic read —
  audio sits directly on C6 GPIOs (so the Clawdmeter `board.h` "TCA9554 only
  services audio" note looks stale for this revision).
- **Caveat:** the C6 has **no PSRAM** (512 KB HP SRAM), so large audio
  ring-buffers and heavier DSP are tighter than on the S3 boards.

_Confirmed from:_ [C6 schematic][sch-c6] (`U9`/`U7`/`U8`, dual `MIC1`/`MIC2`,
AEC, I2S GPIOs, no IO expander), [C6 wiki][wiki-c6] + [C6 resource page][res-c6]
("Dual Microphone Array Design", ES8311 + ES7210 + speaker-amp wording, no-PSRAM
note), [ES8311 datasheet][ds-es8311].

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

_Confirmed from:_ [AXP2101 datasheet][ds-axp2101] (rails, `PWRON`/IRQ,
shutdown behaviour), the schematics ([1.8][sch-18], [2.16][sch-216]) for the
AXP/button/rail wiring, in-repo
`docs/decisions/2026-06-03-axp-power-off-vs-deep-sleep.md` (the deep-sleep
rejection rationale) and the boards' `power.cpp` / `board_init.cpp`. ESP32-S3/C6
sleep modes per the [ESP32-S3 datasheet][ds-esp32s3].

---

## 3. RTC & scheduled self-wake — the key board difference

All boards carry a **PCF85063** RTC (battery-backed, 32.768 kHz crystal, backup
pad to survive main-battery swaps). The decisive difference is **where the RTC
alarm interrupt is wired** — i.e. whether the *PCF85063 alarm* can serve as a
wake source:

| | **AMOLED-2.16 (S3)** | **AMOLED-1.8 (S3)** | **AMOLED-2.16-C6** |
|---|---|---|---|
| RTC alarm `INT` routed to | **ESP32 `GPIO13`** (RTC-capable GPIO) | **TCA9554 I/O expander, EXIO3** (behind I²C) | **RTC + test point `TP12` only** (not to the MCU) |
| Schematic net | `RTC_INT → U1 pin18 (GPIO13)` + `U4 pin4` | `RTC_INT/EXIO3 → U7 P3 (pin7)` + `U6 pin4` | `NLRTC0INT = {U6 pin4, TP12}` — no C6 GPIO found |
| PCF85063 alarm can wake MCU from **deep sleep** | **Yes** — `GPIO13` as `ext0`/`ext1` wake source | **No** — alarm only readable by polling the expander while awake | **No** — alarm not connected to the MCU |
| Wake from **full AXP-off** via RTC | No (INT not on AXP `PWRON`) | No (same) | No (same) |

**Consequences**

- **2.16-S3:** supports a clean "sleep, then auto-wake at time T" flow driven by
  the accurate external RTC: set the PCF85063 alarm, drop the panel ALDO rails via
  AXP, arm `GPIO13` as the wake source, `esp_deep_sleep`; at T the alarm pulls
  `GPIO13` low → ESP wakes → re-enables panel rails over I²C → resumes. The GPIO0
  strapping hazard from §2 does **not** apply (RTC is on GPIO13, not GPIO0).
- **1.8:** no hardware deep-sleep wake from the RTC — the alarm lands on the I²C
  IO expander (which also carries AXP_IRQ, IMU INT, and even SD `CS`), so the ESP
  must be awake/polling to notice it.
- **2.16-C6:** the RTC alarm is brought out only to a test point, not to any C6
  GPIO (per the netlist read — verify on the schematic), so the PCF85063 can't
  wake the MCU at all.
- **None** of the boards can self-wake from a true AXP `shutdown()` (RTC INT isn't
  wired to the AXP `PWRON`); only button/USB revives that state.

> **Important nuance:** "the RTC can't wake it" ≠ "no scheduled wake at all."
> ESP32-S3 **and** C6 support an MCU-**internal** deep-sleep timer
> (`esp_sleep_enable_timer_wakeup()`) that needs no external RTC — so any of the
> three can sleep and self-wake after N seconds. The external-RTC routing above
> only matters when you want the **PCF85063 alarm** as the wake source (better
> wall-clock accuracy over long sleeps, and survives across power loss). For
> short, repeated wake intervals the internal timer is usually enough on all
> three; for accurate absolute-time wake-ups, only the 2.16-S3 has the RTC wired
> to do it in hardware.

**AMOLED-1.8 TCA9554 EXIO map (from schematic), for reference:**
EXIO0=LCD_RESET, EXIO1=DSI_PWR_EN, EXIO2=TP_RESET, **EXIO3=RTC_INT**,
EXIO4=SYS_OUT (power-button sense), EXIO5=AXP_IRQ, EXIO6=QMI_INT1 (IMU),
EXIO7=SD_CS.

**Recommendation:** if accurate absolute-time scheduled wake matters, pick the
**2.16 (S3)** (only board with the RTC alarm on a wake-capable GPIO). If short
interval wakes suffice, the internal deep-sleep timer works on all three. If the
device will always be on USB power, the whole RTC-wake question is moot.

_Confirmed from:_ the **schematics** ([1.8][sch-18]: `RTC_INT → TCA9554 EXIO3`;
[2.16][sch-216]: `RTC_INT → ESP32 GPIO13`; [C6][sch-c6]: `RTC_INT → RTC + TP12
only`) for the alarm routing — these are the load-bearing source for the table
above; [PCF85063 datasheet][ds-pcf85063] (alarm/timer + `INT` behaviour);
[1.8][wiki-18] / [2.16][wiki-216] / [C6][wiki-c6] wikis (RTC + backup-battery
wording). RTC-GPIO/`ext0`/`ext1` and internal-timer deep-sleep wake per the
[ESP32-S3][ds-esp32s3] / [ESP32-C6][ds-esp32c6] datasheets.

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

_Confirmed from:_ microSD/TF slot per the [1.8][wiki-18] / [2.16][wiki-216] /
[C6][wiki-c6] wikis and the schematics (`SD-CARD` block in [1.8][sch-18] /
[2.16][sch-216] / [C6][sch-c6]). Storage sizes are arithmetic (sample-rate ×
depth × channels × duration). Current draw and capture/DSP headroom are
**engineering estimates**, not measured — validate on hardware. The C6's lack of
PSRAM limits buffer sizes. Compression options (ADPCM/Opus/MP3) per the ESP-ADF
encoder set.

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

_Confirmed from:_ in-repo `firmware/platformio.ini`
(`board_build.partitions = default_16MB.csv`, 16 MB flash) and the stock
`default_16MB.csv` layout (dual `ota_0`/`ota_1` + `otadata`); "no OTA today"
verified by grepping the firmware tree (no `esp_ota_*`/`esp_https_ota`/
`ArduinoOTA`/`Update.*`). OTA/rollback/secure-boot mechanics per the
[ESP32-S3 datasheet][ds-esp32s3] and Espressif's OTA docs.

---

## 6. Recommendations for the new project

- **Board:** start on **AMOLED-2.16 (S3)** — best mic (2-mic far-field ES7210
  array + ES8311 + NS4150B speaker), PSRAM, and the only clean RTC→GPIO13
  absolute-time scheduled-wake path. The **2.16-C6** has identical audio hardware
  but no PSRAM (tighter audio buffers), one RISC-V core, and no RTC-alarm wake —
  choose it only if cost/BLE outweigh those. Keep the **1.8** as a
  smaller/cheaper near-field option (single mono mic; no deep-sleep RTC wake).
- **Audio:** build from Waveshare's ES8311/ES7210 + SD examples; for snore
  capture, bypass the speech AFE and record raw channels; consider event-gated
  recording to shrink storage.
- **Storage:** microSD; 16 kHz/16-bit mono ≈ 0.9 GB/night raw, far less
  compressed/gated.
- **Power:** display off + USB power for overnight runs. If you need battery +
  absolute-time wake, use ESP deep sleep with panel rails dropped via AXP and the
  PCF85063 alarm on GPIO13 (2.16-S3 only); for simple interval wakes the
  MCU-internal deep-sleep timer works on any board.
- **Updates:** plan OTA in from the start (pull-based HTTPS, signed image,
  served by your host); the 16 MB/dual-slot layout already supports it. First
  flash over USB.

---

## 7. Sources

The inline _"Confirmed from:"_ lines in each section cite these by label.
Schematics are authoritative for all wiring/GPIO/designator claims; wikis for
marketing/feature wording; in-repo paths for firmware/partition facts.

**Wikis & resource pages**
- AMOLED-1.8: [wiki][wiki-18] · [resources][res-18]
- AMOLED-2.16: [wiki][wiki-216] · [resources][res-216]
- AMOLED-2.16-C6: [wiki][wiki-c6] · [resources][res-c6]

**Schematics (PDF)**
- [1.8][sch-18] · [2.16][sch-216] · [C6][sch-c6]

**Datasheets**
- [ES8311 codec][ds-es8311] · [AXP2101 PMU][ds-axp2101] ·
  [PCF85063 RTC][ds-pcf85063] · [QMI8658 IMU][ds-qmi8658] ·
  [ESP32-S3][ds-esp32s3] · [ESP32-C6][ds-esp32c6]
- ES7210 4-ch ADC: identified from the 2.16/C6 schematics (`U7`); fetch ESS's
  ES7210 datasheet separately — not on Waveshare's common-datasheet list.

**Example code**
- [1.8][ex-18] · [2.16][ex-216] · [C6][ex-c6]

> Caveat carried from the research: Waveshare does not publish mic sensitivity/SNR
> or capture range. Validate real-world pickup distance with a quick recording
> test before committing to a board.

<!-- Reference-style link definitions -->
[wiki-18]: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8
[wiki-216]: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16
[wiki-c6]: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16
[res-18]: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8/Resources-And-Documents
[res-216]: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/Resources-And-Documents
[res-c6]: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16/Resources-And-Documents
[sch-18]: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf
[sch-216]: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.16/ESP32-S3-Touch-AMOLED-2.16-Schematic.pdf
[sch-c6]: https://files.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-2.16/ESP32-C6-Touch-AMOLED-2.16-Schematic.pdf
[ds-es8311]: https://files.waveshare.com/wiki/common/ES8311.DS.pdf
[ds-axp2101]: https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf
[ds-pcf85063]: https://files.waveshare.com/wiki/common/PCF85063A.pdf
[ds-qmi8658]: https://files.waveshare.com/wiki/common/QMI8658C.pdf
[ds-esp32s3]: https://documentation.espressif.com/esp32-s3_datasheet_en.pdf
[ds-esp32c6]: https://documentation.espressif.com/esp32-c6_datasheet_en.pdf
[ex-18]: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8
[ex-216]: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16/tree/main/examples
[ex-c6]: https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16

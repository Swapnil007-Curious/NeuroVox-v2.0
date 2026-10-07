# NeuroVox v2.0

A forearm band that reads tiny muscle signals and turns them into spoken sentences. It's for people who can't talk anymore (ALS, stroke, vocal cord damage, some spinal injuries) but can still twitch a muscle in their forearm, which turns out to be most of them for a long time.

![NeuroVox on the forearm](images/device-on-arm.png)

**Hardware:** ₹10,000
**Software (companion website + AI voice):** ₹20,000 per year

For comparison, commercial AAC devices (eye trackers, switch boards, dedicated speech generators) go for roughly ₹85,000 to well over ₹12 lakh, and most of them need a clinician to set up. That gap is basically the whole reason this project exists.

---

## Why

Stephen Hawking ran his whole speech system off one cheek muscle. The idea here is the same, just moved to the forearm and built out of parts you can actually order from LCSC.

The part I cared about most is the output. A lot of cheap assistive projects stop at "the device can say yes or no." That's not much better than a call bell. NeuroVox only classifies four gestures on the board, but the companion website hands that label to an LLM along with the time of day, who the person is talking to, and what was just said, and gets back a real sentence. Same YES gesture can come out as "Yes, the pain's still there" or "Yes, let's go outside" depending on what was asked a second ago.

---

## How it works

1. **Electrodes.** Four standard 2" x 2" TENS pads stick on the skin. Two go over the flexor digitorum superficialis (inner forearm) and two over the extensor digitorum (outer forearm). Leads plug into the board through JST-SH connectors. The pads are NOT built into the enclosure, I tried that early on and it doesn't work because the two muscle groups are on opposite sides of the arm.
2. **Analog front end.** Four AD8232s amplify and filter the microvolt signals. An INA333 does extra differential conditioning on two channels.
3. **ADC.** An ADS1299 (24 bit, biosignal grade) samples all four channels over SPI. Way better resolution than the ESP32's own ADC.
4. **Classification on the board.** The ESP32-S3 smooths each channel into an envelope, compares it to a threshold that adapts to the person over time, and counts pulses. 1 pulse = YES, 2 = NO, 3 = HELP, 4 or more = WAIT. A BNO055 IMU throws out samples when the arm moves suddenly so a wave of the hand doesn't get read as a gesture.
5. **Bluetooth.** The gesture label plus a confidence score goes out over BLE (something like `CH1:YES:91%`).
6. **Sentence + voice.** The companion website receives it, gets a sentence from Claude, shows it on screen and speaks it out loud in the chosen language (English, Hindi, Marathi, Tamil, Bengali and a few others).

Muscle twitch to spoken sentence is meant to land in under two seconds, most of that is network time not the board.

---

## The board

![PCB 3D view](images/pcb-3d.png)

2 layer FR4, **220mm x 50mm**, designed in KiCad 8. It's long and skinny on purpose so it runs along the forearm instead of across it.

The board is split into three zones along its length:

| Zone | Position | What's in it |
|---|---|---|
| Power | 0 to 60mm | MCP73831 Li-Po charger, TPS61023 boost (5V), AMS1117 3.3V regulator, USB-C input |
| Analog | 60 to 150mm | 4x AD8232, ADS1299, INA333, electrode connectors J3 to J6 |
| Digital | 150 to 220mm | ESP32-S3-WROOM-1-N8, BNO055 IMU, DRV2605L haptic driver, MAX98357A audio amp, RGB LEDs, buzzer, programming header J7 |

Analog and digital grounds are separate pours that only meet at one point, a ferrite bead right where the analog zone ends. The ESP32's antenna sits at the right edge with no copper under it.

**Why 220mm and not 200mm:** the first version was 200mm and the analog section was too packed. There was one ground connection near the instrumentation amp that just could not be routed, I checked it with an exhaustive pathfinding run, not just the autorouter giving up. Stretching the board 20mm and giving the analog zone 90mm instead of 65mm fixed it. If you fork this and want it smaller, expect that fight again.

### Pin map (real hardware)

| Function | GPIO |
|---|---|
| ADS1299 SPI CLK / MOSI / MISO / CS / DRDY | 10, 11, 12, 13, 14 |
| I2C SDA / SCL (BNO055, DRV2605L) | 21, 18 |
| Haptic enable | 8 |
| I2S to MAX98357A | 15, 16, 17 |
| Status LEDs | 2, 4, 47 |
| Buzzer | 48 |

### Fab notes

- You need **Standard PCBA** at JLCPCB, not Economic. The ESP32-S3 module, the MAX98357A (WLCSP) and the DRV2605L (DSBGA) aren't hand solderable at any sane yield.
- Standard PCBA adds temporary 5mm rails on the long edges for the pick and place machine. Ask for V-scores so they snap off and you get the real 220 x 50mm board back.
- The DRV2605L's center ball needs via-in-pad to escape. Mention it in the order notes.
- Gerbers and the full `.kicad_pcb` are in `/PCB`. I didn't make a separate schematic file for this revision.

---

## Firmware

FreeRTOS on the ESP32-S3, four tasks so the Bluetooth stack can't mess up sample timing:

| Task | Core | Priority | Job |
|---|---|---|---|
| ADC sampling | 0 | 5 | reads all 4 channels into a ring buffer |
| Gesture classifier | 1 | 3 | envelope, adaptive threshold, pulse counting, confidence |
| BLE dispatcher | 1 | 1 | sends gesture strings to the website |
| IMU monitor | 1 | 1 | flags motion over 1.5G so those samples get dropped |

A few details that matter for accuracy:

- Envelope is an exponential moving average (alpha 0.05).
- Contractions shorter than 30ms are ignored, they're almost always noise.
- A gesture only gets confirmed after 400ms of quiet, so a slow double pulse doesn't get read as two singles.
- The threshold for each channel updates off a rolling average of recent peaks, because signal strength changes person to person and also drifts for the same person as muscles get tired. A threshold set on day one is wrong by day thirty, especially with something progressive like ALS.
- All four channels are treated the same. An older board revision had a ground problem on two channels and the firmware briefly weighted them differently, that got removed once the redesign fixed the actual problem.

BLE service UUID: `180d1000-1234-1000-8000-00805f9b34fb`
Characteristic: `2a371000-1234-1000-8000-00805f9b34fb`
Device name: `NeuroVox-v2`

**Current state, honestly:** the firmware in `/FIRMWARE` is a validation build. It runs the full classification pipeline against potentiometers standing in for the EMG front end and an MPU6050 standing in for the BNO055, which let me test the logic without waiting on boards. Flip `SIM_BUILD` to false for hardware. The piece still left is swapping the analog reads for a proper SPI driver talking to the ADS1299 and moving I2C to GPIO21/18. That happens when the assembled boards are back.

**On accuracy:** my target is around 88 to 90% gesture accuracy after a short per person calibration (about 10 contractions per gesture), tested seated with the arm resting. I haven't measured that on real muscle yet because the boards aren't made yet, so please don't quote it as a result. When I test I'll report each channel separately and then combined.

---

## Enclosure

![Exploded view](images/exploded-view.png)

Two part snap fit, no screws holding the halves together (four M2 screws just hold the PCB to standoffs inside the top shell).

| File | Size | Notes |
|---|---|---|
| `TOP.stl` | 224 x 54 x 10mm | rigid shell, cutouts for USB-C, programming header, speaker grille, LED window, buzzer, vent slots, antenna window thinned to 1mm |
| `BOTTOM.stl` | 224 x 94.4 x 9.8mm | the skin side, strap wings stick out 20mm each side, cable clips for the electrode leads, battery bay |
| `FULL.stl` | 224 x 94.4 x 16mm | both halves together, for viewing. It isn't watertight because two meshes are combined, that's expected |

0.3mm interference lip, four alignment pegs. So the full width with strap wings is about 94mm, not 54mm, keep that in mind.

**Print it in:** PETG for the top (ABS warps badly at 224mm long on an open printer), TPU 95A for the bottom since it sits on skin for hours and the strap wings need to flex. Don't use PLA, it gets soft around 60C and tacky against skin.

---

## Bill of materials

Full BOM with LCSC part numbers is in `NeuroVox_v2_0_FULL_BOM.csv`. It's split into the PCBA parts (the only section you upload to JLCPCB) and everything else you buy separately.

Main parts:

| Part | LCSC |
|---|---|
| ESP32-S3-WROOM-1-N8 | C2913198 |
| ADS1299IPAGR | C476817 |
| AD8232ACPZ-R7 (x4) | C43216 |
| INA333AIDGKR | C19450 |
| BNO055BST | C93216 |
| DRV2605LYZFR | C2869341 |
| MAX98357AEWL+T | C2682619 |
| MCP73831T-2ACI/OT | C424093 |
| TPS61023DRLR | C919459 |
| AMS1117-3.3 | C6186 |
| USB-C TYPE-C-31-M-12 | C165948 |
| JST-SH 3 pin (x4, electrodes) | C160403 |
| JST-PH 2 pin (battery) | C265016 |

Bought separately: 5000mAh 3.7V Li-Po with a JST-PH lead, a pack of 2" x 2" TENS pads (you need 8 per session), TENS snap leads, a 25mm strap with velcro or a buckle, and the printed shells.

Two things to watch before ordering:
- **FB1 ferrite bead:** the exact BLM18PG601SN1D wasn't confirmed on LCSC. BLM18AG601SN1D (C19330) is the same value and package but a different current series, check the datasheet first.
- **BZ1 buzzer:** the PUI SMT-0825-S-4-R isn't on LCSC at all. Get it from DigiKey or Mouser and solder it after, it's just two pads.
- Also the ADS1299 is the expensive one and the BNO055 has a history of going out of stock. Check live stock right before you order.

---

## Software side

There's no separate phone app. Everything runs inside the NeuroVox website.

- The public site explains the device. The actual companion app is behind an activation code that comes in the box, so only owners can get in.
- Bluetooth runs through the Web Bluetooth API. Works in Chrome on Android and desktop. Doesn't work on iPhone Safari, that's an Apple limitation, there's a demo mode for that case.
- Speech uses the browser's built in text to speech, so no extra voice API is needed.
- The Claude API call goes through a server side function so the API key never sits in the browser.
- Inside the app: live gesture and sentence display, conversation history with translation, charts (hourly activity, weekly trend, gesture mix, live signal trace), device status and a signal test.

The software subscription (₹20,000 a year) covers the AI sentence generation, which is a real running cost per message, plus updates.

---

## Repo layout

```
/PCB        KiCad board file and Gerbers
/CAD        TOP.stl, BOTTOM.stl, FULL.stl
/FIRMWARE   main.cpp, diagram.json, wokwi.toml
NeuroVox_v2_0_FULL_BOM.csv
NeuroVox_v2_0_BOM_organized.csv
```

---

## What's done and what isn't

Done:
- PCB layout, fab ready
- Enclosure, both halves, printable
- Firmware classification pipeline, tested in simulation
- BOM with verified part numbers
- Companion website with the gated app

Not done yet:
- Boards aren't fabricated, waiting on funding
- ADS1299 SPI driver for the real board
- Real accuracy numbers on actual people
- Voice cloning (record someone's voice before they lose it, then speak in it later). The hook is in the code but nothing's connected
- A two step gesture menu (pick a category like body, food, comfort, emergency, then pick inside it) so people can say more specific things

---

## Not a medical device

NeuroVox is a student research project. It isn't certified or clinically validated, and it shouldn't replace an AAC device a clinician has prescribed or be the only way someone calls for help in an emergency. If you're a caregiver please stay involved and don't rely only on what the band says.

---

## Links

- Main repo: https://github.com/Swapnil007-Curious/NeuroVox-v2.0
- CAD: https://github.com/Swapnil007-Curious/NeuroVox-v2.0/tree/main/CAD
- PCB: https://github.com/Swapnil007-Curious/NeuroVox-v2.0/tree/main/PCB
- Firmware: https://github.com/Swapnil007-Curious/NeuroVox-v2.0/tree/main/FIRMWARE

## License

See the LICENSE file. If you build one, I'd honestly love to hear how it went.

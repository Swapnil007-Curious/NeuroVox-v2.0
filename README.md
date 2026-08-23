# NeuroVox v2.0

A forearm-worn EMG device that turns muscle twitches into speech. Built for people who've lost the ability to talk but haven't lost the ability to move a muscle somewhere — most commonly the forearm, even after a stroke or with advanced ALS.

<img width="1536" height="1024" alt="image" src="https://github.com/user-attachments/assets/d2b3fa7e-3171-4c20-a8b0-e32521bf3689" />


## Why this exists

Stephen Hawking ran his entire speech synthesizer off a twitch in his cheek. That's the whole premise here, just moved to the forearm and built with parts that cost less than a used textbook.

Commercial AAC (augmentative and alternative communication) hardware — eye trackers, switch boards, dedicated speech devices — runs anywhere from ₹1.5 lakh to ₹5 lakh and usually needs a clinician to configure it. That price tag puts it out of reach for most families in India, and honestly most places. NeuroVox is an attempt to get 80% of the functionality for under ₹4,000 in parts, using an approach any electronics student with a soldering iron (or, in my case, a JLCPCB assembly order because I once destroyed an Arduino trying to hand-solder it) can replicate.

## How it actually works

Four EMG channels read muscle activity from two sites on the forearm — the flexor digitorum superficialis on the inner arm and the extensor digitorum on the outer arm, two channels each. The electrodes themselves aren't part of the enclosure. I used standard 2"x2" medical-grade TENS pads, stuck directly to the skin, with leads running into the device through small cable clips on the side of the housing. Simpler to build, easier to source, and honestly more comfortable than trying to mold electrodes into a plastic shell.

Each channel goes through an AD8232 front end for amplification and bandpass filtering, then into an ADS1299 24-bit ADC that samples all four channels together. An INA333 handles extra differential conditioning on two of the channels. A BNO055 IMU sits on the board too — its whole job is catching sudden arm movement and telling the firmware "ignore this sample, that's motion, not a gesture," which cuts down on false triggers a lot more than I expected going in.

The ESP32-S3 runs the actual classification: an envelope detector smooths the raw signal, an adaptive threshold per channel adjusts itself against a rolling average so the system doesn't drift out of calibration as it gets used, and a state machine counts pulses to map them onto phrases (one pulse = yes, two = no, three = help, and so on — twelve-plus combinations are possible once you start combining channels). Once a gesture clears a silence window, it goes out over BLE to a phone.

## The board

Two-layer, 220mm x 50mm, split into three zones along its length — power, analog front end, digital/radio. The narrow strip shape isn't an aesthetic choice, it's so the board sits along the forearm instead of across it.

| | |
|---|---|
| MCU | ESP32-S3-WROOM-1 |
| ADC | ADS1299, 24-bit, 4 channels used |
| Analog front end | 4x AD8232 + 1x INA333 |
| IMU | BNO055 |
| Charging | MCP73831, USB-C input |
| Power | TPS61023 boost converter, AMS1117 3.3V regulator |
| Battery | 5000mAh LiPo pouch cell |
| Radio | BLE 5.x via the ESP32-S3's own stack |
| Extras | DRV2605L haptic driver, MAX98357A audio amp, RGB status LEDs, piezo buzzer |

Analog and digital ground are kept as two separate pours on the board and only meet at one point, through a ferrite bead, right where the analog zone ends and the digital zone begins. This matters more than it sounds like it should — the first pass at this board was 200mm long and the analog section was cramped enough that routing a clean ground reference to the instrumentation amp was genuinely not possible in the space available, proven by exhaustive pathfinding search, not just a bad autorouter run. Stretching the board to 220mm and giving the analog zone 90mm instead of 65mm fixed it. If you're forking this and want to shrink the board back down, budget real time for that fight.

## The shell

Two pieces — a rigid top half and a slightly flexible underside that actually touches skin — that snap together with a small interference lip instead of screws. Four alignment pegs keep the halves lined up during assembly. Two strap wings on the long edges take a fabric or silicone band. There's a thinned section over the antenna so the ESP32-S3's onboard antenna isn't shooting through 3mm of solid plastic, ventilation slots over the parts that run warm, and cutouts for the USB-C port, a small speaker grille, the status LEDs, the buzzer, and a programming header for flashing firmware without opening the case every time.

Files are exported as both STEP and STL, top and bottom as separate solids so you can reprint just one half if you mess up a dimension (ask me how I know this is worth doing).

## Firmware

C++ on the Arduino/ESP-IDF framework, split across four FreeRTOS tasks: ADC sampling on its own core so BLE traffic can't jitter the sample timing, gesture classification, BLE dispatch, and IMU monitoring for motion rejection.

Right now the classification pipeline is fully built and validated against a bench setup using potentiometers standing in for the EMG front end and an MPU6050 standing in for the BNO055 — same signal shapes, same timing, easier to test without a person attached to alligator clips at 1am. The part that's left is swapping the potentiometer reads for a real SPI driver talking to the ADS1299 on the actual board. That's next, once the assembled PCB is back from fab.

All four channels are treated equally in the classifier. An earlier board revision had a ground reference issue on two of the four channels and the firmware briefly compensated for it with per-channel weighting — that compensation got pulled once the board redesign actually fixed the underlying problem, because leaving asymmetric logic in for a hardware issue that no longer exists would just make things worse.

## What's not done yet

The phone app doesn't exist yet. The plan is for it to take the raw gesture label the board sends over BLE and hand it to an LLM (Claude, most likely, since that's what I've used for everything else building this) to turn "CH1:YES" into an actual sentence with context, rather than just flashing a static word on screen. That's the next phase.

## Rough cost

Bill of materials lands somewhere around ₹3,500–5,000 depending on order quantity from JLCPCB and whatever the LiPo pouch cell costs that week. That number is just parts and assembly — it doesn't include the PCB design iterations, the enclosure prototyping, or the software work that went into getting here, which is a separate and much larger number if you're trying to account for total development cost rather than unit replication cost. Worth keeping those two figures separate when you're citing either one.

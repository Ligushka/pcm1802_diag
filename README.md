# PCM1802 slot diagnostic

Diagnostic firmware for the [RP2350B_DualADC_DualPCM](https://github.com/Sev5000/RP2350B_DualADC_DualPCM)
carrier board, for when the PCM1802 audio streams of the
[hsdaoh-rp2350](https://github.com/steve-m/hsdaoh-rp2350) `dual_external_adc` firmware
produce 0 KB files.

It drives the same 40 MHz clock as the production firmware (GP20/GP22 ADC clocks,
GP21 = SCKI for both PCM1802 slots), measures what each PCM1802 sends back, and
prints a report with a verdict over USB serial once per second.

A 0 KB audio file means the RP2350 never received LRCK edges from the PCM1802.
With MODE0 high / MODE1 low the PCM1802 is the clock master and must generate
LRCK = 78.125 kHz and BCK = 5 MHz from the 40 MHz SCKI by itself. If it doesn't,
nothing is recorded. This firmware shows which part of that chain is missing.

## Build

Needs the Pico SDK 2.x. Either import this folder as a project with the
Raspberry Pi Pico VS Code extension and build, or:

```
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=<path to pico-sdk>
cmake --build build
```

It builds for `solderparty_rp2350_stamp_xl` (RP2350B, same as `dual_external_adc`)
unless you pass another `-DPICO_BOARD=`.

## Run

1. Hold BOOTSEL on the carrier, plug in USB, release. Copy `build/pcm1802_diag.uf2`
   to the `RP2350` drive.
2. Open the new COM port / ttyACM device in any serial terminal (baud rate does not
   matter).
3. When done, flash the production `dual_external_adc.uf2` again.

Keys (send over the serial terminal):

- `d` cycles the GP21 pad drive: 4 mA slow (production default), 4 mA fast,
  8 mA fast, 12 mA fast.
- `f` cycles SCK: 40 (production), 30, 24, 20, 12 MHz. Expected LRCK/BCK follow
  the selected clock (LRCK = SCK / 512, BCK = SCK / 8).

The clock is paused for 20 ms on every change so the PCM1802s restart cleanly.
The firmware runs at 240 MHz from RAM (production runs 320 MHz).

Always unplug USB before plugging or swapping modules.

## Slots

| Carrier silkscreen | DOUT / BCK / LRCK | Recorded by |
|---|---|---|
| PCM1802 #1 | GP0 / GP1 / GP2 | `hsdaoh_file -3 <file>` |
| PCM1802 #2 | GP3 / GP4 / GP5 | `hsdaoh_file -2 <file>` |

Note the crossed numbering. `-4` / `-5` carry the aux (head switch) bits.

## Reading the report

A working module looks like this:

```
[PCM1802 #2]  GP3-GP5, recorded by: hsdaoh_file -2 <file>
  LRCK GP5    78.124 kHz   expect   78.125 kHz
  BCK  GP4     5.000 MHz   expect    5.000 MHz
  DOUT GP3   468.473 kHz   expect toggling
  => Master mode, SCKI = 512 fs (MODE0 high, MODE1 low), fs = 78124 Hz.
     Correct for the current firmware.
```

DOUT toggles even with no audio connected: the noise in the lowest bits of a
24-bit converter is enough.

Lines that don't toggle are tested by switching the RP2350's internal pull-up and
pull-down:

- **FLOATING**: nothing drives the line (empty slot, bad contact, or a chip input).
- **held LOW / held HIGH**: the chip is driving it but not clocking.

| Symptom | Likely cause |
|---|---|
| All lines FLOATING | Empty slot, module not making contact, or chip idle |
| BCK/LRCK FLOATING, DOUT held | Slave mode: MODE0 not high at IC pin 19 |
| BCK/LRCK held LOW or HIGH | Chip not running: check SCK -> pin 15, PDW -> pin 7, 3.3V -> pin 14; otherwise a bad chip |
| Clocks OK, DOUT held LOW | Converter not running: 5V analog supply missing (see below) |
| Clocks OK, DOUT FLOATING | DOUT not connected (header -> IC pin 12, socket) |
| SCKI = 256 fs | MODE0 and MODE1 both bridged. Right for the cxadc clockgen, not for current hsdaoh |

Before testing modules, run it once with **no modules plugged in**. Every line in
both slots should read FLOATING and SCK 40 MHz. Anything else is a carrier fault.

## Module checklist

These cheap breakout boards come with several design and assembly faults, which
differ between sellers. Check every module **unpowered, with the module out of the
socket**. Touching the IC legs with a needle is safe without power.

Pin 1 is at the dot on the chip. Pins 1-10 run down that side and pins 11-20 come
back up the other, so pin 11 is directly across from pin 10 and pin 20 is across
from pin 1. It is easy to count 11-20 the wrong way; confirm with pin 14 (VDD),
which always beeps to the 3.3V header pin.

| From | To IC pin | Expected |
|---|---|---|
| SCK header | 15 (SCKI) | beep |
| PDW header | 7 (PDWN) | beep |
| 3.3V header | 14 (VDD) | beep |
| 3.3V header | 19 (MODE0) | beep (after the MODE0 mod) |
| 3.3V header | 20 (MODE1) | **no** beep (for hsdaoh) |
| 5V header | 5 (VCC) | **~0 ohm** (resistance mode) |
| 5V header | 4 (VREF2) | **~1 kohm** through R1 (resistance mode, a beep test shows "open") |
| GND header | 6 and 13 | beep |
| LRCK / BCK / DOUT header | 10 / 11 / 12 | beep |

Press gently on each leg while measuring to find cracked joints.

## Known board faults

1. **"+" jumper pads not connected to 3.3V** (all versions seen so far). Bridging
   MODE0 does nothing on its own. Bridge MODE0 and run a wire from the 3.3V header
   pin to the "+" pad. All "+" pads are connected to each other, so only bridge the
   jumpers you want high. OSR reads ~2.8 V: that is a 10 k pull-up against the
   chip's internal 50 k pull-down, and still counts as high.
2. **Regulator output not connected to the 3.3V rail** (some AliExpress boards). The
   chip gets no VDD. Wire the regulator's 3.3V output to the 3.3V capacitor.
3. **No 5V at VCC (IC pin 5)** (some AliExpress boards). The digital side runs on
   3.3V and generates correct clocks, but the converter has no supply and DOUT
   stays low. 5V header -> pin 5 reads open. Fix: bridge the regulator's 5V input
   leg (the one that beeps to the 5V header) to the capacitor pad that beeps to
   pin 5. Afterwards 5V header -> pin 5 ~0 ohm, -> pin 4 ~1 kohm, and pin 5 -> GND
   must not be a short.

## Bad chips

If a module passes every check above and still does not clock at any `f` setting,
the chip itself is the likely problem. One batch of boards seen so far had good
wiring but chips that never started in any mode (512 fs or 256 fs), at any SCK from
12 to 40 MHz, or after repeated power cycles. One of them left BCK/LRCK floating
with both MODE pins measured high at the chip, which a genuine PCM1802 cannot do.
Those were almost certainly counterfeit or remarked parts.

The board and chip faults are independent: one seller's boards needed wiring fixes
but had working chips, another seller's boards were wired correctly but had dead
chips. A genuine TI PCM1802 (PCM1802DBR, SSOP-20) can be fitted to a correctly
wired board.

## Testing modules for the cxadc clockgen

The cxadc clockgen runs the PCM1802 at 256 fs (MODE0 and MODE1 bridged) from a
12 or 12.288 MHz clock. To test a module in that configuration, bridge both, press
`f` until SCK is 12 MHz, and expect LRCK = 46.875 kHz, BCK = 3 MHz. The report's
"expect" column assumes 512 fs, so it shows half those values; the verdict line
recognises 256 fs. The clockgen's header faces the other way, so the module needs
jumper wires to reach the carrier socket.

## Carrier notes

- GP21 (both PCM1802 SCKI pins) is only used for the PCM1802s. The ADC clocks
  being fine says nothing about GP21. It sits on PGA2350 header J3, pin 4.
- The production firmware leaves GP20-22 at the pad default (4 mA, slow slew). In
  testing that was not a problem; the `d` key lets you check.
- A multimeter on DC volts reads a 40 MHz 3.3 V clock as ~1.2-1.6 V.

## License

BSD-3-Clause, same as hsdaoh-rp2350. `pico_sdk_import.cmake` is copied from the
Raspberry Pi Pico SDK.

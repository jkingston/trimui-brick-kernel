TrimUI Brick
============

Configuration: ``trimui_brick_defconfig``.
Device tree: ``allwinner/sun50i-a133-trimui-brick.dtb``.

This port targets A133 revision 6. Boot firmware must initialize DRAM and
PMIC rails. Removable SD uses 3.3 V, four data lines and a 25 MHz limit;
internal eMMC remains disabled. UART0 is PB9/PB10 at 115200 8N1. USB is fixed
peripheral mode.

Display
-------

JD9168, four-lane DSI, 1024 x 768. DSI pins are PD0..PD9, panel reset PD22,
backlight PWM0 PD23 (50 kHz), backlight enable PH18. CLDO1 is a shared 1.8 V
rail and must remain enabled; CLDO4 is 3.3 V. The panel supply driver checks
inherited selectors and never programs charging or voltages.

DE requires SRAM C ownership (system-control 0x03000004 bit 24 cleared),
then its internal reset, bus and module gates. The working clock plan uses
DE at 300 MHz, TCON at 300 MHz with 75 MHz DCLK, and DPHY at 150 MHz.

GPU
---

GE8300, BVNC 22.102.54.38, vendor DDK 1.19@6345021. Matching firmware and
userspace libraries are required. The A133 power domain sequences power-on
and hands the PPU to hardware control before the GPU driver probes.
The operating point is fixed at 432 MHz, DFS=1. Power-off, suspend and DVFS
are unsupported; a failed power-on requires reboot.

The optional display-metadata adapter supports stock NULL_WSEGL pbuffer
rendering. It does not provide scanout or zero-copy KMS presentation.

Controls
--------

GPIO keys are active-low. A/B/X/Y use PD12/PD13/PD18/PD19; the D-pad uses
PD14..PD17 (down/left/right/up). L/R use PB4/PH17, L2/R2 use PB2/PB5,
select/start use PH15/PH16, and menu/F1/F2 use PH14/PH12/PH13.
Volume down/up use PD20/PD21. The AXP2202 power key uses NMI IRQ0 and
reports KEY_POWER press/release; inherited long-press timing is preserved.
The rumble motor uses PH3, active-high, through the standard force-feedback
interface; its supply remains boot-established.

Other peripherals
-----------------

SD card detect is PF6, active-low with a pull-up. LEDC drives the stock
23-entry RGB chain on PE5 at 24 MHz, using PIO. Physical LED index mapping
has not been verified.

AXP2202 telemetry reads battery voltage, initialized gauge capacity and USB
input presence. Invalid or uninitialized readings return no data. Confirmed
charger states report charging; ambiguous idle/full states report unknown.
Charging configuration, battery-presence/full flags and shutdown are not
implemented.
The PMIC core preserves boot-established voltages and charging settings.
ALDO3 and BLDO1 are boot-enabled at 3.3 V for the radio; ALDO4 is 1.8 V
for audio. Shared rails cannot be disabled or have their voltage changed.

The audio codec exposes 44.1/48 kHz S16 stereo playback. Headphones and the speaker
amplifier share HPOUTL/R. PH6 enables the amplifier with a 40 ms settling delay;
the Speaker switch defaults off. Jack detection and other rates are unsupported.
DMA request 7 feeds the DAC at 0x05096020; ALDO4/CLDO1 supply AVCC/CPVIN at 1.8 V.
PLL_AUDIO must remain enabled at 98.304 MHz even for 44.1 kHz playback, which
uses PLL_COM/5 at 90.3168 MHz and a 22.5792 MHz DAC module clock.

TCS4838 at I2C address 0x41 preserves the boot-enabled CPU supply in bank 0.
CPU frequency control requires speed fuse 0x8000 and checks PLL/mux completion
before allowing voltage reduction. The table spans 408–1992 MHz, from 900 to
1250 mV. The default userspace governor preserves the boot frequency until
requested. A failed clock transition latches a 600 MHz bypass until reboot.
The stock 2000 MHz request rounds to 1992 MHz on the 24 MHz integer PLL.

MMC1 enumerates SDIO at 25 MHz and 3.3 V, with PL5 enable and a 100 ms delay.
The populated XR829 enumerates as 0a9e:2282; its driver is not integrated yet.
RTC time uses stock firmware's one-based days from 1970. The bus clock stays
enabled after a startup counter check. Time writes use bounded busy checks
and readback; a partial failure blocks reads until a successful explicit set
within the current driver instance. Rebinding or rebooting clears that state.
Armed or queued alarms block time writes. Awake one-shot alarms use absolute
day/time, with a 255-day scheduling horizon matching stock policy. Clock
outputs remain boot-established. The system clock is initialized from the RTC;
automatic RTC writeback and suspend/power-off wake are unsupported. Wireless operation, USB host mode
and GPU power management remain unsupported. All CPU operating points,
clock-failure recovery, automatic scaling and passive thermal throttling have
been exercised. Audio DMA and amplifier sequencing have been exercised;
audible output remains unverified.

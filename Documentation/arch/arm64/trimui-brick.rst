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
Volume down/up use PD20/PD21. The PMIC power key is not yet supported.

# Audio, Wi-Fi and board peripherals

## Audio codec and ALSA

Patches303 add the RK3128 internal codec and binding. The RK3128 reference board has one fitted mono speaker even though the PCM path is stereo. The reference Linux mix runs **44.1 kHz**, S16_LE, two channels, with plug→softvol→dmix→hardware. At mclk-fs256 the master clock is **11.2896 MHz**. Open PCM only while playing; a48 kHz source needs resampling before the fixed-rate mix.

[examples/audio/asound.conf](../examples/audio/asound.conf) is a generic configuration extracted around this proven PCM arrangement. It uses period1024, buffer3072 (about69.7 ms total), shared IPC permissions0660/group audio, and explicit `hw:0,0,0`. Choose card/IPC policy for your system. A PCM subdevice must be explicit when sharing dmix across processes on hardware/test cards with multiple subdevices.

The alsa-lib patches remove reads from the uncached slave DMA buffer in dmix and add a NEON interleaved stereo S16 softvol loop. Build against the pinned Debian orig tarball **plus its Debian patches**, then apply this local series. Preserve the rest of the matching ALSA configuration/plugin installation rather than replacing arbitrary sonames across versions.

Keep codec gain at or below **0 dB** (the observed raw volume26) and turn **Speaker Switch off at mute**. Softvol can supply attenuation below the hardware floor; maximum software gain here is0 dB. A successful `aplay` exit does not establish acceptable acoustic output or transitions.

## Measured audio CPU cost

At **1008 MHz CPU**, the same **44.1 kHz stereo** playback through plug→softvol→dmix used **2.18% of one core with Debian's libasound**, versus **0.82% with the dmix and NEON softvol patches** (about **62% less CPU**). Both used volume index 10, period 1024 and buffer 3072; the recorded runs had **zero underruns**. This comparison holds the PCM path and buffer settings constant. It is not a prediction for other sample rates, volume settings or CPUs; a 48 kHz source also incurs resampling cost.

## SSV6006C SDIO Wi-Fi

`linux/modules/ssv6x5x` is the preserved SSV6006C/SV6256P station-mode source, with a generic mainline entry path. Board power/reset and card discovery belong to `mmc-pwrseq-simple`/the SDIO host, rather than legacy Rockchip rfkill callbacks.

The recorded RK3128 reference board SDIO profile uses **PL330 DMA at37.5 MHz**, matching the vendor host cap; configurable PIO/clock alternatives were diagnostic comparisons. The module uses one consistent set of Kbuild feature defines so all translation units agree on structure layouts. The observed vendor output-timing setting is0 and block size128. Keep bounds/FORTIFY fixes and rate-control/scan behavior together when comparing variants.

**Licensing boundary:** many iComm source files explicitly grant GPL-3.0-or-later, while other files use GPL2+/BSD terms and module metadata says “GPL” or “Dual BSD/GPL”. Module metadata does not override file grants. This directory is a separate mixed-license reference aggregate; do not assume it is a legally linkable GPL-2.0-only kernel module. See [NOTICE.md](../NOTICE.md).

**No firmware blob is included.** Obtain the firmware separately under its own terms; this source license is not a redistribution grant for `ssv6x5x-sw.bin`. The included minimal cfg is independently rewritten from functional station settings and contains no SSID, credential or sample MAC address. Do not invent a globally shared fixed MAC.

The wpa_supplicant patches try the previous channel when reconnecting and prefer2.4 GHz with fallback when no preferred candidate is usable. Apply them to the pinned Debian source/patch baseline. Regulatory policy remains separate: the kernel must trust the signature on the installed regulatory.db, and an early failed load may need a reload after the root is available. Match the signing-key policy to the database; do not disable regulatory enforcement to fix missing5 GHz channels. Country and allowed channels depend on your location.

## Touch and input

The fitted Silead controller is at I2C0 address0x40, with interrupt GPIO2_D1 and reset/power GPIO2_D0. The calibration maps the measured orientation to X0..800 and Y0..1280 inclusive (DT size801×1281), which is not the display pixel coordinate space. [Touch](TOUCH.md) explains the firmware requirement, calibration and included point algorithm.

The power button is a real GPIO key. A stock “sensor” GPIO is an accelerometer control line, not another button. ADC keys and IR are disabled in the selected wrapper: on the tested board the ADC's idle value fell inside a volume-key window, and enabling an unused IR receiver would expose key events. Probe candidate hardware before enabling such inputs.

The supplied board data also records the HYM8563 RTC on I2C0 address0x51 and the sensor/input wiring found on this RK3128 reference board revision. Their presence on a different RK3128 board is not implied.

## USB, eMMC, power and reset

The RK3128 OTG controller is dwc2; `rk3128-usb.dtsi` selects peripheral mode. A CDC-NCM or other USB gadget still requires your own userspace configfs/service setup, which is outside this repository. There are no embedded host addresses or network provisioning settings.

The eMMC reference is8-bit with PL330 DMA and DDR52 at50 MHz, alongside PIO25/DMA25/DMA50 alternatives for diagnosis. A long PIO transfer in hard IRQ can occupy CPU0 and interfere with frame deadlines; avoid attributing such stalls to the GPU without interrupt evidence. The board cache opt-out patch is a separate policy knob, not a general claim that every eMMC cache should be disabled.

The halt/power-key patch handles a board that cannot cut mains supply: stop the watchdog, turn the display off, halt and restart from the power key. Reboot-mode/PMU state and explicit panel sequencing are board/bootloader integrations. None of this repository's tools is a generic safe flashing/update system; identify your actual partition layout and recovery route first.

The optional [CDC-NCM configfs example](../examples/usb/rk3128-usb-gadget) creates only the gadget. It assigns stable locally administered MAC addresses from the local serial and includes no IP addresses or login configuration. Bring the NCM interface down **before** UDC unbinding: the opposite order triggered a gadget-debug teardown fault on the tested kernel.

The matched alsa-lib dmix patch also changes the shared-area magic to prevent mixing incompatible shared-ring layouts. All dmix writers in the same IPC group must use the matching library; running patched and stock writers together is deliberately rejected. The measured16-step music curve in centidB is `{mute,-5021,-4211,-3400,-3055,-2653,-2250,-1905,-1503,-1100,-935,-743,-550,-385,-193,0}`; split attenuation between the codec and softvol while respecting the hardware floor and0dB maximum.

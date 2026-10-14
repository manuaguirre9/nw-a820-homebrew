# Investigación del firmware stock NW-A829 (JP 1.02) — 2026-09-26

Análisis de solo lectura del rootfs JP, kernel (`fw/jp_file1.bin`), initrd (`fw/jp_file2.bin`) y dumps reales (`debug_dump_8`, `debug_dump_jp1`). Nada flasheado, nada de DRM/NVP tocado.

## Hardware
- SoC NEC MP201: ARM926EJ-S (ARMv5TEJ) + DSP NEC SPXK5 (decodifica todos los códecs). Modos de reloj 248/124/83/50/12 MHz; arranca ~124 MHz (lpj=618496).
- RAM 20.8 MB, sin swap. ~2.4 MB libres con gemini; ~14-15 MB estimados sin gemini.
- NAND 16 GB (4x Toshiba). USB device-only (Epson S1R72V17): no hay USB host.
- DAC/códec: **Cirrus Logic CS42L51** (SPI control, I2S+DMA), con amplificador de auriculares propio. No es S-Master.
- Bluetooth: chip **CSR BlueCore** por UART `/dev/tts/0` a 2.592.000 baud (BCSP). Pila CSR BCHS entera dentro de `hci_bchs.ko` (incluye SBC). La UART se comparte con la consola serie.
- PMIC RW5T735. LCD 240x320 vertical 16 bpp (rotado por el kernel), backlight por ioctl.
- FM: driver Sanyo presente pero desactivado (ICX_FM_ON=0). **Noise cancelling analógico presente** (ICX_NC_ON=1, icx1111_ncan.ko, MAX5489).

## Flash / particiones
| mtdblock | Tamaño | FS | Montaje | Notas |
|---|---|---|---|---|
| 1 RootFS | 48 MB | ext2 ro | / | se reescribe en cada update; ~4.7 MB libres (≈7 MB con reservados; +8-9 MB quitando mptapp/test) |
| 2 Contents | 15.2 GB | VFAT | /contents | disco USB; fmask 0022 → se pueden ejecutar binarios desde acá |
| 3 Var | 4 MB | ext2 | /var | logs |
| 4 Db | 10 MB | ext2 | /db | certificados DRM — NO TOCAR |
Bloques 72-143 = NVP de fábrica. U-Boot + kernel de actualización en zona de loader (fuera del paquete; tocarlo = riesgo sin recuperación).

## Arranque
rcS → rc 2 (rc.altinitrd, rc.firmversion, rc.sysinfo) → S10 módulos (DRM, USB g_nwm/msc/mtp, NC, BT) → S20 hwclock → S42gemini (mptapp si flag tst; si no glogd, cogemini, `watchman -s 20 -X reboot.sh gemini`, fallback rescue USB) → S60 getty (solo si NVP gty) → S70 red (sale) → S80 telnetd (inútil) → S90 watchdog cada 5 s + loggers.
- Punto de doble arranque: S41 nuevo o en /etc/init.d/gemini. No tocar S10 (USB masivo + fwtrig para reflashear). Montar /contents a mano, manejar USB como `apl_conn -a`, mantener el watchdog.
- Lectura de teclas: `/dev/input/event0` (keycodes Sony 0-7,16-23), ADC crudo `/dev/mp200_ad`; `ad_test` (canal 3, ≤29 = Play); HOLD por ioctl 0x80044519 en event0. 11 botones: V+ V- UP DW RW FF PL OP BK BT HO.

## Kernel 2.6.10 (Image sin comprimir, 0xC0008000)
Incluye: devfs, sysfs, tmpfs, ramfs, ext2, VFAT, NFS, loop, input/evdev, fb, V4L2, OSS+ALSA 1.0.6, TCP/IPv4, AF_UNIX, epoll, ptrace, core dumps, /sys/power/state. Falta: inotify, IPv6. Gadgets USB solo como módulos → g_serial/g_ether compilables desde el GPL del A808 (consola/red por USB para desarrollo).

## Audio
- PCM: OSS `/dev/sound/dsp`, S16_LE estéreo, **solo 11025/22050/44100 Hz**. Mixer `/dev/sound/mixer` (volumen 32 pasos, `/etc/AudioC_VolTbl`), beep, switch auriculares/cradle line-out. No hay libasound.
- Decodificación en DSP vía OpenMAX IL (`/usr/lib/omf`, `libomx_core.so`, `/dev/InterDSP/control`): MP3, AAC/HE-AAC, WMA, LPCM, ATRAC3/3plus/AAL. Sin FLAC ni Vorbis. Video MPEG-4 SP y H.264 Baseline.
- Efectos (EQ 6 bandas, VPT, DSEE, Clear Stereo, normalizador, AVLS) viven en el DSP: un PCM propio escrito a /dev/sound/dsp NO los recibe.
- `/usr/local/bin/dsp_play <dev> file.wav` reproduce WAV (prueba rápida).

## Bluetooth
- Perfiles: A2DP (fuente y sumidero), AVRCP (1.3), RFCOMM/SDP/L2CAP/BNEP genéricos. **Sin OBEX/OPP/FTP/HFP/HID/SPP** → pasar archivos desde el teléfono no es práctico.
- Nodos: /dev/icx_bt (on/off ioctl 0x400448C9/CA), /dev/icx_uart_sw, /dev/peer_com (API de mensajes CSR), /dev/sc_db (llaves de emparejamiento), /dev/sound/adsp (PCM → SBC en kernel).
- Herramientas de fábrica: bt_test (1/2/3/4), bc_attach, setup_bt2, a2d_demo_app (menú: buscar, emparejar PIN, conectar; guarda sc_db.db local). gemini guarda emparejamientos en NVP 35 — no escribir.
- Experimento propuesto: sin gemini, bt_test 3 + bc_attach + a2d_demo_app por FIFO + dsp_play a /dev/sound/adsp.

## Software
- Toolchain Sony: MontaVista gcc 3.4.3 EABI, glibc 2.3.3 con backport EABI (símbolos GLIBC_2.4), LinuxThreads. Reutilizables: libc, libm, libpthread, libdl, libgcc_s, libstdc++ 6.0.3, ncurses 5.4. Nada de zlib/png/freetype/SDL compartidos.
- Busybox 1.1.0 (123 applets; tiene dd, hexdump, tar, vi, awk, wget, mke2fs, chroot; no tiene printf, mkfifo, nc).
- gemini: frameworks DUI (UI), dwarf (rasterizador a /dev/fb/0), GAP, OmfMgr. SQLite y libjpeg estáticos dentro.
- Fuentes bitmap 1-bit 22 px en `dui_font.dpk` (latín completo, kana, 6351 kanji, coreano, chino) — extraíbles para nuestras apps.
- Funciones del JP que el intl NO tiene: **Grabación directa (line-in → ATRAC/PCM)**, página de noise cancelling, ratings.
- Sin podcasts, alarma, sleep timer ni juegos en el firmware stock.
- Modo fábrica mptapp (flag NVP tst) y getty serie (flag NVP gty): requieren escribir NVP → evitar.
- Tests útiles: key_event_test, icx1087_power_test (batería), alarm_test (alarmas RTC), backlight_test, colorbar (fb), dhry21 (benchmark), lrz/lsz, cde (core dumps).

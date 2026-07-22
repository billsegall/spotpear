# Toolchain setup

Verified working on this machine (Ubuntu 24.04 noble, x86_64) on 2026-07-22.

## Installed

| Component | Version | Location |
| --------- | ------- | -------- |
| ESP-IDF | v5.4 | `~/esp/esp-idf` |
| Tools + Python env | — | `~/.espressif` (2.6 GB) |
| Compiler | `riscv32-esp-elf-gcc` 14.2.0 (crosstool-NG esp-14.2.0_20241119) | via export.sh |
| esptool | 4.12.0 | via export.sh |
| cmake | 3.28.3 | system |
| python3 | 3.12.3 | system |

Cloned shallow: `git clone -b v5.4 --recursive --depth 1 --shallow-submodules`.
Installed for a single target: `install.sh esp32c3`.

## System packages

Already present on this box: `wget flex bison gperf libffi-dev libssl-dev
libusb-1.0-0 python3-venv python3-pip cmake ccache git`.

Had to be added:

```sh
sudo apt install -y ninja-build dfu-util
```

ESP-IDF does not ship ninja for Linux — the build fails without the apt package.

## Per-shell activation

```sh
. ~/esp/esp-idf/export.sh
```

Sets `IDF_PATH` and puts `idf.py` plus the riscv32 toolchain on `PATH`. Needed in
every new shell; nothing is added to `.bashrc`.

## Flashing prerequisites

The C3 uses the built-in USB-Serial-JTAG, so the board enumerates as
`/dev/ttyACM0` (not `ttyUSB*`).

Serial access needs group membership — **not yet done on this machine**:

```sh
sudo usermod -aG dialout $USER   # requires logout/login to take effect
```

## Flash size gotcha

Default `sdkconfig` builds with `--flash_size 2MB`. The board carries more than
that (see [board.md](board.md)), so set the real size before shipping anything
that needs the space:

```sh
idf.py menuconfig   # Serial flasher config -> Flash size
```

Otherwise the partition table is sized against 2 MB and the rest is wasted.

## Verification done

`examples/get-started/hello_world` built clean for `esp32c3`: 961 targets,
exit 0, `hello_world.bin` = 0x2bcd0 bytes (~179 KB), 83% of the app partition
free. See [../apps/hello_world](../apps/hello_world).

Not yet verified: flashing and serial monitor — no board was connected.

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

`/dev/ttyACM0` is `root:dialout 0660`, so serial access needs `dialout`
membership. **`bill` was added to `dialout` on 2026-07-22** — no further group
change is needed.

Group membership is fixed at login, though, so any shell that logged in before
that change still runs without it and cannot open the port. Check the running
session rather than the account:

```sh
id -nG | tr ' ' '\n' | grep -x dialout   # this process
id -nG bill                              # the account, per /etc/group
```

If the first prints nothing while the second lists `dialout`, log out and back
in (or `newgrp dialout` for a single shell).

Until then, access rides on the `uaccess` ACL logind grants to the seat owner,
which is only applied when the device is plugged in during an active local
session. After a headless replug or a re-enumeration — which the board does on
every reset — the ACL is gone and `idf.py flash` fails with
`Path '/dev/ttyACM0' is not readable`. Replugging at the desk restores it, or:

```sh
sudo setfacl -m u:$USER:rw /dev/ttyACM0   # lost again on re-enumeration
```

On a machine where the account is genuinely not a member, the durable fix is
`sudo usermod -aG dialout $USER` — note `-aG`, not `-G`, which would replace
every supplementary group including `sudo`.

## Flash size

ESP-IDF defaults to `--flash_size 2MB`; the board carries 16 MB (see
[board.md](board.md)). A 2 MB header makes the bootloader log a mismatch warning
on every boot and caps what the partition table can address.

Each app therefore commits a `sdkconfig.defaults` with:

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

`sdkconfig` itself is generated and gitignored. **An existing `sdkconfig` wins
over `sdkconfig.defaults`** — defaults only fill in symbols that are not already
set. So in a checkout that has already been built, adding or changing this file
does nothing until you regenerate:

```sh
rm sdkconfig && idf.py set-target esp32c3   # or: idf.py menuconfig
```

New apps need the same file; there is no repo-wide defaults mechanism.

Note this only corrects the image header. Using the extra flash for app data
takes a partition table change on top.

## Verification done

`examples/get-started/hello_world` built clean for `esp32c3`: 961 targets,
exit 0, `hello_world.bin` = 0x2bcd0 bytes (~179 KB), 83% of the app partition
free. See [../apps/hello_world](../apps/hello_world).

Flashing and serial output verified on hardware 2026-07-22: `idf.py flash` over
`/dev/ttyACM0`, all three images hash-verified, board boots and prints chip info.
With the 16 MB default in place it reports `16MB external flash` and the size
mismatch warning is gone.

# Running Kestrel on a real STM32F4 board

## 1. Which board do you have?

Look at the text printed on the board, then pick the matching `BOARD=` value:

| Printed on the board | `BOARD=` | LED | Serial console |
|---|---|---|---|
| NUCLEO-F401RE / F411RE / F446RE | `nucleo_f4` | LD2 (green, PA5) | built in: the ST-LINK USB port is also a serial port |
| STM32F4DISCOVERY (STM32F407VG) | `disco_f407` | LD4 (green, PD12) | USB-UART adapter on PA2/PA3 (some newer revisions also have one built in) |
| WeAct "Black Pill" STM32F401/F411 | `blackpill` | blue LED on PC13 | USB-UART adapter on PA2/PA3 |

Every board runs on the chip's built-in 16 MHz oscillator, so no crystal or clock setup is
needed. The console is always **USART2: PA2 = TX, PA3 = RX, 115200 baud, 8N1**.

## 2. Install the tools

**Linux (Ubuntu/Debian)**
```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi make openocd stlink-tools qemu-system-arm python3
```

**Windows**: the easiest way is to build inside WSL2 (Ubuntu, same commands as Linux), then
flash from Windows with **STM32CubeProgrammer** (free from st.com). Windows paths: your WSL
files are at `\\wsl$\Ubuntu\home\<you>\kestrel-rtos`.

**macOS**
```bash
brew install --cask gcc-arm-embedded
brew install make openocd stlink qemu
```

## 3. Build

```bash
make BOARD=nucleo_f4        # or disco_f407 / blackpill
```

Output: `build/nucleo_f4/kestrel.elf` (with debug info) and `build/nucleo_f4/kestrel.bin`
(raw image for address `0x08000000`).

## 4. Flash (pick one)

| Method | How |
|---|---|
| **Nucleo drag-and-drop** (easiest) | The Nucleo shows up as a USB drive (`NOD_F401RE` or similar). Copy `kestrel.bin` onto it. The board reflashes itself and resets. |
| **STM32CubeProgrammer** (any OS, GUI) | Connect with ST-LINK, open `kestrel.elf`, click *Download*. |
| **OpenOCD** | `make BOARD=nucleo_f4 flash` |
| **stlink tools** | `st-flash write build/nucleo_f4/kestrel.bin 0x08000000` |
| **Black Pill over USB (no ST-LINK)** | Hold BOOT0, tap NRST, release BOOT0. Then `dfu-util -a 0 -s 0x08000000:leave -D build/blackpill/kestrel.bin`, or use STM32CubeProgrammer in USB mode. |

If the programmer cannot connect to a board that is already running firmware, choose
"connect under reset" (CubeProgrammer: *Mode: Under reset*). Kestrel keeps the debug port
alive during sleep (`DBGMCU_CR`), so this should rarely be needed.

## 5. Open the serial console

- **Nucleo:** the ST-LINK port appears as `COMx` (Windows) or `/dev/ttyACM0` (Linux/macOS: `/dev/cu.usbmodem*`).
- **USB-UART adapter (3.3 V!):** adapter **TX → PA3**, adapter **RX → PA2**, **GND → GND**.
  TX and RX cross over: each side's transmit goes to the other's receive.

Then connect at 115200 baud:

```bash
screen /dev/ttyACM0 115200          # Linux / macOS (quit: Ctrl-A, then K)
python3 -m serial.tools.miniterm /dev/ttyACM0 115200   # needs: pip install pyserial
```
On Windows, use PuTTY: *Serial*, `COMx`, speed `115200`.

Press the board's reset button. You should see:

```text
Kestrel RTOS - nucleo-f4, 16 MHz
[kernel] MPU on, starting scheduler

Kestrel shell. Type 'help'.
kestrel>
```

The LED should blink once per second. That blink comes from the unprivileged `heartbeat`
task, which drives the GPIO registers directly through its MPU grant.

## 6. Things to try, and numbers to record

| Command | What you'll see |
|---|---|
| `test` | the same 24 tests CI runs in QEMU, now on real silicon |
| `bench` | **real cycle counts** for the three NN kernels. Put these in the README table |
| `pi` | priority inversion with and without inheritance |
| `crash stack` / `crash kernel` / `crash null` / `crash div0` / ... | the kernel kills the bad task; the shell survives |
| `ps` | tasks, priorities, stack high-water marks, CPU% |
| `nn 4` | four digits drawn as ASCII art with the network's answer |

The `semaphore_ping_pong` test prints the cost of a round trip in cycles: 2 context switches
plus 4 system calls. That number, measured on your board, is a good one to quote.

## 7. Troubleshooting

| Symptom | Likely cause |
|---|---|
| No output at all | Wrong COM port, wrong baud, or TX/RX not crossed. Try pressing reset with the terminal already open. |
| Garbage characters | Baud rate is not 115200, or the board isn't an STM32F4. |
| LED doesn't blink, but the console works | Wrong `BOARD=` (each board's LED is on a different pin). |
| `crash ppb` says MemManage instead of BusFault | Either is acceptable to the test. |
| A test fails on hardware but passes in QEMU | That's a real finding, so keep the log. QEMU's timing differs from silicon, and the timing-based tests use tolerances. |

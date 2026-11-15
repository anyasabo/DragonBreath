# Profiling on the dev board

This page is for an ESP32-C3 dev board with the native USB Serial/JTAG port.
Do not use the profiling build on the Panda Breath or for a release.

## Safety of a JTAG halt

When OpenOCD halts the CPU, no firmware runs. Each GPIO keeps its last level,
so a relay output that is on stays on. OpenOCD also stops the watchdogs during
the halt. No soft cutoff, no watchdog and no comms deadman can act. Thus:

- Use a dev-board build only (`CONFIG_PB_DEVBOARD_SAFE`). It has no relay GPIO
  code. `tools/jtag_profile.py` refuses an ELF whose build does not have this
  option. It reads `config/sdkconfig.h` next to the ELF, or `--sdkconfig`.
- Never attach a debugger to a Panda Breath that is connected to mains.
- The script sends `resume` at these times: at the connect, after each sample,
  after an error, after a reconnect and at exit (also on Ctrl-C, SIGTERM and
  SIGHUP).
  If it cannot resume, it stops and tells you. Then resume the target by hand
  (`resume` in the OpenOCD console) or reset the board.

## The profiling build

`sdkconfig.profile-devboard` is an overlay on the native-USB debug profile. It
needs ESP-IDF v6.1, because ESP-IDF v5.3 has no frame-pointer option.

```bash
. "$IDF_PATH/export.sh"     # ESP-IDF v6.1
B=build-profile-devboard
idf.py -B "$B" \
  -DSDKCONFIG="$PWD/$B/sdkconfig.profile" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.debug-devboard-native;sdkconfig.profile-devboard" \
  build flash
```

The overlay sets these options:

- `CONFIG_ESP_SYSTEM_USE_FRAME_POINTER`: all compiled code keeps a frame
  pointer in `s0`, also in leaf functions. The panic output then has a real
  backtrace, and a sampler can walk the stack.
- `CONFIG_HEAP_TRACING_STANDALONE` with 10 callers for each record: `app_main`
  starts a leak trace at boot with 64 records.
- `CONFIG_HEAP_TASK_TRACKING`: the heap use of each task.
- `CONFIG_ESP_DEBUG_OCDAWARE`: with a debugger attached, a panic stops at a
  breakpoint and does not reboot.

## Heap callers

Attach GDB (see below) and dump the leak trace. Each record shows the size, the
task and 10 callers:

```text
(gdb) call heap_trace_dump()
```

The output goes to the USB console. To trace a short time only, stop the trace
first with `call heap_trace_stop()`, then use `call heap_trace_start(1)`
(`HEAP_TRACE_LEAKS`).

## Flame graph by sampling over JTAG

1. Start OpenOCD in one terminal. It opens the GDB port 3333 and the Tcl
   port 6666:

   ```bash
   openocd -f board/esp32c3-builtin.cfg
   ```

2. Sample in a second terminal. The script halts the CPU each 20 ms, reads
   `pc`, `ra` and `s0`, walks the frame-pointer chain and resumes the CPU:

   ```bash
   python3 tools/jtag_profile.py --elf build-profile-devboard/dragonbreath.elf \
     --interval-ms 20 --samples 3000 -o profile.folded
   ```

3. Make the graph. Use `flamegraph.pl` from Brendan Gregg's FlameGraph
   repository, or open `profile.folded` in <https://www.speedscope.app>:

   ```bash
   flamegraph.pl profile.folded > profile.svg
   ```

Read the result with care:

- Each sample halts the CPU for some milliseconds. This changes the timing.
  Use the graph to find hot code, not to measure exact times.
- A sample shows the code that runs at the halt. `prvIdleTask` and
  `esp_cpu_wait_for_intr` are the free CPU time.
- ROM code and prebuilt libraries (Wi-Fi, PHY, newlib) have no frame pointer.
  If the sample is in such code, the script adds the `ra` register as the
  caller. Then one level can be missing.
- Without the profiling build, the stacks have the `pc` and the `ra` only.

For one manual sample, use GDB:

```text
riscv32-esp-elf-gdb -ex "target extended-remote :3333" build-profile-devboard/dragonbreath.elf
(gdb) monitor halt
(gdb) bt
(gdb) continue
```


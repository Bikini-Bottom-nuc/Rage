# Field Navigation External

This Buildroot external adds the `field_nav_demo` target without replacing the
existing face detection demo source tree.

## Build

From `smartsens_sdk` inside the A1 builder container:

```bash
bash ./field_nav_external/scripts/build_field_nav.sh
```

The final flashable image is:

```text
output/images/zImage.smartsens-m1-evb
```

## Model Placement

Before building the final board image, place the converted model here:

```text
field_nav_external/src/field_nav_demo/app_assets/models/navroad_640x480.m1model
```

If the model is missing, the SDK can still build, but the target app exits with
a clear message at boot.

The default runtime path on the board is:

```text
/field_nav/app_assets/models/navroad_640x480.m1model
```

## UART Navigation Output

The A1 app sends a 16-byte navigation frame at 10 Hz from `GPIO_PIN_0` configured
as `UART0_TX`:

```text
A1 P4-15 / A1_D0_UART0TX -> level shifter 1.8V to 3.3V -> RDK X5 40Pin Pin10 / UART_RXD
A1 GND -> RDK X5 GND
```

Runtime options are available through environment variables in
`/field_nav/scripts/run.sh`:

```sh
FIELD_NAV_UART=1 FIELD_NAV_BAUD=115200 FIELD_NAV_RATE=10 /field_nav/scripts/run.sh
```

`FIELD_NAV_OSD_RATE` limits OSD update frequency so drawing does not dominate
the 90fps application loop. The default is `15`; set it to `0` for a no-OSD
performance run.

For competition evidence collection, run a fixed 60 second session and keep the
Aurora UART log:

```sh
FIELD_NAV_UART=1 FIELD_NAV_BAUD=115200 FIELD_NAV_RATE=90 FIELD_NAV_SENSOR_FPS=90 FIELD_NAV_OSD_RATE=15 FIELD_NAV_TEST_SECONDS=60 /field_nav/scripts/run.sh
```

The app prints one metrics line per second. The key fields are:

- `FPS_app`: measured application loop frame rate.
- `P95_frame_ms`: 95th percentile frame processing time in the current 60s window.
- `max_frame_ms`: slowest frame in the current 60s window.
- `image_ms`, `predict_ms`, `uart_ms`, `osd_ms`: per-stage timing summaries
  printed as `[avg,p95,max]` milliseconds.
- `valid_nav`, `no_line`, `predict_fail`, `image_fail`: navigation and failure counts.
- `uart_sent`, `uart_fail`: UART publish evidence.
- `max_invalid_ms`: longest continuous invalid-navigation run in the current window.

`FIELD_NAV_SENSOR_FPS` is recorded as the target sensor frame rate for scoring
evidence. The current app does not reprogram the sensor mode by itself; judge
whether the 90fps performance item is satisfied from the measured `FPS_app`
and the board/sensor configuration.

On the RDK X5, find the 40Pin UART device first:

```bash
ls -l /dev/ttyS* /dev/ttyUSB* /dev/ttyACM*
dmesg | grep -i tty
```

Then run the bridge script from this repository:

```bash
python3 field_nav_external/scripts/rdk_x5_nav_bridge.py --port /dev/ttyS1 --baud 115200
```

The RDK bridge receives A1 navigation frames and sends 16-byte velocity command
frames from `RDK X5 40Pin Pin8 / UART_TXD` to the lower controller.

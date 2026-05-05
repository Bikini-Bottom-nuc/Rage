#!/bin/sh

cd /field_nav || exit 1

MODEL_PATH="@FIELD_NAV_MODEL_PATH@"
if [ -z "$MODEL_PATH" ] || [ "${MODEL_PATH#@}" != "$MODEL_PATH" ]; then
    MODEL_PATH="app_assets/models/navroad_640x480.m1model"
fi
MODEL="/field_nav/$MODEL_PATH"
if [ ! -f "$MODEL" ]; then
    echo "[field_nav] missing model: $MODEL"
    echo "[field_nav] convert navroad_640x480.onnx to .m1model and rebuild the SDK"
    exit 1
fi
LUT="/field_nav/app_assets/shared_colorLUT.sscl"
if [ ! -f "$LUT" ]; then
    LUT="/field_nav/app_assets/colorLUT.sscl"
fi
if [ ! -f "$LUT" ]; then
    echo "[field_nav] missing OSD LUT: /field_nav/app_assets/shared_colorLUT.sscl"
    exit 1
fi

NAV_RATE="${FIELD_NAV_RATE:-10}"
SENSOR_FPS="${FIELD_NAV_SENSOR_FPS:-0}"
OSD_RATE="${FIELD_NAV_OSD_RATE:-15}"
TEST_SECONDS="${FIELD_NAV_TEST_SECONDS:-0}"

echo "[field_nav] run config: model=$MODEL lut=$LUT nav_uart=${FIELD_NAV_UART:-1} nav_baud=${FIELD_NAV_BAUD:-115200} nav_rate=${NAV_RATE} sensor_fps=${SENSOR_FPS} osd_rate=${OSD_RATE} test_seconds=${TEST_SECONDS}"

chmod +x ./field_nav_demo
exec ./field_nav_demo \
    --model "$MODEL" \
    --lut "$LUT" \
    --nav-uart "${FIELD_NAV_UART:-1}" \
    --nav-baud "${FIELD_NAV_BAUD:-115200}" \
    --nav-rate "$NAV_RATE" \
    --sensor-fps "$SENSOR_FPS" \
    --osd-rate "$OSD_RATE" \
    --test-seconds "$TEST_SECONDS"

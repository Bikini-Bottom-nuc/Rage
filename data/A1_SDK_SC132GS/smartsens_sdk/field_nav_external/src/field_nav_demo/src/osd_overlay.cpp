#include "field_nav.hpp"

#include "osd-device.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <unistd.h>

namespace field_nav {

namespace {

sst::device::osd::OsdDevice g_osd_device;

long FileSize(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
    if (!file) {
        return -1;
    }
    return static_cast<long>(file.tellg());
}

float ClampFloat(float value, float low, float high) {
    return std::max(low, std::min(high, value));
}

std::array<float, 4> ClampBox(const Box& box) {
    float x1 = ClampFloat(static_cast<float>(box.x_min), 0.0f, static_cast<float>(kOriginalWidth - 1));
    float y1 = ClampFloat(static_cast<float>(box.y_min), 0.0f, static_cast<float>(kOriginalHeight - 1));
    float x2 = ClampFloat(static_cast<float>(box.x_max), 0.0f, static_cast<float>(kOriginalWidth - 1));
    float y2 = ClampFloat(static_cast<float>(box.y_max), 0.0f, static_cast<float>(kOriginalHeight - 1));
    if (x1 > x2) {
        std::swap(x1, x2);
    }
    if (y1 > y2) {
        std::swap(y1, y2);
    }
    return {x1, y1, x2, y2};
}

}  // namespace

bool OsdOverlay::Initialize(const std::string& lut_path) {
    long lut_size = FileSize(lut_path);
    if (lut_size <= 0) {
        std::fprintf(stderr, "[field_nav] invalid OSD LUT: %s size=%ld\n", lut_path.c_str(), lut_size);
        return false;
    }

    g_osd_device.Initialize(kOriginalWidth, kOriginalHeight, lut_path.c_str());
    initialized_ = true;
    std::printf("[field_nav] osd initialized via face OsdDevice, layer=%dx%d lut=%s lut_size=%ld\n",
                kOriginalWidth, kOriginalHeight, lut_path.c_str(), lut_size);
    std::printf("[field_nav] osd startup test box should be visible for about 3 seconds\n");

    std::vector<sst::device::osd::OsdQuadRangle> startup_box;
    sst::device::osd::OsdQuadRangle box{};
    box.box = {100.0f, 100.0f, 260.0f, 220.0f};
    box.border = 4;
    box.layer_id = 0;
    box.type = fdevice::TYPE_HOLLOW;
    box.alpha = fdevice::TYPE_ALPHA100;
    box.color = 1;
    startup_box.push_back(box);
    g_osd_device.Draw(startup_box, 0);
    usleep(3000000);
    Clear();
    usleep(200000);
    return true;
}

void OsdOverlay::BuildBox(const Box& box, int border, fdevice::VERTEXS_S* out, fdevice::VERTEXS_S* in) const {
    if (out == nullptr || in == nullptr) {
        return;
    }

    Box expanded{
        box.x_min - border,
        box.y_min - border,
        box.x_max + border,
        box.y_max + border,
    };
    std::array<float, 4> clamped = ClampBox(expanded);
    int x1 = static_cast<int>(clamped[0]);
    int y1 = static_cast<int>(clamped[1]);
    int x2 = static_cast<int>(clamped[2]);
    int y2 = static_cast<int>(clamped[3]);

    out->points[0] = {x1, y1};
    out->points[1] = {x1, y2};
    out->points[2] = {x2, y2};
    out->points[3] = {x2, y1};
    *in = *out;
}

void OsdOverlay::DrawBoxes(const std::vector<Box>& boxes, int color, fdevice::QUADRANGLETYPE type) {
    if (!initialized_) {
        return;
    }

    std::vector<std::array<float, 4>> osd_boxes;
    osd_boxes.reserve(boxes.size());
    for (const auto& box : boxes) {
        std::array<float, 4> clamped = ClampBox(box);
        if ((clamped[2] - clamped[0]) >= 1.0f && (clamped[3] - clamped[1]) >= 1.0f) {
            osd_boxes.push_back(clamped);
        }
    }

    std::vector<std::array<float, 4>> empty;
    g_osd_device.Draw(empty, 0, 0, type, fdevice::TYPE_ALPHA100, color);
    if (osd_boxes.empty()) {
        return;
    }
    g_osd_device.Draw(osd_boxes, 0, 0, type, fdevice::TYPE_ALPHA100, color);
}

void OsdOverlay::DrawLine(const NavLine& line) {
    if (!line.valid) {
        Clear();
        return;
    }

    std::vector<Box> boxes;
    const int half = 9;
    const int samples = 24;
    for (int i = 0; i < samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(samples - 1);
        float y = kCropOffsetY + (kCropHeight - 1) * (1.0f - t);
        float x = line.slope * y + line.intercept;
        if (x >= 0.0f && x < kOriginalWidth && y >= 0.0f && y < kOriginalHeight) {
            boxes.push_back({static_cast<int>(x) - half, static_cast<int>(y) - half,
                             static_cast<int>(x) + half, static_cast<int>(y) + half});
        }
    }

    float crop_bottom_y = static_cast<float>(kCropOffsetY + kCropHeight - 1);
    float crop_bottom_x = line.slope * crop_bottom_y + line.intercept;
    int bottom_x = static_cast<int>(
        ClampFloat(crop_bottom_x, 0.0f, static_cast<float>(kOriginalWidth - 1)));
    int bottom_y = kCropOffsetY + kCropHeight - 28;
    boxes.push_back({bottom_x - 18, bottom_y - 18, bottom_x + 18, bottom_y + 18});

    static int draw_count = 0;
    if ((draw_count++ % 30) == 0) {
        std::printf("[field_nav] osd draw line boxes=%zu crop_bottom=(%d,%d)\n",
                    boxes.size(), bottom_x, bottom_y);
    }

    DrawBoxes(boxes, 1, fdevice::TYPE_SOLID);
}

void OsdOverlay::Clear() {
    if (!initialized_) {
        return;
    }
    std::vector<std::array<float, 4>> empty;
    g_osd_device.Draw(empty, 0, 0, fdevice::TYPE_SOLID, fdevice::TYPE_ALPHA100, 1);
}

void OsdOverlay::Release() {
    if (!initialized_) {
        return;
    }
    Clear();
    g_osd_device.Release();
    initialized_ = false;
}

}  // namespace field_nav

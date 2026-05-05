#include "field_nav.hpp"

#include "smartsoc/gpio_api.h"
#include "smartsoc/uart_api.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_stop = 0;

constexpr uint8_t kStatusOk = 0;
constexpr uint8_t kStatusNoLine = 1;
constexpr uint8_t kStatusPredictFailed = 2;
constexpr uint8_t kStatusCameraFailed = 3;

void HandleSignal(int) {
    g_stop = 1;
}

std::string ArgValue(int argc, char** argv, const char* name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

int ArgInt(int argc, char** argv, const char* name, int fallback) {
    std::string value = ArgValue(argc, argv, name, "");
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(parsed);
}

int ClampInt(int value, int low, int high) {
    return std::max(low, std::min(high, value));
}

int16_t ClampInt16(int value) {
    return static_cast<int16_t>(ClampInt(value, -32768, 32767));
}

void PutU16(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xff);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void PutI16(uint8_t* dst, int16_t value) {
    PutU16(dst, static_cast<uint16_t>(value));
}

uint8_t Checksum15(const uint8_t* data) {
    uint32_t sum = 0;
    for (int i = 0; i < 15; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xff);
}

struct UartPublishResult {
    bool attempted = false;
    bool sent = false;
    bool valid = false;
    uint8_t status = kStatusPredictFailed;
};

struct FrameMetric {
    std::chrono::steady_clock::time_point time;
    long frame_ms = 0;
    long image_ms = 0;
    long predict_ms = 0;
    long uart_ms = 0;
    long osd_ms = 0;
    bool image_ok = false;
    bool predict_ok = false;
    bool valid_nav = false;
    bool uart_attempted = false;
    bool uart_sent = false;
    uint8_t status = kStatusPredictFailed;
};

struct DurationSummary {
    double avg_ms = 0.0;
    long p95_ms = 0;
    long max_ms = 0;
};

long ElapsedMs(std::chrono::steady_clock::time_point begin,
               std::chrono::steady_clock::time_point end) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
}

DurationSummary SummarizeDurations(std::vector<long>* durations) {
    DurationSummary summary;
    if (durations == nullptr || durations->empty()) {
        return summary;
    }

    long sum = 0;
    for (long value : *durations) {
        sum += value;
    }
    std::sort(durations->begin(), durations->end());
    size_t p95_index = (durations->size() * 95 + 99) / 100;
    if (p95_index == 0) {
        p95_index = 1;
    }

    summary.avg_ms = static_cast<double>(sum) / static_cast<double>(durations->size());
    summary.p95_ms = (*durations)[p95_index - 1];
    summary.max_ms = durations->back();
    return summary;
}

class RuntimeMetrics {
  public:
    RuntimeMetrics(int window_seconds, int target_sensor_fps)
        : window_seconds_(window_seconds > 0 ? window_seconds : 60),
          target_sensor_fps_(target_sensor_fps) {
        start_ = std::chrono::steady_clock::now();
        last_report_ = start_;
    }

    void Add(const FrameMetric& metric) {
        frames_.push_back(metric);
        Trim(metric.time);
    }

    bool ShouldReport(std::chrono::steady_clock::time_point now) const {
        return now - last_report_ >= std::chrono::seconds(1);
    }

    void Report(int total_frames, std::chrono::steady_clock::time_point now, const char* tag) {
        Trim(now);
        last_report_ = now;

        std::vector<long> frame_durations;
        std::vector<long> image_durations;
        std::vector<long> predict_durations;
        std::vector<long> uart_durations;
        std::vector<long> osd_durations;
        frame_durations.reserve(frames_.size());
        image_durations.reserve(frames_.size());
        predict_durations.reserve(frames_.size());
        uart_durations.reserve(frames_.size());
        osd_durations.reserve(frames_.size());
        int image_fail = 0;
        int predict_fail = 0;
        int no_line = 0;
        int valid_nav = 0;
        int uart_sent = 0;
        int uart_fail = 0;
        int status_counts[4] = {0, 0, 0, 0};
        long invalid_run_ms = 0;
        long max_invalid_run_ms = 0;
        int invalid_run_frames = 0;
        int max_invalid_run_frames = 0;

        for (const auto& metric : frames_) {
            frame_durations.push_back(metric.frame_ms);
            image_durations.push_back(metric.image_ms);
            predict_durations.push_back(metric.predict_ms);
            uart_durations.push_back(metric.uart_ms);
            osd_durations.push_back(metric.osd_ms);
            if (!metric.image_ok) {
                ++image_fail;
            }
            if (metric.image_ok && !metric.predict_ok) {
                ++predict_fail;
            }
            if (metric.status == kStatusNoLine) {
                ++no_line;
            }
            if (metric.valid_nav) {
                ++valid_nav;
            }
            if (metric.uart_sent) {
                ++uart_sent;
            }
            if (metric.uart_attempted && !metric.uart_sent) {
                ++uart_fail;
            }
            if (metric.status <= kStatusCameraFailed) {
                ++status_counts[metric.status];
            }
            if (metric.valid_nav) {
                invalid_run_ms = 0;
                invalid_run_frames = 0;
            } else {
                invalid_run_ms += std::max(1L, metric.frame_ms);
                ++invalid_run_frames;
                max_invalid_run_ms = std::max(max_invalid_run_ms, invalid_run_ms);
                max_invalid_run_frames = std::max(max_invalid_run_frames, invalid_run_frames);
            }
        }

        DurationSummary frame_summary = SummarizeDurations(&frame_durations);
        DurationSummary image_summary = SummarizeDurations(&image_durations);
        DurationSummary predict_summary = SummarizeDurations(&predict_durations);
        DurationSummary uart_summary = SummarizeDurations(&uart_durations);
        DurationSummary osd_summary = SummarizeDurations(&osd_durations);

        double span_s = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count() / 1000.0;
        if (!frames_.empty()) {
            span_s = std::chrono::duration_cast<std::chrono::milliseconds>(now - frames_.front().time).count() / 1000.0;
        }
        if (span_s <= 0.0) {
            span_s = 0.001;
        }
        double fps_app = static_cast<double>(frames_.size()) / span_s;
        double target_ratio = target_sensor_fps_ > 0 ? fps_app / static_cast<double>(target_sensor_fps_) : 0.0;

        std::printf(
            "[field_nav] metrics tag=%s window=%ds total_frames=%d samples=%zu FPS_app=%.2f "
            "target_sensor_fps=%d fps_ratio=%.3f avg_frame_ms=%.2f P95_frame_ms=%ld max_frame_ms=%ld "
            "image_ms=[%.2f,%ld,%ld] predict_ms=[%.2f,%ld,%ld] "
            "uart_ms=[%.2f,%ld,%ld] osd_ms=[%.2f,%ld,%ld] "
            "valid_nav=%d no_line=%d predict_fail=%d image_fail=%d uart_sent=%d uart_fail=%d "
            "status_ok=%d status_no_line=%d status_predict_fail=%d status_camera_fail=%d "
            "max_invalid_ms=%ld max_invalid_frames=%d\n",
            tag, window_seconds_, total_frames, frames_.size(), fps_app, target_sensor_fps_, target_ratio,
            frame_summary.avg_ms, frame_summary.p95_ms, frame_summary.max_ms,
            image_summary.avg_ms, image_summary.p95_ms, image_summary.max_ms,
            predict_summary.avg_ms, predict_summary.p95_ms, predict_summary.max_ms,
            uart_summary.avg_ms, uart_summary.p95_ms, uart_summary.max_ms,
            osd_summary.avg_ms, osd_summary.p95_ms, osd_summary.max_ms,
            valid_nav, no_line, predict_fail, image_fail, uart_sent, uart_fail,
            status_counts[kStatusOk], status_counts[kStatusNoLine], status_counts[kStatusPredictFailed],
            status_counts[kStatusCameraFailed], max_invalid_run_ms, max_invalid_run_frames);
    }

  private:
    void Trim(std::chrono::steady_clock::time_point now) {
        const auto window = std::chrono::seconds(window_seconds_);
        while (!frames_.empty() && now - frames_.front().time > window) {
            frames_.pop_front();
        }
    }

    int window_seconds_;
    int target_sensor_fps_;
    std::deque<FrameMetric> frames_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_report_;
};

class NavUartPublisher {
  public:
    bool Initialize(bool enabled, uint32_t baudrate, int rate_hz) {
        enabled_ = enabled;
        if (!enabled_) {
            std::printf("[field_nav] nav UART disabled\n");
            return true;
        }

        if (rate_hz <= 0) {
            rate_hz = 10;
        }
        interval_ms_ = std::max(1, 1000 / rate_hz);
        log_every_ = std::max(1, 1000 / interval_ms_);

        gpio_ = gpio_init();
        if (gpio_ == nullptr) {
            std::fprintf(stderr, "[field_nav] gpio_init failed for nav UART\n");
            return false;
        }
        int gpio_ret = gpio_set_alternate(gpio_, GPIO_PIN_0, GPIO_AF_INPUT_NONE, GPIO_AF_OUTPUT_UART_TX0);
        if (gpio_ret != GPIO_SUCCESS) {
            std::fprintf(stderr, "[field_nav] gpio_set_alternate GPIO_PIN_0 UART_TX0 failed: %d\n", gpio_ret);
            Release();
            return false;
        }

        uart_ = uart_init();
        if (uart_ == nullptr) {
            std::fprintf(stderr, "[field_nav] uart_init failed for nav UART\n");
            Release();
            return false;
        }
        int baud_ret = uart_set_baudrate(uart_, UART_TX0, baudrate);
        int parity_ret = uart_set_parity(uart_, UART_TX0, UART_PARITY_NONE);
        if (baud_ret != UART_SUCCESS || parity_ret != UART_SUCCESS) {
            std::fprintf(stderr, "[field_nav] nav UART config failed: baud_ret=%d parity_ret=%d\n",
                         baud_ret, parity_ret);
            Release();
            return false;
        }

        initialized_ = true;
        last_send_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(interval_ms_);
        std::printf("[field_nav] nav UART enabled: GPIO_PIN_0=UART_TX0 baud=%u rate=%dHz frame_len=16\n",
                    baudrate, rate_hz);
        return true;
    }

    UartPublishResult Publish(uint8_t status, const field_nav::NavLine& line) {
        UartPublishResult result;
        result.status = status;
        result.valid = status == kStatusOk && line.valid;
        if (!enabled_ || !initialized_) {
            return result;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_send_ < std::chrono::milliseconds(interval_ms_)) {
            return result;
        }
        last_send_ = now;
        result.attempted = true;

        uint8_t packet[16] = {};
        bool valid = result.valid;

        packet[0] = 0xA5;
        packet[1] = 0x5A;
        packet[2] = 0x01;
        packet[3] = valid ? 0x01 : 0x00;
        PutU16(&packet[4], seq_++);
        PutI16(&packet[6], valid ? ClampInt16(static_cast<int>(std::lround(line.deviation_px * 10.0f))) : 0);
        PutI16(&packet[8], valid ? ClampInt16(static_cast<int>(std::lround(line.angle_deg * 100.0f))) : 0);
        packet[10] = valid ? static_cast<uint8_t>(ClampInt(static_cast<int>(std::lround(line.confidence * 100.0f)), 0, 100)) : 0;
        packet[11] = valid ? static_cast<uint8_t>(ClampInt(static_cast<int>(line.points.size()), 0, 255)) : 0;
        uint16_t bottom_x = valid ? static_cast<uint16_t>(ClampInt(static_cast<int>(std::lround(line.bottom_x)), 0, 65534)) : 0xffff;
        PutU16(&packet[12], bottom_x);
        packet[14] = status;
        packet[15] = Checksum15(packet);

        int ret = uart_send_data(uart_, UART_TX0, packet, sizeof(packet));
        if (ret != UART_SUCCESS) {
            std::fprintf(stderr, "[field_nav] nav UART send failed: ret=%d status=%u\n", ret, status);
        } else {
            result.sent = true;
        }
        if (result.sent && (sent_count_++ % log_every_) == 0) {
            std::printf("[field_nav] nav UART frame sent: valid=%d dev=%.1f angle=%.2f status=%u\n",
                        valid ? 1 : 0, valid ? line.deviation_px : 0.0f,
                        valid ? line.angle_deg : 0.0f, status);
        }
        return result;
    }

    void Release() {
        if (uart_ != nullptr) {
            uart_close(uart_);
            uart_ = nullptr;
        }
        if (gpio_ != nullptr) {
            gpio_close(gpio_);
            gpio_ = nullptr;
        }
        initialized_ = false;
    }

  private:
    bool enabled_ = false;
    bool initialized_ = false;
    int interval_ms_ = 100;
    int log_every_ = 10;
    uint16_t seq_ = 0;
    uint32_t sent_count_ = 0;
    gpio_handle_t gpio_ = nullptr;
    uart_handle_t uart_ = nullptr;
    std::chrono::steady_clock::time_point last_send_;
};

}  // namespace

namespace field_nav {

bool FileExists(const std::string& path) {
    return access(path.c_str(), R_OK) == 0;
}

}  // namespace field_nav

int main(int argc, char** argv) {
    using namespace field_nav;

    const std::string model_path = ArgValue(
        argc, argv, "--model", "/field_nav/app_assets/models/navroad_640x480.m1model");
    const std::string lut_path = ArgValue(
        argc, argv, "--lut", "/field_nav/app_assets/shared_colorLUT.sscl");
    const bool nav_uart_enabled = ArgInt(argc, argv, "--nav-uart", 1) != 0;
    int nav_baud_arg = ArgInt(argc, argv, "--nav-baud", 115200);
    if (nav_baud_arg <= 0) {
        nav_baud_arg = 115200;
    }
    const uint32_t nav_baud = static_cast<uint32_t>(nav_baud_arg);
    int nav_rate = ArgInt(argc, argv, "--nav-rate", 10);
    if (nav_rate <= 0) {
        nav_rate = 10;
    }
    int requested_sensor_fps = ArgInt(argc, argv, "--sensor-fps", 0);
    if (requested_sensor_fps < 0) {
        requested_sensor_fps = 0;
    }
    int osd_rate = ArgInt(argc, argv, "--osd-rate", 15);
    if (osd_rate < 0) {
        osd_rate = 0;
    }
    int test_seconds = ArgInt(argc, argv, "--test-seconds", 0);
    if (test_seconds < 0) {
        test_seconds = 0;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::printf("[field_nav] config model=%s lut=%s nav_uart=%d nav_baud=%u nav_rate=%dHz "
                "sensor_fps_request=%d osd_rate=%dHz test_seconds=%d metrics_window=60s\n",
                model_path.c_str(), lut_path.c_str(), nav_uart_enabled ? 1 : 0, nav_baud,
                nav_rate, requested_sensor_fps, osd_rate, test_seconds);

    if (ssne_initial() != 0) {
        std::fprintf(stderr, "[field_nav] ssne_initial failed\n");
        return 1;
    }

    ImageProcessor processor;
    NavLineDetector detector;
    OsdOverlay overlay;
    NavUartPublisher nav_uart;

    if (!processor.Initialize()) {
        ssne_release();
        return 1;
    }
    if (!nav_uart.Initialize(nav_uart_enabled, nav_baud, nav_rate)) {
        processor.Release();
        ssne_release();
        return 1;
    }
    if (!overlay.Initialize(lut_path)) {
        nav_uart.Release();
        processor.Release();
        ssne_release();
        return 1;
    }
    if (!detector.Initialize(model_path)) {
        overlay.Release();
        nav_uart.Release();
        processor.Release();
        ssne_release();
        return 1;
    }

    int frame = 0;
    RuntimeMetrics metrics(60, requested_sensor_fps > 0 ? requested_sensor_fps : nav_rate);
    auto run_start = std::chrono::steady_clock::now();
    const bool osd_enabled = osd_rate > 0;
    const int osd_interval_ms = osd_enabled ? std::max(1, 1000 / osd_rate) : 0;
    auto last_osd_draw = run_start - std::chrono::milliseconds(osd_interval_ms > 0 ? osd_interval_ms : 1000);
    bool has_last_osd_visible = false;
    bool last_osd_visible = false;

    while (!g_stop) {
        auto start = std::chrono::steady_clock::now();
        ssne_tensor_t image{};
        NavLine line;
        auto image_start = std::chrono::steady_clock::now();
        bool image_ok = processor.GetImage(&image);
        auto image_end = std::chrono::steady_clock::now();
        bool predict_ok = false;
        auto predict_start = image_end;
        auto predict_end = image_end;
        if (image_ok) {
            predict_start = std::chrono::steady_clock::now();
            predict_ok = detector.Predict(&image, &line);
            predict_end = std::chrono::steady_clock::now();
        }
        uint8_t status = kStatusPredictFailed;
        if (!image_ok) {
            status = kStatusCameraFailed;
        } else if (!predict_ok) {
            status = kStatusPredictFailed;
        } else if (!line.valid) {
            status = kStatusNoLine;
        } else {
            status = kStatusOk;
        }

        auto uart_start = std::chrono::steady_clock::now();
        UartPublishResult uart_result = nav_uart.Publish(status, line);
        auto uart_end = std::chrono::steady_clock::now();

        auto osd_start = std::chrono::steady_clock::now();
        bool osd_visible = predict_ok && line.valid;
        bool osd_state_changed = !has_last_osd_visible || osd_visible != last_osd_visible;
        bool osd_due = osd_enabled &&
            (osd_state_changed || osd_start - last_osd_draw >= std::chrono::milliseconds(osd_interval_ms));
        if (osd_due) {
            if (osd_visible) {
                overlay.DrawLine(line);
            } else {
                overlay.Clear();
            }
            last_osd_draw = std::chrono::steady_clock::now();
            has_last_osd_visible = true;
            last_osd_visible = osd_visible;
        }
        auto osd_end = std::chrono::steady_clock::now();

        ++frame;
        auto now = std::chrono::steady_clock::now();
        FrameMetric metric;
        metric.time = now;
        metric.frame_ms = ElapsedMs(start, now);
        metric.image_ms = ElapsedMs(image_start, image_end);
        metric.predict_ms = image_ok ? ElapsedMs(predict_start, predict_end) : 0;
        metric.uart_ms = ElapsedMs(uart_start, uart_end);
        metric.osd_ms = osd_due ? ElapsedMs(osd_start, osd_end) : 0;
        metric.image_ok = image_ok;
        metric.predict_ok = predict_ok;
        metric.valid_nav = status == kStatusOk;
        metric.uart_attempted = uart_result.attempted;
        metric.uart_sent = uart_result.sent;
        metric.status = status;
        metrics.Add(metric);

        if (metrics.ShouldReport(now)) {
            metrics.Report(frame, now, "heartbeat");
        }
        if (test_seconds > 0 && now - run_start >= std::chrono::seconds(test_seconds)) {
            std::printf("[field_nav] test_seconds reached: %d, stopping after frame=%d\n", test_seconds, frame);
            metrics.Report(frame, now, "final");
            break;
        }
    }

    overlay.Release();
    nav_uart.Release();
    detector.Release();
    processor.Release();
    if (ssne_release() != 0) {
        std::fprintf(stderr, "[field_nav] ssne_release failed\n");
        return 1;
    }
    return 0;
}

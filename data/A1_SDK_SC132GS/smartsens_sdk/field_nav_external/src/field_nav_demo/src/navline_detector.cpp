#include "field_nav.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <vector>
#include <unistd.h>

namespace field_nav {

namespace {

constexpr float kMaskThreshold = 0.45f;
constexpr int kMinValidPoints = 6;
constexpr int kDiagIntervalFrames = 30;
constexpr int kMorphVerticalRadius = 3;
constexpr int kMorphHorizontalRadius = 0;
constexpr int kBandHeight = 4;
constexpr int kMinComponentArea = 30;
constexpr int kMaxFallbackFrames = 2;

struct TensorStats {
    int width = 0;
    int height = 0;
    uint8_t dtype = 0;
    int total = 0;
    float raw_min = 0.0f;
    float raw_max = 0.0f;
    float raw_mean = 0.0f;
    float prob_min = 0.0f;
    float prob_max = 0.0f;
    float prob_mean = 0.0f;
};

struct LineDiag {
    int scanned_rows = 0;
    int foreground_rows = 0;
    int points = 0;
    int component_count = 0;
    int main_area = 0;
    int band_points = 0;
    int fallback_count = 0;
    const char* reason = "not_run";
    const char* failure_reason = "none";
};

LineDiag g_line_diag;
NavLine g_last_valid_line;
bool g_has_last_valid_line = false;
int g_consecutive_failures = 0;

struct Component {
    int label = -1;
    int area = 0;
    int min_x = 0;
    int max_x = 0;
    int min_y = 0;
    int max_y = 0;
    bool touches_bottom = false;
    bool touches_roi_top = false;
};

struct ScratchBuffers {
    std::vector<float> probs;
    std::vector<uint8_t> mask;
    std::vector<uint8_t> morph_tmp;
    std::vector<uint8_t> closed_mask;
    std::vector<int> labels;
    std::vector<int> stack;
    std::vector<Component> components;
};

ScratchBuffers g_scratch;

double ElapsedMs(std::chrono::steady_clock::time_point begin,
                 std::chrono::steady_clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() / 1000.0;
}

float Sigmoid(float value) {
    if (value >= 0.0f && value <= 1.0f) {
        return value;
    }
    return 1.0f / (1.0f + std::exp(-value));
}

const char* DTypeName(uint8_t dtype) {
    if (dtype == SSNE_FLOAT32) {
        return "FLOAT32";
    }
    if (dtype == SSNE_INT8) {
        return "INT8";
    }
    if (dtype == SSNE_UINT8) {
        return "UINT8";
    }
    return "UNKNOWN_AS_UINT8";
}

float TensorRawValueFromData(void* data, uint8_t dtype, int index) {
    if (data == nullptr) {
        return 0.0f;
    }

    if (dtype == SSNE_FLOAT32) {
        return static_cast<float*>(data)[index];
    }
    if (dtype == SSNE_INT8) {
        return static_cast<float>(static_cast<int>(static_cast<int8_t*>(data)[index]));
    }
    return static_cast<float>(static_cast<uint8_t*>(data)[index]);
}

float TensorRawValue(const ssne_tensor_t& tensor, int index) {
    uint8_t dtype = get_data_type(tensor);
    void* data = get_data(tensor);
    return TensorRawValueFromData(data, dtype, index);
}

float TensorProbabilityFromRaw(float raw, uint8_t dtype) {
    if (dtype == SSNE_FLOAT32) {
        return Sigmoid(raw);
    }
    if (dtype == SSNE_INT8) {
        int value = static_cast<int>(raw) + 128;
        return std::max(0, std::min(255, value)) / 255.0f;
    }
    return std::max(0.0f, std::min(255.0f, raw)) / 255.0f;
}

float TensorProbability(const ssne_tensor_t& tensor, int index) {
    return TensorProbabilityFromRaw(TensorRawValue(tensor, index), get_data_type(tensor));
}

int MaskIndex(int x, int y, int width) {
    return y * width + x;
}

void DilateMask(const std::vector<uint8_t>& mask,
                int width,
                int height,
                int y_start,
                std::vector<uint8_t>* out) {
    out->resize(mask.size());
    std::fill(out->begin(), out->end(), 0);
    for (int y = y_start; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool hit = false;
            for (int dy = -kMorphVerticalRadius; dy <= kMorphVerticalRadius && !hit; ++dy) {
                int yy = y + dy;
                if (yy < y_start || yy >= height) {
                    continue;
                }
                for (int dx = -kMorphHorizontalRadius; dx <= kMorphHorizontalRadius; ++dx) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= width) {
                        continue;
                    }
                    if (mask[MaskIndex(xx, yy, width)] != 0) {
                        hit = true;
                        break;
                    }
                }
            }
            (*out)[MaskIndex(x, y, width)] = hit ? 1 : 0;
        }
    }
}

void ErodeMask(const std::vector<uint8_t>& mask,
               int width,
               int height,
               int y_start,
               std::vector<uint8_t>* out) {
    out->resize(mask.size());
    std::fill(out->begin(), out->end(), 0);
    for (int y = y_start; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool keep = true;
            for (int dy = -kMorphVerticalRadius; dy <= kMorphVerticalRadius && keep; ++dy) {
                int yy = y + dy;
                if (yy < y_start || yy >= height) {
                    continue;
                }
                for (int dx = -kMorphHorizontalRadius; dx <= kMorphHorizontalRadius; ++dx) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= width) {
                        continue;
                    }
                    if (mask[MaskIndex(xx, yy, width)] == 0) {
                        keep = false;
                        break;
                    }
                }
            }
            (*out)[MaskIndex(x, y, width)] = keep ? 1 : 0;
        }
    }
}

void CloseVerticalGaps(const std::vector<uint8_t>& mask,
                       int width,
                       int height,
                       int y_start,
                       std::vector<uint8_t>* morph_tmp,
                       std::vector<uint8_t>* closed) {
    DilateMask(mask, width, height, y_start, morph_tmp);
    ErodeMask(*morph_tmp, width, height, y_start, closed);
    for (std::size_t i = 0; i < closed->size(); ++i) {
        if (mask[i] != 0) {
            (*closed)[i] = 1;
        }
    }
}

void LabelComponents(const std::vector<uint8_t>& mask,
                     int width,
                     int height,
                     int y_start,
                     std::vector<int>* labels,
                     std::vector<int>* stack,
                     std::vector<Component>* components) {
    labels->assign(mask.size(), -1);
    components->clear();
    stack->clear();
    int next_label = 0;
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};

    for (int y = y_start; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int start_idx = MaskIndex(x, y, width);
            if (mask[start_idx] == 0 || (*labels)[start_idx] >= 0) {
                continue;
            }

            Component comp;
            comp.label = next_label++;
            comp.min_x = comp.max_x = x;
            comp.min_y = comp.max_y = y;
            stack->clear();
            stack->push_back(start_idx);
            (*labels)[start_idx] = comp.label;

            while (!stack->empty()) {
                int idx = stack->back();
                stack->pop_back();
                int cx = idx % width;
                int cy = idx / width;
                ++comp.area;
                comp.min_x = std::min(comp.min_x, cx);
                comp.max_x = std::max(comp.max_x, cx);
                comp.min_y = std::min(comp.min_y, cy);
                comp.max_y = std::max(comp.max_y, cy);

                for (int k = 0; k < 4; ++k) {
                    int nx = cx + dx[k];
                    int ny = cy + dy[k];
                    if (nx < 0 || nx >= width || ny < y_start || ny >= height) {
                        continue;
                    }
                    int nidx = MaskIndex(nx, ny, width);
                    if (mask[nidx] != 0 && (*labels)[nidx] < 0) {
                        (*labels)[nidx] = comp.label;
                        stack->push_back(nidx);
                    }
                }
            }

            comp.touches_bottom = comp.max_y >= height - 2;
            comp.touches_roi_top = comp.min_y <= y_start + kBandHeight;
            if (comp.area >= kMinComponentArea) {
                components->push_back(comp);
            }
        }
    }
}

float ExpectedMaskXFromLastLine(int width, int height, int y_mask) {
    if (!g_has_last_valid_line) {
        return width * 0.5f;
    }
    float original_y = kCropOffsetY +
        y_mask * static_cast<float>(kCropHeight) / static_cast<float>(std::max(1, height - 1));
    float original_x = g_last_valid_line.slope * original_y + g_last_valid_line.intercept;
    return original_x * static_cast<float>(std::max(1, width - 1)) / static_cast<float>(kCropWidth);
}

int SelectMainComponent(const std::vector<Component>& components, int width, int height, int y_start) {
    if (components.empty()) {
        return -1;
    }

    const int roi_height = std::max(1, height - y_start);
    float best_score = -std::numeric_limits<float>::max();
    int best_label = -1;

    for (const auto& comp : components) {
        int span_y = comp.max_y - comp.min_y + 1;
        bool through_domain = comp.touches_bottom && span_y >= std::max(4, roi_height * 35 / 100);
        float center_x = (comp.min_x + comp.max_x) * 0.5f;
        float target_x = ExpectedMaskXFromLastLine(width, height, comp.max_y);
        float center_penalty = std::fabs(center_x - target_x);

        float score = static_cast<float>(comp.area);
        score += static_cast<float>(span_y * width) * 0.25f;
        if (comp.touches_bottom) {
            score += static_cast<float>(width * height);
        }
        if (through_domain || comp.touches_roi_top) {
            score += static_cast<float>(width * height) * 0.5f;
        }
        score -= center_penalty * 2.0f;

        if (score > best_score) {
            best_score = score;
            best_label = comp.label;
        }
    }

    return best_label;
}

bool FitLeastSquares(NavLine* line, const char** reason) {
    if (line->points.size() < kMinValidPoints) {
        *reason = "points_below_min";
        return false;
    }

    float sum_y = 0.0f;
    float sum_x = 0.0f;
    float sum_yy = 0.0f;
    float sum_yx = 0.0f;
    float sum_conf = 0.0f;
    for (const auto& p : line->points) {
        sum_y += p.y;
        sum_x += p.x;
        sum_yy += p.y * p.y;
        sum_yx += p.y * p.x;
        sum_conf += p.confidence;
    }

    float n = static_cast<float>(line->points.size());
    float denom = n * sum_yy - sum_y * sum_y;
    if (std::fabs(denom) < 1e-4f) {
        *reason = "degenerate_fit";
        return false;
    }

    line->slope = (n * sum_yx - sum_y * sum_x) / denom;
    line->intercept = (sum_x - line->slope * sum_y) / n;
    line->bottom_x = line->slope * (kOriginalHeight - 1) + line->intercept;
    line->deviation_px = line->bottom_x - (kOriginalWidth / 2.0f);
    line->angle_deg = std::atan(line->slope) * 180.0f / 3.14159265f;
    line->confidence = sum_conf / n;
    line->valid = true;
    *reason = "ok";
    return true;
}

NavLine FailWithFallback(const NavLine& line, const char* reason) {
    g_line_diag.failure_reason = reason;
    if (g_has_last_valid_line && g_consecutive_failures < kMaxFallbackFrames) {
        ++g_consecutive_failures;
        NavLine fallback = g_last_valid_line;
        fallback.confidence *= 0.5f;
        for (auto& p : fallback.points) {
            p.confidence *= 0.5f;
        }
        g_line_diag.fallback_count = g_consecutive_failures;
        g_line_diag.reason = "fallback_last_valid";
        return fallback;
    }

    ++g_consecutive_failures;
    g_line_diag.reason = reason;
    return line;
}

TensorStats ComputeStats(const ssne_tensor_t& tensor) {
    TensorStats stats;
    stats.width = static_cast<int>(get_width(tensor));
    stats.height = static_cast<int>(get_height(tensor));
    stats.dtype = get_data_type(tensor);
    stats.total = stats.width * stats.height;

    if (stats.width <= 0 || stats.height <= 0 || get_data(tensor) == nullptr) {
        return stats;
    }

    float raw_min = std::numeric_limits<float>::max();
    float raw_max = -std::numeric_limits<float>::max();
    float prob_min = std::numeric_limits<float>::max();
    float prob_max = -std::numeric_limits<float>::max();
    double raw_sum = 0.0;
    double prob_sum = 0.0;

    for (int i = 0; i < stats.total; ++i) {
        float raw = TensorRawValue(tensor, i);
        float prob = TensorProbabilityFromRaw(raw, stats.dtype);
        raw_min = std::min(raw_min, raw);
        raw_max = std::max(raw_max, raw);
        prob_min = std::min(prob_min, prob);
        prob_max = std::max(prob_max, prob);
        raw_sum += raw;
        prob_sum += prob;
    }

    stats.raw_min = raw_min;
    stats.raw_max = raw_max;
    stats.raw_mean = static_cast<float>(raw_sum / std::max(1, stats.total));
    stats.prob_min = prob_min;
    stats.prob_max = prob_max;
    stats.prob_mean = static_cast<float>(prob_sum / std::max(1, stats.total));
    return stats;
}

}  // namespace

bool NavLineDetector::Initialize(const std::string& model_path) {
    if (!FileExists(model_path)) {
        std::fprintf(stderr, "[field_nav] model file does not exist: %s\n", model_path.c_str());
        return false;
    }
    char* model_path_c = const_cast<char*>(model_path.c_str());
    model_id_ = ssne_loadmodel(model_path_c, SSNE_STATIC_ALLOC);
    int input_num = ssne_get_model_input_num(model_id_);
    if (input_num <= 0) {
        std::fprintf(stderr, "[field_nav] ssne_loadmodel or model query failed: %s model_id=%u input_num=%d\n",
                     model_path.c_str(), model_id_, input_num);
        return false;
    }
    int input_dtype = -1;
    int dtype_ret = ssne_get_model_input_dtype(model_id_, &input_dtype);

    input_ = create_tensor(kModelWidth, kModelHeight, SSNE_Y_8, SSNE_BUF_AI);
    if (get_data(input_) == nullptr) {
        std::fprintf(stderr, "[field_nav] create_tensor failed for %dx%d SSNE_Y_8 input\n",
                     kModelWidth, kModelHeight);
        return false;
    }

    preprocess_ = GetAIPreprocessPipe();
    if (preprocess_ == nullptr) {
        std::fprintf(stderr, "[field_nav] GetAIPreprocessPipe failed\n");
        release_tensor(input_);
        input_ = {};
        return false;
    }
    initialized_ = true;
    std::printf("[field_nav] model loaded: %s model_id=%u input_num=%d input_dtype=%d dtype_ret=%d\n",
                model_path.c_str(), model_id_, input_num, input_dtype, dtype_ret);
    return true;
}

bool NavLineDetector::Predict(ssne_tensor_t* img, NavLine* line) {
    static int frame = 0;
    const bool diag_frame = (frame++ % kDiagIntervalFrames) == 0;

    auto preprocess_start = std::chrono::steady_clock::now();
    int ret = RunAiPreprocessPipe(preprocess_, *img, input_);
    auto preprocess_end = std::chrono::steady_clock::now();
    if (ret != 0) {
        std::fprintf(stderr, "[field_nav] RunAiPreprocessPipe failed: %d\n", ret);
        return false;
    }
    auto inference_start = std::chrono::steady_clock::now();
    ret = ssne_inference(model_id_, 1, &input_);
    auto inference_end = std::chrono::steady_clock::now();
    if (ret != 0) {
        std::fprintf(stderr, "[field_nav] ssne_inference failed: %d\n", ret);
        return false;
    }
    auto getoutput_start = std::chrono::steady_clock::now();
    ret = ssne_getoutput(model_id_, 1, &output_);
    auto getoutput_end = std::chrono::steady_clock::now();
    if (ret != 0) {
        std::fprintf(stderr, "[field_nav] ssne_getoutput failed: %d\n", ret);
        return false;
    }
    if (get_data(output_) == nullptr) {
        std::fprintf(stderr, "[field_nav] ssne_getoutput returned empty output tensor\n");
        return false;
    }

    int output_width = static_cast<int>(get_width(output_));
    int output_height = static_cast<int>(get_height(output_));
    uint8_t output_dtype = get_data_type(output_);
    static int last_width = -1;
    static int last_height = -1;
    static int last_dtype = -1;
    if (output_width != last_width || output_height != last_height || output_dtype != last_dtype) {
        std::printf("[field_nav] output tensor width=%d height=%d dtype=%u(%s)\n",
                    output_width, output_height, output_dtype, DTypeName(output_dtype));
        last_width = output_width;
        last_height = output_height;
        last_dtype = output_dtype;
    }

    auto postprocess_start = std::chrono::steady_clock::now();
    *line = ExtractLine(output_);
    auto postprocess_end = std::chrono::steady_clock::now();

    if (diag_frame) {
        TensorStats stats = ComputeStats(output_);
        std::printf("[field_nav] perf preprocess_ms=%.3f inference_ms=%.3f getoutput_ms=%.3f "
                    "postprocess_ms=%.3f\n",
                    ElapsedMs(preprocess_start, preprocess_end),
                    ElapsedMs(inference_start, inference_end),
                    ElapsedMs(getoutput_start, getoutput_end),
                    ElapsedMs(postprocess_start, postprocess_end));
        std::printf("[field_nav] output stats raw=[%.4f, %.4f, %.4f] prob=[%.4f, %.4f, %.4f] "
                    "threshold=%.2f scanned=%d fg_rows=%d components=%d main_area=%d "
                    "band_points=%d points=%d fallback=%d valid=%d reason=%s failure=%s\n",
                    stats.raw_min, stats.raw_max, stats.raw_mean,
                    stats.prob_min, stats.prob_max, stats.prob_mean,
                    kMaskThreshold, g_line_diag.scanned_rows, g_line_diag.foreground_rows,
                    g_line_diag.component_count, g_line_diag.main_area, g_line_diag.band_points,
                    g_line_diag.points, g_line_diag.fallback_count, line->valid ? 1 : 0,
                    g_line_diag.reason, g_line_diag.failure_reason);
    }
    return true;
}

float NavLineDetector::TensorValue(const ssne_tensor_t& tensor, int index) const {
    return TensorProbability(tensor, index);
}

NavLine NavLineDetector::ExtractLine(const ssne_tensor_t& output) const {
    NavLine line;
    g_line_diag = LineDiag{};

    int width = static_cast<int>(get_width(output));
    int height = static_cast<int>(get_height(output));
    if (width <= 0 || height <= 0) {
        return FailWithFallback(line, "invalid_output_shape");
    }

    const int total = width * height;
    const int y_start = static_cast<int>(height * 0.35f);
    ScratchBuffers& scratch = g_scratch;
    scratch.probs.resize(total);
    scratch.mask.resize(total);
    std::fill(scratch.mask.begin(), scratch.mask.end(), 0);
    line.points.reserve(std::max(1, (height - y_start + kBandHeight - 1) / kBandHeight));

    void* output_data = get_data(output);
    uint8_t output_dtype = get_data_type(output);

    for (int y = y_start; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = MaskIndex(x, y, width);
            float raw = TensorRawValueFromData(output_data, output_dtype, idx);
            float prob = TensorProbabilityFromRaw(raw, output_dtype);
            scratch.probs[idx] = prob;
            scratch.mask[idx] = prob >= kMaskThreshold ? 1 : 0;
        }
    }

    CloseVerticalGaps(scratch.mask, width, height, y_start, &scratch.morph_tmp, &scratch.closed_mask);
    LabelComponents(scratch.closed_mask, width, height, y_start,
                    &scratch.labels, &scratch.stack, &scratch.components);
    g_line_diag.component_count = static_cast<int>(scratch.components.size());
    if (scratch.components.empty()) {
        return FailWithFallback(line, "no_components");
    }

    int main_label = SelectMainComponent(scratch.components, width, height, y_start);
    if (main_label < 0) {
        return FailWithFallback(line, "no_main_component");
    }

    for (const auto& comp : scratch.components) {
        if (comp.label == main_label) {
            g_line_diag.main_area = comp.area;
            break;
        }
    }

    for (int y1 = height - 1; y1 >= y_start; y1 -= kBandHeight) {
        ++g_line_diag.scanned_rows;

        int y0 = std::max(y_start, y1 - kBandHeight + 1);
        float weighted_x = 0.0f;
        float weighted_y = 0.0f;
        float weight = 0.0f;
        int pixels = 0;

        for (int y = y0; y <= y1; ++y) {
            for (int x = 0; x < width; ++x) {
                int idx = MaskIndex(x, y, width);
                if (scratch.labels[idx] == main_label) {
                    float prob = std::max(scratch.probs[idx], kMaskThreshold);
                    weighted_x += prob * x;
                    weighted_y += prob * y;
                    weight += prob;
                    ++pixels;
                }
            }
        }

        if (pixels > 0 && weight > 1e-5f) {
            ++g_line_diag.foreground_rows;
            ++g_line_diag.band_points;
            float cx = weighted_x / weight;
            float cy = weighted_y / weight;
            float original_x = cx * static_cast<float>(kCropWidth) / static_cast<float>(std::max(1, width - 1));
            float original_y = kCropOffsetY +
                cy * static_cast<float>(kCropHeight) / static_cast<float>(std::max(1, height - 1));
            line.points.push_back({original_x, original_y, weight / std::max(1, pixels)});
        }
    }

    g_line_diag.points = static_cast<int>(line.points.size());
    if (g_line_diag.foreground_rows == 0) {
        return FailWithFallback(line, "no_foreground_bands");
    }

    const char* fit_reason = "not_run";
    if (!FitLeastSquares(&line, &fit_reason)) {
        return FailWithFallback(line, fit_reason);
    }

    g_last_valid_line = line;
    g_has_last_valid_line = true;
    g_consecutive_failures = 0;
    g_line_diag.reason = "ok_tdm_ls";
    return line;
}

void NavLineDetector::Release() {
    if (initialized_) {
        if (get_data(output_) != nullptr) {
            release_tensor(output_);
            output_ = {};
        }
        if (get_data(input_) != nullptr) {
            release_tensor(input_);
            input_ = {};
        }
        if (preprocess_ != nullptr) {
            ReleaseAIPreprocessPipe(preprocess_);
            preprocess_ = nullptr;
        }
        initialized_ = false;
    }
}

}  // namespace field_nav

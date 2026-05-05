#include "field_nav.hpp"

#include <cstdio>

namespace field_nav {

bool ImageProcessor::Initialize() {
    OnlineSetCrop(kPipeline0, 0, kCropWidth, kCropOffsetY, kCropOffsetY + kCropHeight);
    OnlineSetOutputImage(kPipeline0, SSNE_Y_8, kCropWidth, kCropHeight);
    int ret = OpenOnlinePipeline(kPipeline0);
    if (ret != 0) {
        std::fprintf(stderr, "[field_nav] OpenOnlinePipeline failed: %d\n", ret);
        return false;
    }
    initialized_ = true;
    std::printf("[field_nav] online pipe0 opened, crop=%dx%d offset_y=%d\n",
                kCropWidth, kCropHeight, kCropOffsetY);
    return true;
}

bool ImageProcessor::GetImage(ssne_tensor_t* img_sensor) {
    int ret = GetImageData(img_sensor, kPipeline0, kSensor0, 0);
    if (ret != 0) {
        std::fprintf(stderr, "[field_nav] GetImageData failed: %d\n", ret);
        return false;
    }
    return true;
}

void ImageProcessor::Release() {
    if (initialized_) {
        CloseOnlinePipeline(kPipeline0);
        initialized_ = false;
    }
}

}  // namespace field_nav


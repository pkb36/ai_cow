#pragma once

#include <functional>
#include <optional>
#include <opencv2/opencv.hpp>
#include <gst/gst.h>

// 비디오 프레임 처리를 위한 인터페이스
class VideoProcessor {
public:
    struct FrameInfo {
        int width;
        int height;
        int format;
        uint64_t timestamp;
        int cameraIndex;
    };

    struct BoundingBox {
        int x, y, width, height;
        int classId;
        float confidence;
        std::string label;
        int trackingId;
    };

    using ProcessCallback = std::function<void(cv::Mat& frame, 
                                              const FrameInfo& info,
                                              std::vector<BoundingBox>& objects)>;

    VideoProcessor();
    ~VideoProcessor();

    void setProcessCallback(ProcessCallback callback) { 
        processCallback_ = callback; 
    }

    // GStreamer 버퍼를 OpenCV Mat으로 변환
    std::optional<cv::Mat> bufferToMat(GstBuffer* buffer, const FrameInfo& info);

    // 메타데이터 추출
    std::vector<BoundingBox> extractMetadata(GstBuffer* buffer);

    // 오버레이 그리기
    void drawOverlay(cv::Mat& frame, const std::vector<BoundingBox>& objects);

private:
    ProcessCallback processCallback_;
};
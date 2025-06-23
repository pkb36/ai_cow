#include "video/VideoProcessor.hpp"
#include "core/Logger.hpp"
#include <gst/video/video.h>

VideoProcessor::VideoProcessor() {
    LOG_TRACE("VideoProcessor created");
}

VideoProcessor::~VideoProcessor() = default;

std::optional<cv::Mat> VideoProcessor::bufferToMat(GstBuffer* buffer, const FrameInfo& info) {
    if (!buffer) {
        LOG_ERROR("Invalid buffer");
        return std::nullopt;
    }
    
    // GstBuffer를 매핑
    GstMapInfo mapInfo;
    if (!gst_buffer_map(buffer, &mapInfo, GST_MAP_READ)) {
        LOG_ERROR("Failed to map buffer");
        return std::nullopt;
    }
    
    // OpenCV Mat 생성
    cv::Mat mat;
    
    try {
        // 포맷에 따라 다르게 처리
        switch (info.format) {
            case GST_VIDEO_FORMAT_I420:
            case GST_VIDEO_FORMAT_NV12: {
                // YUV to BGR 변환 필요
                cv::Mat yuv(info.height + info.height / 2, info.width, CV_8UC1, mapInfo.data);
                cv::cvtColor(yuv, mat, cv::COLOR_YUV2BGR_I420);
                break;
            }
            
            case GST_VIDEO_FORMAT_RGB:
                mat = cv::Mat(info.height, info.width, CV_8UC3, mapInfo.data).clone();
                cv::cvtColor(mat, mat, cv::COLOR_RGB2BGR);
                break;
                
            case GST_VIDEO_FORMAT_BGR:
                mat = cv::Mat(info.height, info.width, CV_8UC3, mapInfo.data).clone();
                break;
                
            case GST_VIDEO_FORMAT_RGBA:
                mat = cv::Mat(info.height, info.width, CV_8UC4, mapInfo.data).clone();
                cv::cvtColor(mat, mat, cv::COLOR_RGBA2BGR);
                break;
                
            default:
                LOG_WARNING("Unsupported video format: {}", info.format);
                gst_buffer_unmap(buffer, &mapInfo);
                return std::nullopt;
        }
        
    } catch (const cv::Exception& e) {
        LOG_ERROR("OpenCV exception: {}", e.what());
        gst_buffer_unmap(buffer, &mapInfo);
        return std::nullopt;
    }
    
    gst_buffer_unmap(buffer, &mapInfo);
    
    // 프로세싱 콜백 호출
    if (processCallback_ && !mat.empty()) {
        std::vector<BoundingBox> objects = extractMetadata(buffer);
        processCallback_(mat, info, objects);
    }
    
    return mat;
}

std::vector<VideoProcessor::BoundingBox> VideoProcessor::extractMetadata(GstBuffer* buffer) {
    std::vector<BoundingBox> objects;
    
    if (!buffer) {
        return objects;
    }
    
    // GStreamer 메타데이터에서 객체 정보 추출
    // DeepStream이나 다른 추론 엔진의 메타데이터 형식에 따라 구현
    
    // 예시: NvDsMeta (DeepStream) 처리
    /*
    NvDsBatchMeta* batchMeta = gst_buffer_get_nvds_batch_meta(buffer);
    if (batchMeta) {
        for (NvDsMetaList* l_frame = batchMeta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
            NvDsFrameMeta* frameMeta = (NvDsFrameMeta*)(l_frame->data);
            
            for (NvDsMetaList* l_obj = frameMeta->obj_meta_list; l_obj != nullptr; l_obj = l_obj->next) {
                NvDsObjectMeta* objMeta = (NvDsObjectMeta*)(l_obj->data);
                
                BoundingBox box;
                box.x = objMeta->rect_params.left;
                box.y = objMeta->rect_params.top;
                box.width = objMeta->rect_params.width;
                box.height = objMeta->rect_params.height;
                box.classId = objMeta->class_id;
                box.confidence = objMeta->confidence;
                box.label = objMeta->obj_label;
                box.trackingId = objMeta->object_id;
                
                objects.push_back(box);
            }
        }
    }
    */
    
    return objects;
}

void VideoProcessor::drawOverlay(cv::Mat& frame, const std::vector<BoundingBox>& objects) {
    if (frame.empty()) {
        return;
    }
    
    // 각 객체에 대해 바운딩 박스와 레이블 그리기
    for (const auto& obj : objects) {
        // 바운딩 박스
        cv::Rect rect(obj.x, obj.y, obj.width, obj.height);
        
        // 클래스별 색상
        cv::Scalar color;
        switch (obj.classId % 6) {
            case 0: color = cv::Scalar(255, 0, 0); break;     // Red
            case 1: color = cv::Scalar(0, 255, 0); break;     // Green
            case 2: color = cv::Scalar(0, 0, 255); break;     // Blue
            case 3: color = cv::Scalar(255, 255, 0); break;   // Cyan
            case 4: color = cv::Scalar(255, 0, 255); break;   // Magenta
            case 5: color = cv::Scalar(0, 255, 255); break;   // Yellow
        }
        
        // 박스 그리기
        cv::rectangle(frame, rect, color, 2);
        
        // 레이블 배경
        std::string label = obj.label;
        if (!label.empty()) {
            label += " (" + std::to_string(static_cast<int>(obj.confidence * 100)) + "%)";
        }
        
        if (obj.trackingId >= 0) {
            label += " #" + std::to_string(obj.trackingId);
        }
        
        int baseline = 0;
        cv::Size textSize = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
        
        cv::rectangle(frame, 
                     cv::Point(obj.x, obj.y - textSize.height - 4),
                     cv::Point(obj.x + textSize.width, obj.y),
                     color, cv::FILLED);
        
        // 레이블 텍스트
        cv::putText(frame, label,
                   cv::Point(obj.x, obj.y - 2),
                   cv::FONT_HERSHEY_SIMPLEX, 0.5,
                   cv::Scalar(255, 255, 255), 1);
    }
}
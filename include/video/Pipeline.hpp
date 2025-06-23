#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <optional>
#include <gst/gst.h>
#include <gst/gstpad.h>

// GStreamer 객체를 위한 커스텀 삭제자
template<typename T>
struct GstDeleter {
    void operator()(T* ptr) const {
        if (ptr) gst_object_unref(GST_OBJECT(ptr));
    }
};

template<typename T>
using GstPtr = std::unique_ptr<T, GstDeleter<T>>;

// 카메라 장치 타입
enum class CameraDevice : int {
    RGB = 0,
    THERMAL = 1
};

// 스트림 타입
enum class StreamType : int {
    MAIN = 0,
    SECONDARY = 1
};

class Pipeline {
public:
    struct CameraConfig {
        std::string source;
        std::string encoder;
        std::string encoder2;
        std::string inferConfig;
        std::string recordEncoder;
        std::string snapshotEncoder;
        int deviceIndex;
    };

    struct PipelineConfig {
        std::vector<CameraConfig> cameras;
        std::string snapshotPath = "/tmp/snapshots";
        int maxStreamCount = 10;
        int basePort = 5000;
        std::string codecName = "H264";
    };

    Pipeline();
    ~Pipeline();

    // 파이프라인 생성 및 제어
    bool create(const PipelineConfig& config);
    bool start();
    bool stop();
    bool isRunning() const;

    // 엘리먼트 접근
    GstElement* getElement(const std::string& name);
    
    // 동적 스트림 추가/제거
    bool addStream(const std::string& peerId, CameraDevice device, StreamType type);
    bool removeStream(const std::string& peerId);

    // 프로브 콜백 타입
    using ProbeCallback = std::function<GstPadProbeReturn(GstPad*, GstPadProbeInfo*)>;
    
    // 프로브 추가
    bool addProbe(const std::string& elementName, const std::string& padName, 
                  GstPadProbeType probeType, ProbeCallback callback);

    // 상태 조회
    GstState getState() const;
    std::string getStateString() const;

    // 통계 정보
    struct Statistics {
        uint64_t framesProcessed = 0;
        uint64_t bytesProcessed = 0;
        double currentFps = 0.0;
        double averageFps = 0.0;
    };
    
    Statistics getStatistics(CameraDevice device) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
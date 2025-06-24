#include "video/Pipeline.hpp"
#include "video/PipelineBuilder.hpp"
#include "core/Logger.hpp"
#include <gst/gstpad.h>
#include <gst/gstbus.h>
#include <algorithm>
#include <atomic>

struct Pipeline::Impl {
    GstPtr<GstElement> pipeline;
    PipelineConfig config;
    std::unordered_map<std::string, GstElement*> elements;
    std::unordered_map<std::string, gulong> probeIds;
    std::unordered_map<std::string, ProbeCallback> probeCallbacks;
    
    // 기존 스트림 관리 (제거 예정)
    struct StreamInfo {
        std::string peerId;
        CameraDevice device;
        StreamType type;
        int port;
        GstElement* udpsink = nullptr;
        bool active = false;
    };
    std::vector<StreamInfo> streams;
    std::mutex streamMutex;
    
    // 동적 스트림 관리 - 새로 추가
    std::unordered_map<std::string, DynamicStreamInfo> dynamicStreams;
    std::mutex dynamicStreamMutex;
    std::set<int> usedDynamicPorts;
    
    // 통계
    mutable std::mutex statsMutex;
    std::unordered_map<CameraDevice, Statistics> stats;
    
    // 상태
    std::atomic<bool> running{false};
    GstState currentState{GST_STATE_NULL};
    
    // 헬퍼 함수들
    std::string buildPipelineString();
    bool setupOsdProbes();
    bool registerElements();
    void initializeStreams();
    
    // 동적 스트림 관련 - 새로 추가
    int allocateDynamicPort();
    void releaseDynamicPort(int port);
    GstElement* getTeeElement(CameraDevice device, StreamType type);
    
    // 정적 콜백
    static GstPadProbeReturn universalProbeCallback(GstPad* pad, GstPadProbeInfo* info, gpointer userData);
    static gboolean busCallback(GstBus* bus, GstMessage* message, gpointer userData);
};

Pipeline::Pipeline() : impl_(std::make_unique<Impl>()) {
    LOG_TRACE("Pipeline created");
}

Pipeline::~Pipeline() {
    stop();
    LOG_TRACE("Pipeline destroyed");
}

bool Pipeline::create(const PipelineConfig& config) {
    LOG_INFO("Creating pipeline with {} cameras", config.webrtcConfig.deviceCnt);
    
    impl_->config = config;
    
    // cameras 초기화 (config에서 deviceCnt를 기반으로)
    impl_->config.cameras.clear();
    for (int i = 0; i < config.webrtcConfig.deviceCnt; ++i) {
        CameraInfo camInfo;
        camInfo.device = static_cast<CameraDevice>(i);
        if (i == 0) {
            camInfo.description = "RGB Camera";
        } else if (i == 1) {
            camInfo.description = "Thermal Camera";
        } else {
            camInfo.description = "Camera " + std::to_string(i);
        }
        impl_->config.cameras.push_back(camInfo);
    }
    
    // 파이프라인 문자열 생성
    std::string pipelineStr = impl_->buildPipelineString();
    LOG_DEBUG("Pipeline string length: {}", pipelineStr.length());
    LOG_DEBUG("Pipeline string: {}", pipelineStr);
    
    // 파이프라인 생성
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipelineStr.c_str(), &error);
    
    if (error) {
        LOG_ERROR("Failed to create pipeline: {}", error->message);
        g_error_free(error);
        return false;
    }
    
    impl_->pipeline.reset(pipeline);
    
    // 버스 설정
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, Impl::busCallback, this);
    gst_object_unref(bus);
    
    // 엘리먼트 등록
    if (!impl_->registerElements()) {
        LOG_ERROR("Failed to register elements");
        return false;
    }
    
    // 스트림 초기화
    impl_->initializeStreams();
    
    // OSD 프로브 설정
    if (!impl_->setupOsdProbes()) {
        LOG_ERROR("Failed to setup OSD probes");
        return false;
    }
    
    LOG_INFO("Pipeline created successfully");
    return true;
}

bool Pipeline::start() {
    if (!impl_->pipeline) {
        LOG_ERROR("Pipeline not created");
        return false;
    }
    
    if (impl_->running) {
        LOG_WARNING("Pipeline already running");
        return true;
    }
    
    LOG_INFO("Starting pipeline");
    
    GstStateChangeReturn ret = gst_element_set_state(
        impl_->pipeline.get(), GST_STATE_PLAYING);
    
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to start pipeline");
        
        // 에러 메시지 가져오기
        GstBus* bus = gst_element_get_bus(impl_->pipeline.get());
        GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        
        if (msg) {
            GError* err;
            gchar* debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            
            LOG_ERROR("Error from element {}: {}", 
                     GST_OBJECT_NAME(msg->src), err->message);
            LOG_DEBUG("Debug info: {}", debug_info ? debug_info : "none");
            
            g_clear_error(&err);
            g_free(debug_info);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
        
        return false;
    }
    
    // 상태 변경 대기
    GstState state, pending;
    ret = gst_element_get_state(impl_->pipeline.get(), &state, &pending, GST_CLOCK_TIME_NONE);
    
    if (ret == GST_STATE_CHANGE_SUCCESS) {
        impl_->currentState = state;
        impl_->running = true;
        LOG_INFO("Pipeline started successfully, state: {}", getStateString());
    }
    
    return impl_->running;
}

bool Pipeline::stop() {
    if (!impl_->pipeline || !impl_->running) {
        return true;
    }
    
    LOG_INFO("Stopping pipeline");
    
    impl_->running = false;
    
    // 모든 프로브 제거
    for (const auto& [elementName, probeId] : impl_->probeIds) {
        if (auto* element = getElement(elementName)) {
            if (auto* pad = gst_element_get_static_pad(element, "sink")) {
                gst_pad_remove_probe(pad, probeId);
                gst_object_unref(pad);
            }
        }
    }
    impl_->probeIds.clear();
    impl_->probeCallbacks.clear();
    
    // 파이프라인 정지
    GstStateChangeReturn ret = gst_element_set_state(
        impl_->pipeline.get(), GST_STATE_NULL);
    
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to stop pipeline");
        return false;
    }
    
    // 상태 변경 대기
    GstState state, pending;
    gst_element_get_state(impl_->pipeline.get(), &state, &pending, GST_CLOCK_TIME_NONE);
    impl_->currentState = state;
    
    LOG_INFO("Pipeline stopped");
    return true;
}

bool Pipeline::isRunning() const {
    return impl_->running;
}

GstElement* Pipeline::getElement(const std::string& name) {
    auto it = impl_->elements.find(name);
    if (it != impl_->elements.end()) {
        return it->second;
    }
    
    // 캐시에 없으면 파이프라인에서 찾기
    if (impl_->pipeline) {
        GstElement* element = gst_bin_get_by_name(GST_BIN(impl_->pipeline.get()), name.c_str());
        if (element) {
            impl_->elements[name] = element;
            return element;
        }
    }
    
    return nullptr;
}

std::string Pipeline::Impl::buildPipelineString() {
    const auto& webrtcConfig = config.webrtcConfig;
    std::stringstream ss;
    
    // 각 비디오 소스에 대한 파이프라인 구성
    for (int i = 0; i < webrtcConfig.deviceCnt && i < 2; ++i) {
        const auto& video = webrtcConfig.video[i];
        
        // 1. 비디오 소스
        ss << video.src << " ";
        
        // 2. 녹화 브랜치 (UDP로 출력)
        ss << video.record << " ";
        
        // 3. 추론 브랜치 - dspostproc 제거하고 nvtracker로 대체
        if (!video.infer.empty()) {
            std::string inferStr = video.infer;
            
            // dspostproc 관련 부분 제거
            size_t dspostprocPos = inferStr.find("dspostproc");
            if (dspostprocPos != std::string::npos) {
                // dspostproc name=dspostproc_X ! 부분을 찾아서 제거
                size_t endPos = inferStr.find("!", dspostprocPos);
                if (endPos != std::string::npos) {
                    // dspostproc 엘리먼트 전체를 nvtracker로 대체
                    std::string trackerStr = "nvtracker ll-lib-file=/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so ll-config-file=/home/nvidia/webrtc/tracker_config.yml !";
                    inferStr.replace(dspostprocPos, endPos - dspostprocPos + 1, trackerStr);
                }
            }
            
            // nvof 제거 (옵션)
            size_t nvofPos = inferStr.find("nvof");
            if (nvofPos != std::string::npos) {
                size_t nvofEndPos = inferStr.find("!", nvofPos);
                if (nvofEndPos != std::string::npos) {
                    inferStr.erase(nvofPos, nvofEndPos - nvofPos + 1);
                }
            }
            
            ss << inferStr << " ";
        }
        
        // 4. 동적 스트리밍을 위한 tee (allow-not-linked 추가)
        ss << "tee name=dynamic_tee_main_" << i << " allow-not-linked=true ";
        
        // 5. 메인 인코더 브랜치
        ss << "dynamic_tee_main_" << i << ". ! queue max-size-buffers=10 leaky=downstream ! " << video.enc;
        ss << "tee name=dynamic_tee_main_enc_" << i << " allow-not-linked=true ";
        
        // 6. 서브 인코더 브랜치
        ss << "dynamic_tee_main_" << i << ". ! queue max-size-buffers=10 leaky=downstream ! " << video.enc2;
        ss << "tee name=dynamic_tee_sub_enc_" << i << " allow-not-linked=true ";
        
        // 7. 스냅샷 브랜치
        ss << "dynamic_tee_main_" << i << ". ! queue ! " << video.snapshot;
        ss << "location=" << webrtcConfig.snapshotPath << "/cam" << i << "_snapshot.jpg ";
    }
    
    return ss.str();
}

// 엘리먼트 등록
bool Pipeline::Impl::registerElements() {
    if (!pipeline) return false;
    
    // nvosd 엘리먼트 찾기 및 등록
    for (size_t i = 0; i < config.cameras.size(); ++i) {
        std::string elementName = "nvosd_" + std::to_string(i + 1);
        GstElement* element = gst_bin_get_by_name(GST_BIN(pipeline.get()), elementName.c_str());
        if (element) {
            elements[elementName] = element;
            LOG_DEBUG("Registered element: {}", elementName);
        }
    }
    
    // udpsink 엘리먼트들 등록
    for (size_t streamIdx = 0; streamIdx < config.maxStreamCount + 2; ++streamIdx) {
        for (size_t camIdx = 0; camIdx < config.cameras.size(); ++camIdx) {
            for (int streamType = 0; streamType < 2; ++streamType) {
                std::string sinkName = "udpsink_" + std::to_string(camIdx) + "_" + 
                                     std::to_string(streamType) + "_" + std::to_string(streamIdx);
                
                GstElement* element = gst_bin_get_by_name(GST_BIN(pipeline.get()), sinkName.c_str());
                if (element) {
                    elements[sinkName] = element;
                }
            }
        }
    }
    
    LOG_INFO("Registered {} elements", elements.size());
    return true;
}

// 스트림 초기화
void Pipeline::Impl::initializeStreams() {
    std::lock_guard<std::mutex> lock(streamMutex);
    
    streams.clear();
    streams.reserve(config.maxStreamCount * config.cameras.size() * 2);
    
    // 모든 가능한 스트림 슬롯 초기화
    for (size_t streamIdx = 0; streamIdx < config.maxStreamCount; ++streamIdx) {
        for (size_t camIdx = 0; camIdx < config.cameras.size(); ++camIdx) {
            for (int streamType = 0; streamType < 2; ++streamType) {
                StreamInfo info;
                info.device = static_cast<CameraDevice>(camIdx);
                info.type = static_cast<StreamType>(streamType);
                info.port = config.basePort + (streamIdx * 100) + (camIdx * 2) + streamType;
                info.active = false;
                
                std::string sinkName = "udpsink_" + std::to_string(camIdx) + "_" + 
                                     std::to_string(streamType) + "_" + std::to_string(streamIdx);
                info.udpsink = elements[sinkName];
                
                streams.push_back(info);
            }
        }
    }
}

// OSD 프로브 설정
bool Pipeline::Impl::setupOsdProbes() {
    for (size_t i = 0; i < config.cameras.size(); ++i) {
        std::string elementName = "nvosd_" + std::to_string(i + 1);
        auto it = elements.find(elementName);
        
        if (it != elements.end() && it->second) {
            GstPad* pad = gst_element_get_static_pad(it->second, "sink");
            if (!pad) {
                LOG_ERROR("Failed to get sink pad for {}", elementName);
                continue;
            }
            
            // 통계 데이터 포인터
            auto& camStats = stats[static_cast<CameraDevice>(i)];
            
            gulong probeId = gst_pad_add_probe(pad,
                GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad* pad, GstPadProbeInfo* info, gpointer userData) -> GstPadProbeReturn {
                    auto* stats = static_cast<Statistics*>(userData);
                    stats->framesProcessed++;
                    
                    // FPS 계산
                    static auto lastTime = std::chrono::steady_clock::now();
                    static uint64_t lastFrameCount = 0;
                    
                    auto now = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime);
                    
                    if (duration.count() >= 1000) {
                        stats->currentFps = static_cast<double>(stats->framesProcessed - lastFrameCount) * 1000.0 / duration.count();
                        lastFrameCount = stats->framesProcessed;
                        lastTime = now;
                        
                        // 평균 FPS 업데이트
                        if (stats->averageFps == 0) {
                            stats->averageFps = stats->currentFps;
                        } else {
                            stats->averageFps = stats->averageFps * 0.9 + stats->currentFps * 0.1;
                        }
                    }
                    
                    return GST_PAD_PROBE_OK;
                },
                &camStats,
                nullptr);
            
            probeIds[elementName] = probeId;
            gst_object_unref(pad);
            
            LOG_DEBUG("Added OSD probe for {}", elementName);
        }
    }
    
    return true;
}

// 프로브 추가
bool Pipeline::addProbe(const std::string& elementName, const std::string& padName,
                       GstPadProbeType probeType, ProbeCallback callback) {
    GstElement* element = getElement(elementName);
    if (!element) {
        LOG_ERROR("Element not found: {}", elementName);
        return false;
    }
    
    GstPad* pad = gst_element_get_static_pad(element, padName.c_str());
    if (!pad) {
        LOG_ERROR("Pad not found: {} on element {}", padName, elementName);
        return false;
    }
    
    // 콜백 저장
    std::string probeKey = elementName + ":" + padName;
    impl_->probeCallbacks[probeKey] = callback;
    
    gulong probeId = gst_pad_add_probe(pad, probeType,
        Impl::universalProbeCallback,
        &impl_->probeCallbacks[probeKey],
        nullptr);
    
    if (probeId == 0) {
        LOG_ERROR("Failed to add probe to {}:{}", elementName, padName);
        impl_->probeCallbacks.erase(probeKey);
        gst_object_unref(pad);
        return false;
    }
    
    impl_->probeIds[probeKey] = probeId;
    gst_object_unref(pad);
    
    LOG_DEBUG("Added probe to {}:{}", elementName, padName);
    return true;
}

// 범용 프로브 콜백
GstPadProbeReturn Pipeline::Impl::universalProbeCallback(GstPad* pad, 
                                                        GstPadProbeInfo* info, 
                                                        gpointer userData) {
    auto* callback = static_cast<ProbeCallback*>(userData);
    if (callback && *callback) {
        return (*callback)(pad, info);
    }
    return GST_PAD_PROBE_OK;
}

// 동적 스트림 추가
bool Pipeline::addStream(const std::string& peerId, CameraDevice device, StreamType type) {
    std::lock_guard<std::mutex> lock(impl_->streamMutex);
    
    // 이미 존재하는지 확인
    auto it = std::find_if(impl_->streams.begin(), impl_->streams.end(),
        [&](const auto& stream) {
            return stream.peerId == peerId && 
                   stream.device == device && 
                   stream.type == type;
        });
    
    if (it != impl_->streams.end() && it->active) {
        LOG_WARNING("Stream already exists for peer: {}", peerId);
        return false;
    }
    
    // 사용 가능한 스트림 슬롯 찾기
    auto slotIt = std::find_if(impl_->streams.begin(), impl_->streams.end(),
        [&](const auto& stream) {
            return !stream.active && 
                   stream.device == device && 
                   stream.type == type;
        });
    
    if (slotIt == impl_->streams.end()) {
        LOG_ERROR("No available stream slot for device {} type {}", 
                 static_cast<int>(device), static_cast<int>(type));
        return false;
    }
    
    // 스트림 활성화
    slotIt->peerId = peerId;
    slotIt->active = true;
    
    // UDP sink 설정 (필요한 경우)
    if (slotIt->udpsink) {
        // 필요한 속성 설정
        g_object_set(slotIt->udpsink, "sync", FALSE, "async", FALSE, nullptr);
    }
    
    LOG_INFO("Added stream for peer {} on port {} (device: {}, type: {})", 
             peerId, slotIt->port, static_cast<int>(device), static_cast<int>(type));
    
    return true;
}

// 스트림 제거
bool Pipeline::removeStream(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(impl_->streamMutex);
    
    bool removed = false;
    for (auto& stream : impl_->streams) {
        if (stream.peerId == peerId && stream.active) {
            stream.active = false;
            stream.peerId.clear();
            removed = true;
            
            LOG_INFO("Removed stream for peer {} (device: {}, type: {})", 
                    peerId, static_cast<int>(stream.device), static_cast<int>(stream.type));
        }
    }
    
    if (!removed) {
        LOG_WARNING("No active stream found for peer: {}", peerId);
    }
    
    return removed;
}

// 상태 조회
GstState Pipeline::getState() const {
    if (!impl_->pipeline) {
        return GST_STATE_NULL;
    }
    
    GstState state, pending;
    GstStateChangeReturn ret = gst_element_get_state(
        impl_->pipeline.get(), &state, &pending, 0);
    
    if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL) {
        return state;
    }
    
    return impl_->currentState;
}

std::string Pipeline::getStateString() const {
    switch (getState()) {
        case GST_STATE_VOID_PENDING: return "VOID_PENDING";
        case GST_STATE_NULL: return "NULL";
        case GST_STATE_READY: return "READY";
        case GST_STATE_PAUSED: return "PAUSED";
        case GST_STATE_PLAYING: return "PLAYING";
        default: return "UNKNOWN";
    }
}

// 통계 정보
Pipeline::Statistics Pipeline::getStatistics(CameraDevice device) const {
    std::lock_guard<std::mutex> lock(impl_->statsMutex);
    
    auto it = impl_->stats.find(device);
    if (it != impl_->stats.end()) {
        return it->second;
    }
    
    return Statistics{};
}

// 버스 메시지 핸들러
gboolean Pipeline::Impl::busCallback(GstBus* bus, GstMessage* message, gpointer userData) {
    auto* pipeline = static_cast<Pipeline*>(userData);
    
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;
            gst_message_parse_error(message, &err, &debug);
            LOG_ERROR("Pipeline error from {}: {}", 
                     GST_OBJECT_NAME(message->src), err->message);
            LOG_DEBUG("Debug info: {}", debug ? debug : "none");
            g_error_free(err);
            g_free(debug);
            break;
        }
        
        case GST_MESSAGE_WARNING: {
            GError* err;
            gchar* debug;
            gst_message_parse_warning(message, &err, &debug);
            LOG_WARNING("Pipeline warning from {}: {}", 
                       GST_OBJECT_NAME(message->src), err->message);
            g_error_free(err);
            g_free(debug);
            break;
        }
        
        case GST_MESSAGE_EOS:
            LOG_INFO("End of stream");
            break;
            
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline->impl_->pipeline.get())) {
                GstState oldState, newState, pending;
                gst_message_parse_state_changed(message, &oldState, &newState, &pending);
                LOG_DEBUG("Pipeline state changed: {} -> {}", 
                         gst_element_state_get_name(oldState),
                         gst_element_state_get_name(newState));
                pipeline->impl_->currentState = newState;
            }
            break;
        }
        
        default:
            break;
    }
    
    return TRUE;
}

// 동적 포트 할당
int Pipeline::Impl::allocateDynamicPort() {
    // 동적 스트림용 포트 범위: 8000-9000
    for (int port = 8000; port < 9000; ++port) {
        if (usedDynamicPorts.find(port) == usedDynamicPorts.end()) {
            usedDynamicPorts.insert(port);
            return port;
        }
    }
    return -1;
}

void Pipeline::Impl::releaseDynamicPort(int port) {
    usedDynamicPorts.erase(port);
}

// Tee 엘리먼트 찾기
GstElement* Pipeline::Impl::getTeeElement(CameraDevice device, StreamType type) {
    std::string teeName;
    if (type == StreamType::MAIN) {
        teeName = "dynamic_tee_main_enc_" + std::to_string(static_cast<int>(device));
    } else {
        teeName = "dynamic_tee_sub_enc_" + std::to_string(static_cast<int>(device));
    }
    
    return gst_bin_get_by_name(GST_BIN(pipeline.get()), teeName.c_str());
}

// 동적 스트림 추가
std::optional<int> Pipeline::addDynamicStream(const std::string& peerId, 
                                             CameraDevice device, 
                                             StreamType type) {
    std::lock_guard<std::mutex> lock(impl_->dynamicStreamMutex);
    
    // 이미 존재하는지 확인
    if (impl_->dynamicStreams.find(peerId) != impl_->dynamicStreams.end()) {
        LOG_WARNING("Dynamic stream already exists for peer: {}", peerId);
        return std::nullopt;
    }
    
    LOG_INFO("Adding dynamic stream for peer: {} (device: {}, type: {})", 
             peerId, static_cast<int>(device), static_cast<int>(type));
    
    // Tee 엘리먼트 찾기
    GstElement* tee = impl_->getTeeElement(device, type);
    if (!tee) {
        LOG_ERROR("Tee element not found for device {} type {}", 
                 static_cast<int>(device), static_cast<int>(type));
        return std::nullopt;
    }
    
    DynamicStreamInfo streamInfo;
    streamInfo.peerId = peerId;
    streamInfo.device = device;
    streamInfo.type = type;
    streamInfo.port = impl_->allocateDynamicPort();
    
    if (streamInfo.port < 0) {
        LOG_ERROR("Failed to allocate port for dynamic stream");
        gst_object_unref(tee);
        return std::nullopt;
    }
    
    // Queue 생성
    std::string queueName = "queue_" + peerId;
    streamInfo.queue = gst_element_factory_make("queue", queueName.c_str());
    if (!streamInfo.queue) {
        LOG_ERROR("Failed to create queue element");
        impl_->releaseDynamicPort(streamInfo.port);
        gst_object_unref(tee);
        return std::nullopt;
    }
    
    g_object_set(streamInfo.queue,
                 "max-size-buffers", 10,
                 "max-size-bytes", 0,
                 "max-size-time", 0,
                 "leaky", 2, // downstream
                 nullptr);
    
    // UDP sink 생성
    std::string sinkName = "udpsink_" + peerId;
    streamInfo.udpsink = gst_element_factory_make("udpsink", sinkName.c_str());
    if (!streamInfo.udpsink) {
        LOG_ERROR("Failed to create udpsink element");
        gst_object_unref(streamInfo.queue);
        impl_->releaseDynamicPort(streamInfo.port);
        gst_object_unref(tee);
        return std::nullopt;
    }
    
    g_object_set(streamInfo.udpsink,
                 "host", "127.0.0.1",
                 "port", streamInfo.port,
                 "sync", FALSE,
                 "async", FALSE,
                 nullptr);
    
    // 파이프라인에 추가
    if (!gst_bin_add(GST_BIN(impl_->pipeline.get()), streamInfo.queue)) {
        LOG_ERROR("Failed to add queue to pipeline");
        gst_object_unref(streamInfo.queue);
        gst_object_unref(streamInfo.udpsink);
        impl_->releaseDynamicPort(streamInfo.port);
        gst_object_unref(tee);
        return std::nullopt;
    }
    
    if (!gst_bin_add(GST_BIN(impl_->pipeline.get()), streamInfo.udpsink)) {
        LOG_ERROR("Failed to add udpsink to pipeline");
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.queue);
        gst_object_unref(streamInfo.queue);
        gst_object_unref(streamInfo.udpsink);
        impl_->releaseDynamicPort(streamInfo.port);
        gst_object_unref(tee);
        return std::nullopt;
    }
    
    // Queue와 UDP sink 연결
    if (!gst_element_link(streamInfo.queue, streamInfo.udpsink)) {
        LOG_ERROR("Failed to link queue to udpsink");
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.queue);
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.udpsink);
        gst_object_unref(streamInfo.queue);
        gst_object_unref(streamInfo.udpsink);
        impl_->releaseDynamicPort(streamInfo.port);
        gst_object_unref(tee);
        return std::nullopt;
    }
    
    // Tee에서 새 패드 요청
    GstPadTemplate* padTemplate = gst_element_class_get_pad_template(
        GST_ELEMENT_GET_CLASS(tee), "src_%u");
    streamInfo.teeSrcPad = gst_element_request_pad(tee, padTemplate, nullptr, nullptr);
    
    if (!streamInfo.teeSrcPad) {
        LOG_ERROR("Failed to request pad from tee");
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.queue);
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.udpsink);
        impl_->releaseDynamicPort(streamInfo.port);
        gst_object_unref(tee);
        return std::nullopt;
    }
    
    // Queue의 sink 패드 가져오기
    streamInfo.queueSinkPad = gst_element_get_static_pad(streamInfo.queue, "sink");
    
    // 패드 연결
    GstPadLinkReturn linkRet = gst_pad_link(streamInfo.teeSrcPad, streamInfo.queueSinkPad);
    if (linkRet != GST_PAD_LINK_OK) {
        LOG_ERROR("Failed to link tee to queue: {}", linkRet);
        gst_element_release_request_pad(tee, streamInfo.teeSrcPad);
        gst_object_unref(streamInfo.teeSrcPad);
        gst_object_unref(streamInfo.queueSinkPad);
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.queue);
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.udpsink);
        impl_->releaseDynamicPort(streamInfo.port);
        gst_object_unref(tee);
        return std::nullopt;
    }
    
    // 엘리먼트 상태 동기화
    gst_element_sync_state_with_parent(streamInfo.queue);
    gst_element_sync_state_with_parent(streamInfo.udpsink);
    
    // 정보 저장
    impl_->dynamicStreams[peerId] = streamInfo;
    
    gst_object_unref(tee);
    
    LOG_INFO("Successfully added dynamic stream for peer {} on port {}", 
             peerId, streamInfo.port);
    
    return streamInfo.port;
}

// 동적 스트림 제거
bool Pipeline::removeDynamicStream(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(impl_->dynamicStreamMutex);
    
    auto it = impl_->dynamicStreams.find(peerId);
    if (it == impl_->dynamicStreams.end()) {
        LOG_WARNING("Dynamic stream not found for peer: {}", peerId);
        return false;
    }
    
    LOG_INFO("Removing dynamic stream for peer: {}", peerId);
    
    DynamicStreamInfo& streamInfo = it->second;
    
    // 1. EOS 이벤트 전송 (깨끗한 종료)
    if (streamInfo.queueSinkPad) {
        gst_pad_send_event(streamInfo.queueSinkPad, gst_event_new_eos());
        // EOS 처리를 위한 짧은 대기
        g_usleep(50000); // 50ms
    }
    
    // 2. 패드 연결 해제
    if (streamInfo.teeSrcPad && streamInfo.queueSinkPad) {
        gst_pad_unlink(streamInfo.teeSrcPad, streamInfo.queueSinkPad);
    }
    
    // 3. 엘리먼트 상태 변경
    if (streamInfo.udpsink) {
        gst_element_set_state(streamInfo.udpsink, GST_STATE_NULL);
    }
    if (streamInfo.queue) {
        gst_element_set_state(streamInfo.queue, GST_STATE_NULL);
    }
    
    // 4. 파이프라인에서 제거
    if (streamInfo.udpsink) {
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.udpsink);
    }
    if (streamInfo.queue) {
        gst_bin_remove(GST_BIN(impl_->pipeline.get()), streamInfo.queue);
    }
    
    // 5. Tee 패드 해제
    if (streamInfo.teeSrcPad) {
        GstElement* tee = impl_->getTeeElement(streamInfo.device, streamInfo.type);
        if (tee) {
            gst_element_release_request_pad(tee, streamInfo.teeSrcPad);
            gst_object_unref(tee);
        }
        gst_object_unref(streamInfo.teeSrcPad);
    }
    
    // 6. Queue sink 패드 해제
    if (streamInfo.queueSinkPad) {
        gst_object_unref(streamInfo.queueSinkPad);
    }
    
    // 7. 포트 해제
    impl_->releaseDynamicPort(streamInfo.port);
    
    // 8. 맵에서 제거
    impl_->dynamicStreams.erase(it);
    
    LOG_INFO("Successfully removed dynamic stream for peer: {}", peerId);
    return true;
}

// 동적 스트림 정보 조회
std::optional<Pipeline::DynamicStreamInfo> 
Pipeline::getDynamicStreamInfo(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(impl_->dynamicStreamMutex);
    
    auto it = impl_->dynamicStreams.find(peerId);
    if (it != impl_->dynamicStreams.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

// 활성 peer ID 목록
std::vector<std::string> Pipeline::getActivePeerIds() const {
    std::lock_guard<std::mutex> lock(impl_->dynamicStreamMutex);
    
    std::vector<std::string> peerIds;
    peerIds.reserve(impl_->dynamicStreams.size());
    
    for (const auto& [peerId, info] : impl_->dynamicStreams) {
        peerIds.push_back(peerId);
    }
    
    return peerIds;
}
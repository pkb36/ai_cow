#include "video/Pipeline.hpp"
#include "video/PipelineBuilder.hpp"
#include "core/Logger.hpp"
#include <gst/gstpad.h>
#include <gst/gstbus.h>
#include <algorithm>
#include <atomic>
#include <sstream>

struct Pipeline::Impl {
    GstPtr<GstElement> pipeline;
    PipelineConfig config;
    std::unordered_map<std::string, GstElement*> elements;
    std::unordered_map<std::string, gulong> probeIds;
    std::unordered_map<std::string, ProbeCallback> probeCallbacks;
    
    // 동적 스트림 관리
    std::unordered_map<std::string, DynamicStreamInfo> dynamicStreams;
    std::set<int> usedPorts;
    std::mutex streamMutex;
    
    // Tee 엘리먼트들 (스트림 분기용)
    std::unordered_map<std::string, GstElement*> teeElements;
    
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
    int allocatePort();
    void releasePort(int port);
    bool createDynamicSink(DynamicStreamInfo& info);
    bool removeDynamicSink(const std::string& peerId);
    
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
    LOG_INFO("Creating pipeline with {} cameras", config.cameras);
    
    impl_->config = config;
    
    // 파이프라인 문자열 생성
    std::string pipelineStr = impl_->buildPipelineString();
    LOG_DEBUG("Pipeline string length: {}", pipelineStr.length());
    
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
    
    // 사용 가능한 포트 범위 초기화
    for (int port = config.basePort; port < config.basePort + 1000; port += 2) {
        // 녹화용 포트는 제외 (7000, 7001)
        if (port >= 7000 && port <= 7001) continue;
        impl_->usedPorts.insert(port);
    }
    
    // OSD 프로브 설정
    if (!impl_->setupOsdProbes()) {
        LOG_ERROR("Failed to setup OSD probes");
        return false;
    }
    
    LOG_INFO("Pipeline created successfully");
    return true;
}

std::string Pipeline::Impl::buildPipelineString() {
    const auto& webrtcConfig = config.webrtcConfig;
    std::stringstream ss;
    
    // 각 비디오 소스에 대한 파이프라인 구성
    for (int i = 0; i < webrtcConfig.deviceCnt && i < 2; ++i) {
        const auto& video = webrtcConfig.video[i];
        
        // 1. 비디오 소스와 녹화 브랜치
        ss << video.src << " ";
        ss << video.record << " ";
        
        // 2. 추론 브랜치가 있는 경우
        if (!video.infer.empty()) {
            ss << video.infer << " ";
            
            // 추론 후 메인 인코더를 위한 tee
            ss << "tee name=infer_tee_" << i << " ";
            ss << "infer_tee_" << i << ". ! queue ! ";
        }
        
        // 3. 메인 인코더
        ss << video.enc;
        ss << "tee name=stream_tee_main_" << i << " allow-not-linked=true ";
        ss << "stream_tee_main_" << i << ". ! fakesink ";
        
        // 4. 서브 인코더  
        ss << video.enc2;
        ss << "tee name=stream_tee_sub_" << i << " allow-not-linked=true ";
        ss << "stream_tee_sub_" << i << ". ! fakesink ";
        
        // 5. 스냅샷 브랜치
        ss << video.snapshot;
        ss << "location=" << webrtcConfig.snapshotPath << "/cam" << i << "_snapshot.jpg ";
    }
    
    return ss.str();
}

// 동적 스트림 추가 - 개선된 구현
std::optional<int> Pipeline::addDynamicStream(const std::string& peerId, 
                                             CameraDevice device, 
                                             StreamType type) {
    std::lock_guard<std::mutex> lock(impl_->streamMutex);
    
    // 이미 존재하는지 확인
    if (impl_->dynamicStreams.find(peerId) != impl_->dynamicStreams.end()) {
        LOG_WARNING("Stream already exists for peer: {}", peerId);
        return std::nullopt;
    }
    
    // 포트 할당
    int port = impl_->allocatePort();
    if (port < 0) {
        LOG_ERROR("No available ports for dynamic stream");
        return std::nullopt;
    }
    
    // 스트림 정보 생성
    DynamicStreamInfo info;
    info.peerId = peerId;
    info.device = device;
    info.type = type;
    info.port = port;
    info.active = false;
    
    // 동적 싱크 생성
    if (!impl_->createDynamicSink(info)) {
        impl_->releasePort(port);
        LOG_ERROR("Failed to create dynamic sink for peer: {}", peerId);
        return std::nullopt;
    }
    
    // 스트림 정보 저장
    impl_->dynamicStreams[peerId] = info;
    
    LOG_INFO("Added dynamic stream for peer {} on port {} (device: {}, type: {})", 
             peerId, port, static_cast<int>(device), static_cast<int>(type));
    
    return port;
}

// 동적 싱크 생성
bool Pipeline::Impl::createDynamicSink(DynamicStreamInfo& info) {
    if (!pipeline) return false;
    
    // Tee 엘리먼트 찾기
    std::string teeName = "stream_tee_";
    teeName += (info.type == StreamType::MAIN) ? "main_" : "sub_";
    teeName += std::to_string(static_cast<int>(info.device));
    
    GstElement* tee = gst_bin_get_by_name(GST_BIN(pipeline.get()), teeName.c_str());
    if (!tee) {
        LOG_ERROR("Tee element not found: {}", teeName);
        return false;
    }
    
    // 동적 엘리먼트 생성
    std::string queueName = "queue_" + info.peerId;
    std::string sinkName = "udpsink_" + info.peerId;
    
    info.queue = gst_element_factory_make("queue", queueName.c_str());
    info.udpsink = gst_element_factory_make("udpsink", sinkName.c_str());
    
    if (!info.queue || !info.udpsink) {
        LOG_ERROR("Failed to create elements for dynamic stream");
        if (info.queue) gst_object_unref(info.queue);
        if (info.udpsink) gst_object_unref(info.udpsink);
        gst_object_unref(tee);
        return false;
    }
    
    // Queue 설정
    g_object_set(info.queue,
                 "max-size-buffers", 100,
                 "max-size-time", G_GUINT64_CONSTANT(0),
                 "max-size-bytes", 0,
                 nullptr);
    
    // UDP sink 설정
    g_object_set(info.udpsink,
                 "host", "127.0.0.1",
                 "port", info.port,
                 "sync", FALSE,
                 "async", FALSE,
                 nullptr);
    
    // 파이프라인에 추가
    gst_bin_add_many(GST_BIN(pipeline.get()), info.queue, info.udpsink, nullptr);
    
    // 링크
    if (!gst_element_link(info.queue, info.udpsink)) {
        LOG_ERROR("Failed to link queue to udpsink");
        gst_bin_remove_many(GST_BIN(pipeline.get()), info.queue, info.udpsink, nullptr);
        gst_object_unref(tee);
        return false;
    }
    
    // Tee에서 새 패드 요청
    GstPad* teeSrcPad = gst_element_get_request_pad(tee, "src_%u");
    GstPad* queueSinkPad = gst_element_get_static_pad(info.queue, "sink");
    
    // 패드 연결
    if (gst_pad_link(teeSrcPad, queueSinkPad) != GST_PAD_LINK_OK) {
        LOG_ERROR("Failed to link tee to queue");
        gst_element_release_request_pad(tee, teeSrcPad);
        gst_object_unref(teeSrcPad);
        gst_object_unref(queueSinkPad);
        gst_bin_remove_many(GST_BIN(pipeline.get()), info.queue, info.udpsink, nullptr);
        gst_object_unref(tee);
        return false;
    }
    
    info.teepad = teeSrcPad;  // 이제 타입이 맞음
    gst_object_unref(queueSinkPad);
    gst_object_unref(tee);
    
    // 엘리먼트 동기화 및 재생
    gst_element_sync_state_with_parent(info.queue);
    gst_element_sync_state_with_parent(info.udpsink);
    
    info.active = true;
    
    return true;
}

// 동적 스트림 제거
bool Pipeline::removeDynamicStream(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(impl_->streamMutex);
    
    auto it = impl_->dynamicStreams.find(peerId);
    if (it == impl_->dynamicStreams.end()) {
        LOG_WARNING("Stream not found for peer: {}", peerId);
        return false;
    }
    
    DynamicStreamInfo& info = it->second;
    
    if (!impl_->removeDynamicSink(peerId)) {
        LOG_ERROR("Failed to remove dynamic sink for peer: {}", peerId);
        return false;
    }
    
    // 포트 해제
    impl_->releasePort(info.port);
    
    // 스트림 정보 제거
    impl_->dynamicStreams.erase(it);
    
    LOG_INFO("Removed dynamic stream for peer: {}", peerId);
    return true;
}

// 동적 싱크 제거
bool Pipeline::Impl::removeDynamicSink(const std::string& peerId) {
    auto it = dynamicStreams.find(peerId);
    if (it == dynamicStreams.end()) {
        return false;
    }
    
    DynamicStreamInfo& info = it->second;
    
    if (!info.active) {
        return true;
    }
    
    // Tee 엘리먼트 찾기
    std::string teeName = "stream_tee_";
    teeName += (info.type == StreamType::MAIN) ? "main_" : "sub_";
    teeName += std::to_string(static_cast<int>(info.device));
    
    GstElement* tee = gst_bin_get_by_name(GST_BIN(pipeline.get()), teeName.c_str());
    if (!tee) {
        LOG_ERROR("Tee element not found: {}", teeName);
        return false;
    }
    
    // 엘리먼트 상태를 NULL로 변경
    if (info.queue) {
        gst_element_set_state(info.queue, GST_STATE_NULL);
    }
    if (info.udpsink) {
        gst_element_set_state(info.udpsink, GST_STATE_NULL);
    }
    
    // Tee 패드 해제
    if (info.teepad) {
        gst_element_release_request_pad(tee, info.teepad);  // 이제 타입이 맞음
        gst_object_unref(info.teepad);
        info.teepad = nullptr;
    }
    
    // 파이프라인에서 제거
    if (info.queue && info.udpsink) {
        gst_bin_remove_many(GST_BIN(pipeline.get()), info.queue, info.udpsink, nullptr);
    }
    
    gst_object_unref(tee);
    
    info.active = false;
    
    return true;
}

// 포트 할당
int Pipeline::Impl::allocatePort() {
    // 사용 가능한 포트 찾기
    for (int port = config.basePort; port < config.basePort + 1000; port += 2) {
        if (port >= 7000 && port <= 7001) continue; // 녹화용 포트 제외
        
        if (usedPorts.find(port) != usedPorts.end()) {
            usedPorts.erase(port);
            return port;
        }
    }
    return -1;
}

// 포트 해제
void Pipeline::Impl::releasePort(int port) {
    usedPorts.insert(port);
}

// 스트림 정보 조회
std::optional<Pipeline::DynamicStreamInfo> 
Pipeline::getDynamicStreamInfo(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(impl_->streamMutex);
    
    auto it = impl_->dynamicStreams.find(peerId);
    if (it != impl_->dynamicStreams.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

// 활성 peer ID 목록
std::vector<std::string> Pipeline::getActivePeerIds() const {
    std::lock_guard<std::mutex> lock(impl_->streamMutex);
    
    std::vector<std::string> peerIds;
    for (const auto& [peerId, info] : impl_->dynamicStreams) {
        if (info.active) {
            peerIds.push_back(peerId);
        }
    }
    
    return peerIds;
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
        return false;
    }
    
    impl_->running = true;
    LOG_INFO("Pipeline started successfully");
    return true;
}

bool Pipeline::stop() {
    if (!impl_->pipeline || !impl_->running) {
        return true;
    }
    
    LOG_INFO("Stopping pipeline");
    
    impl_->running = false;
    
    // 1. 모든 프로브 제거 (CUDA 작업 중단)
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
    
    // 2. 모든 동적 스트림 제거
    std::vector<std::string> peerIds = getActivePeerIds();
    for (const auto& peerId : peerIds) {
        removeDynamicStream(peerId);
    }
    
    // 3. 파이프라인을 PAUSED 상태로 먼저 전환
    GstStateChangeReturn ret = gst_element_set_state(
        impl_->pipeline.get(), GST_STATE_PAUSED);
    
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to pause pipeline");
    } else {
        // PAUSED 상태 대기
        GstState state, pending;
        gst_element_get_state(impl_->pipeline.get(), &state, &pending, 
                             GST_SECOND * 5); // 5초 타임아웃
    }
    
    // 4. 잠시 대기 (CUDA 작업 완료)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 5. 파이프라인을 NULL 상태로 전환
    ret = gst_element_set_state(impl_->pipeline.get(), GST_STATE_NULL);
    
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to stop pipeline");
        return false;
    }
    
    // 6. NULL 상태 대기
    GstState state, pending;
    ret = gst_element_get_state(impl_->pipeline.get(), &state, &pending, 
                               GST_SECOND * 10); // 10초 타임아웃
    
    if (ret == GST_STATE_CHANGE_SUCCESS) {
        impl_->currentState = GST_STATE_NULL;
        LOG_INFO("Pipeline stopped successfully");
    } else {
        LOG_WARNING("Pipeline state change timeout or failure");
    }
    
    // 7. 엘리먼트 참조 해제
    impl_->elements.clear();
    impl_->teeElements.clear();
    
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

bool Pipeline::Impl::registerElements() {
    if (!pipeline) return false;
    
    // Tee 엘리먼트 등록
    for (int i = 0; i < config.cameras; ++i) {
        std::string mainTeeName = "stream_tee_main_" + std::to_string(i);
        std::string subTeeName = "stream_tee_sub_" + std::to_string(i);
        
        GstElement* mainTee = gst_bin_get_by_name(GST_BIN(pipeline.get()), mainTeeName.c_str());
        GstElement* subTee = gst_bin_get_by_name(GST_BIN(pipeline.get()), subTeeName.c_str());
        
        if (mainTee) {
            teeElements[mainTeeName] = mainTee;
            elements[mainTeeName] = mainTee;
            LOG_DEBUG("Registered tee element: {}", mainTeeName);
        }
        
        if (subTee) {
            teeElements[subTeeName] = subTee;
            elements[subTeeName] = subTee;
            LOG_DEBUG("Registered tee element: {}", subTeeName);
        }
    }
    
    // OSD 엘리먼트 등록
    for (int i = 0; i < config.cameras; ++i) {
        std::string osdName = "nvosd_" + std::to_string(i + 1);
        GstElement* osd = gst_bin_get_by_name(GST_BIN(pipeline.get()), osdName.c_str());
        if (osd) {
            elements[osdName] = osd;
            LOG_DEBUG("Registered OSD element: {}", osdName);
        }
    }
    
    LOG_INFO("Registered {} elements", elements.size());
    return true;
}

// OSD 프로브 설정
bool Pipeline::Impl::setupOsdProbes() {
    for (int i = 0; i < config.cameras; ++i) {
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

GstPadProbeReturn Pipeline::Impl::universalProbeCallback(GstPad* pad, 
                                                        GstPadProbeInfo* info, 
                                                        gpointer userData) {
    auto* callback = static_cast<ProbeCallback*>(userData);
    if (callback && *callback) {
        return (*callback)(pad, info);
    }
    return GST_PAD_PROBE_OK;
}

bool Pipeline::addStream(const std::string& peerId, CameraDevice device, StreamType type) {
    auto port = addDynamicStream(peerId, device, type);
    return port.has_value();
}

bool Pipeline::removeStream(const std::string& peerId) {
    return removeDynamicStream(peerId);
}

// 상태 조회
GstState Pipeline::getState() const {
    if (!impl_->pipeline) {
        return GST_STATE_NULL;
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

Pipeline::Statistics Pipeline::getStatistics(CameraDevice device) const {
    std::lock_guard<std::mutex> lock(impl_->statsMutex);
    
    auto it = impl_->stats.find(device);
    if (it != impl_->stats.end()) {
        return it->second;
    }
    
    return Statistics{};
}

// 버스 메시지 핸들러
gboolean Pipeline::Impl::busCallback(GstBus*, GstMessage* message, gpointer userData) {
    auto* pipeline = static_cast<Pipeline*>(userData);
    
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;
            gst_message_parse_error(message, &err, &debug);
            LOG_ERROR("Pipeline error from {}: {}", 
                     GST_OBJECT_NAME(message->src), err->message);
            LOG_DEBUG("Debug info: {}", debug ? debug : "none");
            
            // CUDA 관련 에러 체크
            if (err->message && (strstr(err->message, "CUDA") || 
                                strstr(err->message, "nvvideoconvert") ||
                                strstr(err->message, "nvinfer"))) {
                LOG_ERROR("CUDA-related error detected. May need to restart.");
                
                // CUDA 디바이스 리셋 시도
                // cudaError_t cudaErr = cudaDeviceReset();
                // if (cudaErr != cudaSuccess) {
                //     LOG_ERROR("Failed to reset CUDA device: {}", 
                //              cudaGetErrorString(cudaErr));
                // }
            }
            
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
        
        case GST_MESSAGE_ELEMENT: {
            // DeepStream 관련 메시지 처리
            const GstStructure* structure = gst_message_get_structure(message);
            if (structure) {
                const gchar* name = gst_structure_get_name(structure);
                if (name && strstr(name, "nvstreammux")) {
                    LOG_DEBUG("nvstreammux message: {}", name);
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return TRUE;
}
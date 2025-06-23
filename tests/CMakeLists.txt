# 테스트 실행 파일
add_executable(webrtc_camera_tests
    test_main.cpp
    test_config.cpp
    test_pipeline.cpp
    test_websocket.cpp
)

# 메인 프로젝트의 소스 파일들 (main.cpp 제외)
set(TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/Application.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Config.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/network/WebSocketClient.cpp
    ${CMAKE_SOURCE_DIR}/src/network/WebRTCPeer.cpp
    ${CMAKE_SOURCE_DIR}/src/network/WebRTCManager.cpp
    ${CMAKE_SOURCE_DIR}/src/network/MessageHandler.cpp
    ${CMAKE_SOURCE_DIR}/src/network/SignalingProtocol.cpp
    ${CMAKE_SOURCE_DIR}/src/video/Pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/video/VideoProcessor.cpp
    ${CMAKE_SOURCE_DIR}/src/video/StreamManager.cpp
    ${CMAKE_SOURCE_DIR}/src/video/EventRecorder.cpp
    ${CMAKE_SOURCE_DIR}/src/hardware/SerialPort.cpp
    ${CMAKE_SOURCE_DIR}/src/monitoring/ThermalMonitor.cpp
    ${CMAKE_SOURCE_DIR}/src/monitoring/SystemMonitor.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/FileWatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/CommandExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/ThreadPool.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Performance.cpp
)

target_sources(webrtc_camera_tests PRIVATE ${TEST_SOURCES})

target_include_directories(webrtc_camera_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GST_INCLUDE_DIRS}
    ${SOUP_INCLUDE_DIRS}
    ${JSON_INCLUDE_DIRS}
    ${NICE_INCLUDE_DIRS}
)

target_link_libraries(webrtc_camera_tests
    ${GST_LIBRARIES}
    ${SOUP_LIBRARIES}
    ${JSON_LIBRARIES}
    ${NICE_LIBRARIES}
    Threads::Threads
    stdc++fs
)

# Google Test 찾기 (선택적)
find_package(GTest QUIET)
if(GTest_FOUND)
    target_link_libraries(webrtc_camera_tests GTest::GTest GTest::Main)
endif()

# 테스트 추가
add_test(NAME webrtc_camera_tests COMMAND webrtc_camera_tests)
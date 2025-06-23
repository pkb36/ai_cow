cmake_minimum_required(VERSION 3.10)
project(WebRTCCamera VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 빌드 타입
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 컴파일 옵션
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -fsanitize=address)
    add_link_options(-fsanitize=address)
else()
    add_compile_options(-O3)
endif()

# 의존성 찾기
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# GStreamer
pkg_check_modules(GST REQUIRED 
    gstreamer-1.0>=1.14
    gstreamer-webrtc-1.0
    gstreamer-sdp-1.0
    gstreamer-video-1.0
    gstreamer-app-1.0
)

# 기타 라이브러리
pkg_check_modules(SOUP REQUIRED libsoup-2.4)
pkg_check_modules(JSON REQUIRED json-glib-1.0)
pkg_check_modules(NICE REQUIRED nice)

# OpenCV (선택적)
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    add_definitions(-DHAVE_OPENCV)
endif()

# 소스 파일
set(SOURCES
    src/main.cpp
    src/core/Application.cpp
    src/core/Config.cpp
    src/core/Logger.cpp
    src/network/WebSocketClient.cpp
    src/network/WebRTCPeer.cpp
    src/network/WebRTCManager.cpp
    src/network/MessageHandler.cpp
    src/network/SignalingProtocol.cpp
    src/video/Pipeline.cpp
    src/video/VideoProcessor.cpp
    src/video/StreamManager.cpp
    src/video/EventRecorder.cpp
    src/hardware/SerialPort.cpp
    src/monitoring/ThermalMonitor.cpp
    src/monitoring/SystemMonitor.cpp
    src/utils/FileWatcher.cpp
    src/utils/CommandExecutor.cpp
    src/utils/ThreadPool.cpp
    src/utils/Performance.cpp
)

# 실행 파일
add_executable(webrtc_camera ${SOURCES})

# 포함 디렉토리
target_include_directories(webrtc_camera PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GST_INCLUDE_DIRS}
    ${SOUP_INCLUDE_DIRS}
    ${JSON_INCLUDE_DIRS}
    ${NICE_INCLUDE_DIRS}
)

if(OpenCV_FOUND)
    target_include_directories(webrtc_camera PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()

# 링크 라이브러리
target_link_libraries(webrtc_camera
    ${GST_LIBRARIES}
    ${SOUP_LIBRARIES}
    ${JSON_LIBRARIES}
    ${NICE_LIBRARIES}
    Threads::Threads
    stdc++fs
)

if(OpenCV_FOUND)
    target_link_libraries(webrtc_camera ${OpenCV_LIBS})
endif()

# 설치
install(TARGETS webrtc_camera RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/ DESTINATION etc/webrtc_camera)

# 테스트
enable_testing()
add_subdirectory(tests)

# 패키징
set(CPACK_PACKAGE_NAME "WebRTCCamera")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Your Name")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-libav")
include(CPack)
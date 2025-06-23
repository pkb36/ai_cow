cmake_minimum_required(VERSION 3.10)
project(WebRTCCamera VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 빌드 타입
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 컴파일 옵션 - DEBUG 매크로 충돌 방지
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
    add_definitions(-DWEBRTC_DEBUG)  # DEBUG 대신 WEBRTC_DEBUG 사용
else()
    add_compile_options(-O3)
endif()

# 의존성 찾기
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# fmt 라이브러리
find_package(fmt REQUIRED)

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
pkg_check_modules(GLIB REQUIRED glib-2.0)

# nlohmann_json 찾기
find_package(nlohmann_json REQUIRED)

add_compile_definitions(GST_USE_UNSTABLE_API)

# OpenCV (선택적)
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    add_definitions(-DHAVE_OPENCV)
    message(STATUS "OpenCV found: ${OpenCV_VERSION}")
endif()

# GLib 의존성 추가
pkg_check_modules(GLIB_DEPS REQUIRED 
    glib-2.0 
    gobject-2.0 
    gio-2.0
)

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

link_directories(/opt/nvidia/deepstream/deepstream-6.2/lib)

# 실행 파일
add_executable(ai_cds ${SOURCES})

# 포함 디렉토리
target_include_directories(ai_cds PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GST_INCLUDE_DIRS}
    ${SOUP_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${GLIB_DEPS_INCLUDE_DIRS}
    /opt/nvidia/deepstream/deepstream-6.2/sources/includes
)

if(OpenCV_FOUND)
    target_include_directories(ai_cds PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()

# 링크 라이브러리
target_link_libraries(ai_cds
    ${GST_LIBRARIES}
    ${SOUP_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${GLIB_DEPS_LIBRARIES}
    Threads::Threads
    stdc++fs
    fmt::fmt
    nlohmann_json::nlohmann_json
)

target_link_libraries(ai_cds
    nvdsgst_meta
)

if(OpenCV_FOUND)
    target_link_libraries(ai_cds ${OpenCV_LIBS})
endif()

# 컴파일 플래그
target_compile_options(ai_cds PRIVATE 
    ${GST_CFLAGS_OTHER}
    ${GLIB_DEPS_CFLAGS_OTHER}
)

# 링크 플래그
target_link_options(ai_cds PRIVATE
    ${GST_LDFLAGS_OTHER}
    ${GLIB_DEPS_LDFLAGS_OTHER}
)

# 설치
install(TARGETS ai_cds RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/ DESTINATION etc/ai_cds)

# 테스트 (선택적)
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# 패키징
set(CPACK_PACKAGE_NAME "WebRTCCamera")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Your Name")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-libav, libfmt-dev, nlohmann-json3-dev, libsoup2.4-dev")
include(CPack)
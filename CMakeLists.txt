cmake_minimum_required(VERSION 3.10)

set(APP_NAME VioDataLogger)
project(${APP_NAME} CXX)

# ==============================================================================
# 1. 编译选项与 RPATH 黄金配置
# ==============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread -O3")

set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib/aarch64:$ORIGIN/lib/aarch64")

# ==============================================================================
# 编码引擎选项：默认不开启硬件加速
# 编译软编版本可使用: cmake -DUSE_HW_ENCODER=OFF ..
# ==============================================================================
option(USE_HW_ENCODER "Enable Allwinner OMX Hardware Encoder" ON)

if(USE_HW_ENCODER)
    message(STATUS ">>> Video Engine: OpenMAX Hardware Encoding (omxh264videoenc) <<<")
    add_definitions(-DUSE_HARDWARE_ENCODER)
else()
    message(STATUS ">>> Video Engine: CPU Software Encoding (x264enc) <<<")
    # 如果用软编，可能需要额外链接 x264 插件，但通常 gst-plugins-ugly 已经包含了
endif()

# ==============================================================================
# 2. 查找双擎依赖：FFmpeg (用于封装落盘) + GStreamer (用于硬件硬编)
# ==============================================================================
find_package(PkgConfig REQUIRED)

# 留着 FFmpeg 给你的 StorageWorker 写 MP4 用
pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libavformat)

# 新增 GStreamer 给你的 EncodeWorker 榨干硬件用
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0)

find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)
find_package(GLEW REQUIRED)

# ==============================================================================
# 3. 设置头文件包含路径
# ==============================================================================
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
    ${FFMPEG_INCLUDE_DIRS}  # FFmpeg 头文件
    ${GST_INCLUDE_DIRS}     # GStreamer 头文件
    ${OpenCV_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
)

file(GLOB_RECURSE SRC_FILES "src/*.cpp")
add_executable(${PROJECT_NAME} ${SRC_FILES})

# ==============================================================================
# 4. 设置耀宇 Carina 本地预编译库绝对路径
# ==============================================================================
set(CARINA_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/aarch64)
macro(find_carina_lib OUT_VAR LIB_NAME)
    if(EXISTS "${CARINA_LIB_DIR}/lib${LIB_NAME}.so")
        set(${OUT_VAR} "${CARINA_LIB_DIR}/lib${LIB_NAME}.so")
    elseif(EXISTS "${CARINA_LIB_DIR}/lib${LIB_NAME}.a")
        set(${OUT_VAR} "${CARINA_LIB_DIR}/lib${LIB_NAME}.a")
    else()
        message(FATAL_ERROR "Critical Error: Cannot find lib${LIB_NAME} in ${CARINA_LIB_DIR}")
    endif()
endmacro()

find_carina_lib(LIB_CARINA_VIO carina_vio)
find_carina_lib(LIB_CARINA_VIO_CLIENT carina_vio_client)
find_carina_lib(LIB_CARINA_CLIENT carina_client)
find_carina_lib(LIB_CLAHE clahe)
find_carina_lib(LIB_DBOW2 DBoW2)
find_carina_lib(LIB_G2O g2o)
find_carina_lib(LIB_LK lk)
find_carina_lib(LIB_TRACKER tracker)
find_carina_lib(LIB_YAML_CPP yaml-cpp)
find_carina_lib(LIB_FPGA_ORB fpga_orb)

# ==============================================================================
# 测试 Demo 配置
# ==============================================================================
add_executable(test_padding test/test_padding.cpp)
target_link_libraries(test_padding PRIVATE
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    ${GST_LIBRARIES}
    ${OpenCV_LIBS}
    GLEW::GLEW
    # 这一大串必须与 VioDataLogger 的链接库完全一致，缺一不可
    ${LIB_CARINA_VIO}
    ${LIB_CARINA_VIO_CLIENT}
    ${LIB_CARINA_CLIENT}
    ${LIB_CLAHE}
    ${LIB_DBOW2}
    ${LIB_G2O}
    ${LIB_LK}
    ${LIB_TRACKER}
    ${LIB_YAML_CPP}
    ${LIB_FPGA_ORB}
)

# ==============================================================================
# 5. 链接所有动态/静态库
# ==============================================================================
target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
    ${FFMPEG_LIBRARIES}     # 链接 FFmpeg 库
    ${GST_LIBRARIES}        # 链接 GStreamer 库
    ${OpenCV_LIBS}          
    GLEW::GLEW              

    ${LIB_CARINA_VIO}
    ${LIB_CARINA_VIO_CLIENT}
    ${LIB_CARINA_CLIENT}
    ${LIB_CLAHE}
    ${LIB_DBOW2}
    ${LIB_G2O}
    ${LIB_LK}
    ${LIB_TRACKER}
    ${LIB_YAML_CPP}
    ${LIB_FPGA_ORB}
)
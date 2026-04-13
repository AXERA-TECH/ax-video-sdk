cmake_minimum_required(VERSION 3.16)

project(ax_video_sdk LANGUAGES CXX)

find_package(Threads REQUIRED)

set(AXSDK_CHIP_TYPE "" CACHE STRING "Target chip type: ax630c, ax620q, ax620qp, ax650, axcl")
set(AXSDK_MSP_DIR "" CACHE PATH "Path to target MSP root, for example /home/axera/msp_dir/ax620e/msp")
set(AXSDK_AXCL_DIR "" CACHE PATH "Path to AXCL SDK root, for example /opt/axcl or /usr")
option(AXSDK_BUILD_SHARED "Build SDK libraries as shared objects so consumers do not need MSP link flags" ON)
option(AXSDK_BUILD_TOOLS "Build internal tools such as demux probes" OFF)
option(AXSDK_BUILD_SMOKE_TESTS "Build internal smoke tests" OFF)

set(AXSDK_COMMON_LIBS "")
set(AXSDK_MSP_INCLUDE_DIR "")
set(AXSDK_MSP_LIB_DIR "")
set(AXSDK_AX_RUNTIME_NAME "AX MSP")

if(NOT AXSDK_CHIP_TYPE)
    message(FATAL_ERROR "This project is AX-only. Set AXSDK_CHIP_TYPE.")
endif()

string(TOLOWER "${AXSDK_CHIP_TYPE}" AXSDK_CHIP_TYPE_NORMALIZED)

if(NOT AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "axcl" AND NOT AXSDK_MSP_DIR)
    message(FATAL_ERROR "Non-AXCL build requires AXSDK_MSP_DIR.")
endif()

if(AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "axcl")
    set(AXSDK_AXCL_HOST_ARCH "")
    if(AXSDK_AXCL_DIR)
        set(AXSDK_AXCL_ROOT "${AXSDK_AXCL_DIR}")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(AXSDK_AXCL_ROOT "/usr")
        set(AXSDK_AXCL_HOST_ARCH "x86")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(AXSDK_AXCL_ROOT "/usr")
        set(AXSDK_AXCL_HOST_ARCH "arm64")
    endif()

    if(NOT AXSDK_AXCL_ROOT)
        message(FATAL_ERROR "AXCL build requires AXSDK_AXCL_DIR or a supported host architecture.")
    endif()

    if(EXISTS "${AXSDK_AXCL_ROOT}/include/axcl/axcl.h")
        set(AXSDK_MSP_INCLUDE_DIR "${AXSDK_AXCL_ROOT}/include/axcl")
    elseif(EXISTS "${AXSDK_AXCL_ROOT}/include/axcl.h")
        set(AXSDK_MSP_INCLUDE_DIR "${AXSDK_AXCL_ROOT}/include")
    else()
        message(FATAL_ERROR "AXCL include directory not found under ${AXSDK_AXCL_ROOT}")
    endif()

    if(EXISTS "${AXSDK_AXCL_ROOT}/lib/axcl/libaxcl_sys.so")
        set(AXSDK_MSP_LIB_DIR "${AXSDK_AXCL_ROOT}/lib/axcl")
    elseif(EXISTS "${AXSDK_AXCL_ROOT}/lib/libaxcl_sys.so")
        set(AXSDK_MSP_LIB_DIR "${AXSDK_AXCL_ROOT}/lib")
    elseif(EXISTS "${AXSDK_AXCL_ROOT}/lib64/libaxcl_sys.so")
        set(AXSDK_MSP_LIB_DIR "${AXSDK_AXCL_ROOT}/lib64")
    else()
        message(FATAL_ERROR "AXCL library directory not found under ${AXSDK_AXCL_ROOT}")
    endif()

    set(AXSDK_CHIP_DEFINE AXSDK_PLATFORM_AXCL=1)
    set(AXSDK_COMMON_LIBS axcl_rt axcl_sys axcl_vdec axcl_venc axcl_ivps)
    set(AXSDK_AX_RUNTIME_NAME "AXCL")
elseif(AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "ax630c")
    set(AXSDK_MSP_INCLUDE_DIR "${AXSDK_MSP_DIR}/out/arm64_glibc/include")
    set(AXSDK_MSP_LIB_DIR "${AXSDK_MSP_DIR}/out/arm64_glibc/lib")
    set(AXSDK_CHIP_DEFINE AXSDK_CHIP_AX620E_FAMILY=1)
elseif(AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "ax620q")
    set(AXSDK_MSP_INCLUDE_DIR "${AXSDK_MSP_DIR}/out/arm_uclibc/include")
    set(AXSDK_MSP_LIB_DIR "${AXSDK_MSP_DIR}/out/arm_uclibc/lib")
    set(AXSDK_CHIP_DEFINE AXSDK_CHIP_AX620E_FAMILY=1)
elseif(AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "ax620qp")
    set(AXSDK_MSP_INCLUDE_DIR "${AXSDK_MSP_DIR}/out/arm_glibc/include")
    set(AXSDK_MSP_LIB_DIR "${AXSDK_MSP_DIR}/out/arm_glibc/lib")
    set(AXSDK_CHIP_DEFINE AXSDK_CHIP_AX620E_FAMILY=1)
elseif(AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "ax650")
    set(AXSDK_MSP_INCLUDE_DIR "${AXSDK_MSP_DIR}/out/include")
    set(AXSDK_MSP_LIB_DIR "${AXSDK_MSP_DIR}/out/lib")
    set(AXSDK_CHIP_DEFINE AXSDK_CHIP_AX650=1)
else()
    message(FATAL_ERROR "Unsupported AXSDK_CHIP_TYPE: ${AXSDK_CHIP_TYPE}")
endif()

if(NOT EXISTS "${AXSDK_MSP_INCLUDE_DIR}")
    message(FATAL_ERROR "AX SDK include directory does not exist: ${AXSDK_MSP_INCLUDE_DIR}")
endif()
if(NOT EXISTS "${AXSDK_MSP_LIB_DIR}")
    message(FATAL_ERROR "AX SDK library directory does not exist: ${AXSDK_MSP_LIB_DIR}")
endif()

if(NOT AXSDK_COMMON_LIBS)
    set(AXSDK_COMMON_LIBS ax_sys ax_vdec ax_venc ax_ivps)
endif()
set(AXSDK_LIBRARY_TYPE STATIC)
if(AXSDK_BUILD_SHARED)
    set(AXSDK_LIBRARY_TYPE SHARED)
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(NOT TARGET rtsp-sdk)
    add_subdirectory(third-party/rtsp-sdk EXCLUDE_FROM_ALL)
endif()

add_library(ax_video_sdk_ax_msp INTERFACE)
add_library(ax_video_sdk::ax_msp ALIAS ax_video_sdk_ax_msp)

target_include_directories(ax_video_sdk_ax_msp
    INTERFACE
        ${AXSDK_MSP_INCLUDE_DIR}
)

target_link_directories(ax_video_sdk_ax_msp
    INTERFACE
        ${AXSDK_MSP_LIB_DIR}
)

target_compile_definitions(ax_video_sdk_ax_msp
    INTERFACE
        ${AXSDK_CHIP_DEFINE}
)

message(STATUS "${AXSDK_AX_RUNTIME_NAME} enabled for ${AXSDK_CHIP_TYPE_NORMALIZED}")
message(STATUS "AX SDK includes: ${AXSDK_MSP_INCLUDE_DIR}")
message(STATUS "AX SDK libs: ${AXSDK_MSP_LIB_DIR}")

add_library(ax_video_sdk ${AXSDK_LIBRARY_TYPE}
    src/codec/ax_jpeg_codec.cpp
    src/common/ax_drawer.cpp
    src/common/ax_image.cpp
    src/common/ax_image_copy.cpp
    src/common/ax_image_processor.cpp
    src/common/ax_system.cpp
    src/codec/ax_mp4_demuxer.cpp
    src/codec/ax_video_decoder.cpp
    src/codec/ax_video_encoder.cpp
    src/pipeline/ax_demuxer.cpp
    src/pipeline/ax_muxer.cpp
    src/pipeline/ax_pipeline.cpp
    src/pipeline/ax_pipeline_manager.cpp
    src/pipeline/ax_v4l2_capture.cpp
    src/pipeline/ax_camera_pipeline.cpp
)

if(AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "ax650")
    target_sources(ax_video_sdk
        PRIVATE
            src/common/ax_image_processor_ax650.cpp
            src/codec/ax_video_decoder_ax650.cpp
            src/codec/ax_video_encoder_ax650.cpp
            src/pipeline/ax_pipeline_osd_ax650.cpp
    )
elseif(AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "axcl")
    target_sources(ax_video_sdk
        PRIVATE
            src/common/ax_image_processor_axcl.cpp
            src/codec/ax_video_decoder_axcl.cpp
            src/codec/ax_video_encoder_axcl.cpp
            src/pipeline/ax_pipeline_osd_axcl.cpp
    )
else()
    target_sources(ax_video_sdk
        PRIVATE
            src/common/ax_image_processor_ax620e.cpp
            src/codec/ax_video_decoder_ax620e.cpp
            src/codec/ax_video_encoder_ax620e.cpp
            src/pipeline/ax_pipeline_osd_ax620e.cpp
    )
endif()

add_library(ax_video_sdk::ax_video_sdk ALIAS ax_video_sdk)

target_compile_features(ax_video_sdk PUBLIC cxx_std_17)
set_target_properties(ax_video_sdk PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(ax_video_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codec
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pipeline
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party
)

target_link_libraries(ax_video_sdk
    PUBLIC
        Threads::Threads
    PRIVATE
        ax_video_sdk::ax_msp
        rtsp-sdk
        ${AXSDK_COMMON_LIBS}
)

if(AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "axcl" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_options(ax_video_sdk PRIVATE -Wl,--allow-shlib-undefined)
endif()

function(axsdk_configure_internal_tool target)
    set(options LINK_AX_MSP LINK_RTSP)
    set(oneValueArgs "")
    set(multiValueArgs EXTRA_INCLUDE_DIRS EXTRA_LIBRARIES)
    cmake_parse_arguments(AXSDK_TOOL "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    target_link_libraries(${target}
        PRIVATE
            ax_video_sdk::ax_video_sdk
            ${AXSDK_TOOL_EXTRA_LIBRARIES}
    )

    if(AXSDK_TOOL_LINK_AX_MSP)
        target_link_libraries(${target} PRIVATE ax_video_sdk::ax_msp)
    endif()

    if(AXSDK_TOOL_LINK_RTSP)
        target_link_libraries(${target} PRIVATE rtsp-sdk)
    endif()

    target_include_directories(${target}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/common
            ${CMAKE_CURRENT_SOURCE_DIR}/src/codec
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pipeline
            ${CMAKE_CURRENT_SOURCE_DIR}/third-party
            ${AXSDK_TOOL_EXTRA_INCLUDE_DIRS}
    )

    target_link_directories(${target}
        PRIVATE
            ${AXSDK_MSP_LIB_DIR}
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(${target} PRIVATE -Wl,--allow-shlib-undefined)
    endif()
endfunction()

if(AXSDK_BUILD_TOOLS)
    add_executable(ax_mp4_demux_probe
        tools/ax_mp4_demux_probe.cpp
    )

    axsdk_configure_internal_tool(ax_mp4_demux_probe)
endif()

if(AXSDK_BUILD_SMOKE_TESTS)
    add_executable(ax_decoder_smoke
        tools/ax_decoder_smoke.cpp
    )
    # Expose MSP headers to internal smoke tool for debug-only frame metadata access.
    axsdk_configure_internal_tool(ax_decoder_smoke LINK_AX_MSP)

    add_executable(ax_transcode_smoke
        tools/ax_transcode_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_transcode_smoke)

    add_executable(ax_pipeline_smoke
        tools/ax_pipeline_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_pipeline_smoke)

    add_executable(ax_image_processor_smoke
        tools/ax_image_processor_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_image_processor_smoke)

    add_executable(ax_image_processor_dump
        tools/ax_image_processor_dump.cpp
    )
    axsdk_configure_internal_tool(ax_image_processor_dump)

    if(NOT AXSDK_CHIP_TYPE_NORMALIZED STREQUAL "axcl")
        add_executable(ax_system_probe
            tools/ax_system_probe.cpp
        )
        # This tool calls AX_* symbols directly (MSP APIs), so it is only meaningful on SoCs.
        axsdk_configure_internal_tool(ax_system_probe LINK_AX_MSP EXTRA_LIBRARIES ${AXSDK_COMMON_LIBS})
    endif()

    add_executable(ax_jpeg_smoke
        tools/ax_jpeg_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_jpeg_smoke)

    add_executable(ax_image_copy_smoke
        tools/ax_image_copy_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_image_copy_smoke
        LINK_AX_MSP
        EXTRA_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    )

    add_executable(ax_osd_draw_smoke
        tools/ax_osd_draw_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_osd_draw_smoke
        EXTRA_INCLUDE_DIRS
            ${CMAKE_CURRENT_SOURCE_DIR}/src/common
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pipeline
    )

    add_executable(ax_pipeline_manager_smoke
        tools/ax_pipeline_manager_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_pipeline_manager_smoke)

    add_executable(ax_pipeline_frame_smoke
        tools/ax_pipeline_frame_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_pipeline_frame_smoke
        LINK_AX_MSP
        EXTRA_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    )

    add_executable(ax_pipeline_osd_smoke
        tools/ax_pipeline_osd_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_pipeline_osd_smoke
        LINK_AX_MSP
        EXTRA_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    )

    add_executable(ax_pipeline_stress_smoke
        tools/ax_pipeline_stress_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_pipeline_stress_smoke)

    add_executable(ax_rtsp_transcode_smoke
        tools/ax_rtsp_transcode_smoke.cpp
    )
    axsdk_configure_internal_tool(ax_rtsp_transcode_smoke LINK_RTSP)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ax_video_sdk PRIVATE -Wall -Wextra -Wpedantic)
endif()

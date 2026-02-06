cmake_minimum_required(VERSION 3.22.1)

project("steamlauncher")

# Find required libraries
find_library(log-lib log)
find_library(android-lib android)

# Minimal native library (X11 handled by Termux:X11)
add_library(steamlauncher SHARED steamlauncher.cpp)
target_link_libraries(steamlauncher ${log-lib})

# FramebufferBridge - HardwareBuffer management for Vortek
add_library(framebuffer_bridge SHARED framebuffer_bridge.cpp)
target_link_libraries(
    framebuffer_bridge
    ${log-lib}
    ${android-lib}
    nativewindow
)
set_target_properties(framebuffer_bridge PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# X11 Socket Helper - creates Unix domain sockets for X11 clients
add_library(x11socket SHARED x11_socket.cpp)
target_link_libraries(x11socket ${log-lib})

# ============================================================
# unsquashfs - SquashFS extraction tool for FEX rootfs setup
# Built as an executable (packaged as libunsquashfs.so by NDK)
# ============================================================
# To build: place squashfs-tools source in vendor/squashfs-tools/
# and xz-utils source in vendor/xz-utils/
# Run: scripts/fetch_unsquashfs_source.sh to download sources
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/squashfs-tools/unsquashfs.c")
    # liblzma from xz-utils (for XZ-compressed SquashFS)
    set(XZ_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xz-utils/src/liblzma")
    set(XZ_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xz-utils/src/common")
    if(EXISTS "${XZ_SRC_DIR}")
        set(LZMA_SOURCES
            # common
            ${XZ_SRC_DIR}/common/common.c
            ${XZ_SRC_DIR}/common/block_util.c
            ${XZ_SRC_DIR}/common/easy_preset.c
            ${XZ_SRC_DIR}/common/filter_common.c
            ${XZ_SRC_DIR}/common/hardware_physmem.c
            ${XZ_SRC_DIR}/common/hardware_cputhreads.c
            ${XZ_SRC_DIR}/common/index.c
            ${XZ_SRC_DIR}/common/stream_flags_common.c
            ${XZ_SRC_DIR}/common/string_conversion.c
            ${XZ_SRC_DIR}/common/vli_size.c
            ${XZ_SRC_DIR}/common/outqueue.c
            # decoder
            ${XZ_SRC_DIR}/common/alone_decoder.c
            ${XZ_SRC_DIR}/common/auto_decoder.c
            ${XZ_SRC_DIR}/common/block_buffer_decoder.c
            ${XZ_SRC_DIR}/common/block_decoder.c
            ${XZ_SRC_DIR}/common/block_header_decoder.c
            ${XZ_SRC_DIR}/common/easy_decoder_memusage.c
            ${XZ_SRC_DIR}/common/file_info.c
            ${XZ_SRC_DIR}/common/filter_buffer_decoder.c
            ${XZ_SRC_DIR}/common/filter_decoder.c
            ${XZ_SRC_DIR}/common/filter_flags_decoder.c
            ${XZ_SRC_DIR}/common/index_decoder.c
            ${XZ_SRC_DIR}/common/index_hash.c
            ${XZ_SRC_DIR}/common/stream_buffer_decoder.c
            ${XZ_SRC_DIR}/common/stream_decoder.c
            ${XZ_SRC_DIR}/common/stream_decoder_mt.c
            ${XZ_SRC_DIR}/common/stream_flags_decoder.c
            ${XZ_SRC_DIR}/common/vli_decoder.c
            ${XZ_SRC_DIR}/common/lzip_decoder.c
            ${XZ_SRC_DIR}/common/microlzma_decoder.c
            # encoder (needed for index/stream encoding used during decompression)
            ${XZ_SRC_DIR}/common/alone_encoder.c
            ${XZ_SRC_DIR}/common/block_buffer_encoder.c
            ${XZ_SRC_DIR}/common/block_encoder.c
            ${XZ_SRC_DIR}/common/block_header_encoder.c
            ${XZ_SRC_DIR}/common/easy_buffer_encoder.c
            ${XZ_SRC_DIR}/common/easy_encoder.c
            ${XZ_SRC_DIR}/common/easy_encoder_memusage.c
            ${XZ_SRC_DIR}/common/filter_buffer_encoder.c
            ${XZ_SRC_DIR}/common/filter_encoder.c
            ${XZ_SRC_DIR}/common/filter_flags_encoder.c
            ${XZ_SRC_DIR}/common/index_encoder.c
            ${XZ_SRC_DIR}/common/stream_buffer_encoder.c
            ${XZ_SRC_DIR}/common/stream_encoder.c
            ${XZ_SRC_DIR}/common/stream_encoder_mt.c
            ${XZ_SRC_DIR}/common/stream_flags_encoder.c
            ${XZ_SRC_DIR}/common/vli_encoder.c
            ${XZ_SRC_DIR}/common/microlzma_encoder.c
            # check
            ${XZ_SRC_DIR}/check/check.c
            ${XZ_SRC_DIR}/check/crc32_fast.c
            ${XZ_SRC_DIR}/check/crc32_table.c
            ${XZ_SRC_DIR}/check/crc64_fast.c
            ${XZ_SRC_DIR}/check/crc64_table.c
            ${XZ_SRC_DIR}/check/sha256.c
            # lz
            ${XZ_SRC_DIR}/lz/lz_decoder.c
            ${XZ_SRC_DIR}/lz/lz_encoder.c
            ${XZ_SRC_DIR}/lz/lz_encoder_mf.c
            # lzma
            ${XZ_SRC_DIR}/lzma/lzma_decoder.c
            ${XZ_SRC_DIR}/lzma/lzma_encoder.c
            ${XZ_SRC_DIR}/lzma/lzma_encoder_presets.c
            ${XZ_SRC_DIR}/lzma/lzma_encoder_optimum_fast.c
            ${XZ_SRC_DIR}/lzma/lzma_encoder_optimum_normal.c
            ${XZ_SRC_DIR}/lzma/lzma2_decoder.c
            ${XZ_SRC_DIR}/lzma/lzma2_encoder.c
            ${XZ_SRC_DIR}/lzma/fastpos_table.c
            # delta
            ${XZ_SRC_DIR}/delta/delta_common.c
            ${XZ_SRC_DIR}/delta/delta_decoder.c
            ${XZ_SRC_DIR}/delta/delta_encoder.c
            # rangecoder
            ${XZ_SRC_DIR}/rangecoder/price_table.c
            # simple filters
            ${XZ_SRC_DIR}/simple/simple_coder.c
            ${XZ_SRC_DIR}/simple/simple_decoder.c
            ${XZ_SRC_DIR}/simple/simple_encoder.c
            ${XZ_SRC_DIR}/simple/x86.c
            ${XZ_SRC_DIR}/simple/arm.c
            ${XZ_SRC_DIR}/simple/arm64.c
            ${XZ_SRC_DIR}/simple/armthumb.c
            ${XZ_SRC_DIR}/simple/powerpc.c
            ${XZ_SRC_DIR}/simple/ia64.c
            ${XZ_SRC_DIR}/simple/sparc.c
            # tuklib helpers
            ${XZ_COMMON_DIR}/tuklib_physmem.c
            ${XZ_COMMON_DIR}/tuklib_cpucores.c
        )
        add_library(lzma_static STATIC ${LZMA_SOURCES})
        target_include_directories(lzma_static PUBLIC
            "${XZ_SRC_DIR}/api"
            "${XZ_SRC_DIR}/common"
            "${XZ_SRC_DIR}/check"
            "${XZ_SRC_DIR}/lz"
            "${XZ_SRC_DIR}/lzma"
            "${XZ_SRC_DIR}/delta"
            "${XZ_SRC_DIR}/rangecoder"
            "${XZ_SRC_DIR}/simple"
            "${XZ_COMMON_DIR}"
        )
        # Use our Android-specific config.h (in vendor/xz-config/)
        target_include_directories(lzma_static PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xz-config"
        )
        target_compile_definitions(lzma_static PRIVATE HAVE_CONFIG_H)
    endif()

    # libzstd from facebook/zstd (for ZSTD-compressed SquashFS)
    set(ZSTD_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zstd/lib")
    if(EXISTS "${ZSTD_SRC_DIR}")
        set(ZSTD_SOURCES
            ${ZSTD_SRC_DIR}/common/debug.c
            ${ZSTD_SRC_DIR}/common/entropy_common.c
            ${ZSTD_SRC_DIR}/common/error_private.c
            ${ZSTD_SRC_DIR}/common/fse_decompress.c
            ${ZSTD_SRC_DIR}/common/pool.c
            ${ZSTD_SRC_DIR}/common/threading.c
            ${ZSTD_SRC_DIR}/common/xxhash.c
            ${ZSTD_SRC_DIR}/common/zstd_common.c
            ${ZSTD_SRC_DIR}/compress/fse_compress.c
            ${ZSTD_SRC_DIR}/compress/hist.c
            ${ZSTD_SRC_DIR}/compress/huf_compress.c
            ${ZSTD_SRC_DIR}/compress/zstd_compress.c
            ${ZSTD_SRC_DIR}/compress/zstd_compress_literals.c
            ${ZSTD_SRC_DIR}/compress/zstd_compress_sequences.c
            ${ZSTD_SRC_DIR}/compress/zstd_compress_superblock.c
            ${ZSTD_SRC_DIR}/compress/zstd_double_fast.c
            ${ZSTD_SRC_DIR}/compress/zstd_fast.c
            ${ZSTD_SRC_DIR}/compress/zstd_lazy.c
            ${ZSTD_SRC_DIR}/compress/zstd_ldm.c
            ${ZSTD_SRC_DIR}/compress/zstdmt_compress.c
            ${ZSTD_SRC_DIR}/compress/zstd_opt.c
            ${ZSTD_SRC_DIR}/decompress/huf_decompress.c
            ${ZSTD_SRC_DIR}/decompress/zstd_ddict.c
            ${ZSTD_SRC_DIR}/decompress/zstd_decompress_block.c
            ${ZSTD_SRC_DIR}/decompress/zstd_decompress.c
        )
        add_library(zstd_static STATIC ${ZSTD_SOURCES})
        target_include_directories(zstd_static PUBLIC "${ZSTD_SRC_DIR}")
        target_compile_definitions(zstd_static PRIVATE
            ZSTD_MULTITHREAD
            XXH_NAMESPACE=ZSTD_
        )
    endif()

    # unsquashfs sources — from squashfs-tools Makefile UNSQUASHFS_OBJS
    # No xattr support on Android (no libattr)
    set(SQFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/squashfs-tools")
    set(UNSQUASHFS_SOURCES
        ${SQFS_DIR}/unsquashfs.c
        ${SQFS_DIR}/unsquash-1.c
        ${SQFS_DIR}/unsquash-2.c
        ${SQFS_DIR}/unsquash-3.c
        ${SQFS_DIR}/unsquash-4.c
        ${SQFS_DIR}/unsquash-123.c
        ${SQFS_DIR}/unsquash-34.c
        ${SQFS_DIR}/unsquash-1234.c
        ${SQFS_DIR}/unsquash-12.c
        ${SQFS_DIR}/swap.c
        ${SQFS_DIR}/compressor.c
        ${SQFS_DIR}/unsquashfs_info.c
        ${SQFS_DIR}/date.c
        ${SQFS_DIR}/gzip_wrapper.c
        ${SQFS_DIR}/xz_wrapper.c
        ${SQFS_DIR}/zstd_wrapper.c
    )

    add_executable(unsquashfs ${UNSQUASHFS_SOURCES})
    target_include_directories(unsquashfs PRIVATE ${SQFS_DIR})
    target_compile_definitions(unsquashfs PRIVATE
        _GNU_SOURCE
        GZIP_SUPPORT
        XZ_SUPPORT
        ZSTD_SUPPORT
        REPRODUCIBLE_DEFAULT
        _FILE_OFFSET_BITS=64
        _LARGEFILE_SOURCE
        VERSION="4.6.1"
        DATE="2023/03/12"
        YEAR="2023"
    )
    # Suppress -Werror for implicit function declarations (lutimes on Android)
    target_compile_options(unsquashfs PRIVATE
        -Wno-error=implicit-function-declaration
        -Wno-implicit-function-declaration
    )
    target_link_libraries(unsquashfs z)
    if(TARGET lzma_static)
        target_link_libraries(unsquashfs lzma_static)
    endif()
    if(TARGET zstd_static)
        target_link_libraries(unsquashfs zstd_static)
    endif()
    # Android Bionic includes pthreads in libc — no separate -lpthread needed

    # NDK requires executables to be named lib*.so to be packaged
    set_target_properties(unsquashfs PROPERTIES
        OUTPUT_NAME "unsquashfs"
        SUFFIX ".so"
        PREFIX "lib"
    )
else()
    message(STATUS "squashfs-tools source not found in vendor/. Run scripts/fetch_unsquashfs_source.sh")
endif()

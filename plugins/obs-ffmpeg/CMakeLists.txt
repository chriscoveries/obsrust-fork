cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_FFMPEG_LOGGING "Enables obs-ffmpeg logging" OFF)
option(ENABLE_NEW_MPEGTS_OUTPUT "Use native SRT/RIST mpegts output" ON)

if(OS_LINUX OR OS_WINDOWS)
  option(ENABLE_FFMPEG_NVENC "Enable legacy FFmpeg NVENC encoder" OFF)
endif()

include(cmake/dependencies.cmake)

add_library(obs-ffmpeg MODULE)
add_library(OBS::ffmpeg ALIAS obs-ffmpeg)

add_subdirectory(ffmpeg-mux)

target_sources(
  obs-ffmpeg
  PRIVATE
    $<$<BOOL:${ENABLE_FFMPEG_LOGGING}>:build/obs-ffmpeg.logging+.h.c>
    $<$<BOOL:${ENABLE_FFMPEG_NVENC}>:build/obs-ffmpeg.nvenc+.h.c>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:build/obs-ffmpeg.mpegts+.h.c>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-rist.h>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-srt.h>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:obs-ffmpeg-url.h>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:build/obs-ffmpeg.vaapi+.h.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:build/obs-ffmpeg.vaapi-device-select+.h.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-device-select.h>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:build/obs-ffmpeg.vaapi-probe-policy+.h.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-probe-policy.h>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-utils.h>
    build/obs-ffmpeg.core+.h.c
    build/obs-ffmpeg.mux+.h.c
    build/obs-ffmpeg.output+.h.c
    build/obs-ffmpeg.source+.h.c
    build/obs-ffmpeg.video-encoders+.h.c
    obs-ffmpeg-compat.h
    obs-ffmpeg-formats.h
    obs-ffmpeg-mux.h
    obs-ffmpeg-output.h
)

target_compile_options(obs-ffmpeg PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-shorten-64-to-32>)
target_compile_definitions(
  obs-ffmpeg
  PRIVATE
    $<$<BOOL:${ENABLE_FFMPEG_LOGGING}>:ENABLE_FFMPEG_LOGGING>
    $<$<BOOL:${ENABLE_FFMPEG_NVENC}>:ENABLE_FFMPEG_NVENC>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:NEW_MPEGTS_OUTPUT>
)

target_link_libraries(
  obs-ffmpeg
  PRIVATE
    OBS::libobs
    OBS::media-playback
    OBS::opts-parser
    FFmpeg::avcodec
    FFmpeg::avfilter
    FFmpeg::avformat
    FFmpeg::avdevice
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libva::va>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libva::drm>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libpci::pci>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libdrm::Libdrm>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:Librist::Librist>
    $<$<BOOL:${ENABLE_NEW_MPEGTS_OUTPUT}>:Libsrt::Libsrt>
)

if(OS_WINDOWS)
  if(CMAKE_VS_PLATFORM_NAME STREQUAL x64)
    target_sources(obs-ffmpeg PRIVATE texture-amf-opts.hpp texture-amf.cpp)
    target_link_libraries(obs-ffmpeg PRIVATE AMF::AMF)
  endif()

  configure_file(cmake/windows/obs-module.rc.in obs-ffmpeg.rc)
  target_sources(obs-ffmpeg PRIVATE obs-ffmpeg.rc)
endif()

set_target_properties_obs(obs-ffmpeg PROPERTIES FOLDER plugins/obs-ffmpeg PREFIX "")

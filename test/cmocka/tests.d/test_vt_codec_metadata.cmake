# VideoToolbox keeps the HLG-compatible HDR configuration record (the dvvC
# atom of the first keyframe's format description) and returns it through
# get_codec_metadata. Includes the whole encoder; samples are built from a real
# encode's hvcC and record, so no compression session is created.
if(OS_MACOS)
  add_executable(test_vt_codec_metadata test_vt_codec_metadata.c)
  target_link_libraries(
    test_vt_codec_metadata
    PRIVATE
      OBS::libobs
      "-framework CoreFoundation"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework VideoToolbox"
  )
  target_compile_options(test_vt_codec_metadata PRIVATE -fblocks)
  target_compile_definitions(test_vt_codec_metadata PRIVATE $<$<BOOL:${ENABLE_HEVC}>:ENABLE_HEVC>)
  add_dependencies(cmocka-tests test_vt_codec_metadata)
  add_test(NAME test_vt_codec_metadata COMMAND test_vt_codec_metadata)
endif()

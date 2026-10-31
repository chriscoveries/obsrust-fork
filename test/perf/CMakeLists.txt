# Windowless perf harnesses (Apple only for now). These are CLI measurement
# tools, not cmocka unit tests: they need full builds plus real GPU/media
# frameworks. macOS-only until a second platform needs one.

if(APPLE)
  add_executable(zero-copy-harness zero-copy-harness.c)

  target_link_libraries(zero-copy-harness PRIVATE OBS::libobs)

  set_target_properties(zero-copy-harness PROPERTIES FOLDER "tests and examples")
endif()

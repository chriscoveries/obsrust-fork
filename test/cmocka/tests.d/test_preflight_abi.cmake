if(OS_LINUX OR OS_MACOS)
  if(TARGET obs-preflight-rust)
    # Frontend builds already define this target in cmake/preflight-rust.cmake.
    set(_preflight_archive "${OBS_PREFLIGHT_ARCHIVE}")
  else()
    # Windows is excluded: linking a Rust static library there needs extra
    # system libraries that this test does not set up.
    find_program(SOLDR_EXECUTABLE soldr)
    find_program(RUSTUP_EXECUTABLE rustup)

    set(_preflight_cargo_command)
    set(_preflight_cargo_environment)
    set(_preflight_rustc_executable)
    if(RUSTUP_EXECUTABLE)
      # Use the pinned toolchain's real Cargo binary instead of the Rustup proxy.
      # Corrosion uses the same toolchain from the workspace's rust-toolchain.toml.
      execute_process(
        COMMAND "${RUSTUP_EXECUTABLE}" which cargo
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rustup_which_cargo_result
        OUTPUT_VARIABLE _rustup_which_cargo
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(_rustup_which_cargo_result EQUAL 0 AND EXISTS "${_rustup_which_cargo}")
        list(APPEND _preflight_cargo_command "${_rustup_which_cargo}")
      endif()

      execute_process(
        COMMAND "${RUSTUP_EXECUTABLE}" which rustc
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rustup_which_rustc_result
        OUTPUT_VARIABLE _rustup_which_rustc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(_rustup_which_rustc_result EQUAL 0 AND EXISTS "${_rustup_which_rustc}")
        set(_preflight_rustc_executable "${_rustup_which_rustc}")
      endif()
    endif()

    if(NOT _preflight_rustc_executable AND Rust_CARGO_CACHED)
      get_filename_component(_preflight_toolchain_bin "${Rust_CARGO_CACHED}" DIRECTORY)
      if(EXISTS "${_preflight_toolchain_bin}/rustc")
        set(_preflight_rustc_executable "${_preflight_toolchain_bin}/rustc")
      endif()
    endif()

    if(NOT _preflight_cargo_command AND SOLDR_EXECUTABLE)
      list(APPEND _preflight_cargo_command "${SOLDR_EXECUTABLE}" cargo)
    elseif(NOT _preflight_cargo_command AND TARGET Rust::Cargo)
      # Reuse the cargo executable Corrosion found for libobs's Rust build.
      get_target_property(_preflight_cargo_executable Rust::Cargo IMPORTED_LOCATION)
      if(_preflight_cargo_executable AND NOT _preflight_cargo_executable MATCHES "-NOTFOUND$")
        list(APPEND _preflight_cargo_command "${_preflight_cargo_executable}")
      endif()
    endif()

    if(NOT _preflight_cargo_command)
      message(STATUS "Skipping test_preflight_abi: no usable Cargo executable was found via rustup, soldr or Corrosion")
      return()
    endif()

    set(_preflight_manifest "${CMAKE_SOURCE_DIR}/rust/obs-preflight-ffi/Cargo.toml")
    set(_preflight_target_dir "${CMAKE_BINARY_DIR}/obs-preflight-cargo")
    list(APPEND _preflight_cargo_environment "CARGO_TARGET_DIR=${_preflight_target_dir}")
    if(_preflight_rustc_executable)
      # Cargo otherwise finds rustc's Rustup proxy on PATH. Concurrent proxy
      # calls can race while synchronizing components from rust-toolchain.toml.
      list(APPEND _preflight_cargo_environment "RUSTC=${_preflight_rustc_executable}")
    endif()
    file(
      GLOB_RECURSE _preflight_rust_sources
      CONFIGURE_DEPENDS
      "${CMAKE_SOURCE_DIR}/rust/obs-preflight/src/*.rs"
      "${CMAKE_SOURCE_DIR}/rust/obs-preflight-ffi/src/*.rs"
    )
    set(
      _preflight_archive
      "${_preflight_target_dir}/release/${CMAKE_STATIC_LIBRARY_PREFIX}obs_preflight_ffi${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_custom_command(
      OUTPUT "${_preflight_archive}"
      COMMAND
        "${CMAKE_COMMAND}" -E env ${_preflight_cargo_environment} ${_preflight_cargo_command} build --manifest-path
        "${CMAKE_SOURCE_DIR}/Cargo.toml" --package obs-preflight-ffi --lib --release --locked
      DEPENDS
        "${_preflight_manifest}"
        "${CMAKE_SOURCE_DIR}/rust/obs-preflight/Cargo.toml"
        "${CMAKE_SOURCE_DIR}/Cargo.toml"
        "${CMAKE_SOURCE_DIR}/Cargo.lock"
        ${_preflight_rust_sources}
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Building obs-preflight Rust C ABI"
      VERBATIM
    )
    add_custom_target(obs-preflight-rust DEPENDS "${_preflight_archive}")
  endif()

  # Corrosion builds each imported crate with a separate Cargo command. Keep
  # this invocation behind libobs's Cargo target so Rustup cannot install the
  # pinned toolchain's components concurrently in both commands.
  if(TARGET libobs_rust-static)
    if(TARGET cargo-build_libobs_rust)
      add_dependencies(obs-preflight-rust cargo-build_libobs_rust)
    else()
      message(FATAL_ERROR "Corrosion's cargo-build_libobs_rust target was not found")
    endif()
  endif()

  add_executable(test_preflight_abi test_preflight_abi.c)
  target_include_directories(test_preflight_abi PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/frontend/preflight")
  target_link_libraries(
    test_preflight_abi
    PRIVATE "${_preflight_archive}" ${CMOCKA_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS}
  )
  add_dependencies(test_preflight_abi obs-preflight-rust)
  add_dependencies(cmocka-tests test_preflight_abi)
  add_test(NAME test_preflight_abi COMMAND test_preflight_abi)
  set_tests_properties(test_preflight_abi PROPERTIES TIMEOUT 60)
endif()

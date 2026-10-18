option(OBS_PREFLIGHT_ENABLE "Build the OBS Preflight bench and Rust engine" ON)

# The Flatpak sandbox does not provision soldr or its Rust toolchain; that
# package disables this demo until those build tools are available in-sandbox.
# Libobs-only Tier 2 builds also skip this frontend-only engine target.
if(OBS_PREFLIGHT_ENABLE AND ENABLE_FRONTEND AND (OS_LINUX OR OS_MACOS))
  find_program(SOLDR_EXECUTABLE soldr REQUIRED)
  find_package(Threads REQUIRED)

  set(OBS_PREFLIGHT_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/obs-preflight-cargo")
  set(OBS_PREFLIGHT_CARGO_TARGET_ARGS)
  set(OBS_PREFLIGHT_CARGO_OUTPUT_DIR "${OBS_PREFLIGHT_CARGO_TARGET_DIR}")
  if(OS_MACOS AND CMAKE_OSX_ARCHITECTURES)
    set(_preflight_osx_architectures ${CMAKE_OSX_ARCHITECTURES})
    list(LENGTH _preflight_osx_architectures _preflight_osx_architecture_count)
    if(NOT _preflight_osx_architecture_count EQUAL 1)
      message(FATAL_ERROR "The Preflight Rust ABI requires one CMAKE_OSX_ARCHITECTURES value")
    endif()

    list(GET _preflight_osx_architectures 0 _preflight_osx_architecture)
    if(_preflight_osx_architecture STREQUAL "arm64")
      set(_preflight_cargo_target aarch64-apple-darwin)
    elseif(_preflight_osx_architecture STREQUAL "x86_64")
      set(_preflight_cargo_target x86_64-apple-darwin)
    else()
      message(FATAL_ERROR "Unsupported Preflight Rust macOS architecture: ${_preflight_osx_architecture}")
    endif()

    # CMake's macOS app architecture must match the Rust archive's architecture.
    list(APPEND OBS_PREFLIGHT_CARGO_TARGET_ARGS --target "${_preflight_cargo_target}")
    set(OBS_PREFLIGHT_CARGO_OUTPUT_DIR "${OBS_PREFLIGHT_CARGO_TARGET_DIR}/${_preflight_cargo_target}")
  endif()
  set(OBS_PREFLIGHT_MANIFEST "${CMAKE_SOURCE_DIR}/rust/obs-preflight-ffi/Cargo.toml")
  set(
    OBS_PREFLIGHT_ARCHIVE
    "${OBS_PREFLIGHT_CARGO_OUTPUT_DIR}/release/${CMAKE_STATIC_LIBRARY_PREFIX}obs_preflight_ffi${CMAKE_STATIC_LIBRARY_SUFFIX}"
  )
  file(
    GLOB_RECURSE OBS_PREFLIGHT_RUST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/rust/obs-preflight/src/*.rs"
    "${CMAKE_SOURCE_DIR}/rust/obs-preflight-ffi/src/*.rs"
  )

  add_custom_command(
    OUTPUT "${OBS_PREFLIGHT_ARCHIVE}"
    COMMAND
      "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${OBS_PREFLIGHT_CARGO_TARGET_DIR}" "${SOLDR_EXECUTABLE}" cargo build
      --manifest-path "${CMAKE_SOURCE_DIR}/Cargo.toml" --package obs-preflight-ffi --lib --release --locked
      ${OBS_PREFLIGHT_CARGO_TARGET_ARGS}
    DEPENDS
      "${OBS_PREFLIGHT_MANIFEST}"
      "${CMAKE_SOURCE_DIR}/rust/obs-preflight/Cargo.toml"
      "${CMAKE_SOURCE_DIR}/Cargo.toml"
      "${CMAKE_SOURCE_DIR}/Cargo.lock"
      ${OBS_PREFLIGHT_RUST_SOURCES}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building obs-preflight Rust C ABI"
    VERBATIM
  )
  add_custom_target(obs-preflight-rust DEPENDS "${OBS_PREFLIGHT_ARCHIVE}")
endif()

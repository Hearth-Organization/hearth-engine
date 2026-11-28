# Hearth Engine's Rust parts (hearth-rust/hearth-ffi), built with cargo and linked into this gem.
# The owner, Nov 28, 2026: keep O3DE's C++ core, make Hearth's own parts Rust.

if(NOT PAL_PLATFORM_NAME STREQUAL "Linux")
    return()
endif()

find_program(HEARTH_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
if(NOT HEARTH_CARGO)
    message(FATAL_ERROR "HearthRust needs Rust's cargo to build (https://rustup.rs)")
endif()

set(HEARTH_RUST_DIR "${LY_ROOT_FOLDER}/hearth-rust")
set(HEARTH_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/hearth-rust-target")
set(HEARTH_FFI_LIB "${HEARTH_RUST_TARGET_DIR}/release/libhearth_ffi.a")
file(GLOB_RECURSE HEARTH_RUST_SOURCES CONFIGURE_DEPENDS
    "${HEARTH_RUST_DIR}/hearth-ffi/src/*.rs" "${HEARTH_RUST_DIR}/hearth-script/src/*.rs" "${HEARTH_RUST_DIR}/hearth-detail/src/*.rs"
    "${HEARTH_RUST_DIR}/*/Cargo.toml" "${HEARTH_RUST_DIR}/Cargo.toml")

add_custom_command(
    OUTPUT "${HEARTH_FFI_LIB}"
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${HEARTH_RUST_TARGET_DIR}"
            "${HEARTH_CARGO}" build --release -p hearth-ffi --manifest-path "${HEARTH_RUST_DIR}/Cargo.toml"
    DEPENDS ${HEARTH_RUST_SOURCES}
    COMMENT "Building Hearth Engine's Rust parts (hearth-ffi)"
    VERBATIM
)
add_custom_target(HearthRust.Cargo DEPENDS "${HEARTH_FFI_LIB}")

ly_add_target(
    NAME HearthRust.Static STATIC
    NAMESPACE Gem
    FILES_CMAKE
        hearthrust_files.cmake
    INCLUDE_DIRECTORIES
        PUBLIC
            Include
        PRIVATE
            Source
            ${HEARTH_RUST_DIR}/hearth-ffi/include
    BUILD_DEPENDENCIES
        PUBLIC
            AZ::AzCore
            AZ::AzFramework
            Gem::Atom_RPI.Public
            Gem::Atom_Feature_Common.Public
)
add_dependencies(HearthRust.Static HearthRust.Cargo)
target_link_libraries(HearthRust.Static PUBLIC "${HEARTH_FFI_LIB}" pthread dl m)

ly_add_target(
    NAME HearthRust ${PAL_TRAIT_MONOLITHIC_DRIVEN_MODULE_TYPE}
    NAMESPACE Gem
    FILES_CMAKE
        hearthrust_module_files.cmake
    INCLUDE_DIRECTORIES
        PUBLIC
            Include
        PRIVATE
            Source
    BUILD_DEPENDENCIES
        PRIVATE
            Gem::HearthRust.Static
)

ly_add_source_properties(
    SOURCES Source/HearthRustModule.cpp
    PROPERTY COMPILE_DEFINITIONS
    VALUES O3DE_GEM_NAME=${gem_name} O3DE_GEM_VERSION=${gem_version})

# The same module runs in the game, the editor and the asset tools.
ly_create_alias(NAME HearthRust.Clients NAMESPACE Gem TARGETS Gem::HearthRust)
ly_create_alias(NAME HearthRust.Servers NAMESPACE Gem TARGETS Gem::HearthRust)
ly_create_alias(NAME HearthRust.Unified NAMESPACE Gem TARGETS Gem::HearthRust)
ly_create_alias(NAME HearthRust.Tools NAMESPACE Gem TARGETS Gem::HearthRust)
ly_create_alias(NAME HearthRust.Builders NAMESPACE Gem TARGETS Gem::HearthRust)

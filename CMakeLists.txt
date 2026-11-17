cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-prelude REQUIRED PATHS node_modules/cmake-prelude)
find_package(cmake-test REQUIRED PATHS node_modules/cmake-test)

project(js LANGUAGES C CXX VERSION 0.0.0)

fetch_package("github:holepunchto/libintrusive")

set(HERMES_VERSION "250829098.0.17" CACHE STRING "The version of the prebuilt Hermes to fetch")

if(NOT TARGET hermesvm)
  if(NOT APPLE)
    message(FATAL_ERROR "Prebuilt Hermes is only fetched for Apple platforms; provide a `hermesvm` target instead")
  endif()

  if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(hermes_build_type debug)
  else()
    set(hermes_build_type release)
  endif()

  fetch_package(
    "https://repo1.maven.org/maven2/com/facebook/hermes/hermes-ios/${HERMES_VERSION}/hermes-ios-${HERMES_VERSION}-hermes-ios-${hermes_build_type}.tar.gz"
    SOURCE_DIR hermes
  )

  if(CMAKE_SYSTEM_NAME MATCHES "iOS")
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
      set(hermes_framework "${hermes}/destroot/Library/Frameworks/universal/hermesvm.xcframework/ios-arm64_x86_64-simulator/hermesvm.framework")
    else()
      set(hermes_framework "${hermes}/destroot/Library/Frameworks/universal/hermesvm.xcframework/ios-arm64/hermesvm.framework")
    endif()
  else()
    set(hermes_framework "${hermes}/destroot/Library/Frameworks/macosx/hermesvm.framework")
  endif()

  add_library(hermesvm SHARED IMPORTED GLOBAL)

  set_target_properties(
    hermesvm
    PROPERTIES
    IMPORTED_LOCATION "${hermes_framework}/hermesvm"
    INTERFACE_INCLUDE_DIRECTORIES "${hermes}/destroot/include"
  )

  get_filename_component(hermes_framework_dir "${hermes_framework}" DIRECTORY)

  set_property(
    TARGET hermesvm
    APPEND
    PROPERTY INTERFACE_LINK_OPTIONS
      "-Wl,-rpath,${hermes_framework_dir}"
  )
endif()

add_library_targets(
  js
  FEATURES
    c_std_99
  SOURCES
    src/js.cc
  LINK
    utf::<TYPE>
    PRIVATE
      intrusive::headers
      hermesvm
    STATIC
      uv_a
    SHARED
      uv
  EXPORT OFF
)

target_compile_features(
  js
  PRIVATE
    cxx_std_20
)

target_compile_definitions(
  js
  PRIVATE
    JS_HERMES_VERSION="${HERMES_VERSION}"
)

fetch_package("github:holepunchto/libjs" SOURCE_DIR js)

add_test_subdirectory(test)

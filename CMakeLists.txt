cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

project(js C CXX)

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

add_library(js OBJECT)

fetch_package("github:holepunchto/libjs" SOURCE_DIR js)
fetch_package("github:holepunchto/libintrusive")

set_target_properties(
  js
  PROPERTIES
  CXX_STANDARD 20
  CXX_SCAN_FOR_MODULES OFF
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  js
  PRIVATE
    src/js.cc
)

target_compile_definitions(
  js
  PRIVATE
    JS_HERMES_VERSION="${HERMES_VERSION}"
)

target_include_directories(
  js
  INTERFACE
    include
  PUBLIC
    ${js}/include
    $<TARGET_PROPERTY:uv,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:utf,INTERFACE_INCLUDE_DIRECTORIES>
  PRIVATE
    $<TARGET_PROPERTY:intrusive,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js
  PUBLIC
    hermesvm
)

add_library(js_shared SHARED)

set_target_properties(
  js_shared
  PROPERTIES
  OUTPUT_NAME js
)

target_include_directories(
  js_shared
  INTERFACE
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js_shared
  PUBLIC
    uv
    utf_shared
  PRIVATE
    js
)

add_library(js_static STATIC)

set_target_properties(
  js_static
  PROPERTIES
  OUTPUT_NAME js
  PREFIX lib
)

target_include_directories(
  js_static
  INTERFACE
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js_static
  PUBLIC
    uv_a
    utf_static
  PRIVATE
    js
)

install(TARGETS js_shared js_static)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  add_subdirectory(test)
endif()

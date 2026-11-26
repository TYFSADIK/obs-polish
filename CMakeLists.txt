cmake_minimum_required(VERSION 3.24)
# macOS: universal binary (Apple silicon + Intel); must be set before project()
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
project(obs-polish VERSION 0.5.0 LANGUAGES C)
if(APPLE)
  enable_language(OBJC)
endif()

# Built against the oldest supported libobs (30.0.0) so the same binary loads in OBS 30 to 33:
# OBS refuses plugins built against a newer libobs than itself. Headers come from a sparse
# checkout made by scripts/fetch-deps.sh.
set(OBS_SRC "${CMAKE_SOURCE_DIR}/deps/obs-studio" CACHE PATH "obs-studio source (headers only)")
# Windows: import libraries generated from the OBS release's DLLs (scripts/make-import-libs.ps1)
set(OBS_LIB_DIR "${CMAKE_SOURCE_DIR}/deps/obs-lib" CACHE PATH "obs.lib and w32-pthreads.lib (Windows)")
# ONNX Runtime library for this platform (scripts/fetch-onnxruntime.*), bundled next to the plugin
set(ORT_LIB "" CACHE FILEPATH "ONNX Runtime shared library to bundle")

if(APPLE)
  set(POLISH_BACKEND "vision" CACHE STRING "Face analysis backend: vision or onnx")
else()
  set(POLISH_BACKEND "onnx" CACHE STRING "Face analysis backend: onnx")
endif()

set(SOURCES src/plugin.c src/polish-filter.c src/analyzer-core.c src/image-load.c)
if(POLISH_BACKEND STREQUAL "vision")
  list(APPEND SOURCES src/backend-vision.m)
else()
  list(APPEND SOURCES src/backend-onnx.c)
endif()

if(APPLE)
  add_library(obs-polish MODULE ${SOURCES})
elseif(WIN32)
  add_library(obs-polish SHARED ${SOURCES})
else()
  add_library(obs-polish MODULE ${SOURCES})
  set_target_properties(obs-polish PROPERTIES PREFIX "")
endif()

target_include_directories(obs-polish PRIVATE
  "${CMAKE_SOURCE_DIR}/cmake" "${OBS_SRC}/libobs" "${CMAKE_SOURCE_DIR}/third_party/stb"
  "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/include")
if(EXISTS "${CMAKE_SOURCE_DIR}/deps/simde")
  target_include_directories(obs-polish PRIVATE "${CMAKE_SOURCE_DIR}/deps/simde")
endif()

if(MSVC)
  target_include_directories(obs-polish PRIVATE "${OBS_SRC}/deps/w32-pthreads")
  target_compile_options(obs-polish PRIVATE /W3 /std:c11 /utf-8 /wd4152 /wd4200)
  target_compile_definitions(obs-polish PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(obs-polish PRIVATE "${OBS_LIB_DIR}/obs.lib" "${OBS_LIB_DIR}/w32-pthreads.lib")
else()
  target_compile_options(obs-polish PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

if(APPLE)
  # libobs symbols resolve against the running OBS, so the plugin builds without OBS installed
  target_link_options(obs-polish PRIVATE -undefined dynamic_lookup)
  target_compile_options(obs-polish PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(obs-polish PRIVATE "-framework Foundation")
  if(POLISH_BACKEND STREQUAL "vision")
    target_link_libraries(obs-polish PRIVATE "-framework Vision" "-framework CoreVideo")
  endif()
  set_target_properties(obs-polish PROPERTIES
    BUNDLE TRUE BUNDLE_EXTENSION plugin OUTPUT_NAME obs-polish
    MACOSX_BUNDLE_GUI_IDENTIFIER org.tyfsadik.obs-polish
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  set(DATA_DEST "$<TARGET_BUNDLE_CONTENT_DIR:obs-polish>/Resources")
  set(ORT_DEST "$<TARGET_BUNDLE_CONTENT_DIR:obs-polish>/Frameworks/libobs-polish-onnxruntime.dylib")
elseif(WIN32)
  set(DATA_DEST "${CMAKE_BINARY_DIR}/data")
  set(ORT_DEST "$<TARGET_FILE_DIR:obs-polish>/obs-polish-onnxruntime.dll")
else()
  # Linux: libobs symbols resolve against the running OBS (shared objects may have undefined symbols)
  target_link_libraries(obs-polish PRIVATE m dl)
  set_target_properties(obs-polish PROPERTIES C_VISIBILITY_PRESET hidden)
  set(DATA_DEST "${CMAKE_BINARY_DIR}/data")
  set(ORT_DEST "$<TARGET_FILE_DIR:obs-polish>/libobs-polish-onnxruntime.so")
endif()

# data/ goes where OBS looks for it. A separate target that always runs, so changing only an
# asset or a shader still updates the build.
add_custom_target(obs-polish-data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${DATA_DEST}"
  COMMENT "Copying data")
add_dependencies(obs-polish-data obs-polish)

if(POLISH_BACKEND STREQUAL "onnx")
  if(ORT_LIB AND EXISTS "${ORT_LIB}")
    add_custom_command(TARGET obs-polish POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<PATH:GET_PARENT_PATH,${ORT_DEST}>"
      COMMAND ${CMAKE_COMMAND} -E copy "${ORT_LIB}" "${ORT_DEST}"
      COMMENT "Bundling ONNX Runtime as a privately named copy")
  else()
    message(WARNING "ORT_LIB not set: the plugin will build but cannot analyse faces until ONNX Runtime is bundled")
  endif()
endif()

# Install layout: what OBS expects in its plugin folders.
#   macOS    obs-polish.plugin
#   Windows  obs-polish/bin/64bit/obs-polish.dll (+ runtime), obs-polish/data/...
#   Linux    obs-polish/bin/64bit/obs-polish.so  (+ runtime), obs-polish/data/...
if(APPLE)
  install(TARGETS obs-polish LIBRARY DESTINATION . BUNDLE DESTINATION .)
else()
  install(TARGETS obs-polish LIBRARY DESTINATION obs-polish/bin/64bit RUNTIME DESTINATION obs-polish/bin/64bit)
  install(DIRECTORY data/ DESTINATION obs-polish/data)
  if(ORT_LIB AND EXISTS "${ORT_LIB}")
    if(WIN32)
      install(FILES "${ORT_LIB}" DESTINATION obs-polish/bin/64bit RENAME obs-polish-onnxruntime.dll)
    else()
      install(FILES "${ORT_LIB}" DESTINATION obs-polish/bin/64bit RENAME libobs-polish-onnxruntime.so)
    endif()
  endif()
endif()

# ---------------------------------------------------------------- tests
# render-test starts a real libobs headless, runs an image through the filter and writes a PNG.
option(POLISH_TESTS "Build the headless render test (needs libobs to link against)" OFF)
if(POLISH_TESTS)
  add_executable(render-test tests/render_test.c)
  target_include_directories(render-test PRIVATE
    "${CMAKE_SOURCE_DIR}/cmake" "${OBS_SRC}/libobs" "${CMAKE_SOURCE_DIR}/third_party/stb")
  if(EXISTS "${CMAKE_SOURCE_DIR}/deps/simde")
    target_include_directories(render-test PRIVATE "${CMAKE_SOURCE_DIR}/deps/simde")
  endif()
  if(APPLE)
    set(OBS_APP "/Applications/OBS.app" CACHE PATH "OBS Studio application bundle")
    target_link_options(render-test PRIVATE -F${OBS_APP}/Contents/Frameworks -Wl,-rpath,${OBS_APP}/Contents/Frameworks)
    target_link_libraries(render-test PRIVATE "-framework libobs")
    # the installed libobs is built for a newer macOS; the test tool only runs on the build machine
    set_target_properties(render-test PROPERTIES OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    target_compile_options(render-test PRIVATE -mmacosx-version-min=13.0)
    target_link_options(render-test PRIVATE -mmacosx-version-min=13.0)
  elseif(WIN32)
    target_include_directories(render-test PRIVATE "${OBS_SRC}/deps/w32-pthreads")
    target_compile_options(render-test PRIVATE /std:c11 /utf-8)
    target_compile_definitions(render-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(render-test PRIVATE "${OBS_LIB_DIR}/obs.lib")
  else()
    find_library(LIBOBS NAMES obs libobs.so.0 libobs.so.30 PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu)
    if(NOT LIBOBS)
      file(GLOB LIBOBS_CANDIDATES /usr/lib/*/libobs.so.* /usr/lib/libobs.so.*)
      list(GET LIBOBS_CANDIDATES 0 LIBOBS)
    endif()
    message(STATUS "render-test links ${LIBOBS}")
    target_link_libraries(render-test PRIVATE "${LIBOBS}" m dl)
  endif()
endif()

cmake_minimum_required(VERSION 3.22)
# universal binary (Apple silicon + Intel); must be set before project()
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
project(obs-polish VERSION 0.1.0 LANGUAGES C OBJC)

# Builds against the OBS that is installed in /Applications (headers from the matching
# obs-studio tag, fetched by scripts/fetch-obs-headers.sh into deps/obs-studio).
set(OBS_APP "/Applications/OBS.app" CACHE PATH "OBS Studio application bundle")
set(OBS_SRC "${CMAKE_SOURCE_DIR}/deps/obs-studio" CACHE PATH "obs-studio source (headers only)")

add_library(obs-polish MODULE
  src/plugin.c
  src/polish-filter.c
  src/analyzer.m
)
target_include_directories(obs-polish PRIVATE
  "${CMAKE_SOURCE_DIR}/cmake" "${OBS_SRC}/libobs" "${OBS_SRC}/frontend/api" "${CMAKE_SOURCE_DIR}/deps/simde")
target_compile_options(obs-polish PRIVATE -Wall -Wextra -Wno-unused-parameter -fobjc-arc)
# libobs symbols resolve against the running OBS, so the plugin builds without OBS installed
target_link_options(obs-polish PRIVATE -undefined dynamic_lookup)
target_link_libraries(obs-polish PRIVATE
  "-framework Foundation" "-framework Vision" "-framework CoreVideo" "-framework CoreGraphics")
set_target_properties(obs-polish PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION plugin
  OUTPUT_NAME obs-polish
  MACOSX_BUNDLE_GUI_IDENTIFIER org.tyfsadik.obs-polish
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

# data/ goes into Contents/Resources, where OBS looks for a plugin's data on macOS
add_custom_command(TARGET obs-polish POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data"
          "$<TARGET_BUNDLE_CONTENT_DIR:obs-polish>/Resources")

# headless test harness: renders images through the filter with the installed OBS's libobs.
# Built only when OBS is installed (not on CI, which builds the plugin alone).
if(EXISTS "${OBS_APP}/Contents/Frameworks/libobs.framework")
  add_executable(render-test tests/render_test.m)
  set_target_properties(render-test PROPERTIES OSX_ARCHITECTURES arm64 OSX_DEPLOYMENT_TARGET 13.0)
  target_include_directories(render-test PRIVATE "${CMAKE_SOURCE_DIR}/cmake" "${OBS_SRC}/libobs" "${CMAKE_SOURCE_DIR}/deps/simde")
  target_compile_options(render-test PRIVATE -fobjc-arc -Wno-deprecated-declarations)
  target_link_options(render-test PRIVATE -F${OBS_APP}/Contents/Frameworks -Wl,-rpath,${OBS_APP}/Contents/Frameworks)
  target_link_libraries(render-test PRIVATE "-framework libobs" "-framework Foundation" "-framework AppKit")
endif()

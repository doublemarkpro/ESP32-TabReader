# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)
# LVGL 9.5's bundled ThorVG compares a plain char with zero while decoding base64.
# GCC 15 reports that third-party check as type-limits; keep warnings enabled but
# do not promote this one vendor warning to a build failure.
add_compile_options(-Wno-error=type-limits)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
if(BOARD_NAME MATCHES "^m5stack-tab5-reader")
    idf_build_set_property(TAB5_READER_BUILD ON)
    set(ENV{XIAOZHI_FIRMWARE_KIND} "reader")
else()
    idf_build_set_property(TAB5_READER_BUILD OFF)
    set(ENV{XIAOZHI_FIRMWARE_KIND} "standard")
endif()

set(PROJECT_VER "2.4.2")
if(BOARD_NAME MATCHES "^m5stack-tab5-reader")
    project(tab5_reader)
else()
    project(xiaozhi)
endif()

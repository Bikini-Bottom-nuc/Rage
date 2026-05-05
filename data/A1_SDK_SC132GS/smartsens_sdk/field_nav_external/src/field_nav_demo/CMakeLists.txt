cmake_minimum_required(VERSION 3.0.0)

project(field_nav_demo)

include(cmake_config/Paths.cmake)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -O3 -DNDEBUG")
add_compile_options(-Wno-psabi)

set(FACE_DEMO_DIR "$ENV{FIELD_NAV_FACE_DEMO_ROOT}" CACHE PATH "Face demo source directory")
if(NOT EXISTS "${FACE_DEMO_DIR}/src/osd-device.cpp")
    message(FATAL_ERROR "FIELD_NAV_FACE_DEMO_ROOT is invalid: ${FACE_DEMO_DIR}")
endif()

add_executable(field_nav_demo
    src/main.cpp
    src/image_processor.cpp
    src/navline_detector.cpp
    src/osd_overlay.cpp
    ${FACE_DEMO_DIR}/src/osd-device.cpp
)

target_include_directories(field_nav_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${FACE_DEMO_DIR}/include
    ${M1_SDK_INC_DIR}
    ${M1_SDK_INC_DIR}/smartsoc
)

target_link_libraries(field_nav_demo
    ${M1_SSNE_LIB}
    ${M1_CMABUFFER_LIB}
    ${M1_OSD_LIB}
    ${M1_GPIO_LIB}
    ${M1_UART_LIB}
    ${M1_SSZLOG_LIB}
    ${M1_ZLOG_LIB}
    ${M1_EMB_LIB}
)

install(TARGETS field_nav_demo DESTINATION bin)

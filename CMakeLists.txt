cmake_minimum_required(VERSION 3.10)
project(GunzNakama)

set(CMAKE_CXX_STANDARD 17)
add_definitions(-D_CRT_SECURE_NO_WARNINGS)
add_definitions(-DNOMINMAX)

# Nakama - Conditional Linking
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(NAKAMA_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../nakama-client-cpp-sdk/win-x64-Debug")
    if(NOT EXISTS "${NAKAMA_SDK}/include/nakama-cpp/Nakama.h")
        set(NAKAMA_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../adicionais-engines/nakama-client-cpp-sdk/win-x64-Debug")
    endif()
    add_definitions(-D_DEBUG)
else()
    set(NAKAMA_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../nakama-client-cpp-sdk/win-x64-MinSizeRel")
    if(NOT EXISTS "${NAKAMA_SDK}/include/nakama-cpp/Nakama.h")
        set(NAKAMA_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../adicionais-engines/nakama-client-cpp-sdk/win-x64-MinSizeRel")
    endif()
endif()

include_directories("${NAKAMA_SDK}/include")
link_directories("${NAKAMA_SDK}/lib")

# Ultralight
set(ULTRALIGHT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Ultralight")
if(NOT EXISTS "${ULTRALIGHT_ROOT}/include/Ultralight/Ultralight.h")
    set(ULTRALIGHT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../adicionais-engines/Ultralight")
endif()
include_directories("${ULTRALIGHT_ROOT}/include")
link_directories("${ULTRALIGHT_ROOT}/lib")

# App
include_directories("src")
include_directories("src/RealSpace3/Include")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "src/*.cpp" 
    "src/*.h"
    "src/RealSpace3/Source/*.cpp"
    "src/RealSpace3/Include/*.h"
)

add_executable(GunzNakama WIN32 ${SOURCES})

target_link_libraries(GunzNakama 
    nakama-sdk
    ultralight
    UltralightCore
    AppCore
    WebCore
    d3d11
    d3dcompiler
    dxgi
    ws2_32
    crypt32
    advapi32
    shell32
    user32
    gdi32
    ole32
    uuid
)

add_custom_command(TARGET GunzNakama POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ui"
        "$<TARGET_FILE_DIR:GunzNakama>/ui"
)

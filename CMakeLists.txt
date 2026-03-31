cmake_minimum_required(VERSION 3.16)
project(Axolotl VERSION 0.1.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (APPLE)
    enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/IXWebSocket/CMake")

if (MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static" CACHE STRING "Linker flags for executables" FORCE)
    set(ZLIB_USE_STATIC_LIBS ON)
endif()

# Find system libraries
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)
if (NOT TARGET yaml-cpp::yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()

if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
endif()

# glfw or sdl2 for windowing backend. Let's use glfw as standard.
find_package(glfw3 CONFIG QUIET)
if (NOT glfw3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED glfw3)
endif()

# TLS Backend options
option(USE_MBED_TLS "Use mbedtls for TLS" OFF)
option(USE_OPEN_SSL "Use OpenSSL for TLS" ON)

if (USE_MBED_TLS)
    set(USE_OPEN_SSL OFF CACHE BOOL "" FORCE)
    find_package(MbedTLS REQUIRED)
endif()

# IXWebSocket submodule
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
set(USE_TLS ON CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/IXWebSocket)

# ImGui submodule
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
)

if (APPLE)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_metal.mm)
elseif (WIN32)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
else()
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
endif()

add_library(imgui STATIC ${IMGUI_SOURCES})
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

if (APPLE)
    find_library(METAL_LIBRARY Metal)
    find_library(QUARTZ_LIBRARY QuartzCore)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(APPKIT_LIBRARY AppKit)

    if (glfw3_FOUND)
        target_link_libraries(imgui PRIVATE glfw ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${FOUNDATION_LIBRARY})
    else()
        target_link_libraries(imgui PRIVATE ${GLFW3_LIBRARIES} ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${FOUNDATION_LIBRARY})
        target_include_directories(imgui PRIVATE ${GLFW3_INCLUDE_DIRS})
    endif()
else()
    if (glfw3_FOUND)
        target_link_libraries(imgui PRIVATE glfw OpenGL::GL)
    else()
        target_link_libraries(imgui PRIVATE ${GLFW3_LIBRARIES} OpenGL::GL)
        target_include_directories(imgui PRIVATE ${GLFW3_INCLUDE_DIRS})
    endif()
endif()

# Application executable
set(SRC_FILES
    src/Application.cpp
    src/ArchipelagoNetwork.cpp
    src/ChatWindow.cpp
    src/Config.cpp
    src/DataPackageCache.cpp
    src/EmbeddedWebServer.cpp
    src/FontScanner.cpp
    src/HintWindow.cpp
    src/ItemFeedWindow.cpp
    src/OverviewWindow.cpp
    src/Platform.cpp
    src/ReceivedItemsWindow.cpp
    src/SettingsWindow.cpp
    src/SpoilerLog.cpp
    src/SpoilerSphereTrackerWindow.cpp
    src/TrackerWindow.cpp
    src/main.cpp
)
# Options
option(AXOLOTL_OFFICIAL_RELEASE "Build as an official release" OFF)
option(AXOLOTL_IGNORE_DIRTY "Skip the dirty repo check" OFF)

if (APPLE)
    set_source_files_properties(src/Application.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

# Dynamic version generation
add_custom_target(UpdateVersion ALL
    COMMAND ${CMAKE_COMMAND} 
        -DS_DIR=${CMAKE_SOURCE_DIR} 
        -DAXOLOTL_IGNORE_DIRTY=${AXOLOTL_IGNORE_DIRTY}
        -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -DOUT_FILE=${CMAKE_BINARY_DIR}/version.h
        -P ${CMAKE_SOURCE_DIR}/tools/generate_version.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/version.h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Updating version.h with current project version and git hash"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/embedded_static.h
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_SOURCE_DIR}/tools/embed_static.py ${CMAKE_BINARY_DIR}/embedded_static.h ${CMAKE_SOURCE_DIR}/static/feed.html ${CMAKE_SOURCE_DIR}/static/feed.css ${CMAKE_SOURCE_DIR}/static/feed.js ${CMAKE_SOURCE_DIR}/static/overview.html ${CMAKE_SOURCE_DIR}/static/overview.css ${CMAKE_SOURCE_DIR}/static/overview.js
    DEPENDS ${CMAKE_SOURCE_DIR}/static/feed.html ${CMAKE_SOURCE_DIR}/static/feed.css ${CMAKE_SOURCE_DIR}/static/feed.js ${CMAKE_SOURCE_DIR}/static/overview.html ${CMAKE_SOURCE_DIR}/static/overview.css ${CMAKE_SOURCE_DIR}/static/overview.js ${CMAKE_SOURCE_DIR}/tools/embed_static.py
    COMMENT "Generating embedded_static.h"
)

add_custom_target(GenerateStaticEmbedded ALL DEPENDS ${CMAKE_BINARY_DIR}/embedded_static.h)

add_executable(axolotl-apclient WIN32 ${SRC_FILES})
add_dependencies(axolotl-apclient UpdateVersion GenerateStaticEmbedded)
target_include_directories(axolotl-apclient PRIVATE src ${CMAKE_BINARY_DIR})
target_compile_definitions(axolotl-apclient PRIVATE IMGUI_USE_WCHAR32)

if (AXOLOTL_OFFICIAL_RELEASE)
    target_compile_definitions(axolotl-apclient PRIVATE AXOLOTL_OFFICIAL_RELEASE)
endif()

if (APPLE)
    if (USE_MBED_TLS)
        target_link_libraries(axolotl-apclient PRIVATE ${MBEDTLS_LIBRARIES})
        target_include_directories(axolotl-apclient PRIVATE ${MBEDTLS_INCLUDE_DIRS})
    endif()
    if (glfw3_FOUND)
        target_link_libraries(axolotl-apclient PRIVATE imgui glfw Threads::Threads nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp ixwebsocket ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${FOUNDATION_LIBRARY} ${APPKIT_LIBRARY})
    else()
        target_link_libraries(axolotl-apclient PRIVATE imgui ${GLFW3_LIBRARIES} Threads::Threads nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp ixwebsocket ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${FOUNDATION_LIBRARY} ${APPKIT_LIBRARY})
        target_include_directories(axolotl-apclient PRIVATE ${GLFW3_INCLUDE_DIRS})
    endif()
else()
    if (glfw3_FOUND)
        target_link_libraries(axolotl-apclient PRIVATE imgui glfw OpenGL::GL Threads::Threads nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp ${FONTCONFIG_LIBRARIES} ixwebsocket)
        target_include_directories(axolotl-apclient PRIVATE ${FONTCONFIG_INCLUDE_DIRS})
    else()
        target_link_libraries(axolotl-apclient PRIVATE
            imgui 
            ${GLFW3_LIBRARIES}
            OpenGL::GL
            Threads::Threads
            nlohmann_json::nlohmann_json
            yaml-cpp::yaml-cpp
            ${FONTCONFIG_LIBRARIES}
            ixwebsocket
        )
        if (USE_MBED_TLS)
            target_link_libraries(axolotl-apclient PRIVATE ${MBEDTLS_LIBRARIES})
            target_include_directories(axolotl-apclient PRIVATE ${MBEDTLS_INCLUDE_DIRS})
        endif()
        target_include_directories(axolotl-apclient PRIVATE ${GLFW3_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})
    endif()
endif()

if (WIN32)
    message(STATUS "Configuring Windows-specific dependencies...")
    target_compile_definitions(axolotl-apclient PRIVATE YAML_CPP_STATIC_DEFINE)
    target_link_libraries(axolotl-apclient PRIVATE d3d11 dxgi d3dcompiler bcrypt)
endif()

install(TARGETS axolotl-apclient
        RUNTIME DESTINATION bin)

# CPack configuration
set(CPACK_PACKAGE_NAME "AxolotlAPTextClient")
set(CPACK_PACKAGE_VENDOR "MooingLemur")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Axolotl Archipelago text client")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "AxolotlAPTextClient")

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
endif()

if (APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_PACKAGE_FILE_NAME "AxolotlAPTextClient-macOS")
elseif(WIN32)
    set(CPACK_GENERATOR "WIX;ZIP")
    set(CPACK_WIX_PRODUCT_GUID "*")
    set(CPACK_PACKAGE_EXECUTABLES "axolotl-apclient" "Axolotl AP Text Client")
    set(CPACK_CREATE_DESKTOP_LINKS "axolotl-apclient")
    set(CPACK_WIX_UPGRADE_GUID "1455EE6E-174C-4218-8A7B-CC4938B3C048")
endif()

include(CPack)



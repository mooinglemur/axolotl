cmake_minimum_required(VERSION 3.16)
project(Axolotl VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

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
file(GLOB_RECURSE SRC_FILES "src/*.cpp")
if (APPLE)
    set_source_files_properties(src/Application.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

add_executable(axolotl-apclient WIN32 ${SRC_FILES})
target_include_directories(axolotl-apclient PRIVATE src)

if (APPLE)
    if (glfw3_FOUND)
        target_link_libraries(axolotl-apclient PRIVATE imgui glfw Threads::Threads nlohmann_json::nlohmann_json ixwebsocket ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${FOUNDATION_LIBRARY})
    else()
        target_link_libraries(axolotl-apclient PRIVATE imgui ${GLFW3_LIBRARIES} Threads::Threads nlohmann_json::nlohmann_json ixwebsocket ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${FOUNDATION_LIBRARY})
        target_include_directories(axolotl-apclient PRIVATE ${GLFW3_INCLUDE_DIRS})
    endif()
else()
    if (glfw3_FOUND)
        target_link_libraries(axolotl-apclient PRIVATE imgui glfw OpenGL::GL Threads::Threads nlohmann_json::nlohmann_json yaml-cpp ${FONTCONFIG_LIBRARIES} ixwebsocket)
        target_include_directories(axolotl-apclient PRIVATE ${FONTCONFIG_INCLUDE_DIRS})
    else()
        target_link_libraries(axolotl-apclient PRIVATE
            imgui 
            ${GLFW3_LIBRARIES} 
            OpenGL::GL 
            Threads::Threads
            nlohmann_json::nlohmann_json
            yaml-cpp
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
    target_compile_definitions(axolotl-apclient PRIVATE YAML_CPP_STATIC_DEFINE)
    target_link_libraries(axolotl-apclient PRIVATE d3d11 dxgi d3dcompiler)
endif()

install(TARGETS axolotl-apclient
        RUNTIME DESTINATION bin)

# CPack configuration
set(CPACK_PACKAGE_NAME "Axolotl AP Text Client")
set(CPACK_PACKAGE_VENDOR "MooingLemur")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Axolotl Archipelago text client")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "AxolotlAPTextClient")

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
endif()

if(WIN32)
    set(CPACK_GENERATOR "WIX;ZIP")
    set(CPACK_WIX_PRODUCT_GUID "*")
    set(CPACK_PACKAGE_EXECUTABLES "axolotl-apclient" "Axolotl AP Text Client")
    set(CPACK_CREATE_DESKTOP_LINKS "axolotl-apclient")
    set(CPACK_WIX_UPGRADE_GUID "1455EE6E-174C-4218-8A7B-CC4938B3C048")
endif()

include(CPack)



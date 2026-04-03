cmake_minimum_required(VERSION 3.16)
project(Axolotl VERSION 0.1.4 LANGUAGES C CXX)

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

# Use an empty string for PKG_STATIC to avoid the 'STATIC' keyword, which is buggy on some CMake versions.
# Our 'Aggressive Cleanup' already forces PkgConfig to find static libraries on Windows.
set(PKG_STATIC "")

# Try PkgConfig first for yaml-cpp
find_package(PkgConfig REQUIRED)

if (MINGW)
    # For ALL Mingw builds (Native and Cross), we've for-deleted the DLLs/import-libs
    # which makes the system-provided Config files strictly broken. Use PkgConfig.
    pkg_check_modules(YAML_CPP REQUIRED IMPORTED_TARGET ${PKG_STATIC} yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS PkgConfig::YAML_CPP)
else()
    # Other platforms (Linux/macOS) - try PkgConfig with graceful fallback to Config
    pkg_check_modules(YAML_CPP QUIET IMPORTED_TARGET ${PKG_STATIC} yaml-cpp)
    if (YAML_CPP_FOUND)
        add_library(yaml-cpp::yaml-cpp ALIAS PkgConfig::YAML_CPP)
    else()
        find_package(yaml-cpp CONFIG REQUIRED)
        if (NOT TARGET yaml-cpp::yaml-cpp)
            add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
        endif()
    endif()
endif()

# Find libzip
if (MINGW)
    pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET ${PKG_STATIC} libzip)
else()
    pkg_check_modules(LIBZIP QUIET IMPORTED_TARGET ${PKG_STATIC} libzip)
    if (NOT LIBZIP_FOUND)
        find_package(libzip CONFIG REQUIRED)
    endif()
endif()

# glfw or sdl2 for windowing backend. Let's use glfw as standard.
# Prioritize PkgConfig to avoid brittle CONFIG files on some distros.
if (MINGW)
    pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET ${PKG_STATIC} glfw3)
    if (NOT TARGET glfw)
        add_library(glfw ALIAS PkgConfig::GLFW3)
    endif()
    set(glfw3_FOUND TRUE)
else()
    pkg_check_modules(GLFW3 QUIET IMPORTED_TARGET ${PKG_STATIC} glfw3)
    if (GLFW3_FOUND)
        if (NOT TARGET glfw)
            add_library(glfw ALIAS PkgConfig::GLFW3)
        endif()
        set(glfw3_FOUND TRUE)
    else()
        find_package(glfw3 CONFIG QUIET)
    endif()
endif()

if (UNIX AND NOT APPLE)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET ${PKG_STATIC} fontconfig)
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

# Lua dependency (defined locally to avoid modifying submodule dir)
set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/lua)
set(LUA_SOURCES
    ${LUA_DIR}/lapi.c ${LUA_DIR}/lcode.c ${LUA_DIR}/lctype.c ${LUA_DIR}/ldebug.c ${LUA_DIR}/ldo.c ${LUA_DIR}/ldump.c ${LUA_DIR}/lfunc.c ${LUA_DIR}/lgc.c ${LUA_DIR}/llex.c
    ${LUA_DIR}/lmem.c ${LUA_DIR}/lobject.c ${LUA_DIR}/lopcodes.c ${LUA_DIR}/lparser.c ${LUA_DIR}/lstate.c ${LUA_DIR}/lstring.c ${LUA_DIR}/ltable.c
    ${LUA_DIR}/ltm.c ${LUA_DIR}/lundump.c ${LUA_DIR}/lvm.c ${LUA_DIR}/lzio.c
    ${LUA_DIR}/lauxlib.c ${LUA_DIR}/lbaselib.c ${LUA_DIR}/lcorolib.c ${LUA_DIR}/ldblib.c ${LUA_DIR}/liolib.c ${LUA_DIR}/lmathlib.c
    ${LUA_DIR}/loadlib.c ${LUA_DIR}/loslib.c ${LUA_DIR}/lstrlib.c ${LUA_DIR}/ltablib.c ${LUA_DIR}/lutf8lib.c ${LUA_DIR}/linit.c
)
add_library(lua STATIC ${LUA_SOURCES})
if (APPLE)
    target_compile_definitions(lua PRIVATE LUA_USE_MACOSX)
elseif (UNIX AND NOT APPLE)
    target_compile_definitions(lua PRIVATE LUA_USE_LINUX)
endif()
target_include_directories(lua PUBLIC ${LUA_DIR})

if (NOT WIN32)
    target_link_libraries(lua INTERFACE dl m)
endif()

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
    src/LogicManager.cpp
    src/OverviewWindow.cpp
    src/PackStore.cpp
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
target_include_directories(axolotl-apclient PRIVATE
    src
    ${CMAKE_BINARY_DIR}
    thirdparty/sol2/include
    ${LIBZIP_INCLUDE_DIRS}
)
target_compile_definitions(axolotl-apclient PRIVATE
    IMGUI_USE_WCHAR32
    SOL_ALL_SAFETIES_ON=1
)

if (AXOLOTL_OFFICIAL_RELEASE)
    target_compile_definitions(axolotl-apclient PRIVATE AXOLOTL_OFFICIAL_RELEASE)
endif()

if (APPLE)
    if (USE_MBED_TLS)
        target_link_libraries(axolotl-apclient PRIVATE ${MBEDTLS_LIBRARIES})
        target_include_directories(axolotl-apclient PRIVATE ${MBEDTLS_INCLUDE_DIRS})
    endif()

    set(EXTRA_LIBS "")
    if (NOT libzip_FOUND)
        list(APPEND EXTRA_LIBS PkgConfig::LIBZIP)
    else()
        list(APPEND EXTRA_LIBS libzip::zip)
    endif()

    if (glfw3_FOUND)
        target_link_libraries(axolotl-apclient PRIVATE imgui lua glfw Threads::Threads nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp ${EXTRA_LIBS} ixwebsocket ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${FOUNDATION_LIBRARY} ${APPKIT_LIBRARY})
    else()
        target_link_libraries(axolotl-apclient PRIVATE imgui lua ${GLFW3_LIBRARIES} Threads::Threads nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp ${EXTRA_LIBS} ixwebsocket ${METAL_LIBRARY} ${QUARTZ_LIBRARY} ${FOUNDATION_LIBRARY} ${APPKIT_LIBRARY})
        target_include_directories(axolotl-apclient PRIVATE ${GLFW3_INCLUDE_DIRS})
    endif()
else()
    set(EXTRA_LIBS "")
    if (UNIX AND NOT APPLE)
        list(APPEND EXTRA_LIBS PkgConfig::LIBZIP PkgConfig::FONTCONFIG)
    else()
        if (NOT libzip_FOUND)
            list(APPEND EXTRA_LIBS PkgConfig::LIBZIP)
        else()
            list(APPEND EXTRA_LIBS libzip::zip)
        endif()
    endif()

    if (glfw3_FOUND)
        target_link_libraries(axolotl-apclient PRIVATE imgui lua glfw OpenGL::GL Threads::Threads nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp ${EXTRA_LIBS} ixwebsocket)
    else()
        target_link_libraries(axolotl-apclient PRIVATE
            imgui
            lua
            ${GLFW3_LIBRARIES}
            OpenGL::GL
            Threads::Threads
            nlohmann_json::nlohmann_json
            yaml-cpp::yaml-cpp
            ${EXTRA_LIBS}
            ixwebsocket
        )
        target_include_directories(axolotl-apclient PRIVATE ${GLFW3_INCLUDE_DIRS})
    endif()
endif()

if (WIN32)
    message(STATUS "Configuring Windows-specific dependencies...")
    target_compile_definitions(axolotl-apclient PRIVATE YAML_CPP_STATIC_DEFINE ZIP_STATIC)
    # Link against essential Windows system libs.
    set(WIN_LIBS d3d11 dxgi d3dcompiler bcrypt)

    # On Mingwplatforms (Native and Cross), we must manually ensure bz2, lzma, and zstd
    # are linked AFTER libzip for static builds to resolve undefined references.
    if (MINGW)
        foreach(LIB bz2 lzma zstd)
            find_library(OPTIONAL_${LIB} NAMES ${LIB})
            if (OPTIONAL_${LIB})
                message(STATUS "Manually linking static dependency: ${LIB} -> ${OPTIONAL_${LIB}}")
                list(APPEND WIN_LIBS ${OPTIONAL_${LIB}})
            endif()
        endforeach()
    endif()

    target_link_libraries(axolotl-apclient PRIVATE ${WIN_LIBS})
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



cmake_minimum_required(VERSION 3.16)
project(Axolotl VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

# Find system libraries
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)
if (NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
endif()

# glfw or sdl2 for windowing backend. Let's use glfw as standard.
find_package(glfw3 CONFIG QUIET)
if (NOT glfw3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED glfw3)
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
else()
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
endif()

add_library(imgui STATIC ${IMGUI_SOURCES})

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

add_executable(axolotl-apclient ${SRC_FILES})
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
        target_include_directories(axolotl-apclient PRIVATE ${GLFW3_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})
    endif()
endif()

install(TARGETS axolotl-apclient
        RUNTIME DESTINATION bin)

# CPack configuration
set(CPACK_PACKAGE_NAME "Axolotl AP Text Client")
set(CPACK_PACKAGE_VENDOR "MooingLemur")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Axolotl Archipelago text client")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "AxolotlAPTextClient")

if(WIN32)
    set(CPACK_GENERATOR "WIX;ZIP")
    set(CPACK_WIX_PRODUCT_GUID "425C992D-0261-4162-8AFA-57E2BBA737D4")
endif()

include(CPack)



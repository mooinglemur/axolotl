# Axolotl Archipelago Client

# THIS APPLICATION IS IN EARLY ALPHA AND NOT READY FOR GENERAL USE

Axolotl is a lightweight, cross-platform Archipelago multiworld client built with C++ and ImGui. It provides a clean, modular interface for interacting with Archipelago sessions, including chat, item tracking, and hinting.

## Features

- **Archipelago Integration**: Reliable connectivity to Archipelago servers using WebSocket.
- **Responsive UI**: A tabbed window system for managing different aspects of your multiworld session:
  - **Chat**: Real-time communication and server messages.
  - **Item Feed**: Live updates on items received and sent.
  - **Hints**: Track and manage location hints.
  - **Received Items**: Comprehensive list of all items acquired.
- **Customization**: Support for UI scaling and custom font loading.
- **Cross-Platform**: Built to run on Linux, macOS (Metal), and Windows.

## Dependencies

- **CMake** (>= 3.16)
- **C++20 Compiler** (GCC, Clang, or MSVC)
- **GLFW**
- **OpenGL** (or Metal on macOS)
- **Fontconfig** (Linux only)
- **nlohmann_json**
- **yaml-cpp**

## Building

### 1. Clone the Repository
Ensure you initialize submodules to fetch ImGui and IXWebSocket.

```bash
git clone <repository-url>
cd axolotl
git submodule update --init --recursive
```

### 2. Build with CMake

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

After building, run the `axolotl-apclient` executable. Enter your Archipelago server address, slot name, and password (if applicable) to connect.

# The racing wheel probe on its own (no recompiled game needed):
#   tools\vcenv.cmd cmake -S tools/wheelprobe -B build/wheelprobe -G Ninja -DCMAKE_BUILD_TYPE=Release
#   tools\vcenv.cmd cmake --build build/wheelprobe
# fm4.exe --wheel-probe is the same program; the launcher uses that one unless FM4_WHEEL_PROBE names this.
cmake_minimum_required(VERSION 3.25)
project(fm4_wheelprobe CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_executable(fm4-wheelprobe main.cpp "${SRC}/input/wheel/wheel_system.cpp" "${SRC}/input/wheel/wheel_probe.cpp")
target_include_directories(fm4-wheelprobe PRIVATE "${SRC}")
target_compile_definitions(fm4-wheelprobe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(fm4-wheelprobe PRIVATE dinput8 dxguid winmm user32)

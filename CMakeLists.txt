
cmake_minimum_required(VERSION 3.16)

project(WavtoTxt LANGUAGES C CXX)

# C++ 编译标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Whisper.cpp 编译选项
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)

# 加入 Whisper.cpp v1.9.5
add_subdirectory(whisper.cpp)

# 查找依赖库
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

# 编译自己的程序
add_executable(wavtotxt
    /home/lsl/WavtoTxt/src/WtoTClient.cpp
)

# 链接库
target_link_libraries(wavtotxt
    PRIVATE
    whisper
    PkgConfig::SNDFILE
    PkgConfig::SAMPLERATE
    Threads::Threads
)

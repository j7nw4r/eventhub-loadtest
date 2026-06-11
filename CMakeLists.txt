cmake_minimum_required(VERSION 3.16)
project(eventhub_loadtest LANGUAGES CXX)

# Windows-only: the I/O core is native WinHTTP (async) and the SAS crypto is
# CNG/bcrypt. There is no portable fallback here.
if(NOT WIN32)
  message(FATAL_ERROR
    "eventhub-loadtest is Windows-only (WinHTTP + CNG). Build it on Windows "
    "with MSVC. The previous Boost.Beast/OpenSSL backend has been removed.")
endif()

# C++20 for std::atomic niceties and clean structure. (The former Asio
# coroutine requirement is gone; WinHTTP drives the async state machine.)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Statically link the MSVC runtime (/MT) so the binary is self-contained: it can
# run in a minimal Windows container (servercore/nanoserver) with no VC++
# redistributable installed. Only OS DLLs (winhttp/bcrypt/crypt32) are needed.
# CMP0091 (>=3.15, NEW by default here) makes this property authoritative.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Threads REQUIRED)

add_executable(eh-loadtest
  src/main.cpp
  src/config.cpp
  src/sas_token.cpp
  src/client.cpp
)

# winhttp: HTTP/TLS client.  bcrypt: HMAC-SHA256 for SAS tokens.
# crypt32: base64 (CryptBinaryToString).  (The .cpp files also name these via
# #pragma comment(lib, ...), but list them here for non-MSVC linkers too.)
target_link_libraries(eh-loadtest PRIVATE
  winhttp
  bcrypt
  crypt32
  Threads::Threads
)

# Quiet MSVC's CRT "use the _s variant" warning for std::getenv in config.cpp.
target_compile_definitions(eh-loadtest PRIVATE
  _CRT_SECURE_NO_WARNINGS
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

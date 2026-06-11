cmake_minimum_required(VERSION 3.16)
project(eventhub_loadtest LANGUAGES CXX)

# C++20 for Asio coroutines (co_await). The async connection state machine is
# dramatically simpler as a coroutine than as a chain of callbacks.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Beast + Asio are header-only, and Boost.System has been header-only since
# 1.69 (the compiled lib is now just an ABI stub some distros omit). So we ask
# only for Boost headers and link the compiled system lib *if* it exists.
find_package(Boost 1.74 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(eh-loadtest
  src/main.cpp
  src/config.cpp
  src/sas_token.cpp
  src/client.cpp
)

target_include_directories(eh-loadtest PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(eh-loadtest PRIVATE
  Boost::headers
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)
# Link the compiled Boost.System stub only on toolchains that still provide it.
if(TARGET Boost::system)
  target_link_libraries(eh-loadtest PRIVATE Boost::system)
endif()

# Asio standalone-coroutine glue + silence the bundled-library deprecation noise.
target_compile_definitions(eh-loadtest PRIVATE
  BOOST_ASIO_NO_DEPRECATED
)

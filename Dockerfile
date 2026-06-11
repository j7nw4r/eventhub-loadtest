# escape=`
#
# Windows container for eventhub-loadtest. Multi-stage: a servercore build stage
# installs the C++ build tools and compiles, then a clean runtime stage ships
# just the self-contained exe (static CRT, so only OS DLLs are needed).
#
# Build (from the repo root):
#   docker build -t eh-loadtest:1.0 .
# Match the base image to your host's Windows build for process isolation, e.g.
#   docker build --build-arg WINDOWS_VERSION=ltsc2025 -t eh-loadtest:1.0 .
# Run (connection string from the environment, never baked into the image):
#   docker run --rm -e EH_CONNECTION_STRING="Endpoint=sb://...;EntityPath=hub" `
#     eh-loadtest:1.0 --connections 2000 --interval-ms 1000 --ramp-s 60
#
# Authorized use only: this generates heavy, sustained traffic.

ARG WINDOWS_VERSION=ltsc2022

# --- Build stage: VS C++ Build Tools + CMake/Ninja, compile Release -----------
FROM mcr.microsoft.com/windows/servercore:${WINDOWS_VERSION} AS build
SHELL ["cmd", "/S", "/C"]

# Install the C++ toolchain (MSVC + Windows SDK via --includeRecommended, plus
# CMake/Ninja). The bootstrapper returns 3010 ("reboot required") on success in
# a container; treat that as OK. Any other non-zero stays fatal.
ADD https://aka.ms/vs/17/release/vs_buildtools.exe C:\TEMP\vs_buildtools.exe
RUN C:\TEMP\vs_buildtools.exe --quiet --wait --norestart --nocache `
      --installPath C:\BuildTools `
      --add Microsoft.VisualStudio.Workload.VCTools `
      --add Microsoft.VisualStudio.Component.VC.CMake.Project `
      --includeRecommended `
    || IF "%ERRORLEVEL%"=="3010" EXIT 0

WORKDIR C:\src
COPY CMakeLists.txt .
COPY src .\src

# Compile inside the VS developer environment (puts cl/cmake/ninja on PATH).
RUN call "C:\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 && `
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && `
    cmake --build build

# --- Runtime stage: servercore has winhttp/bcrypt/crypt32 -----------------------
# (nanoserver is smaller but does not ship winhttp.dll; servercore is the safe
# default. Static CRT means no VC++ redistributable is required either way.)
FROM mcr.microsoft.com/windows/servercore:${WINDOWS_VERSION} AS runtime
COPY --from=build C:\src\build\eh-loadtest.exe C:\app\eh-loadtest.exe

# host, target, and SAS auth come from EH_CONNECTION_STRING (or the EH_* vars)
# at deploy time; pass tuning flags as container args.
ENTRYPOINT ["C:\\app\\eh-loadtest.exe"]

cmake_minimum_required(VERSION 3.16)

# Fix CMake policy version compatibility for older subprojects
cmake_policy(SET CMP0048 NEW)
if(POLICY CMP0079)
  cmake_policy(SET CMP0079 NEW)
endif()

# Aligned Hardware Device Firmware
# Connects to Aligned backend for LiveKit voice credentials
# Then uses xAI Grok Realtime for speech-to-speech AI

# Backend API endpoint for getting LiveKit credentials
add_compile_definitions(ALIGNED_API_URL="https://aligned-tools.vercel.app/api/py/watcher")

# Signaling endpoint for WebRTC SDP exchange (like OpenAI's Realtime API)
# This proxies through our backend to LiveKit
# For local dev: Use http://192.168.1.xxx:8000/api/py/watcher/signal (your local IP)
# For production: Use the Vercel URL
add_compile_definitions(ALIGNED_SIGNALING_URL="http://192.168.1.135:8000/api/py/watcher/signal")

# Define OPENAI_REALTIMEAPI for backward compatibility with http.cpp (fallback only)
add_compile_definitions(OPENAI_REALTIMEAPI="https://api.openai.com/v1/realtime")

# Use EXTRA_COMPONENT_DIRS to specify only the components we need
# Using LiveKit SDK instead of WHIP/libpeer
set(EXTRA_COMPONENT_DIRS "src")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-fdiagnostics-color=always -Wno-error=format= -Wno-format)
project(aligned-realtime)

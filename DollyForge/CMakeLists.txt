cmake_minimum_required(VERSION 3.16)
project(DollyForge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)  # Rust build.rs 와 같은 엄격한 -std=c99
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
endif()

# ---- C 코어 (모든 언어 도구의 기반) ----
set(DF_CORE_SOURCES core/src/json.c core/src/png.c core/src/skin.c core/src/forge.c)

add_library(dollyforge_static STATIC ${DF_CORE_SOURCES})
target_include_directories(dollyforge_static PUBLIC core/include)
set_target_properties(dollyforge_static PROPERTIES OUTPUT_NAME dollyforge POSITION_INDEPENDENT_CODE ON)

# Python(ctypes) · C#(P/Invoke) 가 여는 공유 라이브러리
add_library(dollyforge SHARED ${DF_CORE_SOURCES})
target_include_directories(dollyforge PUBLIC core/include)
set_target_properties(dollyforge PROPERTIES C_VISIBILITY_PRESET hidden VERSION ${PROJECT_VERSION} SOVERSION 0)

# ---- C++ CLI + Bedrock 패키저 ----
add_executable(dolly-forge cpp/main.cpp cpp/bedrock_pack.cpp cpp/zip_writer.cpp)
target_link_libraries(dolly-forge PRIVATE dollyforge_static)

# ---- 테스트 ----
enable_testing()
add_executable(test_core core/tests/test_core.c)
target_link_libraries(test_core PRIVATE dollyforge_static)
add_test(NAME core COMMAND test_core WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

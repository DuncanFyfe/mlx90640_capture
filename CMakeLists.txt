cmake_minimum_required(VERSION 3.10)
set( CMAKE_VERBOSE_MAKEFILE on )

project(app VERSION 1.0.0)
set(APP_SOURCE_DIR src)
configure_file("${APP_SOURCE_DIR}/mlx90640_config.h.in" mlx90640_config.h)

set(MLX_DEFAULT_CXX_STANDARD 17)
if(NOT MLX_CXX_STANDARD)
    set(MLX_CXX_STANDARD  ${MLX_DEFAULT_CXX_STANDARD} CACHE STRING "Set CXX standard" FORCE)
    set_property(CACHE MLX_CXX_STANDARD PROPERTY STRINGS "11" "17")
endif()

set(CMAKE_CXX_FLAGS "-std=c++${MLX_CXX_STANDARD} -O2 -Wall -W -Werror -fPIC")
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS src/mlx90640_buffer.proto)
# protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS DESCRIPTORS PROTO_DESCS src/mlx90640_buffer.proto)
protobuf_generate_python(PROTO_PY src/mlx90640_buffer.proto)
add_executable(mlx90640_capture ${APP_SOURCE_DIR}/mlx90640_capture.cpp ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(mlx90640_capture ${Protobuf_LIBRARIES})
target_link_libraries(mlx90640_capture Threads::Threads)
target_include_directories(mlx90640_capture PRIVATE src "${PROJECT_BINARY_DIR}")

# Add MLX90640 
find_path(MLX90640_HEADER MLX90640)
target_include_directories(mlx90640_capture PUBLIC ${MLX90640_HEADER})

find_library(MLX90640_LIB MLX90640_API)
target_link_libraries(mlx90640_capture ${MLX90640_LIB})

# Add hiredis
find_path(HIREDIS_HEADER hiredis)
target_include_directories(mlx90640_capture PUBLIC ${HIREDIS_HEADER})

find_library(HIREDIS_LIB hiredis)
target_link_libraries(mlx90640_capture ${HIREDIS_LIB})

# Add redis-plus-plus
find_path(REDIS_PLUS_PLUS_HEADER sw)
target_include_directories(mlx90640_capture PUBLIC ${REDIS_PLUS_PLUS_HEADER})

find_library(REDIS_PLUS_PLUS_LIB redis++)
target_link_libraries(mlx90640_capture ${REDIS_PLUS_PLUS_LIB})


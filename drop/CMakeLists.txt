cmake_minimum_required(VERSION 3.16)
project(drop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Protobuf + gRPC ──────────────────────────────────────────────
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(glog REQUIRED)

# Use pkg-config for libpq and libbpf
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)

# Proto generation
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common/proto)
set(GEN_DIR   ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

set(PROTOS
  ${PROTO_DIR}/common.proto
  ${PROTO_DIR}/hotmethod.proto
  ${PROTO_DIR}/healthcheck.proto
  ${PROTO_DIR}/control.proto
  ${PROTO_DIR}/init.proto
)

foreach(PROTO ${PROTOS})
  get_filename_component(BASE ${PROTO} NAME_WE)
  list(APPEND PB_CS   ${GEN_DIR}/${BASE}.pb.cc)
  list(APPEND PB_HS   ${GEN_DIR}/${BASE}.pb.h)
  list(APPEND GRPC_CS ${GEN_DIR}/${BASE}.grpc.pb.cc)
  list(APPEND GRPC_HS ${GEN_DIR}/${BASE}.grpc.pb.h)
endforeach()

add_custom_command(
  OUTPUT ${PB_CS} ${PB_HS} ${GRPC_CS} ${GRPC_HS}
  COMMAND protobuf::protoc
    --proto_path=${PROTO_DIR}
    --cpp_out=${GEN_DIR}
    --grpc_out=${GEN_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    ${PROTOS}
  DEPENDS ${PROTOS}
  COMMENT "Generating protobuf/gRPC C++ sources"
)

add_library(drop_proto ${PB_CS} ${GRPC_CS})
target_include_directories(drop_proto PUBLIC ${GEN_DIR})
target_link_libraries(drop_proto PUBLIC
  protobuf::libprotobuf
  gRPC::grpc++
  gRPC::grpc++_reflection
)

# ── Shared source (task_queue, PG store, profilers) ──────────────
add_library(drop_common
  common/src/task_queue.cpp
  common/src/perf_profiler.cpp
  common/src/async_profiler_profiler.cpp
  common/src/profiler_factory.cpp
  common/src/ebpf_loader.cpp
  common/src/ebpf_profiler.cpp
  common/src/continuous_profiler.cpp
)
target_include_directories(drop_common PUBLIC
  ${GEN_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/common/src
  ${PQ_INCLUDE_DIRS}
  ${LIBBPF_INCLUDE_DIRS}
)
target_link_libraries(drop_common PUBLIC
  drop_proto
  glog::glog
  PkgConfig::PQ
  PkgConfig::LIBBPF
)

# ── drop_server ──────────────────────────────────────────────────
add_executable(drop_server
  server/main.cpp
)
target_link_libraries(drop_server PRIVATE drop_common glog::glog gflags)
target_include_directories(drop_server PRIVATE ${GEN_DIR})

# ── drop_agent ───────────────────────────────────────────────────
add_executable(drop_agent
  agent/main.cpp
)
target_link_libraries(drop_agent PRIVATE drop_common glog::glog gflags)
target_include_directories(drop_agent PRIVATE ${GEN_DIR})

# ── Tests ────────────────────────────────────────────────────────
enable_testing()

find_package(GTest REQUIRED)

add_executable(drop_ebpf_test
  tests/ebpf_loader_test.cpp
)
target_link_libraries(drop_ebpf_test PRIVATE
  drop_common
  GTest::GTest
  GTest::gtest_main
  glog::glog
  pthread
)
target_include_directories(drop_ebpf_test PRIVATE
  ${GEN_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/common/src
)

add_executable(drop_async_profiler_test
  tests/async_profiler_test.cpp
)
target_link_libraries(drop_async_profiler_test PRIVATE
  drop_common
  GTest::GTest
  GTest::gtest_main
  glog::glog
  pthread
)
target_include_directories(drop_async_profiler_test PRIVATE
  ${GEN_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/common/src
)

add_executable(drop_core_test
  tests/core_test.cpp
)
target_link_libraries(drop_core_test PRIVATE
  drop_common
  GTest::GTest
  GTest::gtest_main
  glog::glog
  pthread
)
target_include_directories(drop_core_test PRIVATE
  ${GEN_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/common/src
)

add_test(NAME ebpf COMMAND drop_ebpf_test)
add_test(NAME async_profiler COMMAND drop_async_profiler_test)
add_test(NAME core COMMAND drop_core_test)

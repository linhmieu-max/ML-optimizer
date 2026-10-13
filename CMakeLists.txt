cmake_minimum_required(VERSION 3.20)
project(ML-Optimizer VERSION 0.10.1 LANGUAGES CXX)

# C++20 is the project baseline. std::expected is only used automatically when
# the compiler is run in C++23 mode (-DCMAKE_CXX_STANDARD=23); otherwise the
# vendored tl::expected is used behind the same opt::Result<T> alias.
set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ standard (20 or 23)")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)
include(CTest)

set(OPT_DEFAULT_CONFIG "/etc/ML-Optimizer/ML-Optimizer.conf" CACHE STRING "Config file path used by the systemd unit")
set(OPT_SYSTEMD_UNIT_DIR "/usr/lib/systemd/system" CACHE PATH "systemd system unit directory")

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
# CMake >= 4.3 renamed the imported target; older versions only know SQLite::SQLite3.
if(TARGET SQLite3::SQLite3)
  set(OPT_SQLITE_TARGET SQLite3::SQLite3)
else()
  set(OPT_SQLITE_TARGET SQLite::SQLite3)
endif()

add_library(opt_core STATIC
  src/core/logging.cpp
  src/core/sysfs.cpp
  src/core/atomic_file.cpp
  src/core/sha256.cpp
  src/core/config.cpp
  src/core/capabilities.cpp
  src/platform/package_manager.cpp
  src/ipc/protocol.cpp
  src/ipc/server.cpp
  src/ipc/client.cpp
  src/monitor/cpu_monitor.cpp
  src/monitor/memory_monitor.cpp
  src/monitor/psi_monitor.cpp
  src/monitor/thermal_monitor.cpp
  src/monitor/power_monitor.cpp
  src/monitor/io_monitor.cpp
  src/monitor/gpu_monitor.cpp
  src/monitor/system_monitor.cpp
  src/monitor/format.cpp
  src/daemon/scheduler.cpp
  src/workload/process_scanner.cpp
  src/workload/names.cpp
  src/workload/game_detector.cpp
  src/workload/browser_detector.cpp
  src/workload/compile_detector.cpp
  src/workload/container_detector.cpp
  src/workload/simple_rules.cpp
  src/workload/classifier.cpp
  src/policy/profiles.cpp
  src/policy/rules.cpp
  src/policy/feedback.cpp
  src/policy/policy_engine.cpp
  src/policy/policy_store.cpp
  src/safety/conflict_detector.cpp
  src/safety/tool_inventory.cpp
  src/safety/thermal_sensors.cpp
  src/safety/thermal_safety.cpp
  src/safety/thermal_guard.cpp
  src/safety/hardware_limits.cpp
  src/safety/rollback_journal.cpp
  src/safety/rollback.cpp
  src/safety/safety_gate.cpp
  src/control/file_state_writer.cpp
  src/control/cpufreq.cpp
  src/control/gpu.cpp
  src/control/memory.cpp
  src/control/io.cpp
  src/control/cgroup.cpp
  src/control/runtime_pm.cpp
  src/control/uclamp.cpp
  src/control/control_manager.cpp
  src/database/database.cpp
  src/database/summaries.cpp
  src/database/telemetry.cpp
  src/telemetry/ids.cpp
  src/telemetry/reward.cpp
  src/telemetry/collector.cpp
  src/ml/feature_vector.cpp
  src/ml/model.cpp
  src/ml/fit.cpp
  src/ml/trainer.cpp
  src/ml/training_gate.cpp
  src/ml/dataset.cpp
  src/ml/adaptation.cpp
  src/ml/base_model.cpp
  src/ml/predictor.cpp)
target_include_directories(opt_core PUBLIC include)
target_include_directories(opt_core SYSTEM PUBLIC third_party)
target_link_libraries(opt_core PUBLIC Threads::Threads ${OPT_SQLITE_TARGET})
target_compile_options(opt_core PUBLIC -Wall -Wextra -Wpedantic -Wshadow)

add_executable(ML-Optimizerd
  src/daemon/main.cpp
  src/daemon/daemon.cpp
  src/daemon/commands.cpp
  src/daemon/sd_notify.cpp)
target_include_directories(ML-Optimizerd PRIVATE src/daemon)
target_link_libraries(ML-Optimizerd PRIVATE opt_core)

add_executable(ML-Optimizer cli/main.cpp)
target_link_libraries(ML-Optimizer PRIVATE opt_core)

if(BUILD_TESTING)
  add_executable(opt_tests tests/test_main.cpp tests/test_tool_inventory.cpp tests/test_monitor.cpp tests/test_workload.cpp tests/test_policy.cpp tests/test_safety.cpp tests/test_control.cpp tests/test_telemetry.cpp tests/test_ml.cpp tests/test_adapt.cpp)
  target_link_libraries(opt_tests PRIVATE opt_core)
  add_test(NAME opt_tests COMMAND opt_tests)
endif()

configure_file(systemd/ML-Optimizerd.service.in ${CMAKE_BINARY_DIR}/ML-Optimizerd.service @ONLY)

install(TARGETS ML-Optimizerd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(TARGETS ML-Optimizer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_BINARY_DIR}/ML-Optimizerd.service DESTINATION ${OPT_SYSTEMD_UNIT_DIR})
# Shipped as .example so an upgrade never overwrites the admin's configuration.
install(FILES config/ML-Optimizer.conf.example DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/ML-Optimizer)

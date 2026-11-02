# Local inference's own tests: the platform pieces it runs on (a child
# process it owns, the loopback connection to its runtime, the GPU driver it
# finds at run time), the machine it plans for, and the domain itself. Each
# is a component's colocated .test.cpp, built from the sources it needs and
# run through ctest. Included from CMakeLists.txt inside its BUILD_TESTING
# block, after NATIVE_APP_HARDWARE_SOURCES is known.

# singularity_add_local_inference_test(<target> <test name> <label> <sources...>)
function(singularity_add_local_inference_test target test_name label)
  singularity_add_executable(${target} ${ARGN})
  target_include_directories(${target} PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
  target_include_directories(${target} SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
  target_compile_definitions(${target} PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  if(APPLE)
    target_link_libraries(${target} PRIVATE "-framework Metal" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ${test_name} COMMAND ${target})
  set_property(TEST ${test_name} APPEND PROPERTY LABELS unit ${label})
endfunction()

# --- Platform ---------------------------------------------------------------

singularity_add_local_inference_test(app_paths_tests app_paths platform
  Platform/AppPaths/AppPaths.test.cpp
  Platform/AppPaths/AppPaths.cpp)

singularity_add_local_inference_test(loopback_socket_tests loopback_socket platform
  Platform/LoopbackSocket/LoopbackSocket.test.cpp
  Platform/LoopbackSocket/LoopbackSocket.cpp)

# The child the contract test starts: one program, several behaviours.
singularity_add_executable(child_process_fixture
  Platform/ChildProcess/ChildProcessForTesting.cpp
  ${NATIVE_APP_CHILD_PROCESS_SOURCES}
  Platform/AppPaths/AppPaths.cpp)
target_include_directories(child_process_fixture PRIVATE "${NATIVE_APP_SOURCE_DIR}")
target_compile_definitions(child_process_fixture PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
singularity_add_local_inference_test(child_process_tests child_process platform
  Platform/ChildProcess/ChildProcess.test.cpp
  ${NATIVE_APP_CHILD_PROCESS_SOURCES}
  Platform/AppPaths/AppPaths.cpp)
target_compile_definitions(child_process_tests PRIVATE
  "CHILD_PROCESS_FIXTURE=\"$<TARGET_FILE:child_process_fixture>\"")
add_dependencies(child_process_tests child_process_fixture)

if(WIN32)
  singularity_add_local_inference_test(windows_text_tests windows_text platform
    Platform/WindowsText/WindowsText.test.cpp)
endif()

if(NOT APPLE)
  # The NVIDIA driver, wherever one is installed; a machine without one
  # checks that it reports none.
  singularity_add_local_inference_test(cuda_driver_tests cuda_driver local
    Platform/CudaDriver/CudaDriver.test.cpp
    Platform/CudaDriver/CudaDriver.cpp
    Platform/DynamicLibrary/DynamicLibrary.cpp)
endif()

# --- Node: the machine local inference plans for -----------------------------

# Detection on the real machine: consistent facts, and an accelerator offered
# only when this build's runtime was compiled for it.
singularity_add_local_inference_test(device_capabilities_tests device_capabilities local
  Node/DeviceCapabilities/DeviceCapabilities.test.cpp
  Node/DeviceCapabilities/DeviceCapabilities.cpp
  LocalModel/ExecutionBackend/ExecutionBackend.cpp
  ${NATIVE_APP_HARDWARE_SOURCES})

singularity_add_local_inference_test(machine_resource_governor_tests machine_resource_governor local
  Node/MachineResourceGovernor/MachineResourceGovernor.test.cpp
  Node/MachineResourceGovernor/MachineResourceGovernor.cpp
  LocalModel/ExecutionCompatibility/ExecutionCompatibility.cpp
  LocalModel/ExecutionBackend/ExecutionBackend.cpp
  Node/DeviceCapabilities/DeviceCapabilities.cpp)

# --- LocalInference -------------------------------------------------------------

# Local inference whole - the domain, and the LocalModel, Node and Platform
# pieces it stands on - compiled once for the tests that exercise it.
set(SINGULARITY_LOCAL_INFERENCE_SOURCES
  LocalInference/DenseExecutionStrategy/DenseExecutionStrategy.cpp
  LocalInference/DiscretePlacement/DiscretePlacement.cpp
  LocalInference/ExecutionPlanner/ExecutionPlanner.cpp
  LocalInference/GenerativeModelService/GenerativeModelService.cpp
  LocalInference/GenerativeRuntime/GenerativeRuntime.cpp
  LocalInference/MoEExecutionStrategy/MoEExecutionStrategy.cpp
  LocalInference/ModelInspector/ModelInspector.cpp
  LocalInference/PlanEvaluation/PlanEvaluation.cpp
  LocalInference/WorkloadProfile/WorkloadProfile.cpp
  LocalModel/ExecutionBackend/ExecutionBackend.cpp
  LocalModel/ExecutionCompatibility/ExecutionCompatibility.cpp
  LocalModel/LocalModelRegistry/LocalModelRegistry.cpp
  Node/DeviceCapabilities/DeviceCapabilities.cpp
  Node/HardwareCalibration/HardwareCalibration.cpp
  Node/HardwareExecutionGraph/HardwareExecutionGraph.cpp
  Node/MachineResourceGovernor/MachineResourceGovernor.cpp
  Platform/AppPaths/AppPaths.cpp
  Platform/LoopbackSocket/LoopbackSocket.cpp
  ${NATIVE_APP_CHILD_PROCESS_SOURCES}
  ${NATIVE_APP_HARDWARE_SOURCES})
list(TRANSFORM SINGULARITY_LOCAL_INFERENCE_SOURCES PREPEND "${NATIVE_APP_SOURCE_DIR}/")
add_library(local_inference_test_objects OBJECT ${SINGULARITY_LOCAL_INFERENCE_SOURCES})
target_include_directories(local_inference_test_objects PRIVATE "${NATIVE_APP_SOURCE_DIR}")
target_include_directories(local_inference_test_objects SYSTEM PRIVATE
  "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(local_inference_test_objects PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
if(MSVC)
  target_compile_options(local_inference_test_objects PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(local_inference_test_objects PRIVATE -Wall -Wextra -Wpedantic)
endif()

# singularity_add_local_inference_domain_test(<target> <test name> <test source>)
function(singularity_add_local_inference_domain_test target test_name test_source)
  singularity_add_local_inference_test(${target} ${test_name} local ${test_source})
  target_link_libraries(${target} PRIVATE local_inference_test_objects)
endfunction()

singularity_add_local_inference_domain_test(execution_planner_tests execution_planner
  LocalInference/ExecutionPlanner/ExecutionPlanner.test.cpp)
singularity_add_local_inference_domain_test(discrete_placement_tests discrete_placement
  LocalInference/DiscretePlacement/DiscretePlacement.test.cpp)
singularity_add_local_inference_domain_test(model_inspector_tests model_inspector
  LocalInference/ModelInspector/ModelInspector.test.cpp)
singularity_add_local_inference_domain_test(plan_evaluation_tests plan_evaluation
  LocalInference/PlanEvaluation/PlanEvaluation.test.cpp)
singularity_add_local_inference_domain_test(generative_runtime_tests generative_runtime
  LocalInference/GenerativeRuntime/GenerativeRuntime.test.cpp)
singularity_add_local_inference_domain_test(generative_model_service_tests generative_model_service
  LocalInference/GenerativeModelService/GenerativeModelService.test.cpp)

# The service the rest of Singularity calls, against an in-process stand-in
# runtime (LocalInferenceServiceForTesting.cpp): a model in use is never torn
# down, Stop ends a generation in flight, a transient failure is retried once.
set(SINGULARITY_LOCAL_INFERENCE_SERVICE_TEST_SOURCES
  LocalInference/LocalInferenceService/LocalInferenceService.cpp
  LocalInference/LocalInferenceService/LocalInferenceServiceForTesting.cpp)
list(TRANSFORM SINGULARITY_LOCAL_INFERENCE_SERVICE_TEST_SOURCES PREPEND "${NATIVE_APP_SOURCE_DIR}/")
singularity_add_local_inference_test(local_model_use_tests local_model_use local
  LocalInference/LocalModelUse/LocalModelUse.test.cpp)
singularity_add_local_inference_domain_test(local_inference_service_concurrency_tests
  local_inference_service_concurrency
  LocalInference/LocalInferenceService/LocalInferenceService.Concurrency.test.cpp)
target_sources(local_inference_service_concurrency_tests PRIVATE
  ${SINGULARITY_LOCAL_INFERENCE_SERVICE_TEST_SOURCES})

# Beta 1's local acceptance (labelled in CMakeLists.txt): discovery,
# registration, loading, generation and Stop through the service.
singularity_add_executable(local_inference_service_tests
  LocalInference/LocalInferenceService/LocalInferenceService.test.cpp
  ${SINGULARITY_LOCAL_INFERENCE_SERVICE_TEST_SOURCES})
target_include_directories(local_inference_service_tests PRIVATE "${NATIVE_APP_SOURCE_DIR}")
target_include_directories(local_inference_service_tests SYSTEM PRIVATE
  "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(local_inference_service_tests PRIVATE local_inference_test_objects
  nlohmann_json::nlohmann_json Threads::Threads)
target_compile_definitions(local_inference_service_tests PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
if(APPLE)
  target_link_libraries(local_inference_service_tests PRIVATE "-framework Metal" "-framework Foundation")
endif()
if(MSVC)
  target_compile_options(local_inference_service_tests PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(local_inference_service_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME beta1_local_inference_service COMMAND local_inference_service_tests)

# Manual proofs against a real installed model, never run by ctest:
# local_inference_real_inference_proof <model id, or -> <prompt> (tokens and
# Stop) and local_inference_context_accounting_proof <model id>.
foreach(proof local_inference_real_inference_proof local_inference_context_accounting_proof)
  singularity_add_executable(${proof}
    tests/${proof}.cpp
    LocalInference/LocalInferenceService/LocalInferenceService.cpp)
  target_include_directories(${proof} PRIVATE "${NATIVE_APP_SOURCE_DIR}")
  target_include_directories(${proof} SYSTEM PRIVATE
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
  target_link_libraries(${proof} PRIVATE local_inference_test_objects
    nlohmann_json::nlohmann_json Threads::Threads)
  target_compile_definitions(${proof} PRIVATE "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
  if(APPLE)
    target_link_libraries(${proof} PRIVATE "-framework Metal" "-framework Foundation")
  endif()
  if(MSVC)
    target_compile_options(${proof} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${proof} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

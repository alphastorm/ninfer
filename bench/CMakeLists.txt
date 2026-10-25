include(cmake/NinferBenchmarks.cmake)
include(ops/benchmarks.cmake)
include(inference/benchmarks.cmake)
include(context_cost/benchmarks.cmake)
include(models/qwen3_5/benchmarks.cmake)
include(models/qwen4_exp/benchmarks.cmake)

add_executable(ninfer_cpu_execution_bench cpu/execution_bench.cpp)
ninfer_internal_includes(ninfer_cpu_execution_bench)
target_link_libraries(ninfer_cpu_execution_bench PRIVATE ninfer_cpu)

add_executable(ninfer_expert_cache_bench core/expert_cache.cu)
ninfer_internal_includes(ninfer_expert_cache_bench)
target_link_libraries(ninfer_expert_cache_bench PRIVATE ninfer_artifact ninfer_core)

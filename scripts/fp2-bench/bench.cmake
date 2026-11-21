# SPDX-License-Identifier: Apache-2.0

# Load the benchmark targets after the upstream libraries are defined.
function(add_fp2_bench)
    foreach(variant IN ITEMS p324_3 p500_27 p664_17)
        add_executable(fp2_bench_${variant} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/bench.c")
        target_link_libraries(fp2_bench_${variant} PRIVATE sqisign_${variant}_nistapi)
        target_link_options(fp2_bench_${variant} PRIVATE "-Wl,--wrap=getrandom")
        target_compile_options(fp2_bench_${variant} PRIVATE -fno-unroll-loops)
    endforeach()
endfunction()

cmake_language(DEFER CALL add_fp2_bench)

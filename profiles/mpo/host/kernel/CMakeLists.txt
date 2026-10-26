# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_kernel (F08a): PSP kernel objects on minicoro fibers. No game data, no
# NID registration (that is F08b); see kernel.hpp.

add_library(mpo_minicoro STATIC minicoro_impl.cpp)
target_include_directories(mpo_minicoro SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/minicoro)

add_library(mpo_kernel STATIC
    block_allocator.cpp
    fiber.cpp
    guest_memory.cpp
    kernel_core.cpp
    kernel_memory.cpp
    kernel_sync.cpp
    kernel_thread.cpp
)
# Headers are included as "kernel/<name>.hpp".
target_include_directories(mpo_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mpo_kernel PRIVATE mpo_minicoro)
mpo_target_warnings(mpo_kernel)

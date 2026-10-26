# SPDX-License-Identifier: GPL-3.0-or-later
#
# C++ analysis tools of the profile. They run on the user's local executable
# and write to analysis/ (ignored by git); see docs/analysis/psp_analyze.md.

add_executable(mpo_analysis_export analysis_export.cpp)
target_link_libraries(mpo_analysis_export PRIVATE psprecomp_core)
mpo_target_warnings(mpo_analysis_export)

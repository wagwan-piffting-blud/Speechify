# Single combined unit-test runner. Splitting into per-suite binaries trips
# Windows Defender heuristics on tiny exes containing 'xor' / 'cipher'.

add_executable(test_common test_common.c)
target_link_libraries(test_common PRIVATE spfy_common spfy_usel)
add_test(NAME common COMMAND test_common)

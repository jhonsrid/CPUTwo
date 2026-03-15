cmake_minimum_required(VERSION 4.1)
project(CPUTwo C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")

add_executable(emulatortwo emulator.c)
add_executable(test_emulator test_emulator.c)
add_executable(assemblertwo assembler.c)
add_executable(test_assembler test_assembler.c)
add_executable(linkertwo linker.c)
add_executable(test_linker test_linker.c)

enable_testing()
add_test(NAME emulator_tests  COMMAND test_emulator)
add_test(NAME assembler_tests COMMAND test_assembler)
add_test(NAME linker_tests    COMMAND test_linker)

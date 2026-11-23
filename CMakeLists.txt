cmake_minimum_required(VERSION 3.16)
project(SystemMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pure C++ core: no OS calls, so it builds and is tested on any platform.
add_library(core STATIC
    analyzer.cpp recommend.cpp diagnosis.cpp process_types.cpp disk_scan.cpp json_report.cpp)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(core PRIVATE /W4 /utf-8)
else()
    target_compile_options(core PRIVATE -Wall -Wextra)
endif()

# Unit tests (portable). Run: ctest  or  ./core_tests
add_executable(core_tests tests_main.cpp analyzer_tests.cpp)
target_link_libraries(core_tests PRIVATE core)
enable_testing()
add_test(NAME core_tests COMMAND core_tests)

# The Windows program (monitor + process tools + CLI).
if(WIN32)
    add_executable(SystemMonitor main.cpp monitor.cpp sys_extensions.cpp analyzer_tests.cpp)
    target_link_libraries(SystemMonitor PRIVATE core)
    if(MSVC)
        target_compile_options(SystemMonitor PRIVATE /W4 /utf-8)
        # Static runtime: no extra DLLs next to the exe (smaller DLL-hijacking surface).
        set_property(TARGET SystemMonitor core core_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # asInvoker: never forces Administrator; elevation is requested only on demand.
        target_link_options(SystemMonitor PRIVATE /MANIFESTUAC:level='asInvoker'
            /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf)
        target_compile_options(SystemMonitor PRIVATE /guard:cf)
    endif()
endif()

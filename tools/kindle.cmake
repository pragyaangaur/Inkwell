# CMake toolchain for the Kindle (armv7 hard-float musl, static).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/kcross-cc)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/kcross-c++)
# The archives hold ELF objects, which only LLVM's ar and ranlib index properly.
find_program(CMAKE_AR llvm-ar PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
find_program(CMAKE_RANLIB llvm-ranlib PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

cmake_minimum_required(VERSION 3.25)

project(sway-master-stack
    VERSION 2.0.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_C REQUIRED json-c)

# Централизованный интерфейс настроек тулчейна под Zen4
add_library(sys_compiler_flags INTERFACE)

target_compile_options(sys_compiler_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wundef
    -Wformat=2
    -Werror=format-security
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    $<$<CONFIG:Debug>:-mno-omit-leaf-frame-pointer>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-pipe>
    $<$<CONFIG:Release>:-fno-plt>
    $<$<CONFIG:Release>:-fstack-clash-protection>
    $<$<CONFIG:Release>:-fcf-protection>
)

# Разделение флагов LTO и линкера для Clang и GCC
if (CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(sys_compiler_flags INTERFACE
        $<$<CONFIG:Release>:-flto=thin>
    )
  target_link_options(sys_compiler_flags INTERFACE
        -fuse-ld=lld
        $<$<CONFIG:Release>:-flto=thin>
    )
else()
  target_compile_options(sys_compiler_flags INTERFACE
        $<$<CONFIG:Release>:-flto=auto>
    )
  target_link_options(sys_compiler_flags INTERFACE
        $<$<CONFIG:Release>:-flto=auto>
    )
endif()

target_compile_definitions(sys_compiler_flags INTERFACE
    $<$<CONFIG:Release>:_FORTIFY_SOURCE=3>
)

target_link_options(sys_compiler_flags INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Release>:-Wl,-O1>
    $<$<CONFIG:Release>:-Wl,--as-needed>
    $<$<CONFIG:Release>:-Wl,-z,relro>
    $<$<CONFIG:Release>:-Wl,-z,now>
    $<$<CONFIG:Release>:-Wl,-z,pack-relative-relocs>
)

# Таргет демона sway-master-stack
add_executable(${PROJECT_NAME} src/main.c)
target_link_libraries(${PROJECT_NAME} PRIVATE sys_compiler_flags ${JSON_C_LIBRARIES})
target_include_directories(${PROJECT_NAME} PRIVATE ${JSON_C_INCLUDE_DIRS})

# Установка
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})

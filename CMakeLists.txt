cmake_minimum_required(VERSION 3.25)

project(sway-master-stack
    VERSION 2.0.1
    LANGUAGES C
)

# Без -DCMAKE_BUILD_TYPE генератор-выражения ниже пусты и сборка молча
# выходит неоптимизированной — дефолтим Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_C REQUIRED IMPORTED_TARGET json-c)

# LTO — штатный механизм CMake: clang получает -flto=thin, gcc — -flto=auto,
# флаги линковки CMake добавляет сам (ручные -flto в опциях таргета его
# дублировали бы).
include(CheckIPOSupported)
check_ipo_supported(RESULT SMS_IPO OUTPUT SMS_IPO_LOG)
if(SMS_IPO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
  message(STATUS "IPO/LTO не поддерживается тулчейном: ${SMS_IPO_LOG}")
endif()

# Тюнинг под хост сборки (Zen4). Для переносимого бинарника: -DSMS_NATIVE=OFF
option(SMS_NATIVE "Tune codegen for the build host (-march=native)" ON)

# Централизованный интерфейс настроек тулчейна
add_library(sys_compiler_flags INTERFACE)

target_compile_options(sys_compiler_flags INTERFACE
    # Диагностика — во всех конфигурациях
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wundef
    -Wformat=2
    -Werror=format-security
    -Wstrict-prototypes
    -Wold-style-definition

    # Харденинг — дёшево, нужен не только в Release
    -fstack-protector-strong
    -fstack-clash-protection
    -fcf-protection

    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    $<$<CONFIG:Debug>:-mno-omit-leaf-frame-pointer>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>

    # Оптимизация — все оптимизированные конфиги (Release/RelWithDebInfo/MinSizeRel)
    $<$<NOT:$<CONFIG:Debug>>:-O3>
    $<$<NOT:$<CONFIG:Debug>>:-pipe>
    $<$<NOT:$<CONFIG:Debug>>:-fno-plt>
    $<$<NOT:$<CONFIG:Debug>>:-fno-semantic-interposition>
    $<$<NOT:$<CONFIG:Debug>>:-ffunction-sections>
    $<$<NOT:$<CONFIG:Debug>>:-fdata-sections>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<BOOL:${SMS_NATIVE}>>:-march=native>
)

target_compile_definitions(sys_compiler_flags INTERFACE
    # FORTIFY без оптимизации бесполезен (glibc даёт #warning) — только
    # в оптимизированных конфигах
    $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=3>
)

target_link_options(sys_compiler_flags INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>

    # Харденинг линка — всегда
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,--as-needed

    # Оптимизация линка: --gc-sections вместе с -ffunction-sections/
    # -fdata-sections вырезает мёртвый код
    $<$<NOT:$<CONFIG:Debug>>:-Wl,-O1>
    $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>
    $<$<NOT:$<CONFIG:Debug>>:-Wl,-z,pack-relative-relocs>
)

if (CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_link_options(sys_compiler_flags INTERFACE
        # lld — в зависимостях проекта
        -fuse-ld=lld
        # Схлопывание идентичных функций (только lld)
        $<$<NOT:$<CONFIG:Debug>>:-Wl,--icf=all>
    )
endif()

# Таргет демона sway-master-stack
add_executable(${PROJECT_NAME} src/main.c)
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    sys_compiler_flags
    PkgConfig::JSON_C
)

# Установка
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

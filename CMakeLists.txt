# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.1.2")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# 设置ESP-ADF路径
# if(DEFINED ENV{ADF_PATH})
#     set(EXTRA_COMPONENT_DIRS $ENV{ADF_PATH}/components)
#     message(STATUS "ESP-ADF path: $ENV{ADF_PATH}")
# else()
#     set(ADF_PATH "D:/esp/.espressif/esp-adf")
#     set(EXTRA_COMPONENT_DIRS ${ADF_PATH}/components)
#     message(STATUS "Using default ESP-ADF path: ${ADF_PATH}")
# endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# 添加SPIFFS支持 - 使用storage分区（主要方案）
spiffs_create_partition_image(storage data/video FLASH_IN_PROJECT)

# 可选：如果需要FAT支持，取消下面的注释
# fatfs_create_spiflash_image(storage data/video FLASH_IN_PROJECT)


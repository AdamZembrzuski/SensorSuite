cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(v0)

target_sources(app PRIVATE 
  src/main.c
  src/services/sense_service.c
  src/services/time_service.c
  src/services/log_service.c
  src/services/error_service.c
  src/vl53l4cd_ulp_api/VL53L4CD_ULP_api.c
  src/vl53l4cd_ulp_api/VL53L4CD_ULP_platform.c
)

target_include_directories(app PUBLIC 
  src/services
  src/vl53l4cd_ulp_api
)

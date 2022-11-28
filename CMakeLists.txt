
cmake_minimum_required(VERSION 3.20) 
project(pueodaq) 

add_executable(turfid src/turfid.c) 

include_directories(src) 
set(LIBSRC src/pueodaq.c)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Default build type: RelWithDebInfo" FORCE) 

endif()

# build both shared and static libs
add_library(pueodaq SHARED ${LIBSRC})
add_library(pueodaq_static STATIC ${LIBSRC})





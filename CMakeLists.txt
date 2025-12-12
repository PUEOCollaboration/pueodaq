cmake_minimum_required(VERSION 3.20) 
project(pueodaq) 


## Set build type #
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Default build type: RelWithDebInfo" FORCE) 

endif()

set_property(GLOBAL PROPERTY C_STANDARD 23)

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)
## defines

include_directories(src) 
set(LIBSRC src/pueodaq.c src/pueodaq-net.c src/pueodaq-util.c src/pueodaq-cfg.c src/daqregs.c src/pueodaq-biquad.c src/pueodaq-decode.c src/pueodaq-corruption.c)
set(PUEODAQ_PUBLIC_HEADERS src/pueodaq.h src/pueo-biquads.h src/pueo-biquad-coeffs.h src/pueo-biquad-limits.h) 
set(PUEODAQ_PRIVATE_HEADERS src/pueodaq-test.h src/pueodaq-net.h src/pueodaq-util.h src/daqregs.h src/pueodaq-private.h)
set(WARNFLAGS -Wall -Wextra -Wno-missing-braces -Wno-override-init)


# build both shared and static libs
add_library(pueodaq SHARED ${LIBSRC})
target_compile_options(pueodaq PRIVATE ${WARNFLAGS})

add_library(pueodaq_static STATIC ${LIBSRC})
target_compile_options(pueodaq_static PRIVATE ${WARNFLAGS})


#################### Tests and test executables######################
add_executable(turfid test/turfid.c) 
add_executable(route-for-addr test/route-for-addr.c src/pueodaq-net.c src/pueodaq-util.c) 
add_executable(pueodaq-test test/pueodaq-test.c ${LIBSRC})
add_executable(pueo-fakedaq test/pueo-fakedaq.c)
target_compile_options(pueodaq-test PRIVATE -DPUEODAQTEST_ENABLE ${WARNFLAGS})
target_link_libraries(pueo-fakedaq Threads::Threads pueodaq m)
target_link_libraries(pueodaq-test Threads::Threads m )
add_custom_target(test COMMAND pueodaq-test)
add_custom_target(memtest COMMAND valgrind --track-origins=yes pueodaq-test) 



################## Analyzers ###################

add_custom_target(cppcheck COMMAND cppcheck --enable=all ${CMAKE_CURRENT_SOURCE_DIR}/src) 



######################Docs ############################

FUNCTION(PREPEND var prefix)
   SET(listVar "")
   FOREACH(f ${ARGN})
      LIST(APPEND listVar "${prefix}/${f}")
   ENDFOREACH(f)
   SET(${var} "${listVar}" PARENT_SCOPE)
ENDFUNCTION(PREPEND)

PREPEND(DOXY_IN ${CMAKE_CURRENT_SOURCE_DIR} ${PUEODAQ_PUBLIC_HEADERS})

set(DOXY_OUTDIR ${CMAKE_CURRENT_BINARY_DIR}/docs/doxygen/)
set(DOXY_INDEX ${DOXY_OUTDIR}/html/index.html)
set(DOXYFILE_IN ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
set(DOXYFILE_OUT ${CMAKE_CURRENT_BINARY_DIR}/docs/Doxyfile)
configure_file(${DOXYFILE_IN} ${DOXYFILE_OUT} @ONLY)


file (MAKE_DIRECTORY ${DOXY_OUTDIR})


add_custom_command(OUTPUT  ${DOXY_INDEX}
                   DEPENDS ${PUEODAQ_PUBLIC_HEADERS} 
                   COMMAND doxygen ${DOXYFILE_OUT}
                   MAIN_DEPENDENCY ${DOXYFILE_OUT} ${DOXYFILE_IN}
                   COMMENT "Running Doxygen") 


add_custom_target (doxygen DEPENDS ${DOXY_INDEX})

set (SPHINX_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/docs)
set (SPHINX_BUILD ${CMAKE_CURRENT_BINARY_DIR}/docs/sphinx)

add_custom_target(sphinx 
                  COMMAND sphinx-build -b html -Dbreathe_projects.pueodaq=${DOXY_OUTDIR}/xml ${SPHINX_SOURCE} ${SPHINX_BUILD} 
                  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} 
                  COMMENT "Running sphinx")








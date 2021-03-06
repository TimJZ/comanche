project(comanche-nvmestore CXX)

set(GCC_COVERAGE_COMPILE_FLAGS "-std=c++11 -g -O2 -fPIC -msse3")

include_directories(${CMAKE_INSTALL_PREFIX}/include)
link_directories(${CMAKE_INSTALL_PREFIX}/lib)
link_directories(${CMAKE_INSTALL_PREFIX}/lib64)

include_directories(${PMDK_PREFIX}/src/examples/libpmemobj/hashmap/)

include_directories(./src)

# add_definitions(${GCC_COVERAGE_COMPILE_FLAGS} -DCONFIG_DEBUG)

file(GLOB SOURCES 
  ./src/block_manager.cpp
  ./src/nvme_store.cpp
  ./src/persist_session.cpp
  ./src/state_map.cpp)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

# target_compile_options(${PROJECT_NAME} PUBLIC -g -pedantic -Wall -Werror -Wextra -Wcast-align -Wcast-qual -Wconversion -Weffc++ -Wold-style-cast -Wredundant-decls -Wshadow -Wtype-limits -Wunused-parameter -Wwrite-strings)

target_link_libraries(${PROJECT_NAME} common comanche-core pthread numa dl rt boost_system boost_filesystem pmem pmemobj pmempool tbb cityhash)

set_target_properties(${PROJECT_NAME} PROPERTIES 
  INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib;${CMAKE_INSTALL_PREFIX}/lib64;${PMDK_PREFIX}/src/nondebug")

install(TARGETS ${PROJECT_NAME}
    LIBRARY 
    DESTINATION lib)

add_subdirectory(unit_test)

# some experimental testing
add_subdirectory(testing)

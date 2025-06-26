add_executable(rldp-http-proxy rldp-http-proxy.cpp DNSResolver.h DNSResolver.cpp)
target_include_directories(rldp-http-proxy PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(rldp-http-proxy PRIVATE tonhttp rldp rldp2 dht tonlib git ${JEMALLOC_LIBRARIES})

if (JEMALLOC_FOUND)
  target_include_directories(rldp-http-proxy PRIVATE ${JEMALLOC_INCLUDE_DIR})
  target_compile_definitions(rldp-http-proxy PRIVATE -DTON_USE_JEMALLOC=1)
endif()

install(TARGETS rldp-http-proxy RUNTIME DESTINATION bin)

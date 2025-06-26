if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()


set (DHT_SERVER_SOURCE
  dht-server.cpp
  dht-server.hpp
)

add_executable (dht-server ${DHT_SERVER_SOURCE})
target_link_libraries(dht-server tdutils tdactor adnl tl_api dht memprof git ${JEMALLOC_LIBRARIES})

if (JEMALLOC_FOUND)
  target_include_directories(dht-server PRIVATE ${JEMALLOC_INCLUDE_DIR})
  target_compile_definitions(dht-server PRIVATE -DTON_USE_JEMALLOC=1)
endif()

install(TARGETS dht-server RUNTIME DESTINATION bin)

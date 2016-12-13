PROJECT(EventuallyPersistentEngine)
CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

IF (${CMAKE_MAJOR_VERSION} GREATER 2)
    CMAKE_POLICY(SET CMP0042 NEW)
ENDIF (${CMAKE_MAJOR_VERSION} GREATER 2)

INCLUDE(CheckFunctionExists)
INCLUDE(CheckIncludeFileCXX)
INCLUDE(CheckIncludeFiles)
INCLUDE(CheckLibraryExists)
INCLUDE(CheckSymbolExists)
INCLUDE(CheckTypeSize)
INCLUDE(CTest)

INCLUDE_DIRECTORIES(BEFORE ${CMAKE_INSTALL_PREFIX}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_BINARY_DIR}/src
                           ${SNAPPY_INCLUDE_DIR}
                           ${Platform_SOURCE_DIR}/include
                           ${Memcached_SOURCE_DIR}
                           ${Memcached_SOURCE_DIR}/include
                           ${Couchstore_SOURCE_DIR}/include
                           ${ForestDB_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_BINARY_DIR})

INCLUDE_DIRECTORIES(AFTER
                    ${gtest_SOURCE_DIR}/include)

CHECK_INCLUDE_FILES("alloca.h" HAVE_ALLOCA_H)
CHECK_INCLUDE_FILES("arpa/inet.h" HAVE_ARPA_INET_H)
CHECK_INCLUDE_FILES("unistd.h" HAVE_UNISTD_H)
CHECK_INCLUDE_FILES("netdb.h" HAVE_NETDB_H)
CHECK_INCLUDE_FILES("mach/mach_time.h" HAVE_MACH_MACH_TIME_H)
CHECK_INCLUDE_FILES("poll.h" HAVE_POLL_H)
CHECK_INCLUDE_FILES("sysexits.h" HAVE_SYSEXITS_H)
CHECK_INCLUDE_FILES("unistd.h" HAVE_UNISTD_H)
CHECK_INCLUDE_FILES("sched.h" HAVE_SCHED_H)
CHECK_INCLUDE_FILES("sys/socket.h" HAVE_SYS_SOCKET_H)
CHECK_INCLUDE_FILES("sys/time.h" HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILES("netinet/in.h" HAVE_NETINET_IN_H)
CHECK_INCLUDE_FILES("netinet/tcp.h" HAVE_NETINET_TCP_H)
CHECK_INCLUDE_FILE_CXX("unordered_map" HAVE_UNORDERED_MAP)
CHECK_INCLUDE_FILE_CXX("atomic" HAVE_ATOMIC)
CHECK_INCLUDE_FILE_CXX("thread" HAVE_THREAD)

CHECK_FUNCTION_EXISTS(clock_gettime HAVE_CLOCK_GETTIME)
CHECK_FUNCTION_EXISTS(mach_absolute_time HAVE_MACH_ABSOLUTE_TIME)
CHECK_FUNCTION_EXISTS(gettimeofday HAVE_GETTIMEOFDAY)
CHECK_FUNCTION_EXISTS(getopt_long HAVE_GETOPT_LONG)

# ---- uncomment the lines below ONLY for dev/debugging ---
#if ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
#    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
#elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
#    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
#endif()

IF (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
   EXECUTE_PROCESS(COMMAND git rev-parse HEAD
                   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                   OUTPUT_VARIABLE EP_ENGINE_VERSION
                   OUTPUT_STRIP_TRAILING_WHITESPACE)
   IF (EP_ENGINE_VERSION STREQUAL "" OR EP_ENGINE_VERSION STREQUAL "HEAD")
     MESSAGE (FATAL_ERROR "Failed to determine commit SHA!")
   ENDIF (EP_ENGINE_VERSION STREQUAL "" OR EP_ENGINE_VERSION STREQUAL "HEAD")
ELSE (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
   SET(EP_ENGINE_VERSION "unknown")
ENDIF (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)

CONFIGURE_FILE (${CMAKE_CURRENT_SOURCE_DIR}/src/config.cmake.h
                ${CMAKE_CURRENT_BINARY_DIR}/src/config.h)

# Generate the python wrappers
CONFIGURE_FILE (${CMAKE_CURRENT_SOURCE_DIR}/wrapper/wrapper
                ${CMAKE_CURRENT_BINARY_DIR}/wrapper/cbepctl)
CONFIGURE_FILE (${CMAKE_CURRENT_SOURCE_DIR}/wrapper/wrapper
                ${CMAKE_CURRENT_BINARY_DIR}/wrapper/cbstats)
CONFIGURE_FILE (${CMAKE_CURRENT_SOURCE_DIR}/wrapper/wrapper
                ${CMAKE_CURRENT_BINARY_DIR}/wrapper/cbcompact)
CONFIGURE_FILE (${CMAKE_CURRENT_SOURCE_DIR}/wrapper/wrapper
                ${CMAKE_CURRENT_BINARY_DIR}/wrapper/cbvdiff)
CONFIGURE_FILE (${CMAKE_CURRENT_SOURCE_DIR}/wrapper/wrapper
                ${CMAKE_CURRENT_BINARY_DIR}/wrapper/cbvbucketctl)

IF (WIN32)
   INCLUDE_DIRECTORIES(AFTER ${CMAKE_SOURCE_DIR}/platform/include/win32)
ENDIF (WIN32)

ADD_EXECUTABLE(gencode tools/gencode.cc)
TARGET_LINK_LIBRARIES(gencode cJSON platform)

ADD_EXECUTABLE(genconfig tools/genconfig.cc)
TARGET_LINK_LIBRARIES(genconfig cJSON platform)

ADD_CUSTOM_COMMAND(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/src/stats-info.c
                          ${CMAKE_CURRENT_BINARY_DIR}/src/stats-info.h
                  COMMAND
                     gencode -j ${CMAKE_CURRENT_SOURCE_DIR}/docs/stats.json -h ${CMAKE_CURRENT_BINARY_DIR}/src/stats-info.h -c ${CMAKE_CURRENT_BINARY_DIR}/src/stats-info.c -f get_stats_info
                  DEPENDS
                        docs/stats.json
                        gencode
                  COMMENT "Generating code for stats info")

ADD_CUSTOM_COMMAND(OUTPUT
                     ${CMAKE_CURRENT_BINARY_DIR}/src/generated_configuration.cc
                     ${CMAKE_CURRENT_BINARY_DIR}/src/generated_configuration.h
                  COMMAND
                     genconfig ${CMAKE_CURRENT_SOURCE_DIR}/configuration.json
                  DEPENDS
                        configuration.json
                        genconfig
                  COMMENT "Generating code for configuration class")

SET(KVSTORE_SOURCE src/crc32.c src/kvstore.cc)
SET(COUCH_KVSTORE_SOURCE src/couch-kvstore/couch-kvstore.cc
            src/couch-kvstore/couch-fs-stats.cc)
SET(FOREST_KVSTORE_SOURCE src/forest-kvstore/forest-kvstore.cc)
SET(OBJECTREGISTRY_SOURCE src/objectregistry.cc)
SET(CONFIG_SOURCE src/configuration.cc
  ${CMAKE_CURRENT_BINARY_DIR}/src/generated_configuration.cc)

ADD_LIBRARY(ep SHARED
            src/expiry_channel.cc
            src/access_scanner.cc
            src/atomic.cc
            src/backfill.cc
            src/bgfetcher.cc
            src/bloomfilter.cc
            src/checkpoint.cc
            src/checkpoint_remover.cc
            src/compress.cc
            src/conflict_resolution.cc
            src/connmap.cc
            src/dcp/backfill-manager.cc
            src/dcp/backfill.cc
            src/dcp/consumer.cc
            src/dcp/flow-control.cc
            src/dcp/flow-control-manager.cc
            src/dcp/producer.cc
            src/dcp/response.cc
            src/dcp/stream.cc
            src/defragmenter.cc
            src/defragmenter_visitor.cc
            src/ep.cc
            src/ep_engine.cc
            src/ep_time.c
            src/executorpool.cc
            src/executorthread.cc
            src/ext_meta_parser.cc
            src/failover-table.cc
            src/flusher.cc
            src/htresizer.cc
            src/item.cc
            src/item_pager.cc
            src/logger.cc
            src/kvshard.cc
            src/memory_tracker.cc
            src/murmurhash3.cc
            src/mutation_log.cc
            src/replicationthrottle.cc
            src/sizes.cc
            ${CMAKE_CURRENT_BINARY_DIR}/src/stats-info.c
            src/string_utils.cc
            src/stored-value.cc
            src/tapconnection.cc
            src/tasks.cc
            src/taskqueue.cc
            src/vbucket.cc
            src/vbucketmap.cc
            src/warmup.cc
            ${KVSTORE_SOURCE} ${COUCH_KVSTORE_SOURCE}
            ${FOREST_KVSTORE_SOURCE} ${OBJECTREGISTRY_SOURCE}
            ${CONFIG_SOURCE})

SET_TARGET_PROPERTIES(ep PROPERTIES PREFIX "")
TARGET_LINK_LIBRARIES(ep cJSON JSON_checker couchstore forestdb
  dirutils platform ${LIBEVENT_LIBRARIES})

INSTALL(FILES
        docs/stats.org
        DESTINATION share/doc/ep-engine)


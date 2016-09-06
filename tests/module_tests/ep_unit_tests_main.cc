/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/*
 * Main function & globals for the ep_unit_test target.
 */

#include <memcached/extension_loggers.h>
#include "programs/engine_testapp/mock_server.h"

#include <getopt.h>
#include <gtest/gtest.h>

#include "configuration.h"
#include "logger.h"
#include "hash_table.h"

/* static storage for environment variable set by putenv(). */
static char allow_no_stats_env[] = "ALLOW_NO_STATS_UPDATE=yeah";

#ifdef HAVE_JEMALLOC
/* Global replacement of operators new and delete when using jemalloc.
 *
 * This isn't needed for functionality reasons (the code works
 * correctly without these), but to improve interoperability with
 * Valgrind. The issue encountered is that without these replacements,
 * the application will call the standard symbols in libstdc++, which
 * Valgrind intercepts, and replaces with it's own implementation
 * (which doesn't call 'our' je_malloc). As a consequence when the
 * memory tracking code (objectregistery.cc) later calls
 * je_malloc_usable_size() on the returned pointer from Valgrind's
 * operator new, we encounter invalid memory accesses as jemalloc is
 * trying to introspect a non-jemalloc allocation.
 *
 * By replacing operator new/delete in the executable program, we call
 * our own malloc/free (see alloc_hooks.c) which directly call
 * jemalloc.  Note that Valgrind reporting still works, as je_malloc
 * is able to detect which running in Valgrind and makes the
 * appropriate calls to Valgrind to inform it of allocations / frees.
 */
void* operator new(std::size_t count ) {
    return malloc(count);
}

void operator delete(void* ptr ) noexcept
{
    free(ptr);
}
#endif // HAVE_JEMALLOC

int main(int argc, char **argv) {
    bool log_to_stderr = false;
    // Parse command-line options.
    int cmd;
    bool invalid_argument = false;
    while (!invalid_argument &&
           (cmd = getopt(argc, argv, "v")) != EOF) {
        switch (cmd) {
        case 'v':
            log_to_stderr = true;
            break;
        default:
            std::cerr << "Usage: " << argv[0] << " [-v] [gtest_options...]" << std::endl
                      << std::endl
                      << "  -v Verbose - Print verbose output to stderr."
                      << std::endl << std::endl;
            invalid_argument = true;
            break;
        }
    }

    putenv(allow_no_stats_env);

    mock_init_alloc_hooks();
    init_mock_server(log_to_stderr);

    get_mock_server_api()->log->set_level(EXTENSION_LOG_DEBUG);
    if (memcached_initialize_stderr_logger(get_mock_server_api) != EXTENSION_SUCCESS) {
        std::cerr << argv[0] << ": Failed to initialize log system" << std::endl;
        return 1;
    }
    Logger::setLoggerAPI(get_mock_server_api()->log);

    // Default number of hashtable locks is too large for TSan to
    // track. Use the value in configuration.json (47 at time of
    // writing).
    HashTable::setDefaultNumLocks(Configuration().getHtLocks());

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

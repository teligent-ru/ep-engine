/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc.
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
#ifndef SRC_EP_TIME_H
#define SRC_EP_TIME_H 1

#include "config.h"

#include <memcached/server_api.h>
#include <memcached/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the below time functions using the function pointers
 * provided by the specified SERVER_CORE_API. This function should be
 * called before attempting to use them, typically by the first engine
 * loaded.
 * Note: Only the first call to this function will have any effect,
 * i.e.  once initialized the functions should not be modified to
 * prevent data races between the different threads which use them.
 */
void initialize_time_functions(const SERVER_CORE_API* core_api);

extern rel_time_t (*ep_current_time)(void);
extern time_t (*ep_abs_time)(rel_time_t);
extern rel_time_t (*ep_reltime)(time_t);
extern time_t ep_real_time(void);

#ifdef __cplusplus
}
#endif

#endif  /* SRC_EP_TIME_H */

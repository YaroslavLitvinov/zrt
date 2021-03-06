/*
 *
 * Copyright (c) 2013, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef PRECACHE_OBSERVER_H_
#define PRECACHE_OBSERVER_H_

#define HANDLE_ONLY_PRECACHE_SECTION get_precache_observer()

#define PRECACHE_SECTION_NAME         "precache"
#define PRECACHE_PARAM_PRECACHE_KEY   "precache"

#include "nvram_observer.h"

/*get static interface, object not intended to destroy after using*/
struct MNvramObserver* get_precache_observer();

#endif /* PRECACHE_OBSERVER_H_ */

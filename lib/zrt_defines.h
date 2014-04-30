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

#ifndef __ZRT_DEFINES_H__
#define __ZRT_DEFINES_H__

/*execute constructor function*/
#define CONSTRUCT_L(function_return_object_p) function_return_object_p
/*get instance for singleton*/
#define INSTANCE_L(function_return_object_p) function_return_object_p

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

#define __NON_INSTRUMENT_FUNCTION__ __attribute__((__no_instrument_function__))
#define __CONSTRUCTOR_FUNCTION__ __attribute__((constructor))
#define __DESTRUCTOR_FUNCTION__ __attribute__((destructor))

#endif //ZRT_CONFIG_H

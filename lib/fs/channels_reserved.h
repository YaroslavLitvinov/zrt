/*
 * channels_reserved.h
 *
 * Copyright (c) 2012-2013, LiteStack, Inc.
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

#ifndef __CHANNELS_RESERVED_H__
#define __CHANNELS_RESERVED_H__

/*reserved channels list*/
#define DEV_STDIN  "/dev/stdin"
#define DEV_STDOUT "/dev/stdout"
#define DEV_STDERR "/dev/stderr"
#define DEV_NVRAM  "/dev/nvram"
#define DEV_DEBUG  "/dev/debug"
#define DEV_TRACE  "/dev/trace" //trace report for command 'make trace'
#define DEV_ALLOC_REPORT "/dev/alloc_report" //report to the channel about memory leaks in user session

#endif //__CHANNELS_RESERVED_H__

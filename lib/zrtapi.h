/*
 * ZRT API
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

#ifndef __ZRT_API_H__
#define __ZRT_API_H__

/*call zvm_fork() and then reread nvram file and remount removable tar images
 *@return zvm_fork result*/
int zfork();

/*It is intended to use for debugging purposes when using c code
 instrumentation aka ptrace; Tracing is not allowed while environment 
 not fully constructed.
 @return 1 if ptrace is allowed, 0 - not allowed*/
int is_ptrace_allowed() __attribute__((__no_instrument_function__));

/*Currently executing user main() or not. If zrt initialization code
or session finalizing is running then it should return 0; It's normal
always to get 1 calling it from user code.  This function is needed to
distinguish user and system routines for cases when running various
trace, hook functions; 
@return 1 if now executing user code in main function, 0 if not*/
int is_user_main_executing();


#endif //__ZRT_API_H__

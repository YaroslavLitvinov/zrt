/*
 * Filesystem interface implementation. 
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

#ifndef TRANSPARENT_MOUNT_H_
#define TRANSPARENT_MOUNT_H_

struct MountsPublicInterface;
struct MountsManager;

struct MountsPublicInterface* alloc_transparent_mount( struct MountsManager* mounts_manager );

#endif /* TRANSPARENT_MOUNT_H_ */

/*
 * mount_specific_interface.h
 * Interface for functions whose implemetation is different for
 * various mounts: channels and MemMount;
 *
 * Copyright (c) 2012-2013, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this_ file except in compliance with the License.
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


#ifndef __MOUNT_SPECIFIC_INTERFACE_H__
#define __MOUNT_SPECIFIC_INTERFACE_H__

#include <unistd.h>
#include <fcntl.h>

#include "zrt_defines.h" //CONSTRUCT_L

/*name of constructor*/
#define MOUNT_SPECIFIC mount_specific_construct 

struct MountSpecificPublicInterface{
    /*return 0 if handle not valid, or 1 if handle is correct*/
    int  (*check_handle)(struct MountSpecificPublicInterface* this_, int handle);
    /*if wrong handle return NULL*/
    const char* (*handle_path)(struct MountSpecificPublicInterface* this_, int handle);
    /*flags was specified at file opening
     *@param handle fd of opened file
     *@return -1 of bad handle, 0 if OK*/
    int  (*file_status_flags)(struct MountSpecificPublicInterface* this_, int handle);
    int  (*set_file_status_flags)(struct MountSpecificPublicInterface* this_, int handle, int flags);

    const struct flock* (*flock_data)( struct MountSpecificPublicInterface* this_, int fd );
    int (*set_flock_data)( struct MountSpecificPublicInterface* this_, int fd, const struct flock* flock_data );
};


#endif //__MOUNT_SPECIFIC_INTERFACE_H__


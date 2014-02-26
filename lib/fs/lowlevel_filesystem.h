/*
 * Public filesystem interface
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

#ifndef __LOWLEVEL_FILESYSTEM_H__
#define __LOWLEVEL_FILESYSTEM_H__

#include <stdint.h>
#include <stddef.h> //size_t
#include <unistd.h> //ssize_t

struct stat;

struct TopLevelFilesystemObserverInterface{
    size_t (*dirent_size)(size_t namelen);
    char * (*add_dirent)(char *buf, 
			 const char *name,
			 const struct stat *stbuf, 
			 off_t off);
    const struct OpenFileDescription* (*ofd)(int fd, 
					     ino_t inode);
};

struct LowLevelFilesystemPublicInterface{
    int (*chown)(struct LowLevelFilesystemPublicInterface* this_, 
		 ino_t inode, uid_t owner, gid_t group);
    int (*chmod)(struct LowLevelFilesystemPublicInterface* this_, 
		 ino_t inode, uint32_t mode);
    int (*stat)(struct LowLevelFilesystemPublicInterface* this_, 
		ino_t inode, struct stat *buf);
    int (*mkdir)(struct LowLevelFilesystemPublicInterface* this_, 
		 ino_t parent_inode, const char* name, uint32_t mode);
    int (*rmdir)(struct LowLevelFilesystemPublicInterface* this_, 
		 ino_t parent_inode, const char* name);
    ssize_t (*pread)(struct LowLevelFilesystemPublicInterface* this_,
		     ino_t inode, void *buf, size_t nbyte, off_t offset);
    ssize_t (*pwrite)(struct LowLevelFilesystemPublicInterface* this_,
		      ino_t inode, const void *buf, size_t nbyte, off_t offset);
    int (*getdents)(struct LowLevelFilesystemPublicInterface* this_, 
		    ino_t inode, void *buf, unsigned int count);
    int (*fsync)(struct LowLevelFilesystemPublicInterface* this_, 
		 ino_t inode);
    int (*close)(struct LowLevelFilesystemPublicInterface* this_, ino_t inode);
    int (*open)(struct LowLevelFilesystemPublicInterface* this_, 
		ino_t parent_inode, const char* name, int oflag, uint32_t mode);
    int (*unlink)(struct LowLevelFilesystemPublicInterface* this_, 
		  ino_t parent_inode, const char* name);
    int (*link)(struct LowLevelFilesystemPublicInterface* this_, 
		ino_t inode, ino_t new_parent, const char *newname);
    int (*rename)(struct LowLevelFilesystemPublicInterface* this_, 
		      fuse_ino_t parent, const char *name,
		      ino_t new_parent, const char *newname);
    int (*ftruncate_size)(struct LowLevelFilesystemPublicInterface* this_, 
			  ino_t inode, off_t length);
    struct TopLevelFilesystemObserverInterface* toplevelfs;
};


#endif //__LOWLEVEL_FILESYSTEM_H__

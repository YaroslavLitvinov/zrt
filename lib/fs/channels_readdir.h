/*
 * Readdir implementation for channels filesystem
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

#ifndef CHANNELS_READDIR_H_
#define CHANNELS_READDIR_H_

#include <stddef.h> //size_t 

#define DIRECTORIES_MAX_NUMBER 255

//forwards
struct ChannelsArrayPublicInterface;

struct dir_data_t {
    /*directory dir_inode should be in range starting just after channels fd range;
     * can be used also to get inode by INODE_FROM_ZVM_INODE*/
    int dir_inode;
    int parent_dir_inode;
    int nlink;
    char* path;
};

struct manifest_loaded_directories_t{
    struct dir_data_t dir_array[DIRECTORIES_MAX_NUMBER];
    int dircount;
};

/*Get shortname from full name*/
const char* name_from_path_get_path_len(const char *fullpath, int *pathlen);

/*search dir in dir list*/
struct dir_data_t *
match_dir_in_directory_list(struct manifest_loaded_directories_t *manifest_dirs, const char *dirpath, int len);

/*search dir inode in dir list*/
struct dir_data_t *
match_inode_in_directory_list(struct manifest_loaded_directories_t *manifest_dirs, int inode);


/*reading channels list, fetch directories from channel path and add to manifest_dirs,
 * manifest_dirs should point to valid pointer*/
void process_channels_create_dir_list( const struct ChannelsArrayPublicInterface *channels, 
				       struct manifest_loaded_directories_t *manifest_dirs );

/* Search subdir starting from index in manifest_dirs, if matched return matched subdir index
 * To search many subdirs user would call it with index parameter returned by previous function call;
 * @return subdir index if found, -1 if not*/
int get_sub_dir_index( struct manifest_loaded_directories_t *manifest_dirs, 
		       const char *dirpath, 
		       int index );

#endif /* CHANNELS_READDIR_H_ */

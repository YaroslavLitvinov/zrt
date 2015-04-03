/*
 * Channels filesystem implementation 
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

#define _GNU_SOURCE

#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>  //SEEK_SET .. SEEK_END
#include <stdarg.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include "string.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include "zvm.h"
#include "zrtlog.h"
#include "zrt_helper_macros.h"
#include "path_utils.h"
#include "nacl_struct.h"
#include "mount_specific_interface.h"
#include "mounts_interface.h"
#include "handle_allocator.h" //struct HandleAllocator, struct HandleItem
#include "open_file_description.h" //struct OpenFilesPool, struct OpenFileDescription
#include "fcntl_implem.h"
#include "enum_strings.h"
#include "dirent_engine.h"
#include "channels_readdir.h"
#include "channels_mount.h"
#include "channels_mount_magic_numbers.h"
#include "channels_array.h"

enum PosAccess{ EPosSeek=0, EPosRead, EPosWrite };
enum PosWhence{ EPosGet=0, EPosSetAbsolute, EPosSetRelative };

/*0 if check OK*/
#define CHECK_NEW_POS(offset) ((offset) < 0 ? -1: 0)
#define SET_SAFE_OFFSET( whence, pos_p, offset )	\
    if ( EPosSetRelative == whence ){			\
	if ( CHECK_NEW_POS( *pos_p+offset ) == -1 )	\
            { SET_ERRNO( EOVERFLOW ); return -1; }	\
	else  *pos_p +=offset; }			\
    else{						\
	if ( CHECK_NEW_POS( offset ) == -1 )		\
	    { SET_ERRNO( EOVERFLOW ); return -1; }	\
	else *pos_p  =offset; }

#define CHANNEL_IS_OPENED(hallocator_p, handle)			\
    ( (hallocator_p)->mount_interface(handle)==NULL ?0 :1 )

#define CHANNEL_ITEM_BY_INODE(array_p, inode)		\
    (array_p)->match_by_inode((array_p), inode)

#define CHANNEL_NAME(channel_item) (channel_item)->channel->name
#define CHANNEL_SIZE(channel_item) (channel_item) ?			\
						  MAX( (channel_item)->channel_runtime.maxsize, (channel_item)->channel->size ) : (channel_item)->channel->size;

#define HALLOCATOR_BY_MOUNT_SPECIF(mount_specific_interface_p)		\
    ((struct ChannelMounts*)((struct MountSpecificImplem*)(mount_specific_interface_p))->mount)->handle_allocator

#define HALLOCATOR_BY_MOUNT(mount_interface_p)				\
    ((struct ChannelMounts*)(mount_interface_p))->handle_allocator

#define CHECK_FILE_OPEN_FLAGS_OR_RAISE_ERROR(flags, flag1, flag2)	\
    if ( (flags)!=flag1 && (flags)!=flag2 ){				\
	ZRT_LOG(L_ERROR, "file open flags must be %s or %s",		\
		STR_FILE_OPEN_FLAGS(flag1), STR_FILE_OPEN_FLAGS(flag2)); \
	SET_ERRNO( EINVAL );						\
	return -1;							\
    }

struct ChannelMounts{
    struct MountsPublicInterface public;
    struct manifest_loaded_directories_t manifest_dirs;
    struct HandleAllocator* handle_allocator;
    struct OpenFilesPool* open_files_pool;
    struct ChannelsArrayPublicInterface* channels_array;
    struct MountSpecificPublicInterface* mount_specific_interface;
};

/***********************************************************************
   implementation of MountSpecificPublicInterface as part of
   filesystem.  Below resides channels specific functions.*/

struct MountSpecificImplem{
    struct MountSpecificPublicInterface public_;
    //data
    struct ChannelMounts* mount;
    struct ChannelsArrayPublicInterface* channels_array;
};


/*return 0 if handle not valid, or 1 if handle is correct*/
static int check_handle(struct MountSpecificImplem* this, int handle){
    return !HALLOCATOR_BY_MOUNT_SPECIF(this)
	 ->check_handle_is_related_to_filesystem(handle, &this->mount->public);
}

static const char* handle_path(struct MountSpecificImplem* this, int handle){
    if ( this->mount->handle_allocator
	 ->check_handle_is_related_to_filesystem(handle, &this->mount->public) != 0 ) return NULL;
    const struct HandleItem* hentry = this->mount->handle_allocator->entry(handle);
    struct dir_data_t *dir_data;
    struct ChannelArrayItem* item;
    /*choose handle type: channel handle or dir handle */		
    if ( (item=this->channels_array->match_by_inode(this->channels_array, 
						    hentry->inode)) != NULL ){
	return CHANNEL_NAME( item );
    }
    else if ( (dir_data=match_inode_in_directory_list( &this->mount->manifest_dirs, 
						       hentry->inode)) != NULL ){
	/*dir name*/
	return dir_data->path;
    }
    return NULL;
}

static int file_status_flags(struct MountSpecificImplem* this, int handle){
    if ( HALLOCATOR_BY_MOUNT_SPECIF(this)
	 ->check_handle_is_related_to_filesystem(handle, &this->mount->public) == 0 ){

	/*get runtime information related to channel*/
	const struct OpenFileDescription* ofd = HALLOCATOR_BY_MOUNT_SPECIF(this)->ofd(handle);
	assert(ofd);
	return ofd->flags;
    }
    else{
	SET_ERRNO(EBADF);
	return -1;
    }
}

static int set_file_status_flags(struct MountSpecificImplem* this, int fd, int flags){
    if ( HALLOCATOR_BY_MOUNT_SPECIF(this)
	 ->check_handle_is_related_to_filesystem(fd, &this->mount->public) == 0 ){
	const struct HandleItem* hentry;
	hentry = HALLOCATOR_BY_MOUNT_SPECIF(this)->entry(fd);
	this->mount->open_files_pool->set_flags( hentry->open_file_description_id, flags);
	return flags;
    }
    else{
	SET_ERRNO(EBADF);
	return -1;
    }
}

/*return pointer at success, NULL if fd didn't found or flock
  structure has not been set*/
static const struct flock* flock_data( struct MountSpecificImplem* this, int handle ){
    const struct flock* data = NULL;

    if ( HALLOCATOR_BY_MOUNT_SPECIF(this)
	 ->check_handle_is_related_to_filesystem(handle, &this->mount->public) == 0 ){
	const struct HandleItem* hentry = HALLOCATOR_BY_MOUNT_SPECIF(this)->entry(handle);
	struct ChannelArrayItem* item  = CHANNEL_ITEM_BY_INODE(this->channels_array, hentry->inode);
	/*get runtime information related to channel*/
	data = &item->channel_runtime.fcntl_flock;
    }
    return data;
}

/*return 0 if success, -1 if fd didn't found*/
static int set_flock_data( struct MountSpecificImplem* this, int handle, 
			   const struct flock* flock_data ){
    int rc = 1; /*error by default*/
    if ( HALLOCATOR_BY_MOUNT_SPECIF(this)
	 ->check_handle_is_related_to_filesystem(handle, &this->mount->public) == 0 ){
	const struct HandleItem* hentry = HALLOCATOR_BY_MOUNT_SPECIF(this)->entry(handle);
	struct ChannelArrayItem* item = CHANNEL_ITEM_BY_INODE(this->channels_array, hentry->inode);
	/*get runtime information related to channel*/
	memcpy( &item->channel_runtime.fcntl_flock, flock_data, sizeof(struct flock) );
	rc = 0; /*ok*/
    }
    return rc;
}

static struct MountSpecificPublicInterface KMountSpecificImplem = {
    (void*)check_handle,
    (void*)handle_path,
    (void*)file_status_flags,
    (void*)set_file_status_flags,
    (void*)flock_data,
    (void*)set_flock_data
};

static struct MountSpecificPublicInterface*
mount_specific_construct( struct MountSpecificPublicInterface* specific_implem_interface,
			  struct ChannelMounts* mount,
			  struct ChannelsArrayPublicInterface* channels_array ){
    struct MountSpecificImplem* this = malloc(sizeof(struct MountSpecificImplem));
    /*set functions*/
    this->public_ = *specific_implem_interface;
    /*set data members*/
    this->mount = mount;
    this->channels_array = channels_array;
    return (struct MountSpecificPublicInterface*)this;
}


//////////// helpers

int64_t channel_pos( struct ChannelMounts* this, 
		     int handle, int8_t whence, int8_t access, int64_t offset );

uint32_t channel_permissions(const struct ChannelArrayItem *item);

/*return 0 if specified mode is matches to chan AccessType*/
static int check_channel_flags(const struct ZVMChannel *chan, int flags)
{
    assert(chan);

    /*check read / write ability*/
    int canberead = chan->limits[GetsLimit] && chan->limits[GetSizeLimit];
    int canbewrite = chan->limits[PutsLimit] && chan->limits[PutSizeLimit];

    ZRT_LOG(L_EXTRA, "flags=%s, canberead=%d, canbewrite=%d", 
	    STR_FILE_OPEN_FLAGS(flags), canberead, canbewrite );

    /*reset permissions bits, that are not used currently*/
    flags = flags & O_ACCMODE;
    switch( flags ){
    case O_RDONLY:
        return canberead>0 ? 0: -1;
    case O_WRONLY:
        return canbewrite >0 ? 0 : -1;
    case O_RDWR:
    default:
        /*if case1: O_RDWR ; case2: O_RDWR|O_WRONLY logic error handle as O_RDWR*/
        return canberead>0 && canbewrite>0 ? 0 : -1;
    }
    return 1;
}

static int open_channel( struct ChannelMounts* this, const char *name, int flags, int mode )
{
    struct ChannelArrayItem* item 
	= this->channels_array->match_by_name(this->channels_array, name);
    ZRT_LOG(L_EXTRA, 
	    "name=%s, mode=%s, flags=%s", 
	    name, STR_FILE_OPEN_MODE(mode), STR_FILE_OPEN_FLAGS(flags) );

    if ( item == NULL ){
        /* channel name not matched*/
        SET_ERRNO( ENOENT );
        return -1;
    }

    /*Append only channels support
      Do not allow to open append only channel with wrong flags*/
    uint32_t permissions = channel_permissions(item);
    uint32_t ftype = permissions&S_IFMT;
    if ( item->channel->type == RGetSPut && !CHECK_FLAG(flags, O_APPEND) &&
	 CHECK_FLAG(permissions, S_IRUSR) && CHECK_FLAG(permissions, S_IWUSR) &&
	 (S_IFBLK==ftype||S_IFREG==ftype) ){
        ZRT_LOG(L_ERROR, "Channel %s treated as append only channel and must be opened with O_APPEND flag", name );
        SET_ERRNO( EPERM );
        return -1;
    }

    /*truncate not allowed for zerovm channels except FIFO, CHR;
     skip check for emulated channels like /dev/null, ...*/
    if ( !item->channel_runtime.emu &&
	 CHECK_FLAG(flags, O_TRUNC) && S_IFCHR!=ftype && S_IFIFO!=ftype ){
        SET_ERRNO( EPERM );
        return -1;
    }

    /*check access mode for opening channel, limits not checked*/
    if( check_channel_flags( item->channel, flags ) != 0 ){
        ZRT_LOG(L_ERROR, "can't open channel, name=%s ", name );
        SET_ERRNO( EACCES );
        return -1;
    }

    int open_file_descr = this->open_files_pool->getnew_ofd(flags);
    /*open file and return new handle, zvm handle is used as indode, 
      and can be used in order to do multiple opens of single file */
    int handle = this->handle_allocator->allocate_handle(&this->public,
							 item->channel_runtime.inode,
							 item->channel_runtime.parent_inode,
							 open_file_descr);
    if ( handle == -1 ){
	this->open_files_pool->release_ofd(open_file_descr);
	SET_ERRNO(ENFILE);
	return -1;
    }

    /*Append only channels support
     set writing pos into end of file*/
    if ( CHECK_FLAG(flags, O_APPEND) ){
	size_t chan_size = CHANNEL_SIZE(item);
	channel_pos(this, handle, EPosSetAbsolute, EPosWrite, chan_size);
    }

    ZRT_LOG(L_EXTRA, "channel open ok, handle=%d, inode=%d ", 
	    handle, item->channel_runtime.inode );
    return handle;
}


uint32_t channel_permissions(const struct ChannelArrayItem *item){
    uint32_t perm = 0;
    uint mode;
    assert(item);
    /*if nvram type is available for given channel*/
    mode = item->channel_runtime.mode;

    if ( item->channel->limits[GetsLimit] != 0 && item->channel->limits[GetSizeLimit] )
        perm |= S_IRUSR;
    if ( item->channel->limits[PutsLimit] != 0 && item->channel->limits[PutSizeLimit] )
        perm |= S_IWUSR;
    if ( (item->channel->type == SGetSPut && ( (perm&S_IRWXU)==S_IRUSR || (perm&S_IRWXU)==S_IWUSR )) ||
	 (item->channel->type == RGetSPut && (perm&S_IRWXU)==S_IWUSR ) ||
	 (item->channel->type == SGetRPut && (perm&S_IRWXU)==S_IRUSR ) )
	{
	    if ( mode == 0 ) perm |= S_IFIFO;
	}
    else{
	if ( mode == 0 ) perm |= S_IFBLK;
    }

    if ( mode != 0 )
	perm |= mode;

    return perm;
}


static int64_t channel_pos_sequen_get_sequen_put( struct ChannelMounts* this, int open_file_description_id,
						  int8_t whence, int8_t access, int64_t offset ){
    const struct OpenFileDescription* ofd = this->open_files_pool->entry(open_file_description_id);
    off_t openfile_offset_seq = ofd->channel_sequential_offset;
    /*seek get supported by channel for any modes*/
    if ( EPosGet == whence ) return openfile_offset_seq;
    switch ( access ){
    case EPosSeek:
        /*seek set does not supported for this channel type*/
        SET_ERRNO( ESPIPE );
        return -1;
    case EPosRead:
    case EPosWrite:
        SET_SAFE_OFFSET( whence, &openfile_offset_seq, offset );
	this->open_files_pool->set_offset_sequential_channel(open_file_description_id, openfile_offset_seq);
	return openfile_offset_seq;
        break;
    default:
        assert(0);
        break;
    }
}


static int64_t channel_pos_random_get_sequen_put( struct ChannelMounts* this, int open_file_description_id,
						  int flags, int8_t whence, int8_t access, int64_t offset ){
    const struct OpenFileDescription* ofd = this->open_files_pool->entry(open_file_description_id);
    off_t openfile_offset_seq = ofd->channel_sequential_offset;
    off_t openfile_offset_rand = ofd->offset;

    switch ( access ){
    case EPosSeek:
        if ( CHECK_FLAG( flags, O_RDONLY )
	     || CHECK_FLAG( flags, O_RDWR ) )
	    {
		if ( EPosGet == whence ) return openfile_offset_rand;
		SET_SAFE_OFFSET( whence, &openfile_offset_rand, offset );
		this->open_files_pool->set_offset(open_file_description_id, openfile_offset_rand);
		return openfile_offset_rand;
	    }
        else if ( CHECK_FLAG( flags, O_WRONLY ) == 1 )
	    {
		if ( EPosGet == whence ) return openfile_offset_seq;
		else{
		    /*it's does not supported for seeking sequential write*/
		    SET_ERRNO( ESPIPE );
		    return -1;
		}
	    }
        break;
    case EPosRead:
        if ( EPosGet == whence ) return openfile_offset_rand;
        SET_SAFE_OFFSET( whence, &openfile_offset_rand, offset );
	this->open_files_pool->set_offset(open_file_description_id, openfile_offset_rand);
	return openfile_offset_rand;
        break;
    case EPosWrite:
        if ( EPosGet == whence ) return openfile_offset_seq;
        SET_SAFE_OFFSET( whence, &openfile_offset_seq, offset );
	this->open_files_pool->set_offset_sequential_channel(open_file_description_id, openfile_offset_seq);
	return openfile_offset_seq;
        break;
    default:
        assert(0);
        break;
    }
}

static int64_t channel_pos_sequen_get_random_put(struct ChannelMounts* this, int open_file_description_id,
						 int flags, int8_t whence, int8_t access, int64_t offset){
    const struct OpenFileDescription* ofd = this->open_files_pool->entry(open_file_description_id);
    off_t openfile_offset_seq = ofd->channel_sequential_offset;
    off_t openfile_offset_rand = ofd->offset;

    switch ( access ){
    case EPosSeek:
        if ( CHECK_FLAG( flags, O_WRONLY ) == 1
	     || CHECK_FLAG( flags, O_RDWR ) == 1 )
	    {
		if ( EPosGet == whence ) return openfile_offset_rand;
		SET_SAFE_OFFSET( whence, &openfile_offset_rand, offset );
		this->open_files_pool->set_offset(open_file_description_id, openfile_offset_rand);
		return openfile_offset_rand;
	    }
        else if ( CHECK_FLAG( flags, O_RDONLY )  == 1)
	    {
		if ( EPosGet == whence ) return openfile_offset_seq;
		else{
		    /*it's does not supported for seeking sequential read*/
		    SET_ERRNO( ESPIPE );
		    return -1;
		}
	    }
        break;
    case EPosRead:
        if ( EPosGet == whence ) return openfile_offset_seq;
        SET_SAFE_OFFSET( whence, &openfile_offset_seq, offset );
	this->open_files_pool->set_offset_sequential_channel(open_file_description_id, openfile_offset_seq);
	return openfile_offset_seq;
        break;
    case EPosWrite:
	if ( EPosGet == whence ) return openfile_offset_rand;
	SET_SAFE_OFFSET( whence, &openfile_offset_rand, offset );
	this->open_files_pool->set_offset(open_file_description_id, openfile_offset_rand);
	return openfile_offset_rand;
        break;
    default:
        assert(0);
        break;
    }
}

static int64_t channel_pos_random_get_random_put(struct ChannelMounts* this, int open_file_description_id,
						 int8_t whence, int8_t access, int64_t offset){
    const struct OpenFileDescription* ofd = this->open_files_pool->entry(open_file_description_id);
    off_t openfile_offset_rand = ofd->offset;

    if ( EPosGet == whence ) return openfile_offset_rand;
    switch ( access ){
    case EPosSeek:
    case EPosRead:
    case EPosWrite:
	SET_SAFE_OFFSET( whence, &openfile_offset_rand, offset );
	this->open_files_pool->set_offset(open_file_description_id, openfile_offset_rand);
	return openfile_offset_rand;
        break;
    default:
        assert(0);
        break;
    }
}

/*@param pos_whence If EPosGet offset unused, otherwise check and set offset 
 *@return -1 if bad offset, else offset result*/
int64_t channel_pos( struct ChannelMounts* this, 
		     int handle, int8_t whence, int8_t access, int64_t offset ){
    if ( HALLOCATOR_BY_MOUNT(this)
	 ->check_handle_is_related_to_filesystem(handle, &this->public) == 0 ){
	const struct HandleItem* hentry = HALLOCATOR_BY_MOUNT(this)->entry(handle);
	const struct OpenFileDescription* ofd = HALLOCATOR_BY_MOUNT(this)->ofd(handle);
	struct ChannelArrayItem* item = CHANNEL_ITEM_BY_INODE(this->channels_array, hentry->inode);
	int flags = ofd->flags;
        int8_t access_type = item->channel->type;
        switch ( access_type ){
        case SGetSPut:
            return channel_pos_sequen_get_sequen_put(this, hentry->open_file_description_id,
						     whence, access, offset );
            break;
        case RGetSPut:
            return channel_pos_random_get_sequen_put(this, hentry->open_file_description_id, 
						     flags, whence, access, offset );
            break;
        case SGetRPut:
            return channel_pos_sequen_get_random_put(this, hentry->open_file_description_id, 
						     flags, whence, access, offset );
            break;
        case RGetRPut:
            return channel_pos_random_get_random_put(this, hentry->open_file_description_id, 
						     whence, access, offset );
            break;
        }
    }
    else{
        /*bad handle*/
        SET_ERRNO( EBADF );
    }
    return -1;
}

static void set_stat_time( struct stat* st )
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* files does not have real date/time */
    st->st_atime = tv.tv_sec;      /* time of the last access */
    st->st_mtime = tv.tv_sec;      /* time of the last modification */
    st->st_ctime = tv.tv_sec;      /* time of the last status change */
}

/*used by stat, fstat; set stat based on channel type*/
static void set_stat(struct ChannelMounts* this, struct stat *stat, int inode)
{
    int nlink = 1;
    uint32_t permissions;
    int64_t size;
    uint32_t blksize;
    struct ChannelArrayItem* item 
	= this->channels_array->match_by_inode(this->channels_array, inode);

    /*choose handle type: channel handle or dir handle */
    if ( item != NULL ){
        /*channel handle*/
        permissions = channel_permissions( item );
        if ( CHECK_FLAG( permissions, S_IFIFO ) || CHECK_FLAG( permissions, S_IFCHR ) )
	    blksize = DEV_CHAR_DEVICE_BLK_SIZE;
        else 
	    blksize = DEV_BLOCK_DEVICE_BLK_SIZE;
        size = CHANNEL_SIZE( item );
    }
    else{
        /*dir handle*/
        struct dir_data_t *d = match_inode_in_directory_list( &this->manifest_dirs, inode );
	if ( d == NULL ){
	    assert(d);
	}
        nlink = d->nlink;
        permissions = S_IRUSR | S_IFDIR | S_IXUSR;
	blksize = DEV_DIRECTORY_BLK_SIZE;
	size = DEV_DIRECTORY_SIZE;
    }

    /* return stat object */
    stat->st_dev = DEV_DEVICE_ID;     /* ID of device containing handle */
    stat->st_ino = inode;             /* inode number */
    stat->st_nlink = nlink;           /* number of hard links */;
    stat->st_uid = DEV_OWNER_UID;     /* user ID of owner */
    stat->st_gid = DEV_OWNER_GID;     /* group ID of owner */
    stat->st_rdev = 0;                /* device ID (if special handle) */
    stat->st_mode = permissions;
    stat->st_blksize = blksize;        /* block size for file system I/O */
    stat->st_size = size;
    stat->st_blocks =               /* number of 512B blocks allocated */
	((stat->st_size + stat->st_blksize - 1) / stat->st_blksize) * stat->st_blksize / 512;

    set_stat_time( stat );
}


/*@return 0 if matched, or -1 if not*/
static int iterate_dir_contents( struct ChannelMounts* this, int dir_handle, int index, 
			  int* iter_inode, const char** iter_name, int* iter_is_dir ){
    int i=0;
    ino_t inode = this->handle_allocator->entry(dir_handle)->inode;

    /*get directory data by handle*/
    struct dir_data_t* dir_pattern =
	match_inode_in_directory_list(&this->manifest_dirs, inode);

    /*add '.' item*/
    if ( index == 0 ){
	*iter_inode = inode; /*get directory inode*/
	*iter_name = "."; /*get name of item*/
	*iter_is_dir = 1;
	return 0;
    }
    /*add '..' item*/
    if ( index == 1 ){
	*iter_inode = dir_pattern->parent_dir_inode; /*get parent directory inode*/
	*iter_name = ".."; /*get name of item*/
	*iter_is_dir = 1;
	return 0;
    }

    /*nested dirs & files count*/
    int subdir_index = 2;     /*'.', '..'*/
    int file_index = 2;     /*'.', '..'*/

    /*match subdirs*/
    struct dir_data_t* loop_d = NULL; /*loop dir used as loop variable*/
    int namelen =  strlen(dir_pattern->path);
    for( i=0; i < this->manifest_dirs.dircount; i++ ){
	/*iterate every directory to compare it with pattern*/
        loop_d = &this->manifest_dirs.dir_array[i];
        /*match all subsets, exclude the same dir path*/
        if ( ! strncmp(dir_pattern->path, loop_d->path, namelen) && 
	     strlen(loop_d->path) > namelen+1 ){
            /*if can't locate trailing '/' then matched subdir*/
            char *backslash_matched = strchr( &loop_d->path[namelen+1], '/');
            if ( !backslash_matched ){
		/*if matched index of directory contents*/
		if ( index == subdir_index ){
		    /*fetch name from full path*/
		    int shortname_len;
		    const char *short_name = 
			name_from_path_get_path_len(loop_d->path, &shortname_len );
		    *iter_inode = loop_d->dir_inode; /*get directory inode*/
		    *iter_name = short_name; /*get name of item*/
		    *iter_is_dir = 1; /*get info that item is directory*/
		    return 0;
		}
		++subdir_index;
            }
        }
    }

    /*match files*/
    struct ChannelArrayItem* channel_item=NULL;
    int dirlen =  strlen(dir_pattern->path);
    for( i=0; i < this->channels_array->count(this->channels_array); i++ ){
	channel_item = this->channels_array->get(this->channels_array, i );
        /*match all subsets for dir path*/
        if ( ! strncmp(dir_pattern->path, CHANNEL_NAME( channel_item ), dirlen) && 
	     strlen( CHANNEL_NAME( channel_item ) ) > dirlen+1 ){
            /*if can't locate trailing '/' then matched directory's file*/
            char *backslash_matched = strchr( &CHANNEL_NAME( channel_item )[dirlen+1], '/');
            if ( !backslash_matched ){
		/*if matched index of directory contents*/
		if ( index == file_index ){
		    /*fetch name from full path*/
		    int shortname_len;
		    ino_t loop_inode = INODE_FROM_ZVM_INODE(i);		    
		    const char *short_name = 
			name_from_path_get_path_len( CHANNEL_NAME( channel_item ), &shortname_len );
		    *iter_inode = loop_inode; /*channel handle is the same as channel index*/
		    *iter_name = short_name; /*get name of item*/
		    *iter_is_dir = 0; /*get info that item is not directory*/
		    return 0;
		}
		++file_index;
	    }
        }
    }
    return -1;/*specified index not matched, probabbly it's out of bounds*/
}

/*If it's emulated channel and channel not provided by zerovm, then emulate it*/
static int emu_handle_read(struct ChannelMounts* this, 
			   ino_t inode, void *buf, size_t nbyte, int* handled){
    struct ChannelArrayItem* item 
	= this->channels_array->match_by_inode(this->channels_array, inode);
    if ( item != NULL ){
	if ( !strcmp("/dev/zero", item->channel->name) || 
	     !strcmp("/dev/full", item->channel->name) ){
	    memset(buf, '\0', nbyte);
	    *handled=1;
	    return nbyte;
	}
	else if ( !strcmp("/dev/null", item->channel->name) ){
	    *handled=1;
	    return 0;
	}
	else if ( !strcmp("/dev/random", item->channel->name) ||
		  !strcmp("/dev/urandom", item->channel->name) ){
	    int i, r=0;
	    srand(time(NULL));
	    for ( i=0; i < nbyte; i++ ){
		if ( i % 4 == 0 ) 
		    r = rand();
		((char*)buf)[i] = ((char*)&r)[i%4];
	    }
	    *handled=1;
	    return nbyte;
	}
    }
    *handled=0;
    return -1; /*not handled*/
}

/*If it's emulated channel and channel not provided by zerovm, then emulate it*/
static int emu_handle_write(struct ChannelMounts* this, 
			    ino_t inode, const void *buf, size_t nbyte, int* handled){
    struct ChannelArrayItem* item 
	= this->channels_array->match_by_inode(this->channels_array, inode);
    if ( item != NULL ){
	if ( !strcmp("/dev/zero", item->channel->name) ||
	     !strcmp("/dev/null", item->channel->name) ){
	    *handled=1;
	    return nbyte;
	}
	if ( !strcmp("/dev/full", item->channel->name) ){
	    SET_ERRNO(ENOSPC);
	    *handled=1;
	    return -1;
	}
	else if ( !strcmp("/dev/random", item->channel->name) ||
		  !strcmp("/dev/urandom", item->channel->name) ){
	    *handled=1;
	    return nbyte;
	}
    }
    *handled=0;
    return -1; /*not handled*/
}


/*calculated synthetic size as maximum writable position for channels 
  with random access on write. For further calls: stat, fstat*/
static void 
update_artificial_channel_size(struct ChannelMounts* this, 
			       int fd, struct ChannelArrayItem* item ){
    int8_t access_type = item->channel->type;
    if ( access_type == SGetRPut || access_type == RGetRPut || access_type == RGetSPut ){
	item->channel_runtime.maxsize 
	    = MAX(item->channel_runtime.maxsize,
		  channel_pos(this, fd, EPosGet, EPosWrite, 0));
	ZRT_LOG(L_SHORT, "Update fd=%d channel size=%lld", 
		fd, item->channel_runtime.maxsize );
    }
}


//////////// interface implementation

static int channels_chmod(struct MountsPublicInterface* this,const char* path, uint32_t mode){
    SET_ERRNO(EPERM);
    return -1;
}

static int channels_stat(struct MountsPublicInterface* this_,const char* path, struct stat *buf){
    errno = 0;
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    ZRT_LOG(L_EXTRA, "path=%s", path);

    if(path == NULL){
        SET_ERRNO(EFAULT);
        return -1;
    }

    struct dir_data_t *dir = NULL;
    struct ChannelArrayItem* item 
	= this->channels_array->match_by_name(this->channels_array, path);

    if ( item != NULL ){
        set_stat( this, buf, item->channel_runtime.inode);
        return 0;
    }
    else if ( (dir=match_dir_in_directory_list(&this->manifest_dirs, path, strlen(path))) ){
        set_stat( this, buf, dir->dir_inode);
        return 0;
    }
    else{
        SET_ERRNO(ENOENT);
        return -1;
    }

    return 0;
}

static int channels_mkdir(struct MountsPublicInterface* this,const char* path, uint32_t mode){
    SET_ERRNO(ENOSYS);
    return -1;
}

static int channels_rmdir(struct MountsPublicInterface* this,const char* path){
    SET_ERRNO(ENOSYS);
    return -1;
}

static ssize_t __NON_INSTRUMENT_FUNCTION__
channels_read(struct MountsPublicInterface* this_, int fd, void *buf, size_t nbyte){
    off_t offset;
    errno=0;
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    /*file not opened, bad descriptor*/
    if( CHANNEL_IS_OPENED( HALLOCATOR_BY_MOUNT(this), fd) == 0 ){
	ZRT_LOG(L_ERROR, "invalid file descriptor fd=%d", fd);
	SET_ERRNO( EBADF );
	return -1;
    }
    offset = channel_pos(this, fd, EPosGet, EPosRead, 0);
    if ( CHECK_NEW_POS( offset+nbyte ) != 0 ){
        SET_ERRNO( EOVERFLOW );
        return -1;
    }
    return this->public.pread(&this->public, fd, buf, nbyte, offset);
}

static ssize_t __NON_INSTRUMENT_FUNCTION__
channels_write(struct MountsPublicInterface* this_, int fd, const void *buf, size_t nbyte){
    off_t offset;
    errno=0;
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    /*file not opened, bad descriptor*/
    if( CHANNEL_IS_OPENED( HALLOCATOR_BY_MOUNT(this), fd) == 0 ){
	ZRT_LOG(L_ERROR, "invalid file descriptor fd=%d", fd);
	SET_ERRNO( EBADF );
	return -1;
    }
    offset = channel_pos(this, fd, EPosGet, EPosWrite, 0);
    if ( CHECK_NEW_POS( offset+nbyte ) != 0 ){
        SET_ERRNO( EOVERFLOW );
        return -1;
    }
    return this->public.pwrite(&this->public, fd, buf, nbyte, offset);
}

static ssize_t __NON_INSTRUMENT_FUNCTION__
channels_pread(struct MountsPublicInterface* this_, int fd, void *buf, 
			      size_t nbyte, off_t offset){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    int32_t readed = 0;
    const struct HandleItem* hentry;
    const struct OpenFileDescription* ofd;
    errno = 0;

    /*case: file not opened, bad descriptor*/
    if ( this->handle_allocator->check_handle_is_related_to_filesystem(fd, &this->public) == -1 ){
	SET_ERRNO( EBADF );
	return -1;
    }

    hentry = this->handle_allocator->entry(fd); 
    ofd = this->handle_allocator->ofd(fd);
    assert(ofd);

    /*check if file was not opened for reading*/
    CHECK_FILE_OPEN_FLAGS_OR_RAISE_ERROR(ofd->flags&O_ACCMODE, O_RDONLY, O_RDWR);

    /*try to read from emulated channel, else read via zvm_pread call */
    int handled=0;
    if ( (readed=emu_handle_read(this, hentry->inode, buf, nbyte, &handled)) == -1 && !handled )
	readed = zvm_pread( ZVM_INODE_FROM_INODE(hentry->inode), buf, nbyte, offset );
    if(readed > 0) channel_pos(this, fd, EPosSetAbsolute, EPosRead, offset+readed);
    
    ZRT_LOG(L_EXTRA, "channel fd=%d, bytes readed=%d", fd, readed );

    if ( readed < 0 ){
	/*negative result returned by zvm_pread is an actual errno*/
        SET_ERRNO( readed );
        return -1;
    }

    return readed;
}

static ssize_t __NON_INSTRUMENT_FUNCTION__
channels_pwrite(struct MountsPublicInterface* this_,int fd, const void *buf, 
			       size_t nbyte, off_t offset){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    int32_t wrote = 0;
    const struct HandleItem* hentry;
    const struct OpenFileDescription* ofd;
    struct ChannelArrayItem* item;

    errno=0;
    //if ( fd < 3 ) disable_logging_current_syscall();

    /*case: file not opened, bad descriptor*/
    if ( this->handle_allocator->check_handle_is_related_to_filesystem(fd, &this->public) == -1 ){
	SET_ERRNO( EBADF );
	return -1;
    }

    hentry = this->handle_allocator->entry(fd); 
    ofd = this->handle_allocator->ofd(fd);
    assert(ofd);

    /*if file was not opened for writing, set errno and get error*/
    CHECK_FILE_OPEN_FLAGS_OR_RAISE_ERROR(ofd->flags&O_ACCMODE, O_WRONLY, O_RDWR);

    /*try to read from emulated channel, else read via zvm_pread call */
    int handled=0;
    if ( (wrote=emu_handle_write(this, hentry->inode, buf, nbyte, &handled)) == -1 && !handled )
	wrote = zvm_pwrite(ZVM_INODE_FROM_INODE(hentry->inode), buf, nbyte, offset );
    if(wrote > 0) channel_pos(this, fd, EPosSetAbsolute, EPosWrite, offset+wrote);
    ZRT_LOG(L_EXTRA, "channel fd=%d, bytes wrote=%d", fd, wrote);

    if ( wrote < 0 ){
	/*negative result returned by zvm_pwrite is an actual errno*/
        SET_ERRNO( wrote );
        return -1;
    }

    item = CHANNEL_ITEM_BY_INODE(this->channels_array, hentry->inode);
    update_artificial_channel_size(this, fd, item);

    return wrote;
}

////////////////


static int channels_fchmod(struct MountsPublicInterface* this,int fd, mode_t mode){
    SET_ERRNO(EPERM);
    return -1;
}

static int channels_fstat(struct MountsPublicInterface* this_, int fd, struct stat *buf){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    errno=0;

    /*case: file not opened, bad descriptor*/
    if ( this->handle_allocator->check_handle_is_related_to_filesystem(fd, &this->public) == -1 ){
	SET_ERRNO( EBADF );
	return -1;
    }

    set_stat( this, buf, this->handle_allocator->entry(fd)->inode); /*channel fd*/
    return 0;
}

static int channels_getdents(struct MountsPublicInterface* this_, int fd, void *buf, unsigned int buf_size){
#define GET_MODE_OF_ENTRY_BY_INODE(this, inode, mode_p){		\
	/*choose handle type: channel handle or dir handle */		\
	struct ChannelArrayItem* item = this->channels_array		\
	    ->match_by_inode(this->channels_array, inode);		\
	if ( item != NULL ){						\
	    /*channel handle*/						\
	    *(mode_p) = channel_permissions( item );			\
	}								\
	else{								\
	    /*dir handle*/						\
	    struct dir_data_t *d = match_inode_in_directory_list( &this->manifest_dirs, inode ); \
	    assert(d);							\
	    *(mode_p) = S_IRUSR | S_IFDIR | S_IXUSR;			\
	}								\
    }

    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    errno=0;

    /*case: file not opened, bad descriptor*/
    if ( this->handle_allocator->check_handle_is_related_to_filesystem(fd, &this->public) == -1 ){
	SET_ERRNO( EBADF );
	return -1;
    }

    /* check null and  make sure the buffer is aligned */
    if ( !buf || 0 != ((sizeof(unsigned long) - 1) & (uintptr_t) buf)) {
        SET_ERRNO(EINVAL);
        return -1;
    }
    int bytes_read=0;
    const struct HandleItem* hentry;
    const struct OpenFileDescription* ofd;

    /*use offset of opened directoctory as cursor pos to iterate directory contents*/
    hentry = this->handle_allocator->entry(fd); 
    ofd = this->handle_allocator->ofd(fd);

    /*through via list all directory contents*/
    int index=ofd->offset;
    int iter_inode=0;
    const char* iter_item_name = NULL;
    int iter_is_dir=0;
    int res=0;
    uint32_t mode;
    while( !(res=iterate_dir_contents( this, fd, index, &iter_inode, &iter_item_name, &iter_is_dir )) ){
	GET_MODE_OF_ENTRY_BY_INODE(this, iter_inode, &mode);
	/*format in buf dirent structure, of variable size, and save current file data;
	  original MemMount implementation was used dirent as having constant size */
	int ret = get_dirent_engine()
	    ->add_dirent_into_buf( ((char*)buf)+bytes_read, buf_size-bytes_read, 
				   iter_inode, 0,
				   mode, iter_item_name );
	/*if put into dirent was success*/
	if ( ret > 0 ){
	    bytes_read += ret;
	    ++index;
	}
	else{
	    break; /*interrupt - insufficient buffer space*/
	}
    }
    /*update offset index of handled directory item*/
    this->open_files_pool->set_offset(hentry->open_file_description_id, index );
    return bytes_read;
}

static int channels_fsync(struct MountsPublicInterface* this,int fd){
    SET_ERRNO(ENOSYS);
    return -1;
}

static int channels_close(struct MountsPublicInterface* this_,int fd){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    const struct HandleItem* hentry;
    errno = 0;

    /*case: file not opened, bad descriptor*/
    if ( this->handle_allocator->check_handle_is_related_to_filesystem(fd, &this->public) == -1 ){
	SET_ERRNO( EBADF );
	return -1;
    }

    hentry = this->handle_allocator->entry(fd);
    
    struct ChannelArrayItem* item 
	= CHANNEL_ITEM_BY_INODE(this->channels_array, hentry->inode);
    if ( item != NULL ){
#define SAVE_SYNTHETIC_SIZE
#ifndef SAVE_SYNTHETIC_SIZE
	item->channel_runtime.maxsize = 0;
#endif

	ZRT_LOG(L_EXTRA, "closed channel=%s", CHANNEL_NAME( item ) );
    }
    else{ /*search fd in directories list*/
	struct dir_data_t * dir = match_inode_in_directory_list(&this->manifest_dirs, hentry->inode);
	assert( dir != NULL );
    }

    int res = this->open_files_pool->release_ofd(hentry->open_file_description_id);
    assert(res==0);

    this->handle_allocator->free_handle(fd);
    return 0;
}

static off_t channels_lseek(struct MountsPublicInterface* this_,int fd, off_t offset, int whence){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    const struct HandleItem* hentry;
    errno = 0;

    /*case: file not opened, bad descriptor*/
    if ( this->handle_allocator->check_handle_is_related_to_filesystem(fd, &this->public) == -1 ){
	SET_ERRNO( EBADF );
	return -1;
    }

    hentry = this->handle_allocator->entry(fd);

    switch(whence)
	{
	case SEEK_SET:
	    offset = channel_pos(this, fd, EPosSetAbsolute, EPosSeek, offset );
	    break;
	case SEEK_CUR:
	    if ( !offset )
		offset = channel_pos(this, fd, EPosGet, EPosSeek, offset );
	    else
		offset = channel_pos(this, fd, EPosSetRelative, EPosSeek, offset );
	    break;
	case SEEK_END:{
	    off_t size = CHANNEL_SIZE( CHANNEL_ITEM_BY_INODE(this->channels_array, hentry->inode) );
	    /*use runtime size instead static size in zvm channel*/
	    offset = channel_pos(this, fd, EPosSetAbsolute, EPosSeek, size + offset );
	    break;
	}
	default:
	    SET_ERRNO( EPERM ); /* in advanced version should be set to conventional value */
	    return -1;
	}

    /*
     * return current position in a special way since 64 bits
     * doesn't fit to return code (32 bits)
     */
    return offset;
}

static int channels_open(struct MountsPublicInterface* this_,const char* path, int oflag, uint32_t mode){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    errno=0;

    /*If specified open flag saying as that trying to open not directory*/
    if ( CHECK_FLAG(oflag, O_DIRECTORY) == 0 ){
        return open_channel( this, path, oflag, mode );
    }
    else { /*trying to open directory*/
        struct dir_data_t *dir = 
	    match_dir_in_directory_list( &this->manifest_dirs, path, strlen(path));
        /*if valid directory path matched */
        if ( dir != NULL ){
	    /*directory exist, try to open*/
	    if ( CHECK_FLAG(oflag, O_RDONLY) ){
		int ofd_id = this->open_files_pool->getnew_ofd(oflag);

		/*it's allowed to open in read only mode*/
		int handle = this->handle_allocator->allocate_handle(&this->public,
								     dir->dir_inode,
								     dir->parent_dir_inode,
								     ofd_id);
		if ( handle == -1 ){
		    this->open_files_pool->release_ofd(ofd_id);
		    SET_ERRNO(ENFILE);
		    return -1;
		}
		return handle; /*directory handle is ok*/
	    }
	    else{  /*Not allowed read-write / write access*/
		SET_ERRNO( EACCES );
		return -1;
	    }
	}
	else{
	    /*no matched directory*/
	    SET_ERRNO( ENOENT );
	    return -1;
	}
    }
    return 0;
}

static int channels_fcntl(struct MountsPublicInterface* this_,int fd, int cmd, ...){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    if ( this->handle_allocator->check_handle_is_related_to_filesystem(fd, &this->public) == 0 ){
	const struct HandleItem* hentry = this->handle_allocator->entry(fd);
	ZRT_LOG(L_INFO, "fcntl cmd=%s", STR_FCNTL_CMD(cmd));
	if ( this->channels_array->match_by_inode(this->channels_array, hentry->inode) == NULL ){
	    SET_ERRNO(EBADF);
	    return -1;
	}
	return 0;
    }
    else{
	SET_ERRNO(ENOENT);
	return -1;
    }
}

static int channels_remove(struct MountsPublicInterface* this_,const char* path){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    struct ChannelArrayItem* item 
	= this->channels_array->match_by_name(this->channels_array, path);
    if ( item == NULL ){
	SET_ERRNO(ENOENT);
    }
    else{
	SET_ERRNO( EPERM );
    }

    return -1;
}

static int channels_unlink(struct MountsPublicInterface* this_,const char* path){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    struct ChannelArrayItem* item 
	= this->channels_array->match_by_name(this->channels_array, path);
    if ( item == NULL ){
	SET_ERRNO(ENOENT);
    }
    else{
	SET_ERRNO( EPERM );
    }

    return -1;
}

int channels_rename(struct MountsPublicInterface* this_, const char *oldpath, const char *newpath){
   SET_ERRNO( ENOSYS );
   return -1;
}

// access() uses the Mount's Stat().
static int channels_access(struct MountsPublicInterface* this,const char* path, int amode){
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_ftruncate_size(struct MountsPublicInterface* this,int fd, off_t length){
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_truncate_size(struct MountsPublicInterface* this,const char* path, off_t length){
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_isatty(struct MountsPublicInterface* this,int fd){
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_dup(struct MountsPublicInterface* this,int oldfd){
    /*see generic implementation in transparent_mount*/
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_dup2(struct MountsPublicInterface* this,int oldfd, int newfd){
    /*see generic implementation in transparent_mount*/
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_link(struct MountsPublicInterface* this,const char* path1, const char* path2){
    SET_ERRNO( ENOSYS );
    return -1;
}

ssize_t channels_readlink(struct MountsPublicInterface* this,
			  const char *path, char *buf, size_t bufsize){
    SET_ERRNO( ENOSYS );
    return -1;
}

int channels_symlink(struct MountsPublicInterface* this,
		     const char *oldpath, const char *newpath){
    SET_ERRNO( ENOSYS );
    return -1;
}

int channels_statvfs(struct MountsPublicInterface* this_, const char* path, struct statvfs *buf){
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_chown(struct MountsPublicInterface* this,const char* p, uid_t u, gid_t g){
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_fchown(struct MountsPublicInterface* this,int f, uid_t u, gid_t g){
    SET_ERRNO( ENOSYS );
    return -1;
}

static int channels_utime(struct MountsPublicInterface *this,
                          const char *filename, const struct utimbuf *times){
    SET_ERRNO( ENOSYS );
    return -1;
}


struct MountSpecificPublicInterface* channels_implem(struct MountsPublicInterface* this_){
    struct ChannelMounts *this = (struct ChannelMounts *)this_;
    return this->mount_specific_interface;
}


/*filesystem interface initialisation*/
static struct MountsPublicInterface KChannels_mount = {
    channels_readlink,
    channels_symlink,
    channels_chown,
    channels_chmod,
    channels_statvfs,
    channels_stat,
    channels_mkdir,
    channels_rmdir,
    channels_read,
    channels_write,
    channels_pread,
    channels_pwrite,
    channels_fchown,
    channels_fchmod,
    channels_fstat,
    channels_getdents,
    channels_fsync,
    channels_close,
    channels_lseek,
    channels_open,
    channels_fcntl,
    channels_remove,
    channels_unlink,
    channels_rename,
    channels_access,
    channels_ftruncate_size,
    channels_truncate_size,
    channels_isatty,
    channels_dup,
    channels_dup2,
    channels_link,
    channels_utime,
    channels_implem  /*mount_specific_interface*/
};

struct ChannelsModeUpdater{
    struct ChannelsModeUpdaterPublicInterface public;
    //data
    struct MountsPublicInterface* channels_mount;
};

/*used by mapping nvram section for setting custom channel type*/
void mode_updater_set_channel_mode(struct ChannelsModeUpdaterPublicInterface* this, 
				   const char* channel_name,
				   uint mode){
    struct ChannelsModeUpdater* this_ = (struct ChannelsModeUpdater*)this;
    struct ChannelMounts* mounts = (struct ChannelMounts*)this_->channels_mount;
    struct ChannelArrayItem* item 
	= mounts->channels_array->match_by_name(mounts->channels_array, channel_name);
    if ( item != NULL )
	item->channel_runtime.mode = mode;
}



struct ChannelsModeUpdaterPublicInterface*
channel_mode_updater_construct(struct MountsPublicInterface* channels_mount){
    struct ChannelsModeUpdater* this = malloc(sizeof(struct ChannelsModeUpdater));
    this->public.set_channel_mode = mode_updater_set_channel_mode;
    this->channels_mount = channels_mount;

    return (struct ChannelsModeUpdaterPublicInterface*)this;
}



struct MountsPublicInterface* 
channels_filesystem_construct( struct ChannelsModeUpdaterPublicInterface** mode_updater,
			       struct HandleAllocator* handle_allocator,
			       struct OpenFilesPool* open_files_pool,
			       const struct ZVMChannel* zvm_channels, int zvm_channels_count,
			       const struct ZVMChannel* emu_channels, int emu_channels_count ){
    struct ChannelMounts* this = malloc( sizeof(struct ChannelMounts) );

    /*set functions*/
    this->public = KChannels_mount;
    /*set data members*/
    this->handle_allocator = handle_allocator; /*use existing handle allocator*/
    this->open_files_pool = open_files_pool; /*use existing opened files pool*/
    this->channels_array = CONSTRUCT_L(CHANNELS_ARRAY)(zvm_channels, zvm_channels_count,
						       emu_channels, emu_channels_count);
    this->mount_specific_interface = 
	CONSTRUCT_L(MOUNT_SPECIFIC)( &KMountSpecificImplem,
				     this,
				     this->channels_array);
    this->manifest_dirs.dircount=0;

    *mode_updater = CONSTRUCT_L(CHANNEL_MODE_UPDATER)((struct MountsPublicInterface*)this);
    
    /*perform object construct*/
    process_channels_create_dir_list( this->channels_array, &this->manifest_dirs );

    /*assign parent_inode for channel items*/
    int i;
    for ( i=0; i < this->channels_array->count(this->channels_array); i++ ){
	struct ChannelArrayItem* current_chan =  this->channels_array->get(this->channels_array, i);
	int parentdir_len=0;
	int cursor;
	INIT_TEMP_CURSOR(&cursor) ;
	path_subpath_backward(&cursor, current_chan->channel->name, &parentdir_len); /*skip full component*/
	const char *parentdir_path = path_subpath_backward(&cursor, current_chan->channel->name, &parentdir_len);
	assert(parentdir_path);
	struct dir_data_t *parentdir_data = match_dir_in_directory_list(&this->manifest_dirs, parentdir_path, parentdir_len);
	assert(parentdir_data!=NULL);
	current_chan->channel_runtime.parent_inode = parentdir_data->dir_inode;
    }

    /*this info will not be logged here because logs not yet created */
#if 0
#ifdef DEBUG
    ZRT_LOG(L_EXTRA, "Based on manifest static directories count=%d", this->manifest_dirs.dircount);
    int i;
    for ( i=0; i < this->manifest_dirs.dircount; i++ ){
        ZRT_LOG( L_EXTRA, "dir[%d].handle=%d; .path=%20s; .nlink=%d", i,
		 this->manifest_dirs.dir_array[i].handle,
		 this->manifest_dirs.dir_array[i].path,
		 this->manifest_dirs.dir_array[i].nlink);
    }
#endif
#endif

    return (struct MountsPublicInterface*)this;
}



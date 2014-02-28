/*
 *
 * Copyright (c) 2014, LiteStack, Inc.
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


#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>

extern "C" {
#include "zrtlog.h"
#include "zrt_helper_macros.h"
}
#include "nacl-mounts/memory/MemMount.h"
#include "nacl-mounts/util/Path.h"
#include "mem_mount_wraper.h"
#include "mounts_interface.h"
#include "mount_specific_interface.h"
extern "C" {
#include "handle_allocator.h" //struct HandleAllocator, struct HandleItem
#include "open_file_description.h" //struct OpenFilesPool, struct OpenFileDescription
#include "fstab_observer.h" /*lazy mount*/
#include "fcntl_implem.h"
#include "channels_mount.h"
#include "enum_strings.h"
}

#define GET_DESCRIPTOR_ENTRY_CHECK(fs, entry)		\
    *(entry) = fs->handle_allocator->handle(fd);	\
    if ( entry == NULL ){				\
	SET_ERRNO(EBADF);				\
	return -1;					\
    }

#define NAME_LENGTH_CHECK(name)
    if ( strlen(name) > NAME_MAX ){
	SET_ERRNO(ENAMETOOLONG);
        return -1;
    }


#define NODE_OBJECT_BYINODE(memount_p, inode) memount_p->ToMemNode(inode)
#define NODE_OBJECT_BYPATH(memount_p, path) memount_p->GetMemNode(path)

/*retcode -1 at fail, 0 if OK*/
#define GET_INODE_BY_HANDLE(halloc_p, handle, inode_p, retcode_p){	\
	*retcode_p = halloc_p->get_inode( handle, inode_p );		\
    }

#define GET_INODE_BY_HANDLE_OR_RAISE_ERROR(halloc_p, handle, inode_p){	\
	int ret;							\
	GET_INODE_BY_HANDLE(halloc_p, handle, inode_p, &ret);		\
	if ( ret != 0 ){						\
	    SET_ERRNO(EBADF);						\
	    return -1;							\
	}								\
    }

#define GET_STAT_BYPATH_OR_RAISE_ERROR(memount_p, path, stat_p ){	\
	int ret = memount_p->GetNode( path, stat_p);			\
	if ( ret != 0 ) return ret;					\
    }

#define HALLOCATOR_BY_MOUNT_SPECIF(mount_specific_interface_p)		\
    ((struct InMemoryMounts*)((struct MountSpecificImplem*)(mount_specific_interface_p))->mount)->handle_allocator

#define OFILESPOOL_BY_MOUNT_SPECIF(mount_specific_interface_p)		\
    ((struct InMemoryMounts*)((struct MountSpecificImplem*)(mount_specific_interface_p))->mount)->open_files_pool

#define MOUNT_INTERFACE_BY_MOUNT_SPECIF(mount_specific_interface_p)	\
    ((struct MountsPublicInterface*)((struct MountSpecificImplem*)(mount_specific_interface_p))->mount)

#define MEMOUNT_BY_MOUNT_SPECIF(mount_specific_interface_p)		\
    MEMOUNT_BY_MOUNT( MOUNT_INTERFACE_BY_MOUNT_SPECIF(mount_specific_interface_p) )

#define HALLOCATOR_BY_MOUNT(mount_interface_p)				\
    ((struct InMemoryMounts*)(mount_interface_p))->handle_allocator

#define OFILESPOOL_BY_MOUNT(mount_interface_p)				\
    ((struct InMemoryMounts*)(mount_interface_p))->open_files_pool

#define MEMOUNT_BY_MOUNT(mounts_interface_p) ((struct InMemoryMounts*)mounts_interface_p)->mem_mount_cpp

#define CHECK_FILE_OPEN_FLAGS_OR_RAISE_ERROR(flags, flag1, flag2)	\
    if ( (flags)!=flag1 && (flags)!=flag2 ){				\
	ZRT_LOG(L_ERROR, "file open flags must be %s or %s",		\
		STR_FILE_OPEN_FLAGS(flag1), STR_FILE_OPEN_FLAGS(flag2)); \
	SET_ERRNO( EINVAL );						\
	return -1;							\
    }


struct InMemoryMounts{
    struct MountsPublicInterface public_;
    struct HandleAllocator* handle_allocator;
    struct OpenFilesPool*   open_files_pool;
    struct LowLevelFilesystemPublicInterface* lowlevelfs;
    MemMount*               mem_mount_cpp;
    struct MountSpecificPublicInterface* mount_specific_interface;
};


static const char* name_from_path( std::string path ){
    /*retrieve directory name, and compare name length with max available*/
    size_t pos=path.rfind ( '/' );
    int namelen = 0;
    if ( pos != std::string::npos ){
	namelen = path.length() -(pos+1);
	return path.c_str()+path.length()-namelen;
    }
    return NULL;
}

static int is_dir( struct LowLevelFilesystemPublicInterface* this_, ino_t inode ){
    struct stat st;
    int ret = this_->stat( this_, inode, &st );
    assert( ret == 0 );
    if ( S_ISDIR(st.st_mode) )
	return 1;
    else
	return 0;
}

static ssize_t get_file_len( struct MountsPublicInterface* this_, ino_t node) {
    struct stat st;
    if (0 != ((struct InMemoryMounts*)this_)->mem_mount_cpp->Stat(node, &st)) {
	return -1;
    }
    return (ssize_t) st.st_size;
}


/***********************************************************************
   implementation of MountSpecificPublicInterface as part of
   filesystem.  Below resides related functions.*/

struct MountSpecificImplem{
    struct MountSpecificPublicInterface public_;
    struct InMemoryMounts* mount;
};

/*return 0 if handle not valid, or 1 if handle is correct*/
static int check_handle(struct MountSpecificPublicInterface* this_, int handle){
    return !HALLOCATOR_BY_MOUNT_SPECIF(this_)
	 ->check_handle_is_related_to_filesystem(handle, 
						 &((struct MountSpecificImplem*)this_)->mount->public_);
}

static const char* path_handle(struct MountSpecificPublicInterface* this_, int handle){
    if ( HALLOCATOR_BY_MOUNT_SPECIF(this_)
	 ->check_handle_is_related_to_filesystem(handle, 
						 &((struct MountSpecificImplem*)this_)->mount->public_) == 0 ){
	const struct HandleItem* hentry;
	hentry = HALLOCATOR_BY_MOUNT_SPECIF(this_)->entry(handle);

	/*get runtime information related to channel*/
	MemNode* mnode = NODE_OBJECT_BYINODE( MEMOUNT_BY_MOUNT_SPECIF(this_), hentry->inode);
	if ( mnode ){
	    return mnode->name().c_str();
	}
	else
	    return NULL;
    }
    else
	return NULL;
}

static int file_status_flags(struct MountSpecificPublicInterface* this_, int fd){
    if ( HALLOCATOR_BY_MOUNT_SPECIF(this_)
	 ->check_handle_is_related_to_filesystem(fd, 
						 &((struct MountSpecificImplem*)this_)->mount->public_) == 0 ){
	const struct OpenFileDescription* ofd;
	ofd = HALLOCATOR_BY_MOUNT_SPECIF(this_)->ofd(fd);
	assert(ofd);
	return ofd->flags;
    }
    else{
	SET_ERRNO(EBADF);						\
	return -1;							\
    }
}

static int set_file_status_flags(struct MountSpecificPublicInterface* this_, int fd, int flags){
    if ( HALLOCATOR_BY_MOUNT_SPECIF(this_)
	 ->check_handle_is_related_to_filesystem(fd, 
						 &((struct MountSpecificImplem*)this_)->mount->public_) == 0 ){
	const struct HandleItem* hentry;
	hentry = HALLOCATOR_BY_MOUNT_SPECIF(this_)->entry(fd);
	OFILESPOOL_BY_MOUNT_SPECIF(this_)->set_flags( hentry->open_file_description_id,
						      flags);
	return flags;
    }
    else{
	SET_ERRNO(EBADF);
	return -1;
    }
}


/*return pointer at success, NULL if fd didn't found or flock structure has not been set*/
static const struct flock* flock_data(struct MountSpecificPublicInterface* this_, int fd ){
    if ( HALLOCATOR_BY_MOUNT_SPECIF(this_)
	 ->check_handle_is_related_to_filesystem(fd, 
						 &((struct MountSpecificImplem*)this_)->mount->public_) == 0 ){
	MemNode* mnode;	
	const struct HandleItem* hentry;
	hentry = HALLOCATOR_BY_MOUNT_SPECIF(this_)->entry(fd);

	/*get runtime information related to channel*/
    	mnode = NODE_OBJECT_BYINODE( MEMOUNT_BY_MOUNT_SPECIF(this_), hentry->inode);
	assert(mnode);
	return mnode->flock();
    }
    else{
	SET_ERRNO(EBADF);
	return NULL;
    }
}

/*return 0 if success, -1 if fd didn't found*/
static int set_flock_data(struct MountSpecificPublicInterface* this_, int fd, const struct flock* flock_data ){
    if ( HALLOCATOR_BY_MOUNT_SPECIF(this_)
	 ->check_handle_is_related_to_filesystem(fd, 
						 &((struct MountSpecificImplem*)this_)->mount->public_) == 0 ){
	MemNode* mnode;	
	const struct HandleItem* hentry;
	hentry = HALLOCATOR_BY_MOUNT_SPECIF(this_)->entry(fd);

	/*get runtime information related to channel*/
    	mnode = NODE_OBJECT_BYINODE( MEMOUNT_BY_MOUNT_SPECIF(this_), hentry->inode);
	assert(mnode);
	mnode->set_flock(flock_data);
	return 0;
    }
    else{
	SET_ERRNO(EBADF);
	return -1;
    }
}

static struct MountSpecificPublicInterface KMountSpecificImplem = {
    check_handle,
    path_handle,
    file_status_flags,
    set_file_status_flags,
    flock_data,
    set_flock_data
};


static struct MountSpecificPublicInterface*
mount_specific_construct( struct MountSpecificPublicInterface* specific_implem_interface,
			  struct InMemoryMounts* mount ){
    struct MountSpecificImplem* this_ = (struct MountSpecificImplem*)malloc(sizeof(struct MountSpecificImplem));
    /*set functions*/
    this_->public_ = *specific_implem_interface;
    this_->mount = mount;
    return (struct MountSpecificPublicInterface*)this_;
}


/*helpers*/

/*@return 0 if success, -1 if we don't need to mount*/
static int lazy_mount( struct MountsPublicInterface* this_, const char* path){
    struct stat st;
    int ret = MEMOUNT_BY_MOUNT(this_)->GetNode( path, &st);
    (void)ret;
    /*if it's time to do mount, then do all waiting mounts*/
    FstabObserver* observer = get_fstab_observer();
    struct FstabRecordContainer* record;
    while( NULL != (record = observer->locate_postpone_mount( observer, path, 
							      EFstabMountWaiting)) ){
	observer->mount_import(observer, record);
	return 0;
    }
    return -1;
}


/*wraper implementation*/

static int mem_chown(struct MountsPublicInterface* this_, const char* path, uid_t owner, gid_t group){
    SET_ERRNO(ENOSYS);
    return -1;
}

static int mem_chmod(struct MountsPublicInterface* this_, const char* path, uint32_t mode){
    SET_ERRNO(ENOSYS);
    return -1;
}

static int mem_stat(struct MountsPublicInterface* this_, const char* path, struct stat *buf){
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    MemNode *node;
    int ret = -1;

    NAME_LENGTH_CHECK;

    node = fs->mem_mount_cpp->GetMemNode(path);

    if ( !node ){
	SET_ERRNO(ENOENT);
	return -1;
    }

    if ( fs->lowlevelfs->stat
	 && (ret=fs->lowlevelfs->stat(fs->lowlevelfs, node->slot(), buf)) != -1
	 && node->hardinode() > 0 ){
	/*fixes for hardlinks pseudo support, different hardlinks must have same inode,
	 *but internally all nodes have separeted inodes*/
	/*patch inode if it has hardlinks*/
	buf->st_ino = (ino_t)node->hardinode();
    }
    
    return ret;
}

static int mem_mkdir(struct MountsPublicInterface* this_, const char* path, uint32_t mode){
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    MemNode *parent_node;
    int ret=-1;

    NAME_LENGTH_CHECK(path);

    if ( fs->mem_mount_cpp->GetMemNode(path) != NULL ){
	SET_ERRNO(EEXIST);
	return -1;
    }

    if ( (parent_node=GetParentMemNode(path)) == NULL ){
	SET_ERRNO(ENOENT);
	return -1;
    }

    const char* name = name_from_path( path);
    if ( name == NULL ){
	SET_ERRNO(ENOTDIR);
	return -1;
    }

    if (fs->lowlevelfs->mkdir != NULL ){
	ret = fs->lowlevelfs->mkdir(fs->lowlevelfs, parent_node->slot(), name, mode);
    }
    return ret;
}


static int mem_rmdir(struct MountsPublicInterface* this_, const char* path){
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    MemNode *parent_node;
    int ret;

    NAME_LENGTH_CHECK(path);

    if ( fs->mem_mount_cpp->GetMemNode(path) != NULL ){
	SET_ERRNO(EEXIST);
	return -1;
    }

    if ( (parent_node=GetParentMemNode(path)) == NULL ){
	SET_ERRNO(ENOENT);
	return -1;
    }

    const char* name = name_from_path( path);
    if ( name == NULL ){
	SET_ERRNO(ENOTDIR);
	return -1;
    }

    if (fs->lowlevelfs->rmdir != NULL ){
	ret = fs->lowlevelfs->rmdir(fs->lowlevelfs, parent_node->slot(), name, mode);
    }
    return ret;
}

static ssize_t mem_read(struct MountsPublicInterface* this_, int fd, void *buf, size_t nbyte){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;
    const struct OpenFileDescription* ofd = fs->handle_allocator->ofd(fd);

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( !CHECK_FLAG(flags, O_RDONLY) && !CHECK_FLAG(flags, O_RDWR) ){
	SET_ERRNO(EINVAL);
        return -1;
    }

    if ( fs->lowlevelfs->pread &&
	(ret=fs->lowlevelfs->pread(fs->lowlevelfs, entry->inode, buf, nbytes, ofd->offset)) != -1 ){
	/*update resulted offset*/
	int ret2 = fs->open_files_pool->set_offset(entry->open_file_description_id, ofd->offset+ret );
	assert(ret2==0);
    }
    
    return ret;
}

static ssize_t mem_write(struct MountsPublicInterface* this_, int fd, const void *buf, size_t nbyte){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;
    const struct OpenFileDescription* ofd = fs->handle_allocator->ofd(fd);

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( !CHECK_FLAG(flags, O_WRONLY) && !CHECK_FLAG(flags, O_RDWR) ){
	SET_ERRNO(EINVAL);
        return -1;
    }

    if ( fs->lowlevelfs->pwrite &&
	 (ret=fs->lowlevelfs->pwrite(fs->lowlevelfs, entry->inode, buf, nbytes, ofd->offset)) != -1 ){
	/*update resulted offset*/
	int ret2 = fs->open_files_pool->set_offset(entry->open_file_description_id, ofd->offset+ret );
	assert(ret2==0);
    }
    
    return ret;
}

static ssize_t mem_pread(struct MountsPublicInterface* this_, 
			 int fd, void *buf, size_t nbyte, off_t offset){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( !CHECK_FLAG(flags, O_RDONLY) && !CHECK_FLAG(flags, O_RDWR) ){
	SET_ERRNO(EINVAL);
        return -1;
    }

    if ( fs->lowlevelfs->pread &&
	 (ret=fs->lowlevelfs->pread(fs->lowlevelfs, entry->inode, buf, nbytes, offset)) != -1 ){
	/*update resulted offset*/
	int ret2 = fs->open_files_pool->set_offset(entry->open_file_description_id, offset+ret );
	assert(ret2==0);
    }
    
    return ret;
}

static ssize_t mem_pwrite(struct MountsPublicInterface* this_, 
			  int fd, const void *buf, size_t nbyte, off_t offset){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( !CHECK_FLAG(flags, O_WRONLY) && !CHECK_FLAG(flags, O_RDWR) ){
	SET_ERRNO(EINVAL);
        return -1;
    }

    if ( fs->lowlevelfs->pwrite &&
	 (ret=fs->lowlevelfs->pwrite(fs->lowlevelfs, entry->inode, buf, nbytes, offset)) != -1 ){
	/*update resulted offset*/
	int ret2 = fs->open_files_pool->set_offset(entry->open_file_description_id, offset+ret );
	assert(ret2==0);
    }
    
    return ret;
}

static int mem_fchown(struct MountsPublicInterface* this_, int fd, uid_t owner, gid_t group){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( fs->lowlevelfs->chown &&
	 (ret=fs->lowlevelfs->chown( fs->lowlevelfs, entry->inode, owner, group)) != -1 ){
	;
    }
    return ret;
}

static int mem_fchmod(struct MountsPublicInterface* this_, int fd, uint32_t mode){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( fs->lowlevelfs->chmod &&
	 (ret=fs->lowlevelfs->chmod( fs->lowlevelfs, entry->inode, mode)) != -1 ){
	;
    }
    return ret;
}


static int mem_fstat(struct MountsPublicInterface* this_, int fd, struct stat *buf){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( fs->lowlevelfs->stat &&
	 (ret=fs->lowlevelfs->stat( fs->lowlevelfs, entry->inode, buf)) != -1 ){
	;
    }
    return ret;
}

static int mem_getdents(struct MountsPublicInterface* this_, int fd, void *buf, unsigned int count){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;
    const struct OpenFileDescription* ofd = fs->handle_allocator->ofd(fd);

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    ssize_t readed=0;

    if ( fs->lowlevelfs->getdents &&
	 (readed=fs->lowlevelfs->getdents( fs->lowlevelfs, entry->inode, buf, count)) != -1 ){
    }
    else if ( (readed=fs->mem_mount_cpp->Getdents(entry->inode, ofd->offset, (DIRENT*)buf, count)) != -1 ){
    }

    if ( (ret=readed) > 0 ){
	int ret2;
	ret2 = ofd->set_offset( entry->open_file_description_id, 
				ofd->offset + readed );
	assert( ret2 == 0 );
    }

    return ret;
}

static int mem_fsync(struct MountsPublicInterface* this_, int fd){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    errno=ENOSYS;
    return -1;
}

static int mem_close(struct MountsPublicInterface* this_, int fd){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( fs->lowlevelfs->close &&
	 (ret=fs->lowlevelfs->close( fs->lowlevelfs, entry->inode)) != -1 ){
    }

    struct MemNode* node = fs->mem_mount_cpp->ToMemNode(entry->inode);
    assert(node!=NULL);
    node->Unref(entry->inode); /*decrement use count*/

    if ( node->UnlinkisTrying() ){
        ret = fs->mem_mount_cpp->UnlinkInternal(mnode);
        assert( ret == 0 );	
    }

    ret=fs->open_files_pool->release_ofd(entry->open_file_description_id);    
    assert( ret == 0 );
    ret = fs->handle_allocator->free_handle(fd);

    return ret;
}

static off_t mem_lseek(struct MountsPublicInterface* this_, int fd, off_t offset, int whence){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;
    const struct OpenFileDescription* ofd = fs->open_files_pool->ofd(fd);
    struct stat st;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    int ret = fs->lowlevelfs->stat( fs->lowlevelfs, inode, &st );
    if ( ret!=0 || !S_ISDIR(st.st_mode) ){
	SET_ERRNO(EBADF);
	return -1;
    }

    off_t next = ofd->offset;
    int ret;
    ssize_t len;
    switch (whence) {
    case SEEK_SET:
	next = offset;
	break;
    case SEEK_CUR:
	next += offset;
	break;
    case SEEK_END:
	len = st.st_size;
	if (len == -1) {
	    return -1;
	}
	next = static_cast<size_t>(len) + offset;
	break;
    default:
	SET_ERRNO(EINVAL);
	return -1;
    }
    // Must not seek beyond the front of the file.
    if (next < 0) {
	SET_ERRNO(EINVAL);
	return -1;
    }
    // Go to the new offset.
    ret = fs->open_files_pool->set_offset(entry->open_file_description_id, next);
    assert( ret == 0 );
    return next;
}

static int mem_open(struct MountsPublicInterface* this_, const char* path, int oflag, uint32_t mode){
    int ret=-1;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    MemNode *parent_node;
    struct stat st;
    MemNode *node = fs->mem_mount_cpp->GetMemNode(path);

    NAME_LENGTH_CHECK(path);

    if ( oflag&O_CREAT && oflag&O_EXCL 
	 && node != NULL ){
	SET_ERRNO(EEXIST);
	return -1;
    }

    if ( fs->lowlevelfs->stat ){
	ret=fs->lowlevelfs->stat( fs->lowlevelfs, inode, &st );
    }
    else{
	SET_ERRNO(ENOSYS);
	return -1;
    }

    if ( oflag&O_DIRECTORY ){
	int ret;
	if ( !S_ISDIR(st.st_mode) ){
	    SET_ERRNO(ENOTDIR);
	    return -1;
	}
    }

    if ( (parent_node=GetParentMemNode(path)) == NULL ){
	SET_ERRNO(ENOENT);
	return -1;
    }

    const char* name = name_from_path( path);
    if ( name == NULL ){
	SET_ERRNO(ENOTDIR);
	return -1;
    }

    if (fs->lowlevelfs->open != NULL &&
	(ret = fs->lowlevelfs->open(fs->lowlevelfs, parent_node->slot(), name, oflag, mode)) != -1 ){

	int open_file_description_id = fs->open_files_pool->getnew_ofd(oflag);

	/*ask for file descriptor in handle allocator*/
	ret = fs->handle_allocator->allocate_handle( this_, 
							node->slot(),
							open_file_description_id);
	if ( ret < 0 ){
	    /*it's hipotetical but possible case if amount of open files 
	      are exceeded an maximum value.*/
	    fs->open_files_pool->release_ofd(open_file_description_id);
	    SET_ERRNO(ENFILE);
	    return -1;
	}
	fs->mem_mount_cpp->Ref(node->slot()); /*set file referred*/
	/*success*/
    }
    return ret;
}

//open
	// /*append feature support, is simple*/
	// if ( oflag & O_APPEND ){
	//     ZRT_LOG(L_SHORT, P_TEXT, "handle flag: O_APPEND");
	//     mem_lseek(this_, fd, 0, SEEK_END);
	// }
	// /*file truncate support, only for writable files, reset size*/
	// if ( oflag&O_TRUNC && (oflag&O_RDWR || oflag&O_WRONLY) ){
	//     /*reset file size*/
	//     MemNode* mnode = NODE_OBJECT_BYINODE( MEMOUNT_BY_MOUNT(this_), st.st_ino);
	//     if (mnode){ 
	// 	ZRT_LOG(L_SHORT, P_TEXT, "handle flag: O_TRUNC");
	// 	/*update stat*/
	// 	st.st_size = 0;
	// 	mnode->set_len(st.st_size);
	// 	ZRT_LOG(L_SHORT, "%s, %d", mnode->name().c_str(), mnode->len() );
	//     }
	// }


static int mem_fcntl(struct MountsPublicInterface* this_, int fd, int cmd, ...){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct HandleItem* entry;

    ZRT_LOG(L_INFO, "cmd=%s", STR_FCNTL_CMD(cmd));
    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( is_dir(this_, entry->inode) ){
	SET_ERRNO(EBADF);
	return -1;
    }

    SET_ERRNO(ENOSYS);
    return -1;
}

static int mem_remove(struct MountsPublicInterface* this_, const char* path){
    int ret=-1;
    struct stat st;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    MemNode *parent_node = fs->mem_mount_cpp->GetParentMemNode(path);
    MemNode *node = fs->mem_mount_cpp->GetMemNode(path);

    NAME_LENGTH_CHECK(path);

    if ( node==NULL || parent_node ){
	SET_ERRNO(ENOENT);
	return -1;
    }

    const char* name = name_from_path( path);
    if ( name == NULL ){
	SET_ERRNO(ENOTDIR);
	return -1;
    }

    if ( fs->lowlevelfs->stat ){
	ret=fs->lowlevelfs->stat( fs->lowlevelfs, node->slot(), &st );
    }
    else{
	SET_ERRNO(ENOSYS);
	return -1;
    }

    if ( S_ISDIR(st.st_mode) ){
	if ( fs->lowlevelfs->rmdir &&
	     (ret=fs->lowlevelfs->rmdir( fs->lowlevelfs, parent_node->slot(), name )) == 0 ){
	    ;
	}
    }
    else{
	if ( fs->lowlevelfs->unlink &&
	     (ret=fs->lowlevelfs->unlink( fs->lowlevelfs, parent_node->slot(), name )) == 0 ){
	    ;
	}
    }
    return ret;
}

static int mem_unlink(struct MountsPublicInterface* this_, const char* path){
    int ret=-1;
    struct stat st;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    MemNode *parent_node = fs->mem_mount_cpp->GetParentMemNode(path);
    MemNode *node = fs->mem_mount_cpp->GetMemNode(path);

    NAME_LENGTH_CHECK(path);

    if ( node==NULL || parent_node ){
	SET_ERRNO(ENOENT);
	return -1;
    }

    const char* name = name_from_path( path);
    if ( name == NULL ){
	SET_ERRNO(ENOTDIR);
	return -1;
    }

    if ( fs->lowlevelfs->stat ){
	ret=fs->lowlevelfs->stat( fs->lowlevelfs, node->slot(), &st );
    }
    else{
	SET_ERRNO(ENOSYS);
	return -1;
    }

   if ( S_ISDIR(st.st_mode) ){
	SET_ERRNO(EISDIR);
	return -1;
    }

   if ( fs->lowlevelfs->unlink &&
	(ret=fs->lowlevelfs->unlink( fs->lowlevelfs, parent_node->slot(), name )) == 0 ){
       ;
   }
   return ret;
}

static int mem_access(struct MountsPublicInterface* this_, const char* path, int amode){
    SET_ERRNO(ENOSYS);
    return -1;
}

static int mem_ftruncate_size(struct MountsPublicInterface* this_, int fd, off_t length){
    int ret;
    struct InMemoryFs* fs = (struct InMemoryFs*)this_;
    const struct OpenFileDescription* ofd = fs->handle_allocator->ofd(fd);
    const struct HandleItem* entry;

    GET_DESCRIPTOR_ENTRY_CHECK(fs, entry);

    if ( is_dir(this_, entry->inode) ){
	SET_ERRNO(EISDIR);
	return -1;
    }

    int flags = ofd->flags & O_ACCMODE;
    /*check if file was not opened for writing*/
    if ( flags!=O_WRONLY && flags!=O_RDWR ){
	ZRT_LOG(L_ERROR, "file open flags=%s not allow truncate", 
		STR_FILE_OPEN_FLAGS(flags));
	SET_ERRNO( EINVAL );
	return -1;
    }

   if ( fs->lowlevelfs->ftruncate_size &&
	(ret=fs->lowlevelfs->ftruncate_size( fs->lowlevelfs, entry->inode, length )) == 0 ){
       /*in according to docs: if doing file size reducing then
	 offset should not be changed, but on ubuntu linux
	 an offset can't be setted up to beyond of file bounds and
	 it assignes to max availibale pos. Just do it the same
	 as on host*/
#define DO_NOT_ALLOW_OFFSET_BEYOND_FILE_BOUNDS_IF_TRUNCATE_REDUCES_FILE_SIZE

#ifdef DO_NOT_ALLOW_OFFSET_BEYOND_FILE_BOUNDS_IF_TRUNCATE_REDUCES_FILE_SIZE
       off_t offset = ofd->offset;
       if ( length < offset ){
	   offset = length+1;
	   ret = fs->open_files_pool->set_offset(entry->open_file_description_id,
						 offset);
       }
#endif //DO_NOT_ALLOW_OFFSET_BEYOND_FILE_BOUNDS_IF_TRUNCATE_REDUCES_FILE_SIZE
   }
   return ret;
}

int mem_truncate_size(struct MountsPublicInterface* this_, const char* path, off_t length){
    assert(0); 
    /*truncate implementation replaced by ftruncate call*/
    return -1;
}

static int mem_isatty(struct MountsPublicInterface* this_, int fd){
    return -1;
}

static int mem_dup(struct MountsPublicInterface* this_, int oldfd){
    return -1;
}

static int mem_dup2(struct MountsPublicInterface* this_, int oldfd, int newfd){
    return -1;
}

static int mem_link(struct MountsPublicInterface* this_, const char* oldpath, const char* newpath){
    lazy_mount(this_, oldpath);
    lazy_mount(this_, newpath);
    /*create new hardlink*/
    int ret = MEMOUNT_BY_MOUNT(this_)->Link(oldpath, newpath);
    if ( ret == -1 ){
	/*errno already setted by MemMount*/
	return ret;
    }
    return 0;
}

struct MountSpecificPublicInterface* mem_implem(struct MountsPublicInterface* this_){
    return ((struct InMemoryMounts*)this_)->mount_specific_interface;
}

static struct MountsPublicInterface KInMemoryMountWraper = {
    mem_chown,
    mem_chmod,
    mem_stat,
    mem_mkdir,
    mem_rmdir,
    mem_umount,
    mem_mount,
    mem_read,
    mem_write,
    mem_pread,
    mem_pwrite,
    mem_fchown,
    mem_fchmod,
    mem_fstat,
    mem_getdents,
    mem_fsync,
    mem_close,
    mem_lseek,
    mem_open,
    mem_fcntl,
    mem_remove,
    mem_unlink,
    mem_access,
    mem_ftruncate_size,
    mem_truncate_size,
    mem_isatty,
    mem_dup,
    mem_dup2,
    mem_link,
    EMemMountId,
    mem_implem  /*mount_specific_interface*/
};

struct MountsPublicInterface* 
inmemory_filesystem_construct( struct HandleAllocator* handle_allocator,
			       struct OpenFilesPool* open_files_pool,
			       struct LowLevelFilesystemPublicInterface* lowlevelfs){
    /*use malloc and not new, because it's external c object*/
    struct InMemoryMounts* this_ = (struct InMemoryMounts*)malloc( sizeof(struct InMemoryMounts) );

    /*set functions*/
    this_->public_ = KInMemoryMountWraper;
    /*set data members*/
    this_->handle_allocator = handle_allocator; /*use existing handle allocator*/
    this_->open_files_pool = open_files_pool; /*use existing open files pool*/
    this_->lowlevelfs = lowlevelfs;
    this_->mount_specific_interface = CONSTRUCT_L(MOUNT_SPECIFIC)( &KMountSpecificImplem,
								   this_);
    this_->mem_mount_cpp = new MemMount;
    return (struct MountsPublicInterface*)this_;
}


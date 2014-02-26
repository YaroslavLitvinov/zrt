/*
 * ZFS filesystem interface implementation
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


#include "zfs_filesystem.h"

struct ZfsFilesystem{
    struct LowLevelFilesystemPublicInterface public_;
    vfs_t *vfs;
};


static int internal_stat(vnode_t *vp, struct stat *stbuf)
{
	ASSERT(vp != NULL);
	ASSERT(stbuf != NULL);

	vattr_t vattr;
	vattr.va_mask = AT_STAT | AT_NBLOCKS | AT_BLKSIZE | AT_SIZE;

	cred_t *cred = NULL;
	int error = VOP_GETATTR(vp, &vattr, 0, cred, NULL);
	if(error)
		return error;

	memset(stbuf, 0, sizeof(struct stat));

	stbuf->st_dev = vattr.va_fsid;
	stbuf->st_ino = vattr.va_nodeid == 3 ? 1 : vattr.va_nodeid;
	stbuf->st_mode = VTTOIF(vattr.va_type) | vattr.va_mode;
	stbuf->st_nlink = vattr.va_nlink;
	stbuf->st_uid = vattr.va_uid;
	stbuf->st_gid = vattr.va_gid;
	stbuf->st_rdev = vattr.va_rdev;
	stbuf->st_size = vattr.va_size;
	stbuf->st_blksize = vattr.va_blksize;
	stbuf->st_blocks = vattr.va_nblocks;
	TIMESTRUC_TO_TIME(vattr.va_atime, &stbuf->st_atime);
	TIMESTRUC_TO_TIME(vattr.va_mtime, &stbuf->st_mtime);
	TIMESTRUC_TO_TIME(vattr.va_ctime, &stbuf->st_ctime);

	return 0;
}


static int zfs_stat(struct LowLevelFilesystemPublicInterface* this_, 
	     ino_t inode, struct stat *buf){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *znode;

	int error = zfs_zget(zfsvfs, inode, &znode, B_TRUE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	ASSERT(znode != NULL);
	vnode_t *vp = ZTOV(znode);
	ASSERT(vp != NULL);

	struct stat stbuf;
	error = internal_stat(vp, &stbuf);

	VN_RELE(vp);
	ZFS_EXIT(zfsvfs);
	return error;

}
static int zfs_mkdir(struct LowLevelFilesystemPublicInterface* this_, 
		     ino_t parent_inode, const char* name, uint32_t mode){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;

	if(strlen(name) >= MAXNAMELEN)
		return ENAMETOOLONG;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *znode;

	int error = zfs_zget(zfsvfs, parent, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	ASSERT(znode != NULL);
	vnode_t *dvp = ZTOV(znode);
	ASSERT(dvp != NULL);

	vnode_t *vp = NULL;

	vattr_t vattr = { 0 };
	vattr.va_type = VDIR;
	vattr.va_mode = mode & PERMMASK;
	vattr.va_mask = AT_TYPE | AT_MODE;

	cred_t* cred = NULL;
	error = VOP_MKDIR(dvp, (char *) name, &vattr, &vp, cred, NULL, 0, NULL);
	if(error)
		goto out;

	ASSERT(vp != NULL);

	struct fuse_entry_param e = { 0 };

	e.attr_timeout = 0.0;
	e.entry_timeout = 0.0;

	e.ino = VTOZ(vp)->z_id;
	if(e.ino == 3)
		e.ino = 1;

	e.generation = VTOZ(vp)->z_phys->zp_gen;

	error = internal_stat(vp, &e.attr);

out:
	if(vp != NULL)
		VN_RELE(vp);
	VN_RELE(dvp);
	ZFS_EXIT(zfsvfs);

	return error;
}
static int zfs_rmdir(struct LowLevelFilesystemPublicInterface* this_, 
		     ino_t parent_inode, const char* name){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;

	if(strlen(name) >= MAXNAMELEN)
		return ENAMETOOLONG;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *znode;

	int error = zfs_zget(zfsvfs, parent_inode, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	ASSERT(znode != NULL);
	vnode_t *dvp = ZTOV(znode);
	ASSERT(dvp != NULL);

	cred_t *cred = NULL;

	/* FUSE doesn't care if we remove the current working directory
	   so we just pass NULL as the cwd parameter (no problem for ZFS) */
	error = VOP_RMDIR(dvp, (char *) name, NULL, cred, NULL, 0);

	/* Linux uses ENOTEMPTY when trying to remove a non-empty directory */
	if(error == EEXIST)
		error = ENOTEMPTY;

	VN_RELE(dvp);
	ZFS_EXIT(zfsvfs);

	return error;
}

static ssize_t zfs_pread(struct LowLevelFilesystemPublicInterface* this_,
			 ino_t inode, void *buf, size_t nbyte, off_t offset){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);
	znode_t *znode;

	int error = zfs_zget(zfsvfs, inode, &znode, B_TRUE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}
	ASSERT(znode != NULL);
	vnode_t *vp = ZTOV(znode);
	ASSERT(vp != NULL);

	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_llimit = RLIM64_INFINITY;

	iovec.iov_base = buf;
	iovec.iov_len = size;
	uio.uio_resid = iovec.iov_len;
	uio.uio_loffset = offset;

	cred_t *cred = NULL;

	int error = VOP_READ(vp, &uio, info->flags, cred, NULL);

	ZFS_EXIT(zfsvfs);

	return error;
}

static ssize_t zfs_pwrite(struct LowLevelFilesystemPublicInterface* this_,
			  ino_t inode, const void *buf, size_t nbyte, off_t offset){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_llimit = RLIM64_INFINITY;

	iovec.iov_base = (void *) buf;
	iovec.iov_len = size;
	uio.uio_resid = iovec.iov_len;
	uio.uio_loffset = off;

	cred_t *cred = NULL;
	int error = VOP_WRITE(vp, &uio, info->flags, cred, NULL);

	ZFS_EXIT(zfsvfs);

	if(!error) {
		/* When not using direct_io, we must always write 'size' bytes */
		VERIFY(uio.uio_resid == 0);
	}
	return error;
}

static int zfs_getdents(struct LowLevelFilesystemPublicInterface* this_, 
			ino_t inode, void *buf, unsigned int count){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);
	znode_t *znode;

	int error = zfs_zget(zfsvfs, inode, &znode, B_TRUE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}
	ASSERT(znode != NULL);
	vnode_t *vp = ZTOV(znode);
	ASSERT(vp != NULL);

	if(vp->v_type != VDIR)
		return ENOTDIR;

	ZFS_ENTER(zfsvfs);

	cred_t *cred = NULL;

	union {
		char buf[DIRENT64_RECLEN(MAXNAMELEN)];
		struct dirent64 dirent;
	} entry;

	struct stat fstat = { 0 };

	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;
	uio.uio_llimit = RLIM64_INFINITY;

	int eofp = 0;

	int outbuf_off = 0;
	int outbuf_resid = size;

	off_t next = off;

	int error;

	for(;;) {
		iovec.iov_base = entry.buf;
		iovec.iov_len = sizeof(entry.buf);
		uio.uio_resid = iovec.iov_len;
		uio.uio_loffset = next;

		error = VOP_READDIR(vp, &uio, cred, &eofp, NULL, 0);
		if(error)
			goto out;

		/* No more directory entries */
		if(iovec.iov_base == entry.buf)
			break;

		fstat.st_ino = entry.dirent.d_ino;
		fstat.st_mode = 0;

		int dsize = this_->toplevelfs
		    ->dirent_size(strlen(entry.dirent.d_name));

		if(dsize > outbuf_resid)
			break;

		this_->toplevelfs->add_dirent(buf + outbuf_off, 
					      entry.dirent.d_name,
					      &fstat, 
					      entry.dirent.d_off);

		outbuf_off += dsize;
		outbuf_resid -= dsize;
		next = entry.dirent.d_off;
	}

out:
	ZFS_EXIT(zfsvfs);

	return error;
}

static int zfs_close(struct LowLevelFilesystemPublicInterface* this_, ino_t inode, int fd){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);
	ZFS_ENTER(zfsvfs);
	znode_t *znode;

	int error = zfs_zget(zfsvfs, inode, &znode, B_TRUE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}
	ASSERT(znode != NULL);
	vnode_t *vp = ZTOV(znode);
	ASSERT(vp != NULL);

	cred_t *cred = NULL;

	/////////////////////////////////////////////
	//flags - get from toplevelfs
	int error = VOP_CLOSE(vp, 
			      this_->toplevelfs->ofd(fd)->flags, 
			      1, (offset_t) 0, cred, NULL);
	//////////////////////////////////////////////
	VERIFY(error == 0);

	VN_RELE(vp);

	ZFS_EXIT(zfsvfs);

	return error;
}
static int zfs_open(struct LowLevelFilesystemPublicInterface* this_, 
		    ino_t parent_inode, const char* name, int fflags, uint32_t mode){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;

	if(name && strlen(name) >= MAXNAMELEN)
		return ENAMETOOLONG;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	cred_t *cred = NULL;

	/* Map flags */
	int mode, flags;

	if(fflags & O_WRONLY) {
		mode = VWRITE;
		flags = FWRITE;
	} else if(fflags & O_RDWR) {
		mode = VREAD | VWRITE;
		flags = FREAD | FWRITE;
	} else {
		mode = VREAD;
		flags = FREAD;
	}

	if(fflags & O_CREAT)
		flags |= FCREAT;
	if(fflags & O_SYNC)
		flags |= FSYNC;
	if(fflags & O_DSYNC)
		flags |= FDSYNC;
	if(fflags & O_RSYNC)
		flags |= FRSYNC;
	if(fflags & O_APPEND)
		flags |= FAPPEND;
	if(fflags & O_LARGEFILE)
		flags |= FOFFMAX;
	if(fflags & O_NOFOLLOW)
		flags |= FNOFOLLOW;
	if(fflags & O_TRUNC)
		flags |= FTRUNC;
	if(fflags & O_EXCL)
		flags |= FEXCL;

	znode_t *znode;

	int error = zfs_zget(zfsvfs, ino, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	ASSERT(znode != NULL);
	vnode_t *vp = ZTOV(znode);
	ASSERT(vp != NULL);

	if (flags & FCREAT) {
		enum vcexcl excl;

		/*
		 * Wish to create a file.
		 */
		vattr_t vattr;
		vattr.va_type = VREG;
		vattr.va_mode = mode;
		vattr.va_mask = AT_TYPE|AT_MODE;
		if (flags & FTRUNC) {
			vattr.va_size = 0;
			vattr.va_mask |= AT_SIZE;
		}
		if (flags & FEXCL)
			excl = EXCL;
		else
			excl = NONEXCL;

		vnode_t *new_vp;
		/* FIXME: check filesystem boundaries */
		error = VOP_CREATE(vp, (char *) name, &vattr, excl, mode, &new_vp, cred, 0, NULL, NULL);

		if(error)
			goto out;

		VN_RELE(vp);
		vp = new_vp;
	} else {
		/*
		 * Get the attributes to check whether file is large.
		 * We do this only if the O_LARGEFILE flag is not set and
		 * only for regular files.
		 */
		if (!(flags & FOFFMAX) && (vp->v_type == VREG)) {
			vattr_t vattr;
			vattr.va_mask = AT_SIZE;
			if ((error = VOP_GETATTR(vp, &vattr, 0, cred, NULL)))
				goto out;

			if (vattr.va_size > (u_offset_t) MAXOFF32_T) {
				/*
				 * Large File API - regular open fails
				 * if FOFFMAX flag is set in file mode
				 */
				error = EOVERFLOW;
				goto out;
			}
		}

		/*
		 * Check permissions.
		 */
		if (error = VOP_ACCESS(vp, mode, 0, cred, NULL))
			goto out;
	}

	if ((flags & FNOFOLLOW) && vp->v_type == VLNK) {
		error = ELOOP;
		goto out;
	}

	vnode_t *old_vp = vp;

	error = VOP_OPEN(&vp, flags, cred, NULL);

	ASSERT(old_vp == vp);

	if(error)
		goto out;

	struct fuse_entry_param e = { 0 };

	if(flags & FCREAT) {
		error = internal_stat(vp, &e.attr);
		if(error)
			goto out;
	}

	if(flags & FCREAT) {
		e.attr_timeout = 0.0;
		e.entry_timeout = 0.0;
		e.ino = VTOZ(vp)->z_id;
		if(e.ino == 3)
			e.ino = 1;
		e.generation = VTOZ(vp)->z_phys->zp_gen;
	}

out:
	if(error) {
		ASSERT(vp->v_count > 0);
		VN_RELE(vp);
	}

	ZFS_EXIT(zfsvfs);

	return error;

}
static int zfs_unlink(struct LowLevelFilesystemPublicInterface* this_, 
		      ino_t parent_inode, const char* name){
	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;
	
	if(strlen(name) >= MAXNAMELEN)
		return ENAMETOOLONG;

	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *znode;

	int error = zfs_zget(zfsvfs, parent_inode, &znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	ASSERT(znode != NULL);
	vnode_t *dvp = ZTOV(znode);
	ASSERT(dvp != NULL);

	cred_t *cred = NULL;
	error = VOP_REMOVE(dvp, (char *) name, cred, NULL, 0);

	VN_RELE(dvp);
	ZFS_EXIT(zfsvfs);

	return error;
}
static int zfs_link(struct LowLevelFilesystemPublicInterface* this_, 
		    ino_t inode, ino_t new_parent, const char *newname){
	if(strlen(newname) >= MAXNAMELEN)
		return ENAMETOOLONG;

	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;
	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *td_znode, *s_znode;

	int error = zfs_zget(zfsvfs, inode, &s_znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	ASSERT(s_znode != NULL);

	error = zfs_zget(zfsvfs, new_parent, &td_znode, B_FALSE);
	if(error) {
		VN_RELE(ZTOV(s_znode));
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	vnode_t *svp = ZTOV(s_znode);
	vnode_t *tdvp = ZTOV(td_znode);
	ASSERT(svp != NULL);
	ASSERT(tdvp != NULL);

	cred_t *cred = NULL;
	error = VOP_LINK(tdvp, svp, (char *) newname, cred, NULL, 0);

	vnode_t *vp = NULL;

	if(error)
		goto out;

	error = VOP_LOOKUP(tdvp, (char *) newname, &vp, NULL, 0, NULL, cred, NULL, NULL, NULL);
	if(error)
		goto out;

	ASSERT(vp != NULL);

	struct fuse_entry_param e = { 0 };

	e.attr_timeout = 0.0;
	e.entry_timeout = 0.0;

	e.ino = VTOZ(vp)->z_id;
	if(e.ino == 3)
		e.ino = 1;

	e.generation = VTOZ(vp)->z_phys->zp_gen;

	error = internal_stat(vp, &e.attr);

out:
	if(vp != NULL)
		VN_RELE(vp);
	VN_RELE(tdvp);
	VN_RELE(svp);

	ZFS_EXIT(zfsvfs);

	return error;
}
static int zfs_rename(struct LowLevelFilesystemPublicInterface* this_, 
		      fuse_ino_t parent, const char *name,
		      ino_t new_parent, const char *newname){
	if(strlen(name) >= MAXNAMELEN)
		return ENAMETOOLONG;
	if(strlen(newname) >= MAXNAMELEN)
		return ENAMETOOLONG;

	struct ZfsFilesystem* zfs = (struct ZfsFilesystem*)this_;
	vfs_t *vfs = zfs->vfs;
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *p_znode, *np_znode;

	int error = zfs_zget(zfsvfs, parent, &p_znode, B_FALSE);
	if(error) {
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	ASSERT(p_znode != NULL);

	error = zfs_zget(zfsvfs, new_parent, &np_znode, B_FALSE);
	if(error) {
		VN_RELE(ZTOV(p_znode));
		ZFS_EXIT(zfsvfs);
		/* If the inode we are trying to get was recently deleted
		   dnode_hold_impl will return EEXIST instead of ENOENT */
		return error == EEXIST ? ENOENT : error;
	}

	ASSERT(np_znode != NULL);

	vnode_t *p_vp = ZTOV(p_znode);
	vnode_t *np_vp = ZTOV(np_znode);
	ASSERT(p_vp != NULL);
	ASSERT(np_vp != NULL);

	cred_t *cred = NULL;
	error = VOP_RENAME(p_vp, (char *) name, np_vp, (char *) newname, cred, NULL, 0);

	VN_RELE(p_vp);
	VN_RELE(np_vp);

	ZFS_EXIT(zfsvfs);

	return error;
}
static int zfs_ftruncate_size(struct LowLevelFilesystemPublicInterface* this_, 
			      ino_t inode, off_t length){
}


static struct LowLevelFilesystemPublicInterface s_zfs_filesystem_interface{
    NULL, //chown
	NULL, //chmod
	zfs_stat,
	zfs_mkdir,
	zfs_rmdir,
	zfs_pread,
	zfs_pwrite,
	zfs_getdents,
	NULL, //fsync
	zfs_close,
	zfs_lseek,
	zfs_open,
	zfs_remove,
	zfs_unlink,
	zfs_link,
	zfs_rename,
	zfs_ftruncate_size,
	NULL
};


struct LowLevelFilesystemPublicInterface* 
zfs_filesystem_construct (vfs_t *vfs, 
 struct TopLevelFilesystemObserverInterface* toplevelfs){
	struct ZfsFilesystem* this_ = malloc(sizeof(struct ZfsFilesystem));
	this_->public = s_zfs_filesystem_interface;
	this_->public.toplevelfs = toplevelfs;
	return &this_->public;
}

/*
 * Copyright (c) 2012 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

extern "C" {
#include "zrtlog.h"
}
#include "MemNode.h"

MemNode::MemNode() {
    data_ = NULL;
    len_ = 0;
    capacity_ = 0;
    use_count_ = 0;
}

MemNode::~MemNode() {
    children_.clear();
}

int MemNode::stat(struct stat *buf) {
    memset(buf, 0, sizeof(struct stat));
    buf->st_ino = (ino_t)slot_;
    if (is_dir()) {
        //buf->st_mode = S_IFDIR | 0777;
        /*YaroslavLitvinov added various modes support*/
        buf->st_mode = S_IFDIR | mode_;
    } else {
        buf->st_mode = S_IFREG | mode_;
        buf->st_size = len_;
    }
    buf->st_uid = 1001;
    buf->st_gid = 1002;
    buf->st_blksize = 1024;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* files are not allowed to have real date/time */
    /*currently as time used environment variable TimeStamp*/
    buf->st_atime = tv.tv_sec;      /* time of the last access */
    buf->st_mtime = tv.tv_sec;      /* time of the last modification */
    buf->st_ctime = tv.tv_sec;      /* time of the last status change */

    ZRT_LOG(L_EXTRA, "stat st_atime=%lld", buf->st_atime);
    ZRT_LOG(L_EXTRA, "stat ino=%d", slot_);
    return 0;
}

void MemNode::AddChild(int child) {
    if (!is_dir()) {
        return;
    }
    children_.push_back(child);
}

void MemNode::RemoveChild(int child) {
    if (!is_dir()) {
        return;
    }
    children_.remove(child);
}

void MemNode::ReallocData(int len) {
    assert(len > 0);
    // TODO(arbenson): Handle memory overflow more gracefully.
    data_ = reinterpret_cast<char *>(realloc(data_, len));
    assert(data_);
    set_capacity(len);
}

std::list<int> *MemNode::children() {
    if (is_dir()) {
        return &children_;
    } else {
        return NULL;
    }
}

void MemNode::set_flock(const struct flock* flock) { 
    memcpy( &flock_, flock, sizeof(struct flock) );
}

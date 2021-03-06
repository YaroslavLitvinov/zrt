/*
 *
 * Copyright (c) 2015, Rackspace, Inc.
 * Author: Yaroslav Litvinov, yaroslav.litvinov@rackspace.com
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

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "zrtlog.h"
#include "zcalls_zrt.h"
#include "user_space_fs.h"
#include "path_utils.h"
#include "mounts_manager.h"
#include "fuseglue.h"
#include "dyn_array.h"

#ifdef FUSEGLUE_EXT
#  undef FUSE_USE_VERSION
#  define FUSE_USE_VERSION 28
#  include <fuse.h>
#  include <fuse/archivemount.h>
#  include <fuse/unionfs.h>
#endif /*FUSEGLUE_EXT*/

enum { 
    EConditionWaitingInitialization=0,
    EConditionInitializationError=1,
    EConditionInitialized=2,
    EConditionWaitingFinalization=3 
};

struct fuse_conn_info;

struct mount_args{
    int    expect_absolute_path;
    char   proxy_mode;
    char   mountpoint[100];
    int    mount_argc;
    char **mount_argv;
    int  (*mount_main)(int, char**);
};

struct async_lock_data{
    pthread_mutex_t    mutex;
    pthread_cond_t     cond;
    int                cond_value;
    char               name[20];
    struct mount_args  args;
    struct fuse_conn_info *conn; /*for compatibility only*/
    int index; /*index in storage array*/
};

/*Array for saving multiple fuse file systems, it's needed for
  asynch engine*/
struct DynArray *s_fuse_mount_data=NULL;

/*Specify here all file systems supported by zrt core*/
fs_main
fusefs_entrypoint_get_args_by_fsname(const char *fsname, int write_mode, 
                                     const char *mntfile, const char *mntpoint,
                                     int *expect_absolute_path, char *proxy_mode,
				     int *argc, char ***argv)
{
#ifdef FUSE_ARCHIVEMOUNT_NAME
    if ( !strcasecmp(fsname, FUSE_ARCHIVEMOUNT_NAME) ){
        /*handle args and wrap it into argc, argv. Note: mountpoint -d
          param is not needed and will be ignored*/
        if (write_mode){
            FUSE_ARCHIVEMOUNT_ARGS_FILL_WRITE(argc, argv, mntfile, mntpoint);
        }
        else{
            FUSE_ARCHIVEMOUNT_ARGS_FILL_RONLY(argc, argv, mntfile, mntpoint);
        }
        *expect_absolute_path = EAbsolutePathNotExpected;
        *proxy_mode = EFuseProxyModeDisabled;
	return archivemount_main;
    }
#endif
#ifdef FUSE_UNIONFS_NAME
    if ( !strcasecmp(fsname, FUSE_UNIONFS_NAME) ){
        /*handle args and wrap it into argc, argv. Note: mountpoint -d
          param is not needed and will be ignored*/
        if (write_mode){
            FUSE_UNIONFS_ARGS_FILL_WRITE(argc, argv, mntfile, mntpoint);
        }
        else{
            FUSE_UNIONFS_ARGS_FILL_RONLY(argc, argv, mntfile, mntpoint);
        }
        *expect_absolute_path = EAbsolutePathNotExpected;
        *proxy_mode = EFuseProxyModeEnabled;
	return unionfs_main;
    }
#endif
    return NULL;
}


/********************************************/
/*functions provides waiting for condition*/

inline int match_cond_value(struct async_lock_data * lock_data, int expected_cond_value)
{
    if (lock_data->cond_value == expected_cond_value)
        return 1;
    else
        return 0;
}

/*wait while cond value is LESS THAN expected*/
void wait_cond_value_less_than_expected(struct async_lock_data * lock_data, int expected_cond_value)
{
    pthread_mutex_lock( &lock_data->mutex );
    ZRT_LOG(L_INFO, "condition='%s' current value=%d, wait value=%d", 
	    lock_data->name, lock_data->cond_value, expected_cond_value ); 
    while( lock_data->cond_value < expected_cond_value )
	pthread_cond_wait( &lock_data->cond, &lock_data->mutex );
    pthread_cond_signal( &lock_data->cond );      
    ZRT_LOG(L_INFO, "condition='%s' new value=%d", 
	    lock_data->name, lock_data->cond_value ); 
    pthread_mutex_unlock( &lock_data->mutex );
}

void update_cond_value(struct async_lock_data * lock_data, int new_cond_value)
{
    pthread_mutex_lock( &lock_data->mutex );
    ZRT_LOG(L_INFO, "update condition='%s' current value=%d, new value=%d", 
	    lock_data->name, lock_data->cond_value, new_cond_value ); 
    lock_data->cond_value = new_cond_value;
    pthread_cond_signal( &lock_data->cond );      
    ZRT_LOG(L_INFO, "update condition='%s' current value=%d, new value=%d", 
	    lock_data->name, lock_data->cond_value, new_cond_value ); 
    pthread_mutex_unlock( &lock_data->mutex );
}

struct async_lock_data *
match_mount_data_by_mountpoint(const char *mountpoint)
{
    struct async_lock_data *lock_data = NULL;
    int i;
    if (s_fuse_mount_data == NULL) return NULL;
    /*look lock_data matching it by mountpoint*/
    for (i=0; i < s_fuse_mount_data->num_entries; i++){
        lock_data = DynArrayGet( s_fuse_mount_data, i);
        if (lock_data!=NULL && !strcmp(lock_data->args.mountpoint, mountpoint)){
            break;
        }
        else
            lock_data=NULL;
    }
    return lock_data;
}

struct async_lock_data *
match_mount_data_by_condition_value(int condition_value)
{
    struct async_lock_data *lock_data = NULL;
    int i;
    if (s_fuse_mount_data == NULL) return NULL;
    /*look lock_data matching it by mountpoint*/
    for (i=0; i < s_fuse_mount_data->num_entries; i++){
        lock_data = DynArrayGet( s_fuse_mount_data, i);
        if (lock_data!=NULL && lock_data->cond_value == condition_value){
            break;
        }
        else
            lock_data=NULL;
    }
    return lock_data;
}


/********************************************/

/*fuse must use this function in own implementation, it's should
  always exist at least as stub, to get fuse library compiled*/
int fuse_main_common_implem(struct fuse_operations *op, const char *mountpoint, void *user_data){
    #ifdef FUSEGLUE_EXT
    ZRT_LOG(L_SHORT, "Perform fuse mount and wait in thread, path=%s", mountpoint );
    assert(s_fuse_mount_data!=NULL);
    struct async_lock_data *lock_data = match_mount_data_by_mountpoint(mountpoint);
    if (lock_data==NULL){
        ZRT_LOG(L_ERROR, "Can't find fuse fs lock_data for mountpoint=%s", mountpoint );
        assert(0);
    }

    /*perform filesystem mount here*/
    int res = mount_fuse_fs(op, mountpoint, lock_data->args.expect_absolute_path,
                            lock_data->args.proxy_mode);
    if (res != 0){
        ZRT_LOG(L_ERROR, "Fuse fs mount error %d", res );
        update_cond_value(lock_data, EConditionInitializationError);
    }
    else{
        /*fuse init function*/
        if ( op->init ){
            lock_data->conn = calloc(1, sizeof(struct fuse_conn_info));
            op->init(lock_data->conn);
        }
        /*when it finishes, update cond to continue zrt running*/
        update_cond_value(lock_data, EConditionInitialized);

        /*wait here until exit*/
        wait_cond_value_less_than_expected(lock_data, EConditionWaitingFinalization);
    }
    ZRT_LOG(L_SHORT, "Finalize fuse mount, err=%d, path=%s", res, mountpoint );
    return res;
#else
    return -1;
#endif /*FUSEGLUE_EXT*/
}

/********/


#ifdef FUSEGLUE_EXT

/*Support for calling fuse file system entrypoints in separate threads
  and for these threads cleanup*/
static void *exec_fuse_main_async(void *arg)
{
    struct async_lock_data *lock_data = (struct async_lock_data *)arg;

    /*fuse waiting an existing dir, create it if not yet exist*/
    mkpath_recursively(lock_data->args.mountpoint, 0666);

    /*run fusefs's entry point, while main() is running then
      filesystem interface is alive, wait here until main do exit with
      or without error. */
    int ret = lock_data->args.mount_main(lock_data->args.mount_argc, 
                                         lock_data->args.mount_argv);
    if (ret){
	ZRT_LOG(L_ERROR, "fuse mount error=%d", ret ); 
        update_cond_value(lock_data, EConditionInitializationError);
    }
    else{
        /*1.update condition just to be sure that lock is released, but
          2.in case of normal flow if ret==0 it is expected that lock is
          already released at fuse_mounts termination*/
        update_cond_value(lock_data, EConditionWaitingFinalization);
    }
    return NULL;
}

int exec_fuse_main(const char *mountpoint, int expect_absolute_path, char proxy_mode,
                   int (*fs_main)(int, char**), int fs_argc, char **fs_argv){
    int res=0;
    /*create mounts data array*/
    if ( s_fuse_mount_data == NULL ){
        s_fuse_mount_data = calloc(1, sizeof(struct DynArray));
        DynArrayCtor( s_fuse_mount_data, 2 /*granularity*/ );
    }

    /*prepare parameters for thread function*/
    struct async_lock_data *lock_data = calloc(1, sizeof(struct async_lock_data));
    strncpy(lock_data->args.mountpoint, mountpoint, sizeof(lock_data->args.mountpoint) );
    strncpy(lock_data->name, mountpoint, sizeof(lock_data->name) );
    lock_data->args.expect_absolute_path = expect_absolute_path;
    lock_data->args.proxy_mode = proxy_mode;
    lock_data->args.mount_argc = fs_argc;
    lock_data->args.mount_argv = fs_argv;
    lock_data->args.mount_main = fs_main;
    lock_data->mutex = PTHREAD_MUTEX_INITIALIZER;
    lock_data->cond = PTHREAD_COND_INITIALIZER;
    lock_data->cond_value = EConditionWaitingInitialization;

    /*add to array*/
    if (! DynArraySet( s_fuse_mount_data, s_fuse_mount_data->num_entries, lock_data )){
        /*error adding to array*/
        ZRT_LOG(L_ERROR, "Error adding fuse mount data with mountpoint %s", 
                mountpoint ); 
        free(lock_data);
        return -1;
    }
    /*save array index of just added lock_data*/
    lock_data->index = s_fuse_mount_data->num_entries-1;

    pthread_t thread_create_fuse;
    if (pthread_create(&thread_create_fuse, NULL, exec_fuse_main_async, lock_data) != 0){
        return -1;
    }
 
    /*Wait here for condition while fuse fs will not be created
      successfully or will raise an error*/

    wait_cond_value_less_than_expected( lock_data, EConditionInitializationError);
    if ( match_cond_value(lock_data, EConditionInitializationError) ){
        /*File system init error*/
        res=-1;
    }
    else{
        /*Wait while filesystem initialization is done, it is signalled by
          lock_data.cond. It is expected fuse_main to be called
          inside mount_main, and then it releases condition below*/
        wait_cond_value_less_than_expected( lock_data, EConditionInitialized);
        ZRT_LOG(L_INFO, "%s", "fuse fs initializer finished" ); 
        res=0;
    }

    /*keep all data allocated until EConditionWaitingFinalization*/
    return res;
}

static void free_lock_data_remove_from_array(struct async_lock_data *lock_data){
    if ( lock_data != NULL ){
        /*erase data*/
        free(lock_data->conn);
        free(lock_data);
        DynArraySet( s_fuse_mount_data, lock_data->index, NULL );
    }
 }

/**/
void exit_fuse_main(int retcode){
    /*As fuse filesystems initializes continously one by one, it is
      exist in one moment of time only one file system instance in
      initialization state*/
    struct async_lock_data *lock_data =
        match_mount_data_by_condition_value(EConditionWaitingInitialization);
    if (lock_data!=NULL){
        /*by updating cond value we releasing main thread, and in next
          step exiting current*/
        if (retcode==0)
            update_cond_value(lock_data, EConditionWaitingFinalization);
        else
            update_cond_value(lock_data, EConditionInitializationError);

        free_lock_data_remove_from_array(lock_data);
        /*terminate thread*/
        int retval;
        pthread_exit(&retval);
    }
    /*if lock_data is NULL then it seems fs already awaits for
      termination and probably fuse application called exit() from
      main(), so silently ignore this situation */

}

void terminate_fuse_mounts(){
    int i;
    struct async_lock_data *lock_data;
    if (s_fuse_mount_data == NULL) return;
    for(i=0; i < s_fuse_mount_data->num_entries; i++){
        lock_data = DynArrayGet( s_fuse_mount_data, i);
        /*if exist initialized fs that need to be stopped*/
        if (lock_data != NULL){
            if (lock_data->cond_value >= EConditionInitialized){
                /*release waiting fuse_main, continue exiting*/
                update_cond_value(lock_data, EConditionWaitingFinalization);
            }
            /*remove mount from mounts list, and free all memories*/
            get_system_mounts_manager()->mount_remove( get_system_mounts_manager(),
                                                       lock_data->args.mountpoint );
            /*remove asynch data of fuse mount*/
            free_lock_data_remove_from_array(lock_data);
        }
    }
    DynArrayDtor(s_fuse_mount_data);
}

#endif /*FUSEGLUE_EXT*/

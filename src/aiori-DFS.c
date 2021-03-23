/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
/*
 * Copyright (C) 2018-2020 Intel Corporation
 * See the file COPYRIGHT for a complete copyright notice and license.
 */

/*
 * This file implements the abstract I/O interface for DAOS FS API.
 */

#define _BSD_SOURCE

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>

#include <gurt/common.h>
#include <gurt/hash.h>
#include <daos.h>
#include <daos_fs.h>

#include "ior.h"
#include "iordef.h"
#include "aiori.h"
#include "utilities.h"

dfs_t *dfs;
static daos_handle_t poh, coh;
static daos_oclass_id_t objectClass;
static daos_oclass_id_t dir_oclass;
static struct d_hash_table *dir_hash;
static bool dfs_init;

struct aiori_dir_hdl {
        d_list_t	entry;
        dfs_obj_t	*oh;
        char		name[PATH_MAX];
};

enum handleType {
        POOL_HANDLE,
        CONT_HANDLE,
	DFS_HANDLE
};

/************************** O P T I O N S *****************************/
struct dfs_options{
        char	*pool;
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
        char	*svcl;
#endif
        char	*group;
        char	*cont;
	int	chunk_size;
	char	*oclass;
	char	*dir_oclass;
	char	*prefix;
        int	destroy;
};

static struct dfs_options o = {
        .pool		= NULL,
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
        .svcl		= NULL,
#endif
        .group		= NULL,
        .cont		= NULL,
	.chunk_size	= 1048576,
	.oclass		= NULL,
	.dir_oclass	= NULL,
        .prefix		= NULL,
        .destroy        = 0,
};

static option_help options [] = {
      {0, "dfs.pool", "pool uuid", OPTION_OPTIONAL_ARGUMENT, 's', & o.pool},
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
      {0, "dfs.svcl", "pool SVCL", OPTION_OPTIONAL_ARGUMENT, 's', & o.svcl},
#endif
      {0, "dfs.group", "server group", OPTION_OPTIONAL_ARGUMENT, 's', & o.group},
      {0, "dfs.cont", "DFS container uuid", OPTION_OPTIONAL_ARGUMENT, 's', & o.cont},
      {0, "dfs.chunk_size", "chunk size", OPTION_OPTIONAL_ARGUMENT, 'd', &o.chunk_size},
      {0, "dfs.oclass", "object class", OPTION_OPTIONAL_ARGUMENT, 's', &o.oclass},
      {0, "dfs.dir_oclass", "directory object class", OPTION_OPTIONAL_ARGUMENT, 's', &o.dir_oclass},
      {0, "dfs.prefix", "mount prefix", OPTION_OPTIONAL_ARGUMENT, 's', & o.prefix},
      {0, "dfs.destroy", "Destroy DFS Container", OPTION_FLAG, 'd', &o.destroy},
      LAST_OPTION
};

/**************************** P R O T O T Y P E S *****************************/
static void *DFS_Create(char *, IOR_param_t *);
static void *DFS_Open(char *, IOR_param_t *);
static IOR_offset_t DFS_Xfer(int, void *, IOR_size_t *,
                             IOR_offset_t, IOR_param_t *);
static void DFS_Close(void *, IOR_param_t *);
static void DFS_Delete(char *, IOR_param_t *);
static char* DFS_GetVersion();
static void DFS_Fsync(void *, IOR_param_t *);
static void DFS_Sync(IOR_param_t *);
static IOR_offset_t DFS_GetFileSize(IOR_param_t *, MPI_Comm, char *);
static int DFS_Statfs (const char *, ior_aiori_statfs_t *, IOR_param_t *);
static int DFS_Stat (const char *, struct stat *, IOR_param_t *);
static int DFS_Mkdir (const char *, mode_t, IOR_param_t *);
static int DFS_Rmdir (const char *, IOR_param_t *);
static int DFS_Access (const char *, int, IOR_param_t *);
static void DFS_Init();
static void DFS_Finalize();
static option_help * DFS_options();

/************************** D E C L A R A T I O N S ***************************/

ior_aiori_t dfs_aiori = {
        .name		= "DFS",
        .create		= DFS_Create,
        .open		= DFS_Open,
        .xfer		= DFS_Xfer,
        .close		= DFS_Close,
        .delete		= DFS_Delete,
        .get_version	= DFS_GetVersion,
        .fsync		= DFS_Fsync,
        .sync		= DFS_Sync,
        .get_file_size	= DFS_GetFileSize,
        .statfs		= DFS_Statfs,
        .mkdir		= DFS_Mkdir,
        .rmdir		= DFS_Rmdir,
        .access		= DFS_Access,
        .stat		= DFS_Stat,
        .initialize	= DFS_Init,
        .finalize	= DFS_Finalize,
        .get_options	= DFS_options,
        .enable_mdtest	= true,
};

/***************************** F U N C T I O N S ******************************/

/* For DAOS methods. */
#define DCHECK(rc, format, ...)                                         \
do {                                                                    \
        int _rc = (rc);                                                 \
                                                                        \
        if (_rc != 0) {                                                  \
                fprintf(stderr, "ERROR (%s:%d): %d: %d: "               \
                        format"\n", __FILE__, __LINE__, rank, _rc,      \
                        ##__VA_ARGS__);                                 \
                fflush(stderr);                                         \
                exit(-1);                                       	\
        }                                                               \
} while (0)

#define INFO(level, format, ...)					\
do {                                                                    \
        if (verbose >= level)						\
                printf("[%d] "format"\n", rank, ##__VA_ARGS__);         \
} while (0)

#define GERR(format, ...)                                               \
do {                                                                    \
        fprintf(stderr, format"\n", ##__VA_ARGS__);                     \
        MPI_CHECK(MPI_Abort(MPI_COMM_WORLD, -1), "MPI_Abort() error");  \
} while (0)

static inline struct aiori_dir_hdl *
hdl_obj(d_list_t *rlink)
{
        return container_of(rlink, struct aiori_dir_hdl, entry);
}

static bool
key_cmp(struct d_hash_table *htable, d_list_t *rlink,
	const void *key, unsigned int ksize)
{
        struct aiori_dir_hdl *hdl = hdl_obj(rlink);

        return (strcmp(hdl->name, (const char *)key) == 0);
}

static void
rec_free(struct d_hash_table *htable, d_list_t *rlink)
{
        struct aiori_dir_hdl *hdl = hdl_obj(rlink);

        assert(d_hash_rec_unlinked(&hdl->entry));
        dfs_release(hdl->oh);
        free(hdl);
}

static d_hash_table_ops_t hdl_hash_ops = {
        .hop_key_cmp	= key_cmp,
        .hop_rec_free	= rec_free
};

/* Distribute process 0's pool or container handle to others. */
static void
HandleDistribute(enum handleType type)
{
        d_iov_t global;
        int        rc;

        global.iov_buf = NULL;
        global.iov_buf_len = 0;
        global.iov_len = 0;

        assert(type == POOL_HANDLE || type == CONT_HANDLE || type == DFS_HANDLE);
        if (rank == 0) {
                /* Get the global handle size. */
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(poh, &global);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_local2global(coh, &global);
                else
                        rc = dfs_local2global(dfs, &global);
                DCHECK(rc, "Failed to get global handle size");
        }

        MPI_CHECK(MPI_Bcast(&global.iov_buf_len, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD),
                  "Failed to bcast global handle buffer size");

	global.iov_len = global.iov_buf_len;
        global.iov_buf = malloc(global.iov_buf_len);
        if (global.iov_buf == NULL)
                ERR("Failed to allocate global handle buffer");

        if (rank == 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(poh, &global);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_local2global(coh, &global);
                else
                        rc = dfs_local2global(dfs, &global);
                DCHECK(rc, "Failed to create global handle");
        }

        MPI_CHECK(MPI_Bcast(global.iov_buf, global.iov_buf_len, MPI_BYTE, 0, MPI_COMM_WORLD),
                  "Failed to bcast global pool handle");

        if (rank != 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_global2local(global, &poh);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_global2local(poh, global, &coh);
                else
                        rc = dfs_global2local(poh, coh, 0, global, &dfs);
                DCHECK(rc, "Failed to get local handle");
        }

        free(global.iov_buf);
}

static int
parse_filename(const char *path, char **_obj_name, char **_cont_name)
{
	char *f1 = NULL;
	char *f2 = NULL;
	char *fname = NULL;
	char *cont_name = NULL;
	int rc = 0;

	if (path == NULL || _obj_name == NULL || _cont_name == NULL)
		return -EINVAL;

	if (strcmp(path, "/") == 0) {
		*_cont_name = strdup("/");
		if (*_cont_name == NULL)
			return -ENOMEM;
		*_obj_name = NULL;
		return 0;
	}

	f1 = strdup(path);
	if (f1 == NULL) {
                rc = -ENOMEM;
                goto out;
        }

	f2 = strdup(path);
	if (f2 == NULL) {
                rc = -ENOMEM;
                goto out;
        }

	fname = basename(f1);
	cont_name = dirname(f2);

	if (cont_name[0] == '.' || cont_name[0] != '/') {
		char cwd[1024];

		if (getcwd(cwd, 1024) == NULL) {
                        rc = -ENOMEM;
                        goto out;
                }

		if (strcmp(cont_name, ".") == 0) {
			cont_name = strdup(cwd);
			if (cont_name == NULL) {
                                rc = -ENOMEM;
                                goto out;
                        }
		} else {
			char *new_dir = calloc(strlen(cwd) + strlen(cont_name)
					       + 1, sizeof(char));
			if (new_dir == NULL) {
                                rc = -ENOMEM;
                                goto out;
                        }

			strcpy(new_dir, cwd);
			if (cont_name[0] == '.') {
				strcat(new_dir, &cont_name[1]);
			} else {
				strcat(new_dir, "/");
				strcat(new_dir, cont_name);
			}
			cont_name = new_dir;
		}
		*_cont_name = cont_name;
	} else {
		*_cont_name = strdup(cont_name);
		if (*_cont_name == NULL) {
                        rc = -ENOMEM;
                        goto out;
                }
	}

	*_obj_name = strdup(fname);
	if (*_obj_name == NULL) {
		free(*_cont_name);
		*_cont_name = NULL;
                rc = -ENOMEM;
                goto out;
	}

out:
	if (f1)
		free(f1);
	if (f2)
		free(f2);
	return rc;
}

static void
share_file_handle(dfs_obj_t **file, MPI_Comm comm)
{
        d_iov_t global;
        int        rc;

        global.iov_buf = NULL;
        global.iov_buf_len = 0;
        global.iov_len = 0;

        if (rank == 0) {
                rc = dfs_obj_local2global(dfs, *file, &global);
                DCHECK(rc, "Failed to get global handle size");
        }

        MPI_CHECK(MPI_Bcast(&global.iov_buf_len, 1, MPI_UINT64_T, 0, testComm),
                  "Failed to bcast global handle buffer size");

	global.iov_len = global.iov_buf_len;
        global.iov_buf = malloc(global.iov_buf_len);
        if (global.iov_buf == NULL)
                ERR("Failed to allocate global handle buffer");

        if (rank == 0) {
                rc = dfs_obj_local2global(dfs, *file, &global);
                DCHECK(rc, "Failed to create global handle");
        }

        MPI_CHECK(MPI_Bcast(global.iov_buf, global.iov_buf_len, MPI_BYTE, 0, testComm),
                  "Failed to bcast global pool handle");

        if (rank != 0) {
                rc = dfs_obj_global2local(dfs, 0, global, file);
                DCHECK(rc, "Failed to get local handle");
        }

        free(global.iov_buf);
}

static dfs_obj_t *
lookup_insert_dir(const char *name, mode_t *mode)
{
        struct aiori_dir_hdl *hdl;
        d_list_t *rlink;
        int rc;

        rlink = d_hash_rec_find(dir_hash, name, strlen(name));
        if (rlink != NULL) {
                hdl = hdl_obj(rlink);
                return hdl->oh;
        }

        hdl = calloc(1, sizeof(struct aiori_dir_hdl));
        if (hdl == NULL)
                GERR("failed to alloc dir handle");

        strncpy(hdl->name, name, PATH_MAX-1);
        hdl->name[PATH_MAX-1] = '\0';

        rc = dfs_lookup(dfs, name, O_RDWR, &hdl->oh, mode, NULL);
        if (rc)
                return NULL;
        if (mode && S_ISREG(*mode))
                return hdl->oh;

        rc = d_hash_rec_insert(dir_hash, hdl->name, strlen(hdl->name),
                               &hdl->entry, true);
        DCHECK(rc, "Failed to insert dir handle in hashtable");

        return hdl->oh;
}

static option_help * DFS_options(){
        return options;
}

static void
DFS_Init() {
	int rc;

        /** in case we are already initialized, return */
        if (dfs_init)
                return;

        /** shouldn't be fatal since it can be called with POSIX backend selection */
        if (o.pool == NULL || o.cont == NULL)
                return;

#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
        if (o.svcl == NULL)
                return;
#endif

	rc = daos_init();
        DCHECK(rc, "Failed to initialize daos");

        if (o.oclass) {
                objectClass = daos_oclass_name2id(o.oclass);
		if (objectClass == OC_UNKNOWN)
			GERR("Invalid DAOS object class %s\n", o.oclass);
	}

        if (o.dir_oclass) {
                dir_oclass = daos_oclass_name2id(o.dir_oclass);
		if (dir_oclass == OC_UNKNOWN)
			GERR("Invalid DAOS directory object class %s\n", o.dir_oclass);
	}

        rc = d_hash_table_create(0, 16, NULL, &hdl_hash_ops, &dir_hash);
        DCHECK(rc, "Failed to initialize dir hashtable");

        if (rank == 0) {
                uuid_t pool_uuid, co_uuid;
                daos_pool_info_t pool_info;
                daos_cont_info_t co_info;

                rc = uuid_parse(o.pool, pool_uuid);
                DCHECK(rc, "Failed to parse 'Pool uuid': %s", o.pool);

                rc = uuid_parse(o.cont, co_uuid);
                DCHECK(rc, "Failed to parse 'Cont uuid': %s", o.cont);

                INFO(VERBOSE_1, "Pool uuid = %s", o.pool);
                INFO(VERBOSE_1, "DFS Container namespace uuid = %s", o.cont);

#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
                d_rank_list_t *svcl = NULL;

                svcl = daos_rank_list_parse(o.svcl, ":");
                if (svcl == NULL)
                        ERR("Failed to allocate svcl");
                INFO(VERBOSE_1, "Pool svcl = %s", o.svcl);

                /** Connect to DAOS pool */
                rc = daos_pool_connect(pool_uuid, o.group, svcl, DAOS_PC_RW,
                                       &poh, &pool_info, NULL);
                d_rank_list_free(svcl);
#else
                rc = daos_pool_connect(pool_uuid, o.group, DAOS_PC_RW,
                                       &poh, &pool_info, NULL);
#endif
                DCHECK(rc, "Failed to connect to pool");

                rc = daos_cont_open(poh, co_uuid, DAOS_COO_RW, &coh, &co_info,
                                    NULL);
                /* If NOEXIST we create it */
                if (rc == -DER_NONEXIST) {
                        INFO(VERBOSE_1, "Creating DFS Container ...\n");

                        rc = dfs_cont_create(poh, co_uuid, NULL, &coh, NULL);
                        if (rc)
                                DCHECK(rc, "Failed to create container");
                } else if (rc) {
                        DCHECK(rc, "Failed to create container");
                }

                rc = dfs_mount(poh, coh, O_RDWR, &dfs);
                DCHECK(rc, "Failed to mount DFS namespace");
        }

        HandleDistribute(POOL_HANDLE);
        HandleDistribute(CONT_HANDLE);
        HandleDistribute(DFS_HANDLE);

        if (o.prefix) {
                rc = dfs_set_prefix(dfs, o.prefix);
                DCHECK(rc, "Failed to set DFS Prefix");
        }
        dfs_init = true;
}

static void
DFS_Finalize()
{
        int rc;

	MPI_Barrier(MPI_COMM_WORLD);
        d_hash_table_destroy(dir_hash, true /* force */);

	rc = dfs_umount(dfs);
        DCHECK(rc, "Failed to umount DFS namespace");
	MPI_Barrier(MPI_COMM_WORLD);

	rc = daos_cont_close(coh, NULL);
        DCHECK(rc, "Failed to close container %s (%d)", o.cont, rc);
	MPI_Barrier(MPI_COMM_WORLD);

	if (o.destroy) {
                if (rank == 0) {
                        uuid_t uuid;
                        double t1, t2;

                        INFO(VERBOSE_1, "Destorying DFS Container: %s\n", o.cont);
                        uuid_parse(o.cont, uuid);
                        t1 = MPI_Wtime();
                        rc = daos_cont_destroy(poh, uuid, 1, NULL);
                        t2 = MPI_Wtime();
                        if (rc == 0)
                                INFO(VERBOSE_1, "Container Destroy time = %f secs", t2-t1);
                }

                MPI_Bcast(&rc, 1, MPI_INT, 0, MPI_COMM_WORLD);
                if (rc) {
			if (rank == 0)
				DCHECK(rc, "Failed to destroy container %s (%d)", o.cont, rc);
			MPI_Abort(MPI_COMM_WORLD, -1);
                }
        }

        if (rank == 0)
                INFO(VERBOSE_1, "Disconnecting from DAOS POOL\n");

        rc = daos_pool_disconnect(poh, NULL);
        DCHECK(rc, "Failed to disconnect from pool");

	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD), "barrier error");

        if (rank == 0)
                INFO(VERBOSE_1, "Finalizing DAOS..\n");

	rc = daos_fini();
        DCHECK(rc, "Failed to finalize DAOS");

        /** reset tunables */
	o.pool		= NULL;
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
	o.svcl		= NULL;
#endif
	o.group		= NULL;
	o.cont		= NULL;
	o.chunk_size	= 1048576;
	o.oclass	= NULL;
	o.dir_oclass	= NULL;
	o.prefix	= NULL;
	o.destroy	= 0;
	objectClass	= 0;
	dir_oclass	= 0;
	dfs_init	= false;
}

/*
 * Creat and open a file through the DFS interface.
 */
static void *
DFS_Create(char *testFileName, IOR_param_t *param)
{
	char *name = NULL, *dir_name = NULL;
	dfs_obj_t *obj = NULL, *parent = NULL;
	mode_t mode = 0664;
        int fd_oflag = 0;
	int rc;

        assert(param);

	rc = parse_filename(testFileName, &name, &dir_name);
        DCHECK(rc, "Failed to parse path %s", testFileName);
	assert(dir_name);
	assert(name);

        mode = S_IFREG | mode;
	if (param->filePerProc || rank == 0) {
                fd_oflag |= O_CREAT | O_RDWR | O_EXCL;

                parent = lookup_insert_dir(dir_name, NULL);
                if (parent == NULL)
                        GERR("Failed to lookup parent dir");

                rc = dfs_open(dfs, parent, name, mode, fd_oflag,
                              objectClass, o.chunk_size, NULL, &obj);
                DCHECK(rc, "dfs_open() of %s Failed", name);
        }
        if (!param->filePerProc) {
                share_file_handle(&obj, testComm);
        }

	if (name)
		free(name);
	if (dir_name)
		free(dir_name);

        return ((void *)obj);
}

/*
 * Open a file through the DFS interface.
 */
static void *
DFS_Open(char *testFileName, IOR_param_t *param)
{
	char *name = NULL, *dir_name = NULL;
	dfs_obj_t *obj = NULL, *parent = NULL;
	mode_t mode;
	int rc;
        int fd_oflag = 0;

        fd_oflag |= O_RDWR;
	mode = S_IFREG | param->mode;

	rc = parse_filename(testFileName, &name, &dir_name);
        DCHECK(rc, "Failed to parse path %s", testFileName);

	assert(dir_name);
	assert(name);

	if (param->filePerProc || rank == 0) {
                parent = lookup_insert_dir(dir_name, NULL);
                if (parent == NULL)
                        GERR("Failed to lookup parent dir");

                rc = dfs_open(dfs, parent, name, mode, fd_oflag, objectClass,
                              o.chunk_size, NULL, &obj);
                DCHECK(rc, "dfs_open() of %s Failed", name);
        }

        if (!param->filePerProc) {
                share_file_handle(&obj, testComm);
        }

	if (name)
		free(name);
	if (dir_name)
		free(dir_name);

        return ((void *)obj);
}

/*
 * Write or read access to file using the DFS interface.
 */
static IOR_offset_t
DFS_Xfer(int access, void *file, IOR_size_t *buffer, IOR_offset_t length,
         IOR_param_t *param)
{
        int xferRetries = 0;
        long long remaining = (long long)length;
        char *ptr = (char *)buffer;
        daos_size_t ret;
        int rc;
	dfs_obj_t *obj;

        obj = (dfs_obj_t *)file;

        while (remaining > 0) {
                d_iov_t iov;
                d_sg_list_t sgl;

                /** set memory location */
                sgl.sg_nr = 1;
                sgl.sg_nr_out = 0;
                d_iov_set(&iov, (void *)ptr, remaining);
                sgl.sg_iovs = &iov;

                /* write/read file */
                if (access == WRITE) {
                        rc = dfs_write(dfs, obj, &sgl, param->offset, NULL);
                        if (rc) {
                                fprintf(stderr, "dfs_write() failed (%d)\n", rc);
                                return -1;
                        }
                        ret = remaining;
                } else {
                        rc = dfs_read(dfs, obj, &sgl, param->offset, &ret, NULL);
                        if (rc || ret == 0)
                                fprintf(stderr, "dfs_read() failed(%d)\n", rc);
                }

                if (ret < remaining) {
                        if (param->singleXferAttempt == TRUE)
                                exit(-1);
                        if (xferRetries > MAX_RETRY)
                                ERR("too many retries -- aborting");
                }

                assert(ret >= 0);
                assert(ret <= remaining);
                remaining -= ret;
                ptr += ret;
                xferRetries++;
        }

        return (length);
}

/*
 * Perform fsync().
 */
static void
DFS_Fsync(void *fd, IOR_param_t * param)
{
        /* no cache in DFS, so this is a no-op currently */
	dfs_sync(dfs);
        return;
}

/*
 * Perform sync() on the dfs mount.
 */
static void
DFS_Sync(IOR_param_t * param)
{
        /* no cache in DFS, so this is a no-op currently */
	dfs_sync(dfs);
        return;
}

/*
 * Close a file through the DFS interface.
 */
static void
DFS_Close(void *fd, IOR_param_t * param)
{
        dfs_release((dfs_obj_t *)fd);
}

/*
 * Delete a file through the DFS interface.
 */
static void
DFS_Delete(char *testFileName, IOR_param_t * param)
{
	char *name = NULL, *dir_name = NULL;
	dfs_obj_t *parent = NULL;
	int rc;

	rc = parse_filename(testFileName, &name, &dir_name);
        DCHECK(rc, "Failed to parse path %s", testFileName);

	assert(dir_name);
	assert(name);

        parent = lookup_insert_dir(dir_name, NULL);
        if (parent == NULL)
                GERR("Failed to lookup parent dir");

	rc = dfs_remove(dfs, parent, name, false, NULL);
        DCHECK(rc, "dfs_remove() of %s Failed", name);

	if (name)
		free(name);
	if (dir_name)
		free(dir_name);
}

static char* DFS_GetVersion()
{
	static char ver[1024] = {};

	sprintf(ver, "%s", "DAOS");
	return ver;
}

/*
 * Use DFS stat() to return aggregate file size.
 */
static IOR_offset_t
DFS_GetFileSize(IOR_param_t * test, MPI_Comm comm, char *testFileName)
{
        dfs_obj_t *obj;
        daos_size_t fsize, tmpMin, tmpMax, tmpSum;
        int rc;

	if (test->filePerProc || rank == 0) {
                rc = dfs_lookup(dfs, testFileName, O_RDONLY, &obj, NULL, NULL);
                if (rc) {
                        fprintf(stderr, "dfs_lookup() of %s Failed (%d)", testFileName, rc);
                        return -1;
                }

                rc = dfs_get_size(dfs, obj, &fsize);
                dfs_release(obj);
                if (rc)
                        return -1;
        }

        if (test->filePerProc == TRUE) {
                MPI_CHECK(MPI_Allreduce(&fsize, &tmpSum, 1,
                                        MPI_LONG_LONG_INT, MPI_SUM, comm),
                          "cannot total data moved");
                fsize = tmpSum;
        } else {
                MPI_CHECK(MPI_Allreduce(&fsize, &tmpMin, 1,
                                        MPI_LONG_LONG_INT, MPI_MIN, comm),
                          "cannot total data moved");
                MPI_CHECK(MPI_Allreduce(&fsize, &tmpMax, 1,
                                        MPI_LONG_LONG_INT, MPI_MAX, comm),
                          "cannot total data moved");
                if (tmpMin != tmpMax) {
                        if (rank == 0) {
                                WARN("inconsistent file size by different tasks");
                        }
                        /* incorrect, but now consistent across tasks */
                        fsize = tmpMin;
                }
        }

        return (fsize);
}

static int
DFS_Statfs(const char *path, ior_aiori_statfs_t *sfs, IOR_param_t * param)
{
        return 0;
}

static int
DFS_Mkdir(const char *path, mode_t mode, IOR_param_t * param)
{
        dfs_obj_t *parent = NULL;
	char *name = NULL, *dir_name = NULL;
	int rc;

	rc = parse_filename(path, &name, &dir_name);
        DCHECK(rc, "Failed to parse path %s", path);

	assert(dir_name);
        if (!name)
                return 0;

        parent = lookup_insert_dir(dir_name, NULL);
        if (parent == NULL)
                GERR("Failed to lookup parent dir");

        rc = dfs_mkdir(dfs, parent, name, mode, dir_oclass);
        DCHECK(rc, "dfs_mkdir() of %s Failed", name);

	if (name)
		free(name);
	if (dir_name)
		free(dir_name);
	return rc;
}

static int
DFS_Rmdir(const char *path, IOR_param_t * param)
{
        dfs_obj_t *parent = NULL;
	char *name = NULL, *dir_name = NULL;
	int rc;

	rc = parse_filename(path, &name, &dir_name);
        DCHECK(rc, "Failed to parse path %s", path);

	assert(dir_name);
        assert(name);

        parent = lookup_insert_dir(dir_name, NULL);
        if (parent == NULL)
                GERR("Failed to lookup parent dir");

	rc = dfs_remove(dfs, parent, name, false, NULL);
        DCHECK(rc, "dfs_remove() of %s Failed", name);

	if (name)
		free(name);
	if (dir_name)
		free(dir_name);
        if (rc)
                return -1;
	return rc;
}

static int
DFS_Access(const char *path, int mode, IOR_param_t * param)
{
        dfs_obj_t *obj = NULL;
        mode_t fmode;

        obj = lookup_insert_dir(path, &fmode);
        if (obj == NULL)
                return -1;

        /** just close if it's a file */
        if (S_ISREG(fmode))
                dfs_release(obj);

        return 0;
}

static int
DFS_Stat(const char *path, struct stat *buf, IOR_param_t * param)
{
        dfs_obj_t *parent = NULL;
	char *name = NULL, *dir_name = NULL;
	int rc;

	rc = parse_filename(path, &name, &dir_name);
        DCHECK(rc, "Failed to parse path %s", path);

	assert(dir_name);
        assert(name);

        parent = lookup_insert_dir(dir_name, NULL);
        if (parent == NULL)
                GERR("Failed to lookup parent dir");

	rc = dfs_stat(dfs, parent, name, buf);

	if (name)
		free(name);
	if (dir_name)
		free(dir_name);
        if (rc)
                return -1;
	return rc;
}

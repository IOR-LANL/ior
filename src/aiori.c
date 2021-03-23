/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
/******************************************************************************\
*                                                                              *
*        Copyright (c) 2003, The Regents of the University of California       *
*      See the file COPYRIGHT for a complete copyright notice and license.     *
*                                                                              *
********************************************************************************
*
* Definitions and prototypes of abstract I/O interface
*
\******************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <stdbool.h>

#if defined(HAVE_STRINGS_H)
#include <strings.h>
#endif

#include "aiori.h"

#if defined(HAVE_SYS_STATVFS_H)
#include <sys/statvfs.h>
#endif

#if defined(HAVE_SYS_STATFS_H)
#include <sys/statfs.h>
#endif

/*
 * Bind the global "backend" pointer to the requested backend AIORI's
 * function table.
 */

ior_aiori_t *available_aiori[] = {
#ifdef USE_POSIX_AIORI
        &posix_aiori,
#endif
#ifdef USE_DAOS_AIORI
        &dfs_aiori,
#endif
        & dummy_aiori,
#ifdef USE_HDF5_AIORI
        &hdf5_aiori,
#endif
#ifdef USE_HDFS_AIORI
        &hdfs_aiori,
#endif
#ifdef USE_IME_AIORI
        &ime_aiori,
#endif
#ifdef USE_MPIIO_AIORI
        &mpiio_aiori,
#endif
#ifdef USE_NCMPI_AIORI
        &ncmpi_aiori,
#endif
#ifdef USE_MMAP_AIORI
        &mmap_aiori,
#endif
#ifdef USE_S3_AIORI
        &s3_aiori,
        &s3_plus_aiori,
        &s3_emc_aiori,
#endif
#ifdef USE_RADOS_AIORI
        &rados_aiori,
#endif
#ifdef USE_CEPHFS_AIORI
        &cephfs_aiori,
#endif
#ifdef USE_GFARM_AIORI
        &gfarm_aiori,
#endif
        NULL
};

void * airoi_update_module_options(const ior_aiori_t * backend, options_all_t * opt){
  if (backend->get_options == NULL)
    return NULL;
  char * name = backend->name;
  ior_aiori_t **tmp = available_aiori;
  for (int i=1; *tmp != NULL; ++tmp, i++) {
    if (strcmp(opt->modules[i].prefix, name) == 0){
      opt->modules[i].options = (*tmp)->get_options(& opt->modules[i].defaults,  opt->modules[i].defaults);
      return opt->modules[i].defaults;
    }
  }
  return NULL;
}

options_all_t * airoi_create_all_module_options(option_help * global_options){
  int airoi_c = aiori_count();
  options_all_t * opt = malloc(sizeof(options_all_t));
  opt->module_count = airoi_c + 1;
  opt->modules = malloc(sizeof(option_module) * (airoi_c + 1));
  opt->modules[0].prefix = NULL;
  opt->modules[0].options = global_options;
  ior_aiori_t **tmp = available_aiori;
  for (int i=1; *tmp != NULL; ++tmp, i++) {
    opt->modules[i].prefix = (*tmp)->name;
    if((*tmp)->get_options != NULL){
      opt->modules[i].options = (*tmp)->get_options(& opt->modules[i].defaults, NULL);
    }else{
      opt->modules[i].options = NULL;
    }
  }
  return opt;
}

void aiori_supported_apis(char * APIs, char * APIs_legacy, enum bench_type type)
{
        ior_aiori_t **tmp = available_aiori;
        char delimiter = ' ';

        while (*tmp != NULL)
        {
                if ((type == MDTEST) && !(*tmp)->enable_mdtest)
                {
                    tmp++;
                    continue;
                }

                if (delimiter == ' ')
                {
                        APIs += sprintf(APIs, "%s", (*tmp)->name);
                        delimiter = '|';
                }
                else
                        APIs += sprintf(APIs, "%c%s", delimiter, (*tmp)->name);

                if ((*tmp)->name_legacy != NULL)
                        APIs_legacy += sprintf(APIs_legacy, "%c%s",
                                               delimiter, (*tmp)->name_legacy);
                tmp++;
        }
}

/**
 * Default statfs implementation.
 *
 * @param[in]  path       Path to run statfs on
 * @param[out] statfs_buf AIORI statfs buffer
 *
 * This function provides a AIORI statfs for POSIX-compliant filesystems. It
 * uses statvfs is available and falls back on statfs.
 */
int aiori_posix_statfs (const char *path, ior_aiori_statfs_t *stat_buf, IOR_param_t * param)
{
        int ret;
#if defined(HAVE_STATVFS)
        struct statvfs statfs_buf;

        ret = statvfs (path, &statfs_buf);
#else
        struct statfs statfs_buf;

        ret = statfs (path, &statfs_buf);
#endif
        if (-1 == ret) {
                return -1;
        }

        stat_buf->f_bsize = statfs_buf.f_bsize;
        stat_buf->f_blocks = statfs_buf.f_blocks;
        stat_buf->f_bfree = statfs_buf.f_bfree;
        stat_buf->f_files = statfs_buf.f_files;
        stat_buf->f_ffree = statfs_buf.f_ffree;

        return 0;
}

int aiori_posix_mkdir (const char *path, mode_t mode, IOR_param_t * param)
{
        return mkdir (path, mode);
}

int aiori_posix_rmdir (const char *path, IOR_param_t * param)
{
        return rmdir (path);
}

int aiori_posix_access (const char *path, int mode, IOR_param_t * param)
{
        return access (path, mode);
}

int aiori_posix_stat (const char *path, struct stat *buf, IOR_param_t * param)
{
        return stat (path, buf);
}

char* aiori_get_version()
{
  return "";
}

static bool is_initialized = false;

static void init_or_fini_internal(const ior_aiori_t *test_backend,
                                  const bool init)
{
        if (init)
        {
                if (test_backend->initialize)
                        test_backend->initialize();
        }
        else
        {
                if (test_backend->finalize)
                        test_backend->finalize();
        }
}

static void init_or_fini(IOR_test_t *tests, const bool init)
{
        /* Sanity check, we were compiled with SOME backend, right? */
        if (0 == aiori_count ()) {
                ERR("No IO backends compiled into aiori.  "
                    "Run 'configure --with-<backend>', and recompile.");
        }

        /* Pointer to the initialize of finalize function */


        /* if tests is NULL, initialize or finalize all available backends */
        if (tests == NULL)
        {
                for (ior_aiori_t **tmp = available_aiori ; *tmp != NULL; ++tmp)
                        init_or_fini_internal(*tmp, init);

                return;
        }

        for (IOR_test_t *t = tests; t != NULL; t = t->next)
        {
                IOR_param_t *params = &t->params;
                assert(params != NULL);

                const ior_aiori_t *test_backend = params->backend;
                assert(test_backend != NULL);

                init_or_fini_internal(test_backend, init);
        }
}


/**
 * Initialize IO backends.
 *
 * @param[in]  tests      Pointers to the first test
 *
 * This function initializes all backends which will be used. If tests is NULL
 * all available backends are initialized.
 */
void aiori_initialize(IOR_test_t *tests)
{
        if (is_initialized)
            return;

        init_or_fini(tests, true);

        is_initialized = true;
}

/**
 * Finalize IO backends.
 *
 * @param[in]  tests      Pointers to the first test
 *
 * This function finalizes all backends which were used. If tests is NULL
 * all available backends are finialized.
 */
void aiori_finalize(IOR_test_t *tests)
{
        if (!is_initialized)
            return;

        is_initialized = false;

        init_or_fini(tests, false);
}

const ior_aiori_t *aiori_select (const char *api)
{
        char warn_str[256] = {0};
        for (ior_aiori_t **tmp = available_aiori ; *tmp != NULL; ++tmp) {
                char *name_leg = (*tmp)->name_legacy;
                if (NULL != api &&
                    (strcasecmp(api, (*tmp)->name) != 0) &&
                    (name_leg == NULL || strcasecmp(api, name_leg) != 0))
                    continue;

                if (name_leg != NULL && strcasecmp(api, name_leg) == 0)
                {
                        snprintf(warn_str, 256, "%s backend is deprecated use %s"
                                 " instead", api, (*tmp)->name);
                        WARN(warn_str);
                }

                if (NULL == (*tmp)->statfs) {
                        (*tmp)->statfs = aiori_posix_statfs;
                        snprintf(warn_str, 256, "assuming POSIX-based backend for"
                                 " %s statfs call", api);
                        WARN(warn_str);
                }
                if (NULL == (*tmp)->mkdir) {
                        (*tmp)->mkdir = aiori_posix_mkdir;
                        snprintf(warn_str, 256, "assuming POSIX-based backend for"
                                 " %s mkdir call", api);
                        WARN(warn_str);
                }
                if (NULL == (*tmp)->rmdir) {
                        (*tmp)->rmdir = aiori_posix_rmdir;
                        snprintf(warn_str, 256, "assuming POSIX-based backend for"
                                 " %s rmdir call", api);
                        WARN(warn_str);
                }
                if (NULL == (*tmp)->access) {
                        (*tmp)->access = aiori_posix_access;
                        snprintf(warn_str, 256, "assuming POSIX-based backend for"
                                 " %s access call", api);
                        WARN(warn_str);
                }
                if (NULL == (*tmp)->stat) {
                        (*tmp)->stat = aiori_posix_stat;
                        snprintf(warn_str, 256, "assuming POSIX-based backend for"
                                 " %s stat call", api);
                        WARN(warn_str);
                }

                return *tmp;
        }

        return NULL;
}

int aiori_count (void)
{
        return sizeof (available_aiori)/sizeof(available_aiori[0]) - 1;
}

const char *aiori_default (void)
{
        if (aiori_count () > 0) {
                return available_aiori[0]->name;
        }

        return NULL;
}

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
*  Implement abstract I/O interface for HDF5.
*
\******************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>              /* only for fprintf() */
#include <stdlib.h>
#include <sys/stat.h>
/* HDF5 routines here still use the old 1.6 style.  Nothing wrong with that but
 * save users the trouble of  passing this flag through configure */
#define H5_USE_16_API
#include <hdf5.h>
#include <mpi.h>

#include "aiori.h"              /* abstract IOR interface */
#include "utilities.h"
#include "iordef.h"

#define NUM_DIMS 1              /* number of dimensions to data set */

/******************************************************************************/
/*
 * HDF5_CHECK will display a custom error message and then exit the program
 */

/*
 * should use MPI_Abort(), not exit(), in this macro; some versions of
 * MPI, however, hang with HDF5 property lists et al. left unclosed
 */

/*
 * for versions later than hdf5-1.6, the H5Eget_[major|minor]() functions
 * have been deprecated and replaced with H5Eget_msg()
 */
#if H5_VERS_MAJOR > 1 && H5_VERS_MINOR > 6
#define HDF5_CHECK(HDF5_RETURN, MSG) do {                                \
    char   resultString[1024];                                           \
                                                                         \
    if (HDF5_RETURN < 0) {                                               \
        fprintf(stdout, "** error **\n");                                \
        fprintf(stdout, "ERROR in %s (line %d): %s.\n",                  \
                __FILE__, __LINE__, MSG);                                \
        strcpy(resultString, H5Eget_major((H5E_major_t)HDF5_RETURN));    \
        if (strcmp(resultString, "Invalid major error number") != 0)     \
            fprintf(stdout, "HDF5 %s\n", resultString);                  \
        strcpy(resultString, H5Eget_minor((H5E_minor_t)HDF5_RETURN));    \
        if (strcmp(resultString, "Invalid minor error number") != 0)     \
            fprintf(stdout, "%s\n", resultString);                       \
        fprintf(stdout, "** exiting **\n");                              \
        exit(-1);                                                        \
    }                                                                    \
} while(0)
#else                           /* ! (H5_VERS_MAJOR > 1 && H5_VERS_MINOR > 6) */
#define HDF5_CHECK(HDF5_RETURN, MSG) do {                                \
                                                                         \
    if (HDF5_RETURN < 0) {                                               \
        fprintf(stdout, "** error **\n");                                \
        fprintf(stdout, "ERROR in %s (line %d): %s.\n",                  \
                __FILE__, __LINE__, MSG);                                \
        /*                                                               \
         * H5Eget_msg(hid_t mesg_id, H5E_type_t* mesg_type,              \
         *            char* mesg, size_t size)                           \
         */                                                              \
        fprintf(stdout, "** exiting **\n");                              \
        exit(-1);                                                        \
    }                                                                    \
} while(0)
#endif                          /* H5_VERS_MAJOR > 1 && H5_VERS_MINOR > 6 */
/**************************** P R O T O T Y P E S *****************************/

static IOR_offset_t SeekOffset(void *, IOR_offset_t, aiori_mod_opt_t *);
static void SetupDataSet(void *, int flags, aiori_mod_opt_t *);
static aiori_fd_t *HDF5_Create(char *, int flags, aiori_mod_opt_t *);
static aiori_fd_t *HDF5_Open(char *, int flags, aiori_mod_opt_t *);
static IOR_offset_t HDF5_Xfer(int, aiori_fd_t *, IOR_size_t *,
                           IOR_offset_t, IOR_offset_t, aiori_mod_opt_t *);
static void HDF5_Close(aiori_fd_t *, aiori_mod_opt_t *);
static void HDF5_Delete(char *, aiori_mod_opt_t *);
static char* HDF5_GetVersion();
static void HDF5_Fsync(aiori_fd_t *, aiori_mod_opt_t *);
static IOR_offset_t HDF5_GetFileSize(aiori_mod_opt_t *, char *);
static int HDF5_Access(const char *, int, aiori_mod_opt_t *);
static void HDF5_init_xfer_options(aiori_xfer_hint_t * params);
static int HDF5_check_params(aiori_mod_opt_t * options);

/************************** O P T I O N S *****************************/
typedef struct{
  int collective_md;
  char * hintsFileName;            /* full name for hints file */
  int showHints;                   /* show hints */
  int individualDataSets;          /* datasets not shared by all procs */
  int noFill;                      /* no fill in file creation */
  IOR_offset_t setAlignment;       /* alignment in bytes */
} HDF5_options_t;
/***************************** F U N C T I O N S ******************************/

static option_help * HDF5_options(aiori_mod_opt_t ** init_backend_options, aiori_mod_opt_t * init_values){
  HDF5_options_t * o = malloc(sizeof(HDF5_options_t));

  if (init_values != NULL){
    memcpy(o, init_values, sizeof(HDF5_options_t));
  }else{
    memset(o, 0, sizeof(HDF5_options_t));
    /* initialize the options properly */
    o->collective_md = 0;
    o->setAlignment = 1;
  }

  *init_backend_options = (aiori_mod_opt_t*) o;

  option_help h [] = {
    {0, "hdf5.collectiveMetadata", "Use collectiveMetadata (available since HDF5-1.10.0)", OPTION_FLAG, 'd', & o->collective_md},
    {0, "hdf5.hintsFileName","Full name for hints file", OPTION_OPTIONAL_ARGUMENT, 's', & o->hintsFileName},
    {0, "hdf5.showHints",    "Show MPI hints", OPTION_FLAG, 'd', & o->showHints},
    {0, "hdf5.individualDataSets",        "Datasets not shared by all procs [not working]", OPTION_FLAG, 'd', & o->individualDataSets},
    {0, "hdf5.setAlignment",        "HDF5 alignment in bytes (e.g.: 8, 4k, 2m, 1g)", OPTION_OPTIONAL_ARGUMENT, 'd', & o->setAlignment},
    {0, "hdf5.noFill", "No fill in HDF5 file creation", OPTION_FLAG, 'd', & o->noFill},
    LAST_OPTION
  };
  option_help * help = malloc(sizeof(h));
  memcpy(help, h, sizeof(h));
  return help;
}


/************************** D E C L A R A T I O N S ***************************/

ior_aiori_t hdf5_aiori = {
        .name = "HDF5",
        .name_legacy = NULL,
        .create = HDF5_Create,
        .open = HDF5_Open,
        .xfer = HDF5_Xfer,
        .close = HDF5_Close,
        .delete = HDF5_Delete,
        .get_version = HDF5_GetVersion,
        .xfer_hints = HDF5_init_xfer_options,
        .fsync = HDF5_Fsync,
        .get_file_size = HDF5_GetFileSize,
        .statfs = aiori_posix_statfs,
        .mkdir = aiori_posix_mkdir,
        .rmdir = aiori_posix_rmdir,
        .access = HDF5_Access,
        .stat = aiori_posix_stat,
        .get_options = HDF5_options,
        .check_params = HDF5_check_params
};

static hid_t xferPropList;      /* xfer property list */
hid_t dataSet;                  /* data set id */
hid_t dataSpace;                /* data space id */
hid_t fileDataSpace;            /* file data space id */
hid_t memDataSpace;             /* memory data space id */
int newlyOpenedFile;            /* newly opened file */

/***************************** F U N C T I O N S ******************************/
static aiori_xfer_hint_t * hints = NULL;

static void HDF5_init_xfer_options(aiori_xfer_hint_t * params){
  hints = params;
  /** HDF5 utilizes the MPIIO backend too, so init hints there */
  MPIIO_xfer_hints(params);
}

static int HDF5_check_params(aiori_mod_opt_t * options){
  HDF5_options_t *o = (HDF5_options_t*) options;
  if (o->setAlignment < 0)
      ERR("alignment must be non-negative integer");
  if (o->individualDataSets)
      ERR("individual data sets not implemented");
  if (o->noFill) {
    /* check if hdf5 available */
#if defined (H5_VERS_MAJOR) && defined (H5_VERS_MINOR)
    /* no-fill option not available until hdf5-1.6.x */
#if (H5_VERS_MAJOR > 0 && H5_VERS_MINOR > 5)
#else
    ERR("'no fill' option not available in HDF5");
#endif
#else
    WARN("unable to determine HDF5 version for 'no fill' usage");
#endif
  }
  return 0;
}

/*
 * Create and open a file through the HDF5 interface.
 */
static aiori_fd_t *HDF5_Create(char *testFileName, int flags, aiori_mod_opt_t * param)
{
        return HDF5_Open(testFileName, flags, param);
}

/*
 * Open a file through the HDF5 interface.
 */
static aiori_fd_t *HDF5_Open(char *testFileName, int flags, aiori_mod_opt_t * param)
{
        HDF5_options_t *o = (HDF5_options_t*) param;
        hid_t accessPropList, createPropList;
        hsize_t memStart[NUM_DIMS],
            dataSetDims[NUM_DIMS],
            memStride[NUM_DIMS],
            memCount[NUM_DIMS], memBlock[NUM_DIMS], memDataSpaceDims[NUM_DIMS];
        int tasksPerDataSet;
        unsigned fd_mode = (unsigned)0;
        hid_t *fd;
        MPI_Comm comm;
        MPI_Info mpiHints = MPI_INFO_NULL;

        fd = (hid_t *) malloc(sizeof(hid_t));
        if (fd == NULL)
                ERR("malloc() failed");
        /*
         * HDF5 uses different flags than those for POSIX/MPIIO
         */
        /* set IOR file flags to HDF5 flags */
        /* -- file open flags -- */
        if (flags & IOR_RDONLY) {
                fd_mode |= H5F_ACC_RDONLY;
        }
        if (flags & IOR_WRONLY || flags & IOR_RDWR) {
                fd_mode |= H5F_ACC_RDWR;
        }
        if (flags & IOR_APPEND) {
                fprintf(stdout, "File append not implemented in HDF5\n");
        }
        if (flags & IOR_CREAT) {
                fd_mode |= H5F_ACC_CREAT;
        }
        if (flags & IOR_EXCL) {
                fd_mode |= H5F_ACC_EXCL;
        }
        if (flags & IOR_TRUNC) {
                fd_mode |= H5F_ACC_TRUNC;
        }
        if (flags & IOR_DIRECT) {
                fprintf(stdout, "O_DIRECT not implemented in HDF5\n");
        }

        /* set up file creation property list */
        createPropList = H5Pcreate(H5P_FILE_CREATE);
        HDF5_CHECK(createPropList, "cannot create file creation property list");
        /* set size of offset and length used to address HDF5 objects */
        HDF5_CHECK(H5Pset_sizes
                   (createPropList, sizeof(hsize_t), sizeof(hsize_t)),
                   "cannot set property list properly");

        /* set up file access property list */
        accessPropList = H5Pcreate(H5P_FILE_ACCESS);
        HDF5_CHECK(accessPropList, "cannot create file access property list");

        /*
         * someday HDF5 implementation will allow subsets of MPI_COMM_WORLD
         */
        /* store MPI communicator info for the file access property list */
        if (hints->filePerProc) {
                comm = MPI_COMM_SELF;
        } else {
                comm = testComm;
        }

        SetHints(&mpiHints, o->hintsFileName);
        /*
         * note that with MP_HINTS_FILTERED=no, all key/value pairs will
         * be in the info object.  The info object that is attached to
         * the file during MPI_File_open() will only contain those pairs
         * deemed valid by the implementation.
         */
        /* show hints passed to file */
        if (rank == 0 && o->showHints) {
                fprintf(stdout, "\nhints passed to access property list {\n");
                ShowHints(&mpiHints);
                fprintf(stdout, "}\n");
        }
        HDF5_CHECK(H5Pset_fapl_mpio(accessPropList, comm, mpiHints),
                   "cannot set file access property list");

        /* set alignment */
        HDF5_CHECK(H5Pset_alignment(accessPropList, o->setAlignment, o->setAlignment),
                   "cannot set alignment");

#ifdef HAVE_H5PSET_ALL_COLL_METADATA_OPS

        if (o->collective_md) {
                /* more scalable metadata */

                HDF5_CHECK(H5Pset_all_coll_metadata_ops(accessPropList, 1),
                        "cannot set collective md read");
                HDF5_CHECK(H5Pset_coll_metadata_write(accessPropList, 1),
                        "cannot set collective md write");
        }
#endif

        /* open file */
        if(! hints->dryRun){
          if (flags & IOR_CREAT) {     /* WRITE */
                  *fd = H5Fcreate(testFileName, H5F_ACC_TRUNC, createPropList, accessPropList);
                  HDF5_CHECK(*fd, "cannot create file");
          } else {                /* READ or CHECK */
                  *fd = H5Fopen(testFileName, fd_mode, accessPropList);
                  HDF5_CHECK(*fd, "cannot open file");
          }
        }

        /* show hints actually attached to file handle */
        if (o->showHints || (1) /* WEL - this needs fixing */ ) {
                if (rank == 0 && (o->showHints) /* WEL - this needs fixing */ ) {
                        WARN("showHints not working for HDF5");
                }
        } else {
                MPI_Info mpiHintsCheck = MPI_INFO_NULL;
                hid_t apl;
                apl = H5Fget_access_plist(*fd);
                HDF5_CHECK(H5Pget_fapl_mpio(apl, &comm, &mpiHintsCheck),
                           "cannot get info object through HDF5");
                if (rank == 0) {
                        fprintf(stdout,
                                "\nhints returned from opened file (HDF5) {\n");
                        ShowHints(&mpiHintsCheck);
                        fprintf(stdout, "}\n");
                        if (1 == 1) {   /* request the MPIIO file handle and its hints */
                                MPI_File *fd_mpiio;
                                HDF5_CHECK(H5Fget_vfd_handle
                                           (*fd, apl, (void **)&fd_mpiio),
                                           "cannot get MPIIO file handle");
                                if (mpiHintsCheck != MPI_INFO_NULL)
                                        MPI_Info_free(&mpiHintsCheck);
                                MPI_CHECK(MPI_File_get_info
                                          (*fd_mpiio, &mpiHintsCheck),
                                          "cannot get info object through MPIIO");
                                fprintf(stdout,
                                        "\nhints returned from opened file (MPIIO) {\n");
                                ShowHints(&mpiHintsCheck);
                                fprintf(stdout, "}\n");
                                if (mpiHintsCheck != MPI_INFO_NULL)
                                        MPI_Info_free(&mpiHintsCheck);
                        }
                }
                MPI_CHECK(MPI_Barrier(testComm), "barrier error");
        }

        /* this is necessary for resetting various parameters
           needed for reopening and checking the file */
        newlyOpenedFile = TRUE;

        HDF5_CHECK(H5Pclose(createPropList),
                   "cannot close creation property list");
        HDF5_CHECK(H5Pclose(accessPropList),
                   "cannot close access property list");

        /* create property list for serial/parallel access */
        xferPropList = H5Pcreate(H5P_DATASET_XFER);
        HDF5_CHECK(xferPropList, "cannot create transfer property list");

        /* set data transfer mode */
        if (hints->collective) {
                HDF5_CHECK(H5Pset_dxpl_mpio(xferPropList, H5FD_MPIO_COLLECTIVE),
                           "cannot set collective data transfer mode");
        } else {
                HDF5_CHECK(H5Pset_dxpl_mpio
                           (xferPropList, H5FD_MPIO_INDEPENDENT),
                           "cannot set independent data transfer mode");
        }

        /* set up memory data space for transfer */
        memStart[0] = (hsize_t) 0;
        memCount[0] = (hsize_t) 1;
        memStride[0] = (hsize_t) (hints->transferSize / sizeof(IOR_size_t));
        memBlock[0] = (hsize_t) (hints->transferSize / sizeof(IOR_size_t));
        memDataSpaceDims[0] = (hsize_t) hints->transferSize;
        memDataSpace = H5Screate_simple(NUM_DIMS, memDataSpaceDims, NULL);
        HDF5_CHECK(memDataSpace, "cannot create simple memory data space");

        /* define hyperslab for memory data space */
        HDF5_CHECK(H5Sselect_hyperslab(memDataSpace, H5S_SELECT_SET,
                                       memStart, memStride, memCount,
                                       memBlock), "cannot create hyperslab");

        /* set up parameters for fpp or different dataset count */
        if (hints->filePerProc) {
                tasksPerDataSet = 1;
        } else {
                if (o->individualDataSets) {
                        /* each task in segment has single data set */
                        tasksPerDataSet = 1;
                } else {
                        /* share single data set across all tasks in segment */
                        tasksPerDataSet = hints->numTasks;
                }
        }
        dataSetDims[0] = (hsize_t) ((hints->blockSize / sizeof(IOR_size_t))
                                    * tasksPerDataSet);

        /* create a simple data space containing information on size
           and shape of data set, and open it for access */
        dataSpace = H5Screate_simple(NUM_DIMS, dataSetDims, NULL);
        HDF5_CHECK(dataSpace, "cannot create simple data space");
        if (mpiHints != MPI_INFO_NULL)
                MPI_Info_free(&mpiHints);

        return (aiori_fd_t*)(fd);
}

/*
 * Write or read access to file using the HDF5 interface.
 */
static IOR_offset_t HDF5_Xfer(int access, aiori_fd_t *fd, IOR_size_t * buffer,
                              IOR_offset_t length, IOR_offset_t offset, aiori_mod_opt_t * param)
{
        static int firstReadCheck = FALSE, startNewDataSet;
        IOR_offset_t segmentPosition, segmentSize;

        /*
         * this toggle is for the read check operation, which passes through
         * this function twice; note that this function will open a data set
         * only on the first read check and close only on the second
         */
        if (access == READCHECK) {
                if (firstReadCheck == TRUE) {
                        firstReadCheck = FALSE;
                } else {
                        firstReadCheck = TRUE;
                }
        }

        /* determine by offset if need to start new data set */
        if (hints->filePerProc == TRUE) {
                segmentPosition = (IOR_offset_t) 0;
                segmentSize = hints->blockSize;
        } else {
                segmentPosition =
                    (IOR_offset_t) ((rank + rankOffset) % hints->numTasks)
                    * hints->blockSize;
                segmentSize = (IOR_offset_t) (hints->numTasks) * hints->blockSize;
        }
        if ((IOR_offset_t) ((offset - segmentPosition) % segmentSize) ==
            0) {
                /*
                 * ordinarily start a new data set, unless this is the
                 * second pass through during a read check
                 */
                startNewDataSet = TRUE;
                if (access == READCHECK && firstReadCheck != TRUE) {
                        startNewDataSet = FALSE;
                }
        }

        if(hints->dryRun)
          return length;

        /* create new data set */
        if (startNewDataSet == TRUE) {
                /* if just opened this file, no data set to close yet */
                if (newlyOpenedFile != TRUE) {
                        HDF5_CHECK(H5Dclose(dataSet), "cannot close data set");
                        HDF5_CHECK(H5Sclose(fileDataSpace),
                                   "cannot close file data space");
                }
                SetupDataSet(fd, access == WRITE ? IOR_CREAT : IOR_RDWR, param);
        }

        SeekOffset(fd, offset, param);

        /* this is necessary to reset variables for reaccessing file */
        startNewDataSet = FALSE;
        newlyOpenedFile = FALSE;

        /* access the file */
        if (access == WRITE) {  /* WRITE */
                HDF5_CHECK(H5Dwrite(dataSet, H5T_NATIVE_LLONG,
                                    memDataSpace, fileDataSpace,
                                    xferPropList, buffer),
                           "cannot write to data set");
        } else {                /* READ or CHECK */
                HDF5_CHECK(H5Dread(dataSet, H5T_NATIVE_LLONG,
                                   memDataSpace, fileDataSpace,
                                   xferPropList, buffer),
                           "cannot read from data set");
        }
        return (length);
}

/*
 * Perform fsync().
 */
static void HDF5_Fsync(aiori_fd_t *fd, aiori_mod_opt_t * param)
{
}

/*
 * Close a file through the HDF5 interface.
 */
static void HDF5_Close(aiori_fd_t *fd, aiori_mod_opt_t * param)
{
        if(hints->dryRun)
          return;
        //if (hints->fd_fppReadCheck == NULL) {
                HDF5_CHECK(H5Dclose(dataSet), "cannot close data set");
                HDF5_CHECK(H5Sclose(dataSpace), "cannot close data space");
                HDF5_CHECK(H5Sclose(fileDataSpace),
                           "cannot close file data space");
                HDF5_CHECK(H5Sclose(memDataSpace),
                           "cannot close memory data space");
                HDF5_CHECK(H5Pclose(xferPropList),
                           " cannot close transfer property list");
        //}
        HDF5_CHECK(H5Fclose(*(hid_t *) fd), "cannot close file");
        free(fd);
}

/*
 * Delete a file through the HDF5 interface.
 */
static void HDF5_Delete(char *testFileName, aiori_mod_opt_t * param)
{
  if(hints->dryRun)
    return
  MPIIO_Delete(testFileName, param);
  return;
}

/*
 * Determine api version.
 */
static char * HDF5_GetVersion()
{
  static char version[1024] = {0};
  if(version[0]) return version;

        unsigned major, minor, release;
        if (H5get_libversion(&major, &minor, &release) < 0) {
                WARN("cannot get HDF5 library version");
        } else {
                sprintf(version, "%u.%u.%u", major, minor, release);
        }
#ifndef H5_HAVE_PARALLEL
        strcat(version, " (Serial)");
#else                           /* H5_HAVE_PARALLEL */
        strcat(version, " (Parallel)");
#endif                          /* not H5_HAVE_PARALLEL */
  return version;
}

/*
 * Seek to offset in file using the HDF5 interface and set up hyperslab.
 */
static IOR_offset_t SeekOffset(void *fd, IOR_offset_t offset,
                                            aiori_mod_opt_t * param)
{
        HDF5_options_t *o = (HDF5_options_t*) param;
        IOR_offset_t segmentSize;
        hsize_t hsStride[NUM_DIMS], hsCount[NUM_DIMS], hsBlock[NUM_DIMS];
        hsize_t hsStart[NUM_DIMS];

        if (hints->filePerProc == TRUE) {
                segmentSize = (IOR_offset_t) hints->blockSize;
        } else {
                segmentSize =
                    (IOR_offset_t) (hints->numTasks) * hints->blockSize;
        }

        /* create a hyperslab representing the file data space */
        if (o->individualDataSets) {
                /* start at zero offset if not */
                hsStart[0] = (hsize_t) ((offset % hints->blockSize)
                                        / sizeof(IOR_size_t));
        } else {
                /* start at a unique offset if shared */
                hsStart[0] =
                    (hsize_t) ((offset % segmentSize) / sizeof(IOR_size_t));
        }
        hsCount[0] = (hsize_t) 1;
        hsStride[0] = (hsize_t) (hints->transferSize / sizeof(IOR_size_t));
        hsBlock[0] = (hsize_t) (hints->transferSize / sizeof(IOR_size_t));

        /* retrieve data space from data set for hyperslab */
        fileDataSpace = H5Dget_space(dataSet);
        HDF5_CHECK(fileDataSpace, "cannot get data space from data set");
        HDF5_CHECK(H5Sselect_hyperslab(fileDataSpace, H5S_SELECT_SET,
                                       hsStart, hsStride, hsCount, hsBlock),
                   "cannot select hyperslab");
        return (offset);
}

/*
 * Create HDF5 data set.
 */
static void SetupDataSet(void *fd, int flags, aiori_mod_opt_t * param)
{
        HDF5_options_t *o = (HDF5_options_t*) param;
        char dataSetName[MAX_STR];
        hid_t dataSetPropList;
        int dataSetID;
        static int dataSetSuffix = 0;

        /* may want to use an extendable dataset (H5S_UNLIMITED) someday */
        /* may want to use a chunked dataset (H5S_CHUNKED) someday */

        /* need to reset suffix counter if newly-opened file */
        if (newlyOpenedFile)
                dataSetSuffix = 0;

        /* may want to use individual access to each data set someday */
        if (o->individualDataSets) {
                dataSetID = (rank + rankOffset) % hints->numTasks;
        } else {
                dataSetID = 0;
        }

        sprintf(dataSetName, "%s-%04d.%04d", "Dataset", dataSetID,
                dataSetSuffix++);

        if (flags & IOR_CREAT) {     /* WRITE */
                /* create data set */
                dataSetPropList = H5Pcreate(H5P_DATASET_CREATE);
                /* check if hdf5 available */
#if defined (H5_VERS_MAJOR) && defined (H5_VERS_MINOR)
                /* no-fill option not available until hdf5-1.6.x */
#if (H5_VERS_MAJOR > 0 && H5_VERS_MINOR > 5)
                if (o->noFill == TRUE) {
                        if (rank == 0 && verbose >= VERBOSE_1) {
                                fprintf(stdout, "\nusing 'no fill' option\n");
                        }
                        HDF5_CHECK(H5Pset_fill_time(dataSetPropList,
                                                    H5D_FILL_TIME_NEVER),
                                   "cannot set fill time for property list");
                }
#else
                char errorString[MAX_STR];
                sprintf(errorString, "'no fill' option not available in %s",
                        test->apiVersion);
                ERR(errorString);
#endif
#else
                WARN("unable to determine HDF5 version for 'no fill' usage");
#endif
                dataSet =
                    H5Dcreate(*(hid_t *) fd, dataSetName, H5T_NATIVE_LLONG,
                              dataSpace, dataSetPropList);
                HDF5_CHECK(dataSet, "cannot create data set");
        } else {                /* READ or CHECK */
                dataSet = H5Dopen(*(hid_t *) fd, dataSetName);
                HDF5_CHECK(dataSet, "cannot create data set");
        }
}

/*
 * Use MPIIO call to get file size.
 */
static IOR_offset_t
HDF5_GetFileSize(aiori_mod_opt_t * test, char *testFileName)
{
  if(hints->dryRun)
    return 0;
  return(MPIIO_GetFileSize(test, testFileName));
}

/*
 * Use MPIIO call to check for access.
 */
static int HDF5_Access(const char *path, int mode, aiori_mod_opt_t *param)
{
  if(hints->dryRun)
    return 0;
  return(MPIIO_Access(path, mode, param));
}

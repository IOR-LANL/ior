/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
/******************************************************************************\
*                                                                              *
*        Copyright (c) 2003, The Regents of the University of California       *
*      See the file COPYRIGHT for a complete copyright notice and license.     *
*                                                                              *
\******************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>              /* tolower() */
#include <errno.h>
#include <math.h>
#include <mpi.h>
#include <string.h>

#if defined(HAVE_STRINGS_H)
#include <strings.h>
#endif

#include <sys/stat.h>           /* struct stat */
#include <time.h>

#ifndef _WIN32
# include <sys/time.h>           /* gettimeofday() */
# include <sys/utsname.h>        /* uname() */
#endif

#include <assert.h>

#include "ior.h"
#include "ior-internal.h"
#include "aiori.h"
#include "utilities.h"
#include "parse_options.h"

#define IOR_NB_TIMERS 6

/* file scope globals */
extern char **environ;
static int totalErrorCount;
static const ior_aiori_t *backend;

static void DestroyTests(IOR_test_t *tests_head);
static char *PrependDir(IOR_param_t *, char *);
static char **ParseFileName(char *, int *);
static void InitTests(IOR_test_t * , MPI_Comm);
static void TestIoSys(IOR_test_t *);
static void ValidateTests(IOR_param_t *);
static IOR_offset_t WriteOrRead(IOR_param_t *test, IOR_results_t *results,
                                void *fd, const int access,
                                IOR_io_buffers *ioBuffers);

IOR_test_t * ior_run(int argc, char **argv, MPI_Comm world_com, FILE * world_out){
        IOR_test_t *tests_head;
        IOR_test_t *tptr;
        out_logfile = world_out;
        out_resultfile = world_out;
        mpi_comm_world = world_com;

        MPI_CHECK(MPI_Comm_rank(mpi_comm_world, &rank), "cannot get rank");

        /* setup tests, and validate parameters */
        tests_head = ParseCommandLine(argc, argv);
        InitTests(tests_head, world_com);
        verbose = tests_head->params.verbose;

        PrintHeader(argc, argv);

        /* perform each test */
        for (tptr = tests_head; tptr != NULL; tptr = tptr->next) {
                aiori_initialize(tptr);
                totalErrorCount = 0;
                verbose = tptr->params.verbose;
                backend = tptr->params.backend;
                if (rank == 0 && verbose >= VERBOSE_0) {
                        ShowTestStart(&tptr->params);
                }
                TestIoSys(tptr);
                tptr->results->errors = totalErrorCount;
                ShowTestEnd(tptr);
                aiori_finalize(tptr);
        }

        PrintLongSummaryAllTests(tests_head);

        /* display finish time */
        PrintTestEnds();
        return tests_head;
}



int ior_main(int argc, char **argv)
{
    IOR_test_t *tests_head;
    IOR_test_t *tptr;

    out_logfile = stdout;
    out_resultfile = stdout;

    /*
     * check -h option from commandline without starting MPI;
     */
    tests_head = ParseCommandLine(argc, argv);

    /* start the MPI code */
    MPI_CHECK(MPI_Init(&argc, &argv), "cannot initialize MPI");

    mpi_comm_world = MPI_COMM_WORLD;
    MPI_CHECK(MPI_Comm_rank(mpi_comm_world, &rank), "cannot get rank");

    /* set error-handling */
    /*MPI_CHECK(MPI_Errhandler_set(mpi_comm_world, MPI_ERRORS_RETURN),
       "cannot set errhandler"); */

    /* setup tests, and validate parameters */
    InitTests(tests_head, mpi_comm_world);
    verbose = tests_head->params.verbose;

    aiori_initialize(tests_head); // this is quite suspicious, likely an error when multiple tests need to be executed with different backends and options

    PrintHeader(argc, argv);

    /* perform each test */
    for (tptr = tests_head; tptr != NULL; tptr = tptr->next) {
            verbose = tptr->params.verbose;
            backend = tptr->params.backend;
            if (rank == 0 && verbose >= VERBOSE_0) {
                backend = tptr->params.backend;
                ShowTestStart(&tptr->params);
            }

            // This is useful for trapping a running MPI process.  While
            // this is sleeping, run the script 'testing/hdfs/gdb.attach'
            if (verbose >= VERBOSE_4) {
                    fprintf(out_logfile, "\trank %d: sleeping\n", rank);
                    sleep(5);
                    fprintf(out_logfile, "\trank %d: awake.\n", rank);
            }

            TestIoSys(tptr);
            ShowTestEnd(tptr);
    }

    if (verbose < 0)
            /* always print final summary */
            verbose = 0;
    PrintLongSummaryAllTests(tests_head);

    /* display finish time */
    PrintTestEnds();

    aiori_finalize(tests_head);

    MPI_CHECK(MPI_Finalize(), "cannot finalize MPI");

    DestroyTests(tests_head);

    return totalErrorCount;
}

/***************************** F U N C T I O N S ******************************/

/*
 * Initialize an IOR_param_t structure to the defaults
 */
void init_IOR_Param_t(IOR_param_t * p)
{
        const char *default_aiori = aiori_default ();
        char *hdfs_user;

        assert (NULL != default_aiori);

        memset(p, 0, sizeof(IOR_param_t));

        p->mode = IOR_IRUSR | IOR_IWUSR | IOR_IRGRP | IOR_IWGRP;
        p->openFlags = IOR_RDWR | IOR_CREAT;

        p->api = strdup(default_aiori);
        p->platform = strdup("HOST(OSTYPE)");
        p->testFileName = strdup("testFile");

        p->writeFile = p->readFile = FALSE;
        p->checkWrite = p->checkRead = FALSE;

        /*
         * These can be overridden from the command-line but otherwise will be
         * set from MPI.
         */
        p->numTasks = -1;
        p->numNodes = -1;
        p->numTasksOnNode0 = -1;

        p->repetitions = 1;
        p->repCounter = -1;
        p->open = WRITE;
        p->taskPerNodeOffset = 1;
        p->segmentCount = 1;
        p->blockSize = 1048576;
        p->transferSize = 262144;
        p->randomSeed = -1;
        p->incompressibleSeed = 573;
        p->testComm = mpi_comm_world;
        p->setAlignment = 1;
        p->lustre_start_ost = -1;

        hdfs_user = getenv("USER");
        if (!hdfs_user)
          hdfs_user = "";
        p->hdfs_user = strdup(hdfs_user);
        p->hdfs_name_node      = "default";
        p->hdfs_name_node_port = 0; /* ??? */
        p->hdfs_fs = NULL;
        p->hdfs_replicas = 0;   /* invokes the default */
        p->hdfs_block_size = 0;

        p->URI = NULL;
        p->part_number = 0;

        p->beegfs_numTargets = -1;
        p->beegfs_chunkSize = -1;
}

static void
DisplayOutliers(int numTasks,
                double timerVal,
                char *timeString, int access, int outlierThreshold)
{
        char accessString[MAX_STR];
        double sum, mean, sqrDiff, var, sd;

        /* for local timerVal, don't compensate for wall clock delta */
        timerVal += wall_clock_delta;

        MPI_CHECK(MPI_Allreduce
                  (&timerVal, &sum, 1, MPI_DOUBLE, MPI_SUM, testComm),
                  "MPI_Allreduce()");
        mean = sum / numTasks;
        sqrDiff = pow((mean - timerVal), 2);
        MPI_CHECK(MPI_Allreduce
                  (&sqrDiff, &var, 1, MPI_DOUBLE, MPI_SUM, testComm),
                  "MPI_Allreduce()");
        var = var / numTasks;
        sd = sqrt(var);

        if (access == WRITE) {
                strcpy(accessString, "write");
        } else {                /* READ */
                strcpy(accessString, "read");
        }
        if (fabs(timerVal - mean) > (double)outlierThreshold) {
                char hostname[MAX_STR];
                int ret = gethostname(hostname, MAX_STR);
                if (ret != 0)
                        strcpy(hostname, "unknown");

                fprintf(out_logfile, "WARNING: for %s, task %d, %s %s is %f\n",
                        hostname, rank, accessString, timeString, timerVal);
                fprintf(out_logfile, "         (mean=%f, stddev=%f)\n", mean, sd);
                fflush(out_logfile);
        }
}

/*
 * Check for outliers in start/end times and elapsed create/xfer/close times.
 */
static void
CheckForOutliers(IOR_param_t *test, const double *timer, const int access)
{
        DisplayOutliers(test->numTasks, timer[0],
                        "start time", access, test->outlierThreshold);
        DisplayOutliers(test->numTasks,
                        timer[1] - timer[0],
                        "elapsed create time", access, test->outlierThreshold);
        DisplayOutliers(test->numTasks,
                        timer[3] - timer[2],
                        "elapsed transfer time", access,
                        test->outlierThreshold);
        DisplayOutliers(test->numTasks,
                        timer[5] - timer[4],
                        "elapsed close time", access, test->outlierThreshold);
        DisplayOutliers(test->numTasks, timer[5], "end time",
                        access, test->outlierThreshold);
}

/*
 * Check if actual file size equals expected size; if not use actual for
 * calculating performance rate.
 */
static void CheckFileSize(IOR_test_t *test, IOR_offset_t dataMoved, int rep,
                          const int access)
{
        IOR_param_t *params = &test->params;
        IOR_results_t *results = test->results;
        IOR_point_t *point = (access == WRITE) ? &results[rep].write :
                                                 &results[rep].read;

        MPI_CHECK(MPI_Allreduce(&dataMoved, &point->aggFileSizeFromXfer,
                                1, MPI_LONG_LONG_INT, MPI_SUM, testComm),
                  "cannot total data moved");

        if (strcasecmp(params->api, "HDF5") != 0 && strcasecmp(params->api, "NCMPI") != 0) {
                if (verbose >= VERBOSE_0 && rank == 0) {
                        if ((params->expectedAggFileSize
                             != point->aggFileSizeFromXfer)
                            || (point->aggFileSizeFromStat
                                != point->aggFileSizeFromXfer)) {
                                fprintf(out_logfile,
                                        "WARNING: Expected aggregate file size       = %lld.\n",
                                        (long long) params->expectedAggFileSize);
                                fprintf(out_logfile,
                                        "WARNING: Stat() of aggregate file size      = %lld.\n",
                                        (long long) point->aggFileSizeFromStat);
                                fprintf(out_logfile,
                                        "WARNING: Using actual aggregate bytes moved = %lld.\n",
                                        (long long) point->aggFileSizeFromXfer);
                                if(params->deadlineForStonewalling){
                                  fprintf(out_logfile,
                                        "WARNING: maybe caused by deadlineForStonewalling\n");
                                }
                        }
                }
        }

        point->aggFileSizeForBW = point->aggFileSizeFromXfer;
}

/*
 * Compare buffers after reading/writing each transfer.  Displays only first
 * difference in buffers and returns total errors counted.
 */
static size_t
CompareBuffers(void *expectedBuffer,
               void *unknownBuffer,
               size_t size,
               IOR_offset_t transferCount, IOR_param_t *test, int access)
{
        char testFileName[MAX_PATHLEN];
        char bufferLabel1[MAX_STR];
        char bufferLabel2[MAX_STR];
        size_t i, j, length, first, last;
        size_t errorCount = 0;
        int inError = 0;
        unsigned long long *goodbuf = (unsigned long long *)expectedBuffer;
        unsigned long long *testbuf = (unsigned long long *)unknownBuffer;

        if (access == WRITECHECK || access == READCHECK) {
                strcpy(bufferLabel1, "Expected: ");
                strcpy(bufferLabel2, "Actual:   ");
        } else {
                ERR("incorrect argument for CompareBuffers()");
        }

        length = size / sizeof(IOR_size_t);
        first = -1;
        if (verbose >= VERBOSE_3) {
                fprintf(out_logfile,
                        "[%d] At file byte offset %lld, comparing %llu-byte transfer\n",
                        rank, test->offset, (long long)size);
        }
        for (i = 0; i < length; i++) {
                if (testbuf[i] != goodbuf[i]) {
                        errorCount++;
                        if (verbose >= VERBOSE_2) {
                                fprintf(out_logfile,
                                        "[%d] At transfer buffer #%lld, index #%lld (file byte offset %lld):\n",
                                        rank, transferCount - 1, (long long)i,
                                        test->offset +
                                        (IOR_size_t) (i * sizeof(IOR_size_t)));
                                fprintf(out_logfile, "[%d] %s0x", rank, bufferLabel1);
                                fprintf(out_logfile, "%016llx\n", goodbuf[i]);
                                fprintf(out_logfile, "[%d] %s0x", rank, bufferLabel2);
                                fprintf(out_logfile, "%016llx\n", testbuf[i]);
                        }
                        if (!inError) {
                                inError = 1;
                                first = i;
                                last = i;
                        } else {
                                last = i;
                        }
                } else if (verbose >= VERBOSE_5 && i % 4 == 0) {
                        fprintf(out_logfile,
                                "[%d] PASSED offset = %lld bytes, transfer %lld\n",
                                rank,
                                ((i * sizeof(unsigned long long)) +
                                 test->offset), transferCount);
                        fprintf(out_logfile, "[%d] GOOD %s0x", rank, bufferLabel1);
                        for (j = 0; j < 4; j++)
                                fprintf(out_logfile, "%016llx ", goodbuf[i + j]);
                        fprintf(out_logfile, "\n[%d] GOOD %s0x", rank, bufferLabel2);
                        for (j = 0; j < 4; j++)
                                fprintf(out_logfile, "%016llx ", testbuf[i + j]);
                        fprintf(out_logfile, "\n");
                }
        }
        if (inError) {
                inError = 0;
                GetTestFileName(testFileName, test);
                fprintf(out_logfile,
                        "[%d] FAILED comparison of buffer containing %d-byte ints:\n",
                        rank, (int)sizeof(unsigned long long int));
                fprintf(out_logfile, "[%d]   File name = %s\n", rank, testFileName);
                fprintf(out_logfile, "[%d]   In transfer %lld, ", rank,
                        transferCount);
                fprintf(out_logfile,
                        "%lld errors between buffer indices %lld and %lld.\n",
                        (long long)errorCount, (long long)first,
                        (long long)last);
                fprintf(out_logfile, "[%d]   File byte offset = %lld:\n", rank,
                        ((first * sizeof(unsigned long long)) + test->offset));

                fprintf(out_logfile, "[%d]     %s0x", rank, bufferLabel1);
                for (j = first; j < length && j < first + 4; j++)
                        fprintf(out_logfile, "%016llx ", goodbuf[j]);
                if (j == length)
                        fprintf(out_logfile, "[end of buffer]");
                fprintf(out_logfile, "\n[%d]     %s0x", rank, bufferLabel2);
                for (j = first; j < length && j < first + 4; j++)
                        fprintf(out_logfile, "%016llx ", testbuf[j]);
                if (j == length)
                        fprintf(out_logfile, "[end of buffer]");
                fprintf(out_logfile, "\n");
                if (test->quitOnError == TRUE)
                        ERR("data check error, aborting execution");
        }
        return (errorCount);
}

/*
 * Count all errors across all tasks; report errors found.
 */
static int CountErrors(IOR_param_t * test, int access, int errors)
{
        int allErrors = 0;

        if (test->checkWrite || test->checkRead) {
                MPI_CHECK(MPI_Reduce(&errors, &allErrors, 1, MPI_INT, MPI_SUM,
                                     0, testComm), "cannot reduce errors");
                MPI_CHECK(MPI_Bcast(&allErrors, 1, MPI_INT, 0, testComm),
                          "cannot broadcast allErrors value");
                if (allErrors != 0) {
                        totalErrorCount += allErrors;
                        test->errorFound = TRUE;
                }
                if (rank == 0 && allErrors != 0) {
                        if (allErrors < 0) {
                                WARN("overflow in errors counted");
                                allErrors = -1;
                        }
                        fprintf(out_logfile, "WARNING: incorrect data on %s (%d errors found).\n",
                                access == WRITECHECK ? "write" : "read", allErrors);
                        fprintf(out_logfile,
                                "Used Time Stamp %u (0x%x) for Data Signature\n",
                                test->timeStampSignatureValue,
                                test->timeStampSignatureValue);
                }
        }
        return (allErrors);
}

/*
 * Allocate a page-aligned (required by O_DIRECT) buffer.
 */
static void *aligned_buffer_alloc(size_t size)
{
        size_t pageMask;
        char *buf, *tmp;
        char *aligned;

#ifdef HAVE_SYSCONF
        long pageSize = sysconf(_SC_PAGESIZE);
#else
        size_t pageSize = getpagesize();
#endif

        pageMask = pageSize - 1;
        buf = malloc(size + pageSize + sizeof(void *));
        if (buf == NULL)
                ERR("out of memory");
        /* find the alinged buffer */
        tmp = buf + sizeof(char *);
        aligned = tmp + pageSize - ((size_t) tmp & pageMask);
        /* write a pointer to the original malloc()ed buffer into the bytes
           preceding "aligned", so that the aligned buffer can later be free()ed */
        tmp = aligned - sizeof(void *);
        *(void **)tmp = buf;

        return (void *)aligned;
}

/*
 * Free a buffer allocated by aligned_buffer_alloc().
 */
static void aligned_buffer_free(void *buf)
{
        free(*(void **)((char *)buf - sizeof(char *)));
}

void AllocResults(IOR_test_t *test)
{
  int reps;
  if (test->results != NULL)
    return;

  reps = test->params.repetitions;
  test->results = (IOR_results_t *) safeMalloc(sizeof(IOR_results_t) * reps);
}

void FreeResults(IOR_test_t *test)
{
  if (test->results != NULL) {
      free(test->results);
  }
}


/**
 * Create new test for list of tests.
 */
IOR_test_t *CreateTest(IOR_param_t *init_params, int test_num)
{
        IOR_test_t *newTest = NULL;

        newTest = (IOR_test_t *) malloc(sizeof(IOR_test_t));
        if (newTest == NULL)
                ERR("malloc() of IOR_test_t failed");
        newTest->params = *init_params;
        newTest->params.platform = GetPlatformName();
        newTest->params.id = test_num;
        newTest->next = NULL;
        newTest->results = NULL;

        return newTest;
}

static void DestroyTest(IOR_test_t *test)
{
        FreeResults(test);
        free(test);
}

static void DestroyTests(IOR_test_t *tests_head)
{
        IOR_test_t *tptr, *next;

        for (tptr = tests_head; tptr != NULL; tptr = next) {
                next = tptr->next;
                DestroyTest(tptr);
        }
}

/*
 * Distribute IOR_HINTs to all tasks' environments.
 */
void DistributeHints(void)
{
        char hint[MAX_HINTS][MAX_STR], fullHint[MAX_STR], hintVariable[MAX_STR];
        int hintCount = 0, i;

        if (rank == 0) {
                for (i = 0; environ[i] != NULL; i++) {
                        if (strncmp(environ[i], "IOR_HINT", strlen("IOR_HINT"))
                            == 0) {
                                hintCount++;
                                if (hintCount == MAX_HINTS) {
                                        WARN("exceeded max hints; reset MAX_HINTS and recompile");
                                        hintCount = MAX_HINTS;
                                        break;
                                }
                                /* assume no IOR_HINT is greater than MAX_STR in length */
                                strncpy(hint[hintCount - 1], environ[i],
                                        MAX_STR - 1);
                        }
                }
        }

        MPI_CHECK(MPI_Bcast(&hintCount, sizeof(hintCount), MPI_BYTE,
                            0, MPI_COMM_WORLD), "cannot broadcast hints");
        for (i = 0; i < hintCount; i++) {
                MPI_CHECK(MPI_Bcast(&hint[i], MAX_STR, MPI_BYTE,
                                    0, MPI_COMM_WORLD),
                          "cannot broadcast hints");
                strcpy(fullHint, hint[i]);
                strcpy(hintVariable, strtok(fullHint, "="));
                if (getenv(hintVariable) == NULL) {
                        /* doesn't exist in this task's environment; better set it */
                        if (putenv(hint[i]) != 0)
                                WARN("cannot set environment variable");
                }
        }
}

/*
 * Fill buffer, which is transfer size bytes long, with known 8-byte long long
 * int values.  In even-numbered 8-byte long long ints, store MPI task in high
 * bits and timestamp signature in low bits.  In odd-numbered 8-byte long long
 * ints, store transfer offset.  If storeFileOffset option is used, the file
 * (not transfer) offset is stored instead.
 */

static void
FillIncompressibleBuffer(void* buffer, IOR_param_t * test)

{
        size_t i;
        unsigned long long hi, lo;
        unsigned long long *buf = (unsigned long long *)buffer;

        for (i = 0; i < test->transferSize / sizeof(unsigned long long); i++) {
                hi = ((unsigned long long) rand_r(&test->incompressibleSeed) << 32);
                lo = (unsigned long long) rand_r(&test->incompressibleSeed);
                buf[i] = hi | lo;
        }
}

unsigned int reseed_incompressible_prng = TRUE;

static void
FillBuffer(void *buffer,
           IOR_param_t * test, unsigned long long offset, int fillrank)
{
        size_t i;
        unsigned long long hi, lo;
        unsigned long long *buf = (unsigned long long *)buffer;

        if(test->dataPacketType == incompressible ) { /* Make for some non compressable buffers with randomish data */

                /* In order for write checks to work, we have to restart the psuedo random sequence */
                if(reseed_incompressible_prng == TRUE) {
                        test->incompressibleSeed = test->setTimeStampSignature + rank; /* We copied seed into timestampSignature at initialization, also add the rank to add randomness between processes */
                        reseed_incompressible_prng = FALSE;
                }
                FillIncompressibleBuffer(buffer, test);
        }

        else {
                hi = ((unsigned long long)fillrank) << 32;
                lo = (unsigned long long)test->timeStampSignatureValue;
                for (i = 0; i < test->transferSize / sizeof(unsigned long long); i++) {
                        if ((i % 2) == 0) {
                                /* evens contain MPI rank and time in seconds */
                                buf[i] = hi | lo;
                        } else {
                                /* odds contain offset */
                                buf[i] = offset + (i * sizeof(unsigned long long));
                        }
                }
        }
}

/*
 * Return string describing machine name and type.
 */
char * GetPlatformName()
{
        char nodeName[MAX_STR], *p, *start, sysName[MAX_STR];
        char platformName[MAX_STR];
        struct utsname name;

        if (uname(&name) != 0) {
                EWARN("cannot get platform name");
                sprintf(sysName, "%s", "Unknown");
                sprintf(nodeName, "%s", "Unknown");
        } else {
                sprintf(sysName, "%s", name.sysname);
                sprintf(nodeName, "%s", name.nodename);
        }

        start = nodeName;
        if (strlen(nodeName) == 0) {
                p = start;
        } else {
                /* point to one character back from '\0' */
                p = start + strlen(nodeName) - 1;
        }
        /*
         * to cut off trailing node number, search backwards
         * for the first non-numeric character
         */
        while (p != start) {
                if (*p < '0' || *p > '9') {
                        *(p + 1) = '\0';
                        break;
                } else {
                        p--;
                }
        }

        sprintf(platformName, "%s(%s)", nodeName, sysName);
        return strdup(platformName);
}



/*
 * Parse file name.
 */
static char **ParseFileName(char *name, int *count)
{
        char **fileNames, *tmp, *token;
        char delimiterString[3] = { FILENAME_DELIMITER, '\n', '\0' };
        int i = 0;

        *count = 0;
        tmp = name;

        /* pass one */
        /* if something there, count the first item */
        if (*tmp != '\0') {
                (*count)++;
        }
        /* count the rest of the filenames */
        while (*tmp != '\0') {
                if (*tmp == FILENAME_DELIMITER) {
                        (*count)++;
                }
                tmp++;
        }

        fileNames = (char **)malloc((*count) * sizeof(char **));
        if (fileNames == NULL)
                ERR("out of memory");

        /* pass two */
        token = strtok(name, delimiterString);
        while (token != NULL) {
                fileNames[i] = token;
                token = strtok(NULL, delimiterString);
                i++;
        }
        return (fileNames);
}


/*
 * Return test file name to access.
 * for single shared file, fileNames[0] is returned in testFileName
 */
void GetTestFileName(char *testFileName, IOR_param_t * test)
{
        char **fileNames;
        char   initialTestFileName[MAX_PATHLEN];
        char   testFileNameRoot[MAX_STR];
        char   tmpString[MAX_STR];
        int count;

        /* parse filename for multiple file systems */
        strcpy(initialTestFileName, test->testFileName);
        fileNames = ParseFileName(initialTestFileName, &count);
        if (count > 1 && test->uniqueDir == TRUE)
                ERR("cannot use multiple file names with unique directories");
        if (test->filePerProc) {
                strcpy(testFileNameRoot,
                       fileNames[((rank +
                                   rankOffset) % test->numTasks) % count]);
        } else {
                strcpy(testFileNameRoot, fileNames[0]);
        }

        /* give unique name if using multiple files */
        if (test->filePerProc) {
                /*
                 * prepend rank subdirectory before filename
                 * e.g., /dir/file => /dir/<rank>/file
                 */
                if (test->uniqueDir == TRUE) {
                        strcpy(testFileNameRoot,
                               PrependDir(test, testFileNameRoot));
                }
                sprintf(testFileName, "%s.%08d", testFileNameRoot,
                        (rank + rankOffset) % test->numTasks);
        } else {
                strcpy(testFileName, testFileNameRoot);
        }

        /* add suffix for multiple files */
        if (test->repCounter > -1) {
                sprintf(tmpString, ".%d", test->repCounter);
                strcat(testFileName, tmpString);
        }
        free (fileNames);
}

/*
 * From absolute directory, insert rank as subdirectory.  Allows each task
 * to write to its own directory.  E.g., /dir/file => /dir/<rank>/file.
 */
static char *PrependDir(IOR_param_t * test, char *rootDir)
{
        char *dir;
        char *fname;
        int i;

        dir = (char *)malloc(MAX_STR + 1);
        if (dir == NULL)
                ERR("out of memory");

        /* get dir name */
        strcpy(dir, rootDir);
        i = strlen(dir) - 1;
        while (i > 0) {
                if (dir[i] == '\0' || dir[i] == '/') {
                        dir[i] = '/';
                        dir[i + 1] = '\0';
                        break;
                }
                i--;
        }

        /* get file name */
        fname = rootDir + i + 1;

        /* create directory with rank as subdirectory */
        sprintf(dir + i + 1, "%d", (rank + rankOffset) % test->numTasks);

        /* dir doesn't exist, so create */
        if (backend->access(dir, F_OK, test) != 0) {
                if (backend->mkdir(dir, S_IRWXU, test) < 0) {
                        ERRF("cannot create directory: %s", dir);
                }

                /* check if correct permissions */
        } else if (backend->access(dir, R_OK, test) != 0 ||
                   backend->access(dir, W_OK, test) != 0 ||
                   backend->access(dir, X_OK, test) != 0) {
                ERRF("invalid directory permissions: %s", dir);
        }

        /* concatenate dir and file names */
        strcat(dir, "/");
        strcat(dir, fname);

        return dir;
}

/******************************************************************************/
/*
 * Reduce test results, and show if verbose set.
 */
static void
ReduceIterResults(IOR_test_t *test, double *timer, const int rep, const int access)
{
        double reduced[IOR_NB_TIMERS] = { 0 };
        double diff[IOR_NB_TIMERS / 2 + 1];
        double totalTime, accessTime;
        IOR_param_t *params = &test->params;
        double bw, iops, latency, minlatency;
        int i;
        MPI_Op op;

        assert(access == WRITE || access == READ);

        /* Find the minimum start time of the even numbered timers, and the
           maximum finish time for the odd numbered timers */
        for (i = 0; i < IOR_NB_TIMERS; i++) {
                op = i % 2 ? MPI_MAX : MPI_MIN;
                MPI_CHECK(MPI_Reduce(&timer[i], &reduced[i], 1, MPI_DOUBLE,
                                     op, 0, testComm), "MPI_Reduce()");
        }

        /* Calculate elapsed times and throughput numbers */
        for (i = 0; i < IOR_NB_TIMERS / 2; i++)
                diff[i] = reduced[2 * i + 1] - reduced[2 * i];

        totalTime = reduced[5] - reduced[0];
        accessTime = reduced[3] - reduced[2];

        IOR_point_t *point = (access == WRITE) ? &test->results[rep].write :
                                                 &test->results[rep].read;

        point->time = totalTime;

        if (verbose < VERBOSE_0)
                return;

        bw = (double)point->aggFileSizeForBW / totalTime;

        /* For IOPS in this iteration, we divide the total amount of IOs from
         * all ranks over the entire access time (first start -> last end). */
        iops = (point->aggFileSizeForBW / params->transferSize) / accessTime;

        /* For Latency, we divide the total access time for each task over the
         * number of I/Os issued from that task; then reduce and display the
         * minimum (best) latency achieved. So what is reported is the average
         * latency of all ops from a single task, then taking the minimum of
         * that between all tasks. */
        latency = (timer[3] - timer[2]) / (params->blockSize / params->transferSize);
        MPI_CHECK(MPI_Reduce(&latency, &minlatency, 1, MPI_DOUBLE,
                             MPI_MIN, 0, testComm), "MPI_Reduce()");

        /* Only rank 0 tallies and prints the results. */
        if (rank != 0)
                return;

        PrintReducedResult(test, access, bw, iops, latency, diff, totalTime, rep);
}

/*
 * Check for file(s), then remove all files if file-per-proc, else single file.
 *
 */
static void RemoveFile(char *testFileName, int filePerProc, IOR_param_t * test)
{
        int tmpRankOffset = 0;
        if (filePerProc) {
                /* in random tasks, delete own file */
                if (test->reorderTasksRandom == TRUE) {
                        tmpRankOffset = rankOffset;
                        rankOffset = 0;
                        GetTestFileName(testFileName, test);
                }
                if (backend->access(testFileName, F_OK, test) == 0) {
                        if (verbose >= VERBOSE_3) {
                                fprintf(out_logfile, "task %d removing %s\n", rank,
                                        testFileName);
                        }
                        backend->delete(testFileName, test);
                }
                if (test->reorderTasksRandom == TRUE) {
                        rankOffset = tmpRankOffset;
                        GetTestFileName(testFileName, test);
                }
        } else {
                if ((rank == 0) && (backend->access(testFileName, F_OK, test) == 0)) {
                        if (verbose >= VERBOSE_3) {
                                fprintf(out_logfile, "task %d removing %s\n", rank,
                                        testFileName);
                        }
                        backend->delete(testFileName, test);
                }
        }
}

/*
 * Setup tests by parsing commandline and creating test script.
 * Perform a sanity-check on the configured parameters.
 */
static void InitTests(IOR_test_t *tests, MPI_Comm com)
{
        int mpiNumNodes = 0;
        int mpiNumTasks = 0;
        int mpiNumTasksOnNode0 = 0;

        /*
         * These default values are the same for every test and expensive to
         * retrieve so just do it once.
         */
        mpiNumNodes = GetNumNodes(com);
        mpiNumTasks = GetNumTasks(com);
        mpiNumTasksOnNode0 = GetNumTasksOnNode0(com);

        /*
         * Since there is no guarantee that anyone other than
         * task 0 has the environment settings for the hints, pass
         * the hint=value pair to everyone else in mpi_comm_world
         */
        DistributeHints();

        /* check validity of tests and create test queue */
        while (tests != NULL) {
                IOR_param_t *params = & tests->params;
                params->testComm = com;

                /* use MPI values if not overridden on command-line */
                if (params->numNodes == -1) {
                        params->numNodes = mpiNumNodes;
                }
                if (params->numTasks == -1) {
                        params->numTasks = mpiNumTasks;
                } else if (params->numTasks > mpiNumTasks) {
                        if (rank == 0) {
                                fprintf(out_logfile,
                                        "WARNING: More tasks requested (%d) than available (%d),",
                                        params->numTasks, mpiNumTasks);
                                fprintf(out_logfile, "         running with %d tasks.\n",
                                        mpiNumTasks);
                        }
                        params->numTasks = mpiNumTasks;
                }
                if (params->numTasksOnNode0 == -1) {
                        params->numTasksOnNode0 = mpiNumTasksOnNode0;
                }

                params->tasksBlockMapping = QueryNodeMapping(com,false);
                params->expectedAggFileSize =
                  params->blockSize * params->segmentCount * params->numTasks;

                ValidateTests(&tests->params);
                tests = tests->next;
        }

        init_clock();

        /* seed random number generator */
        SeedRandGen(mpi_comm_world);
}

/*
 * Setup transfer buffers, creating and filling as needed.
 */
static void XferBuffersSetup(IOR_io_buffers* ioBuffers, IOR_param_t* test,
                             int pretendRank)
{
        ioBuffers->buffer = aligned_buffer_alloc(test->transferSize);

        if (test->checkWrite || test->checkRead) {
                ioBuffers->checkBuffer = aligned_buffer_alloc(test->transferSize);
        }
        if (test->checkRead || test->checkWrite) {
                ioBuffers->readCheckBuffer = aligned_buffer_alloc(test->transferSize);
        }

        return;
}

/*
 * Free transfer buffers.
 */
static void XferBuffersFree(IOR_io_buffers* ioBuffers, IOR_param_t* test)

{
        aligned_buffer_free(ioBuffers->buffer);

        if (test->checkWrite || test->checkRead) {
                aligned_buffer_free(ioBuffers->checkBuffer);
        }
        if (test->checkRead) {
                aligned_buffer_free(ioBuffers->readCheckBuffer);
        }

        return;
}



/*
 * malloc a buffer, touching every page in an attempt to defeat lazy allocation.
 */
static void *malloc_and_touch(size_t size)
{
        size_t page_size;
        char *buf;
        char *ptr;

        if (size == 0)
                return NULL;

        page_size = sysconf(_SC_PAGESIZE);

        buf = (char *)malloc(size);
        if (buf == NULL)
                return NULL;

        for (ptr = buf; ptr < buf+size; ptr += page_size) {
                *ptr = (char)1;
        }

        return (void *)buf;
}

static void file_hits_histogram(IOR_param_t *params)
{
        int *rankoffs = NULL;
        int *filecont = NULL;
        int *filehits = NULL;
        int ifile;
        int jfile;

        if (rank == 0) {
                rankoffs = (int *)malloc(params->numTasks * sizeof(int));
                filecont = (int *)malloc(params->numTasks * sizeof(int));
                filehits = (int *)malloc(params->numTasks * sizeof(int));
        }

        MPI_CHECK(MPI_Gather(&rankOffset, 1, MPI_INT, rankoffs,
                             1, MPI_INT, 0, mpi_comm_world),
                  "MPI_Gather error");

        if (rank != 0)
                return;

        memset((void *)filecont, 0, params->numTasks * sizeof(int));
        for (ifile = 0; ifile < params->numTasks; ifile++) {
                filecont[(ifile + rankoffs[ifile]) % params->numTasks]++;
        }
        memset((void *)filehits, 0, params->numTasks * sizeof(int));
        for (ifile = 0; ifile < params->numTasks; ifile++)
                for (jfile = 0; jfile < params->numTasks; jfile++) {
                        if (ifile == filecont[jfile])
                                filehits[ifile]++;
                }
        fprintf(out_logfile, "#File Hits Dist:");
        jfile = 0;
        ifile = 0;
        while (jfile < params->numTasks && ifile < params->numTasks) {
                fprintf(out_logfile, " %d", filehits[ifile]);
                jfile += filehits[ifile], ifile++;
        }
        fprintf(out_logfile, "\n");
        free(rankoffs);
        free(filecont);
        free(filehits);
}


int test_time_elapsed(IOR_param_t *params, double startTime)
{
        double endTime;

        if (params->maxTimeDuration == 0)
                return 0;

        endTime = startTime + (params->maxTimeDuration * 60);

        return GetTimeStamp() >= endTime;
}

/*
 * hog some memory as a rough simulation of a real application's memory use
 */
static void *HogMemory(IOR_param_t *params)
{
        size_t size;
        void *buf;

        if (params->memoryPerTask != 0) {
                size = params->memoryPerTask;
        } else if (params->memoryPerNode != 0) {
                if (verbose >= VERBOSE_3)
                        fprintf(out_logfile, "This node hogging %ld bytes of memory\n",
                                params->memoryPerNode);
                size = params->memoryPerNode / params->numTasksOnNode0;
        } else {
                return NULL;
        }

        if (verbose >= VERBOSE_3)
                fprintf(out_logfile, "This task hogging %ld bytes of memory\n", size);

        buf = malloc_and_touch(size);
        if (buf == NULL)
                ERR("malloc of simulated applciation buffer failed");

        return buf;
}
/*
 * Write times taken during each iteration of the test.
 */
static void
WriteTimes(IOR_param_t *test, const double *timer, const int iteration,
           const int access)
{
        char timerName[MAX_STR];

        for (int i = 0; i < IOR_NB_TIMERS; i++) {

                if (access == WRITE) {
                        switch (i) {
                        case 0:
                                strcpy(timerName, "write open start");
                                break;
                        case 1:
                                strcpy(timerName, "write open stop");
                                break;
                        case 2:
                                strcpy(timerName, "write start");
                                break;
                        case 3:
                                strcpy(timerName, "write stop");
                                break;
                        case 4:
                                strcpy(timerName, "write close start");
                                break;
                        case 5:
                                strcpy(timerName, "write close stop");
                                break;
                        default:
                                strcpy(timerName, "invalid timer");
                                break;
                        }
                }
                else {
                        switch (i) {
                        case 0:
                                strcpy(timerName, "read open start");
                                break;
                        case 1:
                                strcpy(timerName, "read open stop");
                                break;
                        case 2:
                                strcpy(timerName, "read start");
                                break;
                        case 3:
                                strcpy(timerName, "read stop");
                                break;
                        case 4:
                                strcpy(timerName, "read close start");
                                break;
                        case 5:
                                strcpy(timerName, "read close stop");
                                break;
                        default:
                                strcpy(timerName, "invalid timer");
                                break;
                        }
                }
                fprintf(out_logfile, "Test %d: Iter=%d, Task=%d, Time=%f, %s\n",
                        test->id, iteration, (int)rank, timer[i],
                        timerName);
        }
}
/*
 * Using the test parameters, run iteration(s) of single test.
 */
static void TestIoSys(IOR_test_t *test)
{
        IOR_param_t *params = &test->params;
        IOR_results_t *results = test->results;
        char testFileName[MAX_STR];
        double timer[IOR_NB_TIMERS];
        double startTime;
        int pretendRank;
        int rep;
        void *fd;
        MPI_Group orig_group, new_group;
        int range[3];
        IOR_offset_t dataMoved; /* for data rate calculation */
        void *hog_buf;
        IOR_io_buffers ioBuffers;

        /* set up communicator for test */
        MPI_CHECK(MPI_Comm_group(mpi_comm_world, &orig_group),
                  "MPI_Comm_group() error");
        range[0] = 0;                     /* first rank */
        range[1] = params->numTasks - 1;  /* last rank */
        range[2] = 1;                     /* stride */
        MPI_CHECK(MPI_Group_range_incl(orig_group, 1, &range, &new_group),
                  "MPI_Group_range_incl() error");
        MPI_CHECK(MPI_Comm_create(mpi_comm_world, new_group, &testComm),
                  "MPI_Comm_create() error");
        MPI_CHECK(MPI_Group_free(&orig_group), "MPI_Group_Free() error");
        MPI_CHECK(MPI_Group_free(&new_group), "MPI_Group_Free() error");
        params->testComm = testComm;
        if (testComm == MPI_COMM_NULL) {
                /* tasks not in the group do not participate in this test */
                MPI_CHECK(MPI_Barrier(mpi_comm_world), "barrier error");
                return;
        }
        if (rank == 0 && verbose >= VERBOSE_1) {
                fprintf(out_logfile, "Participating tasks: %d\n", params->numTasks);
                fflush(out_logfile);
        }
        if (rank == 0 && params->reorderTasks == TRUE && verbose >= VERBOSE_1) {
                fprintf(out_logfile,
                        "Using reorderTasks '-C' (useful to avoid read cache in client)\n");
                fflush(out_logfile);
        }
        /* show test setup */
        if (rank == 0 && verbose >= VERBOSE_0)
                ShowSetup(params);

        hog_buf = HogMemory(params);

        pretendRank = (rank + rankOffset) % params->numTasks;

        /* IO Buffer Setup */

        if (params->setTimeStampSignature) { // initialize the buffer properly
                params->timeStampSignatureValue = (unsigned int) params->setTimeStampSignature;
        }
        XferBuffersSetup(&ioBuffers, params, pretendRank);
        reseed_incompressible_prng = TRUE; // reset pseudo random generator, necessary to guarantee the next call to FillBuffer produces the same value as it is right now

        /* Initial time stamp */
        startTime = GetTimeStamp();

        /* loop over test iterations */
        uint64_t params_saved_wearout = params->stoneWallingWearOutIterations;
        for (rep = 0; rep < params->repetitions; rep++) {
                PrintRepeatStart();
                /* Get iteration start time in seconds in task 0 and broadcast to
                   all tasks */
                if (rank == 0) {
                        if (! params->setTimeStampSignature) {
                                time_t currentTime;
                                if ((currentTime = time(NULL)) == -1) {
                                        ERR("cannot get current time");
                                }
                                params->timeStampSignatureValue =
                                        (unsigned int) currentTime;
                                if (verbose >= VERBOSE_2) {
                                        fprintf(out_logfile,
                                                "Using Time Stamp %u (0x%x) for Data Signature\n",
                                                params->timeStampSignatureValue,
                                                params->timeStampSignatureValue);
                                }
                        }
                        if (rep == 0 && verbose >= VERBOSE_0) {
                                PrintTableHeader();
                        }
                }
                MPI_CHECK(MPI_Bcast
                          (&params->timeStampSignatureValue, 1, MPI_UNSIGNED, 0,
                           testComm), "cannot broadcast start time value");

                FillBuffer(ioBuffers.buffer, params, 0, pretendRank);
                /* use repetition count for number of multiple files */
                if (params->multiFile)
                        params->repCounter = rep;

                /*
                 * write the file(s), getting timing between I/O calls
                 */

                if (params->writeFile && !test_time_elapsed(params, startTime)) {
                        GetTestFileName(testFileName, params);
                        if (verbose >= VERBOSE_3) {
                                fprintf(out_logfile, "task %d writing %s\n", rank,
                                        testFileName);
                        }
                        DelaySecs(params->interTestDelay);
                        if (params->useExistingTestFile == FALSE) {
                                RemoveFile(testFileName, params->filePerProc,
                                           params);
                        }

                        params->stoneWallingWearOutIterations = params_saved_wearout;
                        MPI_CHECK(MPI_Barrier(testComm), "barrier error");
                        params->open = WRITE;
                        timer[0] = GetTimeStamp();
                        fd = backend->create(testFileName, params);
                        timer[1] = GetTimeStamp();
                        if (params->intraTestBarriers)
                                MPI_CHECK(MPI_Barrier(testComm),
                                          "barrier error");
                        if (rank == 0 && verbose >= VERBOSE_1) {
                                fprintf(out_logfile,
                                        "Commencing write performance test: %s",
                                        CurrentTimeString());
                        }
                        timer[2] = GetTimeStamp();
                        dataMoved = WriteOrRead(params, &results[rep], fd, WRITE, &ioBuffers);
                        if (params->verbose >= VERBOSE_4) {
                          fprintf(out_logfile, "* data moved = %llu\n", dataMoved);
                          fflush(out_logfile);
                        }
                        timer[3] = GetTimeStamp();
                        if (params->intraTestBarriers)
                                MPI_CHECK(MPI_Barrier(testComm),
                                          "barrier error");
                        timer[4] = GetTimeStamp();
                        backend->close(fd, params);

                        timer[5] = GetTimeStamp();
                        MPI_CHECK(MPI_Barrier(testComm), "barrier error");

                        /* get the size of the file just written */
                        results[rep].write.aggFileSizeFromStat =
                                backend->get_file_size(params, testComm, testFileName);

                        /* check if stat() of file doesn't equal expected file size,
                           use actual amount of byte moved */
                        CheckFileSize(test, dataMoved, rep, WRITE);

                        if (verbose >= VERBOSE_3)
                                WriteTimes(params, timer, rep, WRITE);
                        ReduceIterResults(test, timer, rep, WRITE);
                        if (params->outlierThreshold) {
                                CheckForOutliers(params, timer, WRITE);
                        }

                        /* check if in this round we run write with stonewalling */
                        if(params->deadlineForStonewalling > 0){
                          params->stoneWallingWearOutIterations = results[rep].write.pairs_accessed;
                        }
                }

                /*
                 * perform a check of data, reading back data and comparing
                 * against what was expected to be written
                 */
                if (params->checkWrite && !test_time_elapsed(params, startTime)) {
                        MPI_CHECK(MPI_Barrier(testComm), "barrier error");
                        if (rank == 0 && verbose >= VERBOSE_1) {
                                fprintf(out_logfile,
                                        "Verifying contents of the file(s) just written.\n");
                                fprintf(out_logfile, "%s\n", CurrentTimeString());
                        }
                        if (params->reorderTasks) {
                                /* move two nodes away from writing node */
                                int shift = 1; /* assume a by-node (round-robin) mapping of tasks to nodes */
                                if (params->tasksBlockMapping) {
                                    shift = params->numTasksOnNode0; /* switch to by-slot (contiguous block) mapping */
                                }
                                rankOffset = (2 * shift) % params->numTasks;
                        }

                        // update the check buffer
                        FillBuffer(ioBuffers.readCheckBuffer, params, 0, (rank + rankOffset) % params->numTasks);

                        reseed_incompressible_prng = TRUE; /* Re-Seed the PRNG to get same sequence back, if random */

                        GetTestFileName(testFileName, params);
                        params->open = WRITECHECK;
                        fd = backend->open(testFileName, params);
                        dataMoved = WriteOrRead(params, &results[rep], fd, WRITECHECK, &ioBuffers);
                        backend->close(fd, params);
                        rankOffset = 0;
                }
                /*
                 * read the file(s), getting timing between I/O calls
                 */
                if ((params->readFile || params->checkRead ) && !test_time_elapsed(params, startTime)) {
                        /* check for stonewall */
                        if(params->stoneWallingStatusFile){
                          params->stoneWallingWearOutIterations = ReadStoneWallingIterations(params->stoneWallingStatusFile);
                          if(params->stoneWallingWearOutIterations == -1 && rank == 0){
                            fprintf(out_logfile, "WARNING: Could not read back the stonewalling status from the file!\n");
                            params->stoneWallingWearOutIterations = 0;
                          }
                        }
                        int operation_flag = READ;
                        if ( params->checkRead ){
                          // actually read and then compare the buffer
                          operation_flag = READCHECK;
                        }
                        /* Get rankOffset [file offset] for this process to read, based on -C,-Z,-Q,-X options */
                        /* Constant process offset reading */
                        if (params->reorderTasks) {
                                /* move one node away from writing node */
                                int shift = 1; /* assume a by-node (round-robin) mapping of tasks to nodes */
                                if (params->tasksBlockMapping) {
                                    shift=params->numTasksOnNode0; /* switch to a by-slot (contiguous block) mapping */
                                }
                                rankOffset = (params->taskPerNodeOffset * shift) % params->numTasks;
                        }
                        /* random process offset reading */
                        if (params->reorderTasksRandom) {
                                /* this should not intefere with randomOffset within a file because GetOffsetArrayRandom */
                                /* seeds every rand() call  */
                                int nodeoffset;
                                unsigned int iseed0;
                                nodeoffset = params->taskPerNodeOffset;
                                nodeoffset = (nodeoffset < params->numNodes) ? nodeoffset : params->numNodes - 1;
                                if (params->reorderTasksRandomSeed < 0)
                                        iseed0 = -1 * params->reorderTasksRandomSeed + rep;
                                else
                                        iseed0 = params->reorderTasksRandomSeed;
                                srand(rank + iseed0);
                                {
                                        rankOffset = rand() % params->numTasks;
                                }
                                while (rankOffset <
                                       (nodeoffset * params->numTasksOnNode0)) {
                                        rankOffset = rand() % params->numTasks;
                                }
                                /* Get more detailed stats if requested by verbose level */
                                if (verbose >= VERBOSE_2) {
                                        file_hits_histogram(params);
                                }
                        }
                        if(operation_flag == READCHECK){
                            FillBuffer(ioBuffers.readCheckBuffer, params, 0, (rank + rankOffset) % params->numTasks);
                        }

                        /* Using globally passed rankOffset, following function generates testFileName to read */
                        GetTestFileName(testFileName, params);

                        if (verbose >= VERBOSE_3) {
                                fprintf(out_logfile, "task %d reading %s\n", rank,
                                        testFileName);
                        }
                        DelaySecs(params->interTestDelay);
                        MPI_CHECK(MPI_Barrier(testComm), "barrier error");
                        params->open = READ;
                        timer[0] = GetTimeStamp();
                        fd = backend->open(testFileName, params);
                        timer[1] = GetTimeStamp();
                        if (params->intraTestBarriers)
                                MPI_CHECK(MPI_Barrier(testComm),
                                          "barrier error");
                        if (rank == 0 && verbose >= VERBOSE_1) {
                                fprintf(out_logfile,
                                        "Commencing read performance test: %s\n",
                                        CurrentTimeString());
                        }
                        timer[2] = GetTimeStamp();
                        dataMoved = WriteOrRead(params, &results[rep], fd, operation_flag, &ioBuffers);
                        timer[3] = GetTimeStamp();
                        if (params->intraTestBarriers)
                                MPI_CHECK(MPI_Barrier(testComm),
                                          "barrier error");
                        timer[4] = GetTimeStamp();
                        backend->close(fd, params);
                        timer[5] = GetTimeStamp();

                        /* get the size of the file just read */
                        results[rep].read.aggFileSizeFromStat =
                                backend->get_file_size(params, testComm,
                                                       testFileName);

                        /* check if stat() of file doesn't equal expected file size,
                           use actual amount of byte moved */
                        CheckFileSize(test, dataMoved, rep, READ);

                        if (verbose >= VERBOSE_3)
                                WriteTimes(params, timer, rep, READ);
                        ReduceIterResults(test, timer, rep, READ);
                        if (params->outlierThreshold) {
                                CheckForOutliers(params, timer, READ);
                        }
                }

                if (!params->keepFile
                    && !(params->errorFound && params->keepFileWithError)) {
                        double start, finish;
                        start = GetTimeStamp();
                        MPI_CHECK(MPI_Barrier(testComm), "barrier error");
                        RemoveFile(testFileName, params->filePerProc, params);
                        MPI_CHECK(MPI_Barrier(testComm), "barrier error");
                        finish = GetTimeStamp();
                        PrintRemoveTiming(start, finish, rep);
                } else {
                        MPI_CHECK(MPI_Barrier(testComm), "barrier error");
                }
                params->errorFound = FALSE;
                rankOffset = 0;

                PrintRepeatEnd();
        }

        MPI_CHECK(MPI_Comm_free(&testComm), "MPI_Comm_free() error");

        if (params->summary_every_test) {
                PrintLongSummaryHeader();
                PrintLongSummaryOneTest(test);
        } else {
                PrintShortSummary(test);
        }

        XferBuffersFree(&ioBuffers, params);

        if (hog_buf != NULL)
                free(hog_buf);

        /* Sync with the tasks that did not participate in this test */
        MPI_CHECK(MPI_Barrier(mpi_comm_world), "barrier error");

}

/*
 * Determine if valid tests from parameters.
 */
static void ValidateTests(IOR_param_t * test)
{
        IOR_param_t defaults;
        init_IOR_Param_t(&defaults);

        if (test->repetitions <= 0)
                WARN_RESET("too few test repetitions",
                           test, &defaults, repetitions);
        if (test->numTasks <= 0)
                ERR("too few tasks for testing");
        if (test->interTestDelay < 0)
                WARN_RESET("inter-test delay must be nonnegative value",
                           test, &defaults, interTestDelay);
        if (test->readFile != TRUE && test->writeFile != TRUE
            && test->checkRead != TRUE && test->checkWrite != TRUE)
                ERR("test must write, read, or check read/write file");
        if(! test->setTimeStampSignature && test->writeFile != TRUE && test->checkRead == TRUE)
                ERR("using readCheck only requires to write a timeStampSignature -- use -G");
        if (test->segmentCount < 0)
                ERR("segment count must be positive value");
        if ((test->blockSize % sizeof(IOR_size_t)) != 0)
                ERR("block size must be a multiple of access size");
        if (test->blockSize < 0)
                ERR("block size must be non-negative integer");
        if ((test->transferSize % sizeof(IOR_size_t)) != 0)
                ERR("transfer size must be a multiple of access size");
        if (test->setAlignment < 0)
                ERR("alignment must be non-negative integer");
        if (test->transferSize < 0)
                ERR("transfer size must be non-negative integer");
        if (test->transferSize == 0) {
                ERR("test will not complete with zero transfer size");
        } else {
                if ((test->blockSize % test->transferSize) != 0)
                        ERR("block size must be a multiple of transfer size");
        }
        if (test->blockSize < test->transferSize)
                ERR("block size must not be smaller than transfer size");

        /* specific APIs */
        if ((strcasecmp(test->api, "MPIIO") == 0)
            && (test->blockSize < sizeof(IOR_size_t)
                || test->transferSize < sizeof(IOR_size_t)))
                ERR("block/transfer size may not be smaller than IOR_size_t for MPIIO");
        if ((strcasecmp(test->api, "HDF5") == 0)
            && (test->blockSize < sizeof(IOR_size_t)
                || test->transferSize < sizeof(IOR_size_t)))
                ERR("block/transfer size may not be smaller than IOR_size_t for HDF5");
        if ((strcasecmp(test->api, "NCMPI") == 0)
            && (test->blockSize < sizeof(IOR_size_t)
                || test->transferSize < sizeof(IOR_size_t)))
                ERR("block/transfer size may not be smaller than IOR_size_t for NCMPI");
        if ((test->useFileView == TRUE)
            && (sizeof(MPI_Aint) < 8)   /* used for 64-bit datatypes */
            &&((test->numTasks * test->blockSize) >
               (2 * (IOR_offset_t) GIBIBYTE)))
                ERR("segment size must be < 2GiB");
        if ((strcasecmp(test->api, "POSIX") != 0) && test->singleXferAttempt)
                WARN_RESET("retry only available in POSIX",
                           test, &defaults, singleXferAttempt);
        if (((strcasecmp(test->api, "POSIX") != 0)
            && (strcasecmp(test->api, "MPIIO") != 0)
            && (strcasecmp(test->api, "MMAP") != 0)
            && (strcasecmp(test->api, "HDFS") != 0)
            && (strcasecmp(test->api, "DFS") != 0)
            && (strcasecmp(test->api, "Gfarm") != 0)
            && (strcasecmp(test->api, "RADOS") != 0)
            && (strcasecmp(test->api, "CEPHFS") != 0)) && test->fsync)
                WARN_RESET("fsync() not supported in selected backend",
                           test, &defaults, fsync);
        if ((strcasecmp(test->api, "MPIIO") != 0) && test->preallocate)
                WARN_RESET("preallocation only available in MPIIO",
                           test, &defaults, preallocate);
        if ((strcasecmp(test->api, "MPIIO") != 0) && test->useFileView)
                WARN_RESET("file view only available in MPIIO",
                           test, &defaults, useFileView);
        if ((strcasecmp(test->api, "MPIIO") != 0) && test->useSharedFilePointer)
                WARN_RESET("shared file pointer only available in MPIIO",
                           test, &defaults, useSharedFilePointer);
        if ((strcasecmp(test->api, "MPIIO") == 0) && test->useSharedFilePointer)
                WARN_RESET("shared file pointer not implemented",
                           test, &defaults, useSharedFilePointer);
        if ((strcasecmp(test->api, "MPIIO") != 0) && test->useStridedDatatype)
                WARN_RESET("strided datatype only available in MPIIO",
                           test, &defaults, useStridedDatatype);
        if ((strcasecmp(test->api, "MPIIO") == 0) && test->useStridedDatatype)
                WARN_RESET("strided datatype not implemented",
                           test, &defaults, useStridedDatatype);
        if ((strcasecmp(test->api, "MPIIO") == 0)
            && test->useStridedDatatype && (test->blockSize < sizeof(IOR_size_t)
                                            || test->transferSize <
                                            sizeof(IOR_size_t)))
                ERR("need larger file size for strided datatype in MPIIO");
        if ((strcasecmp(test->api, "POSIX") == 0) && test->showHints)
                WARN_RESET("hints not available in POSIX",
                           test, &defaults, showHints);
        if ((strcasecmp(test->api, "POSIX") == 0) && test->collective)
                WARN_RESET("collective not available in POSIX",
                           test, &defaults, collective);
        if ((strcasecmp(test->api, "MMAP") == 0) && test->fsyncPerWrite
            && (test->transferSize & (sysconf(_SC_PAGESIZE) - 1)))
                ERR("transfer size must be aligned with PAGESIZE for MMAP with fsyncPerWrite");

        /* parameter consitency */
        if (test->reorderTasks == TRUE && test->reorderTasksRandom == TRUE)
                ERR("Both Constant and Random task re-ordering specified. Choose one and resubmit");
        if (test->randomOffset && test->reorderTasksRandom
            && test->filePerProc == FALSE)
                ERR("random offset and random reorder tasks specified with single-shared-file. Choose one and resubmit");
        if (test->randomOffset && test->reorderTasks
            && test->filePerProc == FALSE)
                ERR("random offset and constant reorder tasks specified with single-shared-file. Choose one and resubmit");
        if (test->randomOffset && test->checkRead)
                ERR("random offset not available with read check option (use write check)");
        if (test->randomOffset && test->storeFileOffset)
                ERR("random offset not available with store file offset option)");


        if ((strcasecmp(test->api, "MPIIO") == 0) && test->randomOffset
            && test->collective)
                ERR("random offset not available with collective MPIIO");
        if ((strcasecmp(test->api, "MPIIO") == 0) && test->randomOffset
            && test->useFileView)
                ERR("random offset not available with MPIIO fileviews");
        if ((strcasecmp(test->api, "HDF5") == 0) && test->randomOffset)
                ERR("random offset not available with HDF5");
        if ((strcasecmp(test->api, "NCMPI") == 0) && test->randomOffset)
                ERR("random offset not available with NCMPI");
        if ((strcasecmp(test->api, "HDF5") != 0) && test->individualDataSets)
                WARN_RESET("individual datasets only available in HDF5",
                           test, &defaults, individualDataSets);
        if ((strcasecmp(test->api, "HDF5") == 0) && test->individualDataSets)
                WARN_RESET("individual data sets not implemented",
                           test, &defaults, individualDataSets);
        if ((strcasecmp(test->api, "NCMPI") == 0) && test->filePerProc)
                ERR("file-per-proc not available in current NCMPI");
        if (test->noFill) {
                if (strcasecmp(test->api, "HDF5") != 0) {
                        ERR("'no fill' option only available in HDF5");
                } else {
                        /* check if hdf5 available */
#if defined (H5_VERS_MAJOR) && defined (H5_VERS_MINOR)
                        /* no-fill option not available until hdf5-1.6.x */
#if (H5_VERS_MAJOR > 0 && H5_VERS_MINOR > 5)
                        ;
#else
                        ERRF("'no fill' option not available in %s",
                                test->apiVersion);
#endif
#else
                        WARN("unable to determine HDF5 version for 'no fill' usage");
#endif
                }
        }
        if (test->useExistingTestFile && test->lustre_set_striping)
                ERR("Lustre stripe options are incompatible with useExistingTestFile");

        /* allow the backend to validate the options */
        if(test->backend->check_params){
          int check = test->backend->check_params(test);
          if (check == 0){
            ERR("The backend returned that the test parameters are invalid.");
          }
        }
}

/**
 * Returns a precomputed array of IOR_offset_t for the inner benchmark loop.
 * They are sequential and the last element is set to -1 as end marker.
 * @param test IOR_param_t for getting transferSize, blocksize and SegmentCount
 * @param pretendRank int pretended Rank for shifting the offsest corectly
 * @return IOR_offset_t
 */
IOR_offset_t *GetOffsetArraySequential(IOR_param_t * test, int pretendRank)
{
        IOR_offset_t i, j, k = 0;
        IOR_offset_t offsets;
        IOR_offset_t *offsetArray;

        /* count needed offsets */
        offsets = (test->blockSize / test->transferSize) * test->segmentCount;

        /* setup empty array */
        offsetArray =
                (IOR_offset_t *) malloc((offsets + 1) * sizeof(IOR_offset_t));
        if (offsetArray == NULL)
                ERR("malloc() failed");
        offsetArray[offsets] = -1;      /* set last offset with -1 */

        /* fill with offsets */
        for (i = 0; i < test->segmentCount; i++) {
                for (j = 0; j < (test->blockSize / test->transferSize); j++) {
                        offsetArray[k] = j * test->transferSize;
                        if (test->filePerProc) {
                                offsetArray[k] += i * test->blockSize;
                        } else {
                                offsetArray[k] +=
                                        (i * test->numTasks * test->blockSize)
                                        + (pretendRank * test->blockSize);
                        }
                        k++;
                }
        }

        return (offsetArray);
}

/**
 * Returns a precomputed array of IOR_offset_t for the inner benchmark loop.
 * They get created sequentially and mixed up in the end. The last array element
 * is set to -1 as end marker.
 * It should be noted that as the seeds get synchronised across all processes
 * every process computes the same random order if used with filePerProc.
 * For a shared file all transfers get randomly assigned to ranks. The processes
 * can also have differen't numbers of transfers. This might lead to a bigger
 * diversion in accesse as it dose with filePerProc. This is expected but
 * should be mined.
 * @param test IOR_param_t for getting transferSize, blocksize and SegmentCount
 * @param pretendRank int pretended Rank for shifting the offsest corectly
 * @return IOR_offset_t
 * @return
 */
IOR_offset_t *GetOffsetArrayRandom(IOR_param_t * test, int pretendRank, int access)
{
        int seed;
        IOR_offset_t i, value, tmp;
        IOR_offset_t offsets = 0;
        IOR_offset_t offsetCnt = 0;
        IOR_offset_t fileSize;
        IOR_offset_t *offsetArray;

        /* set up seed for random() */
        if (access == WRITE || access == READ) {
                test->randomSeed = seed = rand();
        } else {
                seed = test->randomSeed;
        }
        srand(seed);

        fileSize = test->blockSize * test->segmentCount;
        if (test->filePerProc == FALSE) {
                fileSize *= test->numTasks;
        }

        /* count needed offsets (pass 1) */
        for (i = 0; i < fileSize; i += test->transferSize) {
                if (test->filePerProc == FALSE) {
                        // this counts which process get how many transferes in
                        // a shared file
                        if ((rand() % test->numTasks) == pretendRank) {
                                offsets++;
                        }
                } else {
                        offsets++;
                }
        }

        /* setup empty array */
        offsetArray =
                (IOR_offset_t *) malloc((offsets + 1) * sizeof(IOR_offset_t));
        if (offsetArray == NULL)
                ERR("malloc() failed");
        offsetArray[offsets] = -1;      /* set last offset with -1 */

        if (test->filePerProc) {
                /* fill array */
                for (i = 0; i < offsets; i++) {
                        offsetArray[i] = i * test->transferSize;
                }
        } else {
                /* fill with offsets (pass 2) */
                srand(seed);  /* need same seed  to get same transfers as counted in the beginning*/
                for (i = 0; i < fileSize; i += test->transferSize) {
                        if ((rand() % test->numTasks) == pretendRank) {
                                offsetArray[offsetCnt] = i;
                                offsetCnt++;
                        }
                }
        }
        /* reorder array */
        for (i = 0; i < offsets; i++) {
                value = rand() % offsets;
                tmp = offsetArray[value];
                offsetArray[value] = offsetArray[i];
                offsetArray[i] = tmp;
        }
        SeedRandGen(test->testComm);    /* synchronize seeds across tasks */

        return (offsetArray);
}

static IOR_offset_t WriteOrReadSingle(IOR_offset_t pairCnt, IOR_offset_t *offsetArray, int pretendRank,
  IOR_offset_t * transferCount, int * errors, IOR_param_t * test, int * fd, IOR_io_buffers* ioBuffers, int access){
  IOR_offset_t amtXferred = 0;
  IOR_offset_t transfer;

  void *buffer = ioBuffers->buffer;
  void *checkBuffer = ioBuffers->checkBuffer;
  void *readCheckBuffer = ioBuffers->readCheckBuffer;

  test->offset = offsetArray[pairCnt];

  transfer = test->transferSize;
  if (access == WRITE) {
          /* fills each transfer with a unique pattern
           * containing the offset into the file */
          if (test->storeFileOffset == TRUE) {
                  FillBuffer(buffer, test, test->offset, pretendRank);
          }
          amtXferred =
                  backend->xfer(access, fd, buffer, transfer, test);
          if (amtXferred != transfer)
                  ERR("cannot write to file");
          if (test->interIODelay > 0){
            struct timespec wait = {test->interIODelay / 1000 / 1000, 1000l * (test->interIODelay % 1000000)};
            nanosleep( & wait, NULL);
          }
  } else if (access == READ) {
          amtXferred =
                  backend->xfer(access, fd, buffer, transfer, test);
          if (amtXferred != transfer)
                  ERR("cannot read from file");
          if (test->interIODelay > 0){
            struct timespec wait = {test->interIODelay / 1000 / 1000, 1000l * (test->interIODelay % 1000000)};
            nanosleep( & wait, NULL);
          }
  } else if (access == WRITECHECK) {
          memset(checkBuffer, 'a', transfer);

          if (test->storeFileOffset == TRUE) {
                  FillBuffer(readCheckBuffer, test, test->offset, pretendRank);
          }

          amtXferred = backend->xfer(access, fd, checkBuffer, transfer, test);
          if (amtXferred != transfer)
                  ERR("cannot read from file write check");
          (*transferCount)++;
          *errors += CompareBuffers(readCheckBuffer, checkBuffer, transfer,
                                   *transferCount, test,
                                   WRITECHECK);
  } else if (access == READCHECK) {
          memset(checkBuffer, 'a', transfer);

          amtXferred = backend->xfer(access, fd, checkBuffer, transfer, test);
          if (amtXferred != transfer){
            ERR("cannot read from file");
          }
          if (test->storeFileOffset == TRUE) {
                  FillBuffer(readCheckBuffer, test, test->offset, pretendRank);
          }
          *errors += CompareBuffers(readCheckBuffer, checkBuffer, transfer, *transferCount, test, READCHECK);
  }
  return amtXferred;
}

/*
 * Write or Read data to file(s).  This loops through the strides, writing
 * out the data to each block in transfer sizes, until the remainder left is 0.
 */
static IOR_offset_t WriteOrRead(IOR_param_t *test, IOR_results_t *results,
                                void *fd, const int access, IOR_io_buffers *ioBuffers)
{
        int errors = 0;
        IOR_offset_t transferCount = 0;
        uint64_t pairCnt = 0;
        IOR_offset_t *offsetArray;
        int pretendRank;
        IOR_offset_t dataMoved = 0;     /* for data rate calculation */
        double startForStonewall;
        int hitStonewall;
        IOR_point_t *point = ((access == WRITE) || (access == WRITECHECK)) ?
                             &results->write : &results->read;

        /* initialize values */
        pretendRank = (rank + rankOffset) % test->numTasks;

        if (test->randomOffset) {
                offsetArray = GetOffsetArrayRandom(test, pretendRank, access);
        } else {
                offsetArray = GetOffsetArraySequential(test, pretendRank);
        }

        startForStonewall = GetTimeStamp();
        hitStonewall = 0;

        /* loop over offsets to access */
        while ((offsetArray[pairCnt] != -1) && !hitStonewall ) {
                dataMoved += WriteOrReadSingle(pairCnt, offsetArray, pretendRank, & transferCount, & errors, test, fd, ioBuffers, access);
                pairCnt++;

                hitStonewall = ((test->deadlineForStonewalling != 0
                                && (GetTimeStamp() - startForStonewall)
                                    > test->deadlineForStonewalling)) || (test->stoneWallingWearOutIterations != 0 && pairCnt == test->stoneWallingWearOutIterations) ;

                if ( test->collective && test->deadlineForStonewalling ) {
                        // if collective-mode, you'll get a HANG, if some rank 'accidentally' leave this loop
                        // it absolutely must be an 'all or none':
                        MPI_CHECK(MPI_Bcast(&hitStonewall, 1, MPI_INT, 0, MPI_COMM_WORLD), "hitStonewall broadcast failed");
               }

        }
        if (test->stoneWallingWearOut){
          if (verbose >= VERBOSE_1){
            fprintf(out_logfile, "%d: stonewalling pairs accessed: %lld\n", rank, (long long) pairCnt);
          }
          long long data_moved_ll = (long long) dataMoved;
          long long pairs_accessed_min = 0;
          MPI_CHECK(MPI_Allreduce(& pairCnt, &point->pairs_accessed,
                                  1, MPI_LONG_LONG_INT, MPI_MAX, testComm), "cannot reduce pairs moved");
          double stonewall_runtime = GetTimeStamp() - startForStonewall;
          point->stonewall_time = stonewall_runtime;
          MPI_CHECK(MPI_Reduce(& pairCnt, & pairs_accessed_min,
                                  1, MPI_LONG_LONG_INT, MPI_MIN, 0, testComm), "cannot reduce pairs moved");
          MPI_CHECK(MPI_Reduce(& data_moved_ll, &point->stonewall_min_data_accessed,
                                  1, MPI_LONG_LONG_INT, MPI_MIN, 0, testComm), "cannot reduce pairs moved");
          MPI_CHECK(MPI_Reduce(& data_moved_ll, &point->stonewall_avg_data_accessed,
                                  1, MPI_LONG_LONG_INT, MPI_SUM, 0, testComm), "cannot reduce pairs moved");

          if(rank == 0){
            fprintf(out_logfile, "stonewalling pairs accessed min: %lld max: %zu -- min data: %.1f GiB mean data: %.1f GiB time: %.1fs\n",
             pairs_accessed_min, point->pairs_accessed,
             point->stonewall_min_data_accessed /1024.0 / 1024 / 1024, point->stonewall_avg_data_accessed / 1024.0 / 1024 / 1024 / test->numTasks , point->stonewall_time);
             point->stonewall_min_data_accessed *= test->numTasks;
          }
          if(pairCnt != point->pairs_accessed){
            // some work needs still to be done !
            for(; pairCnt < point->pairs_accessed; pairCnt++ ) {
                    dataMoved += WriteOrReadSingle(pairCnt, offsetArray, pretendRank, & transferCount, & errors, test, fd, ioBuffers, access);
            }
          }
        }else{
          point->pairs_accessed = pairCnt;
        }


        totalErrorCount += CountErrors(test, access, errors);

        free(offsetArray);

        if (access == WRITE && test->fsync == TRUE) {
                backend->fsync(fd, test);       /*fsync after all accesses */
        }
        return (dataMoved);
}

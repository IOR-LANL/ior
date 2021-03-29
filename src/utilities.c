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
* Additional utilities
*
\******************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef HAVE_GETCPU_SYSCALL
#  define _GNU_SOURCE
#  include <unistd.h>
#  include <sys/syscall.h>
#endif

#ifdef __linux__
#  define _GNU_SOURCE            /* Needed for O_DIRECT in fcntl */
#endif                           /* __linux__ */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>               /* pow() */
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef HAVE_CUDA
#include <cuda_runtime.h>
#endif

#ifndef _WIN32
#  include <regex.h>
#  ifdef __sun                    /* SunOS does not support statfs(), instead uses statvfs() */
#    include <sys/statvfs.h>
#  elif (defined __APPLE__)
#    include <sys/param.h>
#    include <sys/mount.h>
#  else                           /* ! __sun  or __APPLE__ */
#    include <sys/statfs.h>
#  endif                          /* __sun */
#  include <sys/time.h>           /* gettimeofday() */
#endif

#include "utilities.h"
#include "aiori.h"
#include "ior.h"
#include "ior-internal.h"

#define RANDALGO_GOLDEN_RATIO_PRIME        0x9e37fffffffc0001UL

/************************** D E C L A R A T I O N S ***************************/

extern int errno;
extern int numTasks;

/* globals used by other files, also defined "extern" in utilities.h */
int      rank = 0;
int      rankOffset = 0;
int      verbose = VERBOSE_0;        /* verbose output */
MPI_Comm testComm;
FILE * out_logfile = NULL;
FILE * out_resultfile = NULL;
enum OutputFormat_t outputFormat;

/* local */
int rand_state_init = 0;
uint64_t rand_state = 0;

/***************************** F U N C T I O N S ******************************/

/**
 * Modifies a buffer for a write.  Performance sensitive because it is called
 * before each write.
 *
 * @param buf pointer to byte buffer to fill
 * @param bytes number of bytes to produce to fill buffer
 * @param rand_seed seed to use for PRNG
 * @param pretendRank unique identifier for this process
 * @param dataPacketType identifier to designate pattern to fill buffer
 */
void update_write_memory_pattern(uint64_t item, char * buf, size_t bytes, int rand_seed, int pretendRank, ior_dataPacketType_e dataPacketType){
  if (dataPacketType == DATA_TIMESTAMP || bytes < 8)
    return;

  size_t size = bytes / sizeof(uint64_t);
  uint64_t * buffi = (uint64_t*) buf;

  if (dataPacketType == DATA_RANDOM) {
      if (!rand_state_init) {
          unsigned seed = rand_seed + pretendRank;
          rand_state_init = 1;
          rand_state = rand_r(&seed);
      }
      for (size_t i = 0; i < size; i++) {
          rand_state *= RANDALGO_GOLDEN_RATIO_PRIME;
          rand_state >>= 3;
          buffi[i] = rand_state;
      }
      return;
  }

  /* DATA_INCOMPRESSIBLE and DATA_OFFSET */
  int k = 1;
  for(size_t i=0; i < size; i+=512, k++){
    buffi[i] = ((uint32_t) item * k) | ((uint64_t) pretendRank) << 32;
  }
}

/**
 * Fills a buffer with bytes of a given pattern.  Not performance-sensitive
 * because it is called once per test.
 *
 * @param buf pointer to byte buffer to fill
 * @param bytes number of bytes to produce to fill buffer
 * @param rand_seed seed to use for PRNG
 * @param pretendRank unique identifier for this process
 * @param dataPacketType identifier to designate pattern to fill buffer
 */
void generate_memory_pattern(char * buf, size_t bytes, int rand_seed, int pretendRank, ior_dataPacketType_e dataPacketType){
  uint64_t * buffi = (uint64_t*) buf;
  // first half of 64 bits use the rank
  const size_t size = bytes / 8;
  // the first 8 bytes of each 4k block are updated at runtime
  unsigned seed = rand_seed + pretendRank;
  if (dataPacketType == DATA_RANDOM && !rand_state_init) {
      rand_state_init = 1;
      rand_state = rand_r(&seed);
  }
  for(size_t i=0; i < size; i++){
    switch(dataPacketType){
      case(DATA_RANDOM):
        rand_state *= RANDALGO_GOLDEN_RATIO_PRIME;
        rand_state >>= 3;
        buffi[i] = rand_state;
        break;
      case(DATA_INCOMPRESSIBLE):{
        uint64_t hi = ((uint64_t) rand_r(& seed) << 32);
        uint64_t lo = (uint64_t) rand_r(& seed);
        buffi[i] = hi | lo;
        break;
      }case(DATA_OFFSET):{
      }case(DATA_TIMESTAMP):{
        buffi[i] = ((uint64_t) pretendRank) << 32 | rand_seed + i;
        break;
      }
    }
  }
  
  for(size_t i=size*8; i < bytes; i++){
    buf[i] = (char) i;
  }
}

int verify_memory_pattern(uint64_t item, char * buffer, size_t bytes, int rand_seed, int pretendRank, ior_dataPacketType_e dataPacketType){
  int error = 0;
  // always read all data to ensure that performance numbers stay the same
  uint64_t * buffi = (uint64_t*) buffer;
  
  // the first 8 bytes are set to item number
  int k=1;  
  unsigned seed = rand_seed + pretendRank;
  const size_t size = bytes / 8;
  for(size_t i=0; i < size; i++){
    uint64_t exp;
        
    switch(dataPacketType){
      case(DATA_RANDOM):
        rand_state *= RANDALGO_GOLDEN_RATIO_PRIME;
        rand_state >>= 3;
        buffi[i] = rand_state;
        break;
      case(DATA_INCOMPRESSIBLE):{
        uint64_t hi = ((uint64_t) rand_r(& seed) << 32);
        uint64_t lo = (uint64_t) rand_r(& seed);
        exp = hi | lo;
        break;
      }case(DATA_OFFSET):{
      }case(DATA_TIMESTAMP):{
        exp = ((uint64_t) pretendRank) << 32 | rand_seed + i;
        break;
      }
    }
    if(i % 512 == 0 && dataPacketType != DATA_TIMESTAMP){
      exp = ((uint32_t) item * k) | ((uint64_t) pretendRank) << 32;
      k++;
    }
    if(buffi[i] != exp){
      error = 1;
    }
  }
  for(size_t i=size*8; i < bytes; i++){
    if(buffer[i] != (char) i){
      error = 1;
    }
  }
  return error;
}

void* safeMalloc(uint64_t size){
  void * d = malloc(size);
  if (d == NULL){
    ERR("Could not malloc an array");
  }
  memset(d, 0, size);
  return d;
}

void FailMessage(int rank, const char *location, char *format, ...) {
    char msg[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, 4096, format, args);
    va_end(args);
    fprintf(out_logfile, "%s: Process %d: FAILED in %s, %s\n",
                PrintTimestamp(), rank, location, msg);
    fflush(out_logfile);
    MPI_Abort(testComm, 1);
}

size_t NodeMemoryStringToBytes(char *size_str)
{
        int percent;
        int rc;
        long page_size;
        long num_pages;
        long long mem;

        rc = sscanf(size_str, " %d %% ", &percent);
        if (rc == 0)
                return (size_t) string_to_bytes(size_str);
        if (percent > 100 || percent < 0)
                ERR("percentage must be between 0 and 100");

#ifdef HAVE_SYSCONF
        page_size = sysconf(_SC_PAGESIZE);
#else
        page_size = getpagesize();
#endif

#ifdef  _SC_PHYS_PAGES
        num_pages = sysconf(_SC_PHYS_PAGES);
        if (num_pages == -1)
                ERR("sysconf(_SC_PHYS_PAGES) is not supported");
#else
        ERR("sysconf(_SC_PHYS_PAGES) is not supported");
#endif
        mem = page_size * num_pages;

        return mem / 100 * percent;
}

ior_dataPacketType_e parsePacketType(char t){
    switch(t) {
    case '\0': return DATA_TIMESTAMP;
    case 'i': /* Incompressible */
            return DATA_INCOMPRESSIBLE;
    case 't': /* timestamp */
            return DATA_TIMESTAMP;
    case 'o': /* offset packet */
            return DATA_OFFSET;
    case 'r': /* randomized blocks */
            return DATA_RANDOM;
    default:
      ERRF("Unknown packet type \"%c\"; generic assumed\n", t);
      return DATA_OFFSET;
    }
}

void updateParsedOptions(IOR_param_t * options, options_all_t * global_options){
    if (options->setTimeStampSignature){
      options->incompressibleSeed = options->setTimeStampSignature;
    }

    if (options->buffer_type && options->buffer_type[0] != 0){
      options->dataPacketType = parsePacketType(options->buffer_type[0]);
    }
    if (options->memoryPerNodeStr){
      options->memoryPerNode = NodeMemoryStringToBytes(options->memoryPerNodeStr);
    }
    const ior_aiori_t * backend = aiori_select(options->api);
    if (backend == NULL)
        ERR("Unrecognized I/O API");

    options->backend = backend;
    /* copy the actual module options into the test */
    options->backend_options = airoi_update_module_options(backend, global_options);
    options->apiVersion = backend->get_version();
}

/* Used in aiori-POSIX.c and aiori-PLFS.c
 */

void set_o_direct_flag(int *flag)
{
/* note that TRU64 needs O_DIRECTIO, SunOS uses directio(),
   and everyone else needs O_DIRECT */
#ifndef O_DIRECT
#  ifndef O_DIRECTIO
     WARN("cannot use O_DIRECT");
#    define O_DIRECT 000000
#  else                           /* O_DIRECTIO */
#    define O_DIRECT O_DIRECTIO
#  endif                          /* not O_DIRECTIO */
#endif                            /* not O_DIRECT */

        *flag |= O_DIRECT;
}


/*
 * Returns string containing the current time.
 *
 * NOTE: On some systems, MPI jobs hang while ctime() waits for a lock.
 * This is true even though CurrentTimeString() is only called for rank==0.
 * ctime_r() fixes this.
 */
char *CurrentTimeString(void)
{
        static time_t currentTime;
        char*         currentTimePtr;

        if ((currentTime = time(NULL)) == -1)
                ERR("cannot get current time");

#if     (_POSIX_C_SOURCE >= 1 || _XOPEN_SOURCE || _BSD_SOURCE || _SVID_SOURCE || _POSIX_SOURCE)
        static char   threadSafeBuff[32]; /* "must be at least 26 characters long" */
        if ((currentTimePtr = ctime_r(&currentTime, threadSafeBuff)) == NULL) {
                ERR("cannot read current time");
        }
#else
        if ((currentTimePtr = ctime(&currentTime)) == NULL) {
                ERR("cannot read current time");
        }
#endif
        /* ctime string ends in \n */
        return (currentTimePtr);
}

/*
 * Dump transfer buffer.
 */
void DumpBuffer(void *buffer,
                size_t size)    /* <size> in bytes */
{
        size_t i, j;
        IOR_size_t *dumpBuf = (IOR_size_t *)buffer;

        /* Turns out, IOR_size_t is unsigned long long, but we don't want
           to assume that it must always be */
        for (i = 0; i < ((size / sizeof(IOR_size_t)) / 4); i++) {
                for (j = 0; j < 4; j++) {
                        fprintf(out_logfile, IOR_format" ", dumpBuf[4 * i + j]);
                }
                fprintf(out_logfile, "\n");
        }
        return;
}                               /* DumpBuffer() */

/* a function that prints an int array where each index corresponds to a rank
   and the value is whether that rank is on the same host as root.
   Also returns 1 if rank 1 is on same host and 0 otherwise
*/
int QueryNodeMapping(MPI_Comm comm, int print_nodemap) {
    char localhost[MAX_PATHLEN], roothost[MAX_PATHLEN];
    int num_ranks;
    MPI_Comm_size(comm, &num_ranks);
    int *node_map = (int*)malloc(sizeof(int) * num_ranks);
    if ( ! node_map ) {
        FAIL("malloc");
    }
    if (gethostname(localhost, MAX_PATHLEN) != 0) {
        FAIL("gethostname()");
    }
    if (rank==0) {
        strncpy(roothost,localhost,MAX_PATHLEN);
    }

    /* have rank 0 broadcast out its hostname */
    MPI_Bcast(roothost, MAX_PATHLEN, MPI_CHAR, 0, comm);
    //printf("Rank %d received root host as %s\n", rank, roothost);
    /* then every rank figures out whether it is same host as root and then gathers that */
    int same_as_root = strcmp(roothost,localhost) == 0;
    MPI_Gather( &same_as_root, 1, MPI_INT, node_map, 1, MPI_INT, 0, comm);
    if ( print_nodemap && rank==0) {
        fprintf( out_logfile, "Nodemap: " );
        for ( int i = 0; i < num_ranks; i++ ) {
            fprintf( out_logfile, "%d", node_map[i] );
        }
        fprintf( out_logfile, "\n" );
    }
    int ret = 1;
    if(num_ranks>1)
        ret = node_map[1] == 1;
    MPI_Bcast(&ret, 1, MPI_INT, 0, comm);
    free(node_map);
    return ret;
}

/*
 * There is a more direct way to determine the node count in modern MPI
 * versions so we use that if possible.
 *
 * For older versions we use a method which should still provide accurate
 * results even if the total number of tasks is not evenly divisible by the
 * tasks on node rank 0.
 */
int GetNumNodes(MPI_Comm comm) {
    if (getenv("IOR_FAKE_NODES")){
      int numNodes = atoi(getenv("IOR_FAKE_NODES"));
      int rank;
      MPI_Comm_rank(comm, & rank);
      if(rank == 0){
        printf("Fake number of node: using %d\n", numNodes);
      }
      return numNodes;
    }
#if MPI_VERSION >= 3
        MPI_Comm shared_comm;
        int shared_rank = 0;
        int local_result = 0;
        int numNodes = 0;

        MPI_CHECK(MPI_Comm_split_type(comm, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &shared_comm),
                  "MPI_Comm_split_type() error");
        MPI_CHECK(MPI_Comm_rank(shared_comm, &shared_rank), "MPI_Comm_rank() error");
        local_result = shared_rank == 0? 1 : 0;
        MPI_CHECK(MPI_Allreduce(&local_result, &numNodes, 1, MPI_INT, MPI_SUM, comm),
                  "MPI_Allreduce() error");
        MPI_CHECK(MPI_Comm_free(&shared_comm), "MPI_Comm_free() error");

        return numNodes;
#else
        int numTasks = 0;
        int numTasksOnNode0 = 0;

        numTasks = GetNumTasks(comm);
        numTasksOnNode0 = GetNumTasksOnNode0(comm);

        return ((numTasks - 1) / numTasksOnNode0) + 1;
#endif
}


int GetNumTasks(MPI_Comm comm) {
        int numTasks = 0;

        MPI_CHECK(MPI_Comm_size(comm, &numTasks), "cannot get number of tasks");

        return numTasks;
}


/*
 * It's very important that this method provide the same result to every
 * process as it's used for redistributing which jobs read from which files.
 * It was renamed accordingly.
 *
 * If different nodes get different results from this method then jobs get
 * redistributed unevenly and you no longer have a 1:1 relationship with some
 * nodes reading multiple files while others read none.
 *
 * In the common case the number of tasks on each node (MPI_Comm_size on an
 * MPI_COMM_TYPE_SHARED communicator) will be the same.  However, there is
 * nothing which guarantees this.  It's valid to have, for example, 64 jobs
 * across 4 systems which can run 20 jobs each.  In that scenario you end up
 * with 3 MPI_COMM_TYPE_SHARED groups of 20, and one group of 4.
 *
 * In the (MPI_VERSION < 3) implementation of this method consistency is
 * ensured by asking specifically about the number of tasks on the node with
 * rank 0.  In the original implementation for (MPI_VERSION >= 3) this was
 * broken by using the LOCAL process count which differed depending on which
 * node you were on.
 *
 * This was corrected below by first splitting the comm into groups by node
 * (MPI_COMM_TYPE_SHARED) and then having only the node with world rank 0 and
 * shared rank 0 return the MPI_Comm_size of its shared subgroup. This yields
 * the original consistent behavior no matter which node asks.
 *
 * In the common case where every node has the same number of tasks this
 * method will return the same value it always has.
 */
int GetNumTasksOnNode0(MPI_Comm comm) {
  if (getenv("IOR_FAKE_TASK_PER_NODES")){
    int tasksPerNode = atoi(getenv("IOR_FAKE_TASK_PER_NODES"));
    int rank;
    MPI_Comm_rank(comm, & rank);
    if(rank == 0){
      printf("Fake tasks per node: using %d\n", tasksPerNode);
    }
    return tasksPerNode;
  }
#if MPI_VERSION >= 3
        MPI_Comm shared_comm;
        int shared_rank = 0;
        int tasks_on_node_rank0 = 0;
        int local_result = 0;

        MPI_CHECK(MPI_Comm_split_type(comm, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &shared_comm),
                  "MPI_Comm_split_type() error");
        MPI_CHECK(MPI_Comm_rank(shared_comm, &shared_rank), "MPI_Comm_rank() error");
        if (rank == 0 && shared_rank == 0) {
                MPI_CHECK(MPI_Comm_size(shared_comm, &local_result), "MPI_Comm_size() error");
        }
        MPI_CHECK(MPI_Allreduce(&local_result, &tasks_on_node_rank0, 1, MPI_INT, MPI_SUM, comm),
                  "MPI_Allreduce() error");
        MPI_CHECK(MPI_Comm_free(&shared_comm), "MPI_Comm_free() error");

        return tasks_on_node_rank0;
#else
/*
 * This version employs the gethostname() call, rather than using
 * MPI_Get_processor_name().  We are interested in knowing the number
 * of tasks that share a file system client (I/O node, compute node,
 * whatever that may be).  However on machines like BlueGene/Q,
 * MPI_Get_processor_name() uniquely identifies a cpu in a compute node,
 * not the node where the I/O is function shipped to.  gethostname()
 * is assumed to identify the shared filesystem client in more situations.
 */
    int size;
    MPI_Comm_size(comm, & size);
    /* for debugging and testing */
    char       localhost[MAX_PATHLEN],
        hostname[MAX_PATHLEN];
    int        count               = 1,
        i;
    MPI_Status status;

    if (( rank == 0 ) && ( verbose >= 1 )) {
        fprintf( out_logfile, "V-1: Entering count_tasks_per_node...\n" );
        fflush( out_logfile );
    }

    if (gethostname(localhost, MAX_PATHLEN) != 0) {
        FAIL("gethostname()");
    }
    if (rank == 0) {
        /* MPI_receive all hostnames, and compares them to the local hostname */
        for (i = 0; i < size-1; i++) {
            MPI_Recv(hostname, MAX_PATHLEN, MPI_CHAR, MPI_ANY_SOURCE,
                     MPI_ANY_TAG, comm, &status);
            if (strcmp(hostname, localhost) == 0) {
                count++;
            }
        }
    } else {
        /* MPI_send hostname to root node */
        MPI_Send(localhost, MAX_PATHLEN, MPI_CHAR, 0, 0, comm);
    }
    MPI_Bcast(&count, 1, MPI_INT, 0, comm);

    return(count);
#endif
}


/*
 * Extract key/value pair from hint string.
 */
void ExtractHint(char *settingVal, char *valueVal, char *hintString)
{
        char *settingPtr, *valuePtr, *tmpPtr2;

        /* find the value */
        settingPtr = (char *)strtok(hintString, " =");
        valuePtr = (char *)strtok(NULL, " =\t\r\n");
        /* is this an MPI hint? */
        tmpPtr2 = (char *) strstr(settingPtr, "IOR_HINT__MPI__");
        if (settingPtr == tmpPtr2) {
                settingPtr += strlen("IOR_HINT__MPI__");
        } else {
                tmpPtr2 = (char *) strstr(hintString, "IOR_HINT__GPFS__");
                /* is it an GPFS hint? */
                if (settingPtr == tmpPtr2) {
                  settingPtr += strlen("IOR_HINT__GPFS__");
                }else{
                  fprintf(out_logfile, "WARNING: Unable to set unknown hint type (not implemented.)\n");
                  return;
                }
        }
        strcpy(settingVal, settingPtr);
        strcpy(valueVal, valuePtr);
}

/*
 * Set hints for MPIIO, HDF5, or NCMPI.
 */
void SetHints(MPI_Info * mpiHints, char *hintsFileName)
{
        char hintString[MAX_STR];
        char settingVal[MAX_STR];
        char valueVal[MAX_STR];
        extern char **environ;
        int i;
        FILE *fd;

        /*
         * This routine checks for hints from the environment and/or from the
         * hints files.  The hints are of the form:
         * 'IOR_HINT__<layer>__<hint>=<value>', where <layer> is either 'MPI'
         * or 'GPFS', <hint> is the full name of the hint to be set, and <value>
         * is the hint value.  E.g., 'setenv IOR_HINT__MPI__IBM_largeblock_io true'
         * or 'IOR_HINT__GPFS__hint=value' in the hints file.
         */
        MPI_CHECK(MPI_Info_create(mpiHints), "cannot create info object");

        /* get hints from environment */
        for (i = 0; environ[i] != NULL; i++) {
                /* if this is an IOR_HINT, pass the hint to the info object */
                if (strncmp(environ[i], "IOR_HINT", strlen("IOR_HINT")) == 0) {
                        strcpy(hintString, environ[i]);
                        ExtractHint(settingVal, valueVal, hintString);
                        MPI_CHECK(MPI_Info_set(*mpiHints, settingVal, valueVal),
                                  "cannot set info object");
                }
        }

        /* get hints from hints file */
        if (hintsFileName != NULL && strcmp(hintsFileName, "") != 0) {

                /* open the hint file */
                fd = fopen(hintsFileName, "r");
                if (fd == NULL) {
                        WARN("cannot open hints file");
                } else {
                        /* iterate over hints file */
                        while (fgets(hintString, MAX_STR, fd) != NULL) {
                                if (strncmp
                                    (hintString, "IOR_HINT",
                                     strlen("IOR_HINT")) == 0) {
                                        ExtractHint(settingVal, valueVal,
                                                    hintString);
                                        MPI_CHECK(MPI_Info_set
                                                  (*mpiHints, settingVal,
                                                   valueVal),
                                                  "cannot set info object");
                                }
                        }
                        /* close the hints files */
                        if (fclose(fd) != 0)
                                ERR("cannot close hints file");
                }
        }
}

/*
 * Show all hints (key/value pairs) in an MPI_Info object.
 */
void ShowHints(MPI_Info * mpiHints)
{
        char key[MPI_MAX_INFO_VAL];
        char value[MPI_MAX_INFO_VAL];
        int flag, i, nkeys;

        MPI_CHECK(MPI_Info_get_nkeys(*mpiHints, &nkeys),
                  "cannot get info object keys");

        for (i = 0; i < nkeys; i++) {
                MPI_CHECK(MPI_Info_get_nthkey(*mpiHints, i, key),
                          "cannot get info object key");
                MPI_CHECK(MPI_Info_get(*mpiHints, key, MPI_MAX_INFO_VAL - 1,
                                       value, &flag),
                          "cannot get info object value");
                fprintf(out_logfile, "\t%s = %s\n", key, value);
        }
}

/*
 * Takes a string of the form 64, 8m, 128k, 4g, etc. and converts to bytes.
 */
IOR_offset_t StringToBytes(char *size_str)
{
        IOR_offset_t size = 0;
        char range;
        int rc;

        rc = sscanf(size_str, "%lld%c", &size, &range);
        if (rc == 2) {
                switch ((int)range) {
                case 'k':
                case 'K':
                        size <<= 10;
                        break;
                case 'm':
                case 'M':
                        size <<= 20;
                        break;
                case 'g':
                case 'G':
                        size <<= 30;
                        break;
                }
        } else if (rc == 0) {
                size = -1;
        }
        return (size);
}

/*
 * Displays size of file system and percent of data blocks and inodes used.
 */
void ShowFileSystemSize(char * filename, const struct ior_aiori * backend, void * backend_options) // this might be converted to an AIORI call
{
  ior_aiori_statfs_t stat;
  if(! backend->statfs){
    WARN("Backend doesn't implement statfs");
    return;
  }
  int ret = backend->statfs(filename, & stat, backend_options);
  if( ret != 0 ){
    WARN("Backend returned error during statfs");
    return;
  }
  long long int totalFileSystemSize;
  long long int freeFileSystemSize;
  long long int totalInodes;
  long long int freeInodes;
  double totalFileSystemSizeHR;
  double usedFileSystemPercentage;
  double usedInodePercentage;
  char *fileSystemUnitStr;

  totalFileSystemSize = stat.f_blocks * stat.f_bsize;
  freeFileSystemSize = stat.f_bfree * stat.f_bsize;
  usedFileSystemPercentage = (1 - ((double)freeFileSystemSize / (double)totalFileSystemSize)) * 100;
  totalFileSystemSizeHR = (double)totalFileSystemSize / (double)(1<<30);

  /* inodes */
  totalInodes = stat.f_files;
  freeInodes = stat.f_ffree;
  usedInodePercentage = (1 - ((double)freeInodes / (double)totalInodes)) * 100;

  fileSystemUnitStr = "GiB";
  if (totalFileSystemSizeHR > 1024) {
          totalFileSystemSizeHR = (double)totalFileSystemSize / (double)((long long)1<<40);
          fileSystemUnitStr = "TiB";
  }
  if(outputFormat == OUTPUT_DEFAULT){
    fprintf(out_resultfile, "%-20s: %s\n", "Path", filename);
    fprintf(out_resultfile, "%-20s: %.1f %s   Used FS: %2.1f%%   ",
            "FS", totalFileSystemSizeHR, fileSystemUnitStr,
            usedFileSystemPercentage);
    fprintf(out_resultfile, "Inodes: %.1f Mi   Used Inodes: %2.1f%%\n",
            (double)totalInodes / (double)(1<<20),
            usedInodePercentage);
    fflush(out_logfile);
  }else if(outputFormat == OUTPUT_JSON){
    fprintf(out_resultfile, "    , \"Path\": \"%s\",", filename);
    fprintf(out_resultfile, "\"Capacity\": \"%.1f %s\", \"Used Capacity\": \"%2.1f%%\",",
            totalFileSystemSizeHR, fileSystemUnitStr,
            usedFileSystemPercentage);
    fprintf(out_resultfile, "\"Inodes\": \"%.1f Mi\", \"Used Inodes\" : \"%2.1f%%\"\n",
            (double)totalInodes / (double)(1<<20),
            usedInodePercentage);
  }else if(outputFormat == OUTPUT_CSV){

  }

  return;
}

/*
 * Return match of regular expression -- 0 is failure, 1 is success.
 */
int Regex(char *string, char *pattern)
{
        int retValue = 0;
#ifndef _WIN32                  /* Okay to always not match */
        regex_t regEx;
        regmatch_t regMatch;

        regcomp(&regEx, pattern, REG_EXTENDED);
        if (regexec(&regEx, string, 1, &regMatch, 0) == 0) {
                retValue = 1;
        }
        regfree(&regEx);
#endif

        return (retValue);
}

/*
 * System info for Windows.
 */
#ifdef _WIN32
int uname(struct utsname *name)
{
        DWORD nodeNameSize = sizeof(name->nodename) - 1;

        memset(name, 0, sizeof(struct utsname));
        if (!GetComputerNameEx
            (ComputerNameDnsFullyQualified, name->nodename, &nodeNameSize))
                ERR("GetComputerNameEx failed");

        strncpy(name->sysname, "Windows", sizeof(name->sysname) - 1);
        /* FIXME - these should be easy to fetch */
        strncpy(name->release, "-", sizeof(name->release) - 1);
        strncpy(name->version, "-", sizeof(name->version) - 1);
        strncpy(name->machine, "-", sizeof(name->machine) - 1);
        return 0;
}
#endif /* _WIN32 */

/*
 * Get time stamp.  Use MPI_Timer() unless _NO_MPI_TIMER is defined,
 * in which case use gettimeofday().
 */
double GetTimeStamp(void)
{
        double timeVal;
        struct timeval timer;

        if (gettimeofday(&timer, (struct timezone *)NULL) != 0)
                ERR("cannot use gettimeofday()");
        timeVal = (double)timer.tv_sec + ((double)timer.tv_usec / 1000000);

        return (timeVal);
}

/*
 * Determine any spread (range) between node times.
 * Obsolete
 */
static double TimeDeviation(MPI_Comm com)
{
        double timestamp;
        double min = 0;
        double max = 0;
        double roottimestamp;

        MPI_CHECK(MPI_Barrier(com), "barrier error");
        timestamp = GetTimeStamp();
        MPI_CHECK(MPI_Reduce(&timestamp, &min, 1, MPI_DOUBLE,
                             MPI_MIN, 0, com),
                  "cannot reduce tasks' times");
        MPI_CHECK(MPI_Reduce(&timestamp, &max, 1, MPI_DOUBLE,
                             MPI_MAX, 0, com),
                  "cannot reduce tasks' times");

        /* delta between individual nodes' time and root node's time */
        roottimestamp = timestamp;
        MPI_CHECK(MPI_Bcast(&roottimestamp, 1, MPI_DOUBLE, 0, com),
                  "cannot broadcast root's time");
        // wall_clock_delta = timestamp - roottimestamp;

        return max - min;
}

void init_clock(MPI_Comm com){

}

char * PrintTimestamp() {
    static char datestring[80];
    time_t cur_timestamp;

    if (( rank == 0 ) && ( verbose >= 1 )) {
        fprintf( out_logfile, "V-1: Entering PrintTimestamp...\n" );
    }

    fflush(out_logfile);
    cur_timestamp = time(NULL);
    strftime(datestring, 80, "%m/%d/%Y %T", localtime(&cur_timestamp));

    return datestring;
}

int64_t ReadStoneWallingIterations(char * const filename, MPI_Comm com){
  long long data;
  if(rank != 0){
    MPI_Bcast( & data, 1, MPI_LONG_LONG_INT, 0, com);
    return data;
  }else{
    FILE * out = fopen(filename, "r");
    if (out == NULL){
      data = -1;
      MPI_Bcast( & data, 1, MPI_LONG_LONG_INT, 0, com);
      return data;
    }
    int ret = fscanf(out, "%lld", & data);
    if (ret != 1){
      return -1;
    }
    fclose(out);
    MPI_Bcast( & data, 1, MPI_LONG_LONG_INT, 0, com);
    return data;
  }
}

void StoreStoneWallingIterations(char * const filename, int64_t count){
  if(rank != 0){
    return;
  }
  FILE * out = fopen(filename, "w");
  if (out == NULL){
    FAIL("Cannot write to the stonewalling file!");
  }
  fprintf(out, "%lld", (long long) count);
  fclose(out);
}

/*
 * Sleep for 'delay' seconds.
 */
void DelaySecs(int delay){
  if (rank == 0 && delay > 0) {
    if (verbose >= VERBOSE_1)
            fprintf(out_logfile, "delaying %d seconds . . .\n", delay);
    sleep(delay);
  }
}


/*
 * Convert IOR_offset_t value to human readable string.  This routine uses a
 * statically-allocated buffer internally and so is not re-entrant.
 */
char *HumanReadable(IOR_offset_t value, int base)
{
        static char valueStr[MAX_STR];
        IOR_offset_t m = 0, g = 0, t = 0;
        char m_str[8], g_str[8], t_str[8];

        if (base == BASE_TWO) {
                m = MEBIBYTE;
                g = GIBIBYTE;
                t = GIBIBYTE * 1024llu;
                strcpy(m_str, "MiB");
                strcpy(g_str, "GiB");
                strcpy(t_str, "TiB");
        } else if (base == BASE_TEN) {
                m = MEGABYTE;
                g = GIGABYTE;
                t = GIGABYTE * 1000llu;
                strcpy(m_str, "MB");
                strcpy(g_str, "GB");
                strcpy(t_str, "TB");
        }

        if (value >= t) {
                if (value % t) {
                        snprintf(valueStr, MAX_STR-1, "%.2f %s",
                                (double)((double)value / t), t_str);
                } else {
                        snprintf(valueStr, MAX_STR-1, "%d %s", (int)(value / t), t_str);
                }
        }else if (value >= g) {
                if (value % g) {
                        snprintf(valueStr, MAX_STR-1, "%.2f %s",
                                (double)((double)value / g), g_str);
                } else {
                        snprintf(valueStr, MAX_STR-1, "%d %s", (int)(value / g), g_str);
                }
        } else if (value >= m) {
                if (value % m) {
                        snprintf(valueStr, MAX_STR-1, "%.2f %s",
                                (double)((double)value / m), m_str);
                } else {
                        snprintf(valueStr, MAX_STR-1, "%d %s", (int)(value / m), m_str);
                }
        } else if (value >= 0) {
                snprintf(valueStr, MAX_STR-1, "%d bytes", (int)value);
        } else {
                snprintf(valueStr, MAX_STR-1, "-");
        }
        return valueStr;
}

#if defined(HAVE_GETCPU_SYSCALL)
// Assume we aren't worried about thread/process migration.
// Test on Intel systems and see if we can get rid of the architecture specificity
// of the code.
unsigned long GetProcessorAndCore(int *chip, int *core){
	return syscall(SYS_getcpu, core, chip, NULL);
}
#elif defined(HAVE_RDTSCP_ASM)
// We're on an intel processor and use the
// rdtscp instruction.
unsigned long GetProcessorAndCore(int *chip, int *core){
	unsigned long a,d,c;
	__asm__ volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));
	*chip = (c & 0xFFF000)>>12;
	*core = c & 0xFFF;
	return ((unsigned long)a) | (((unsigned long)d) << 32);;
}
#else
// TODO: Add in AMD function
unsigned long GetProcessorAndCore(int *chip, int *core){
#warning GetProcessorAndCore is implemented as a dummy
  *chip = 0;
  *core = 0;
	return 1;
}
#endif



/*
 * Allocate a page-aligned (required by O_DIRECT) buffer.
 */
void *aligned_buffer_alloc(size_t size, ior_memory_flags type)
{
  size_t pageMask;
  char *buf, *tmp;
  char *aligned;

  if(type == IOR_MEMORY_TYPE_GPU_MANAGED){
#ifdef HAVE_CUDA
    // use unified memory here to allow drop-in-replacement
    if (cudaMallocManaged((void**) & buf, size, cudaMemAttachGlobal) != cudaSuccess){
      ERR("Cannot allocate buffer on GPU");
    }
    return buf;
#else
    ERR("No CUDA supported, cannot allocate on the GPU");
#endif
  }else if(type == IOR_MEMORY_TYPE_GPU_DEVICE_ONLY){
#ifdef HAVE_GPU_DIRECT
      if (cudaMalloc((void**) & buf, size) != cudaSuccess){
        ERR("Cannot allocate buffer on GPU");
      }
      return buf;
#else
      ERR("No GPUDirect supported, cannot allocate on the GPU");
#endif
    }

#ifdef HAVE_SYSCONF
  long pageSize = sysconf(_SC_PAGESIZE);
#else
  size_t pageSize = getpagesize();
#endif

  pageMask = pageSize - 1;
  buf = safeMalloc(size + pageSize + sizeof(void *));
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
void aligned_buffer_free(void *buf, ior_memory_flags gpu)
{
  if(gpu){
#ifdef HAVE_CUDA
    if (cudaFree(buf) != cudaSuccess){
      WARN("Cannot free buffer on GPU");
    }
    return;
#else
    ERR("No CUDA supported, cannot free on the GPU");
#endif
  }
  free(*(void **)((char *)buf - sizeof(char *)));
}

/*
 * Copyright (C) 2003, The Regents of the University of California.
 *  Produced at the Lawrence Livermore National Laboratory.
 *  Written by Christopher J. Morrone <morrone@llnl.gov>,
 *  Bill Loewe <loewe@loewe.net>, Tyce McLarty <mclarty@llnl.gov>,
 *  and Ryan Kroiss <rrkroiss@lanl.gov>.
 *  All rights reserved.
 *  UCRL-CODE-155800
 *
 *  Please read the COPYRIGHT file.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License (as published by
 *  the Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  terms and conditions of the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * CVS info:
 *   $RCSfile: mdtest.c,v $
 *   $Revision: 1.4 $
 *   $Date: 2013/11/27 17:05:31 $
 *   $Author: brettkettering $
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>

#include "option.h"
#include "utilities.h"

#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#if HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

#if HAVE_SYS_STATFS_H
#include <sys/statfs.h>
#endif

#if HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif

#include <fcntl.h>
#include <string.h>

#if HAVE_STRINGS_H
#include <strings.h>
#endif

#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "aiori.h"
#include "ior.h"
#include "mdtest.h"

#include <mpi.h>

#pragma GCC diagnostic ignored "-Wformat-overflow"

#ifdef HAVE_LUSTRE_LUSTREAPI
#include <lustre/lustreapi.h>
#endif /* HAVE_LUSTRE_LUSTREAPI */

#define FILEMODE S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH
#define DIRMODE S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP|S_IROTH|S_IXOTH
#define RELEASE_VERS META_VERSION
#define TEST_DIR "test-dir"
#define ITEM_COUNT 25000

#define LLU "%lu"

typedef struct {
  int size;
  uint64_t *rand_array;
  char testdir[MAX_PATHLEN];
  char testdirpath[MAX_PATHLEN];
  char base_tree_name[MAX_PATHLEN];
  char **filenames;
  char hostname[MAX_PATHLEN];
  char mk_name[MAX_PATHLEN];
  char stat_name[MAX_PATHLEN];
  char read_name[MAX_PATHLEN];
  char rm_name[MAX_PATHLEN];
  char unique_mk_dir[MAX_PATHLEN];
  char unique_chdir_dir[MAX_PATHLEN];
  char unique_stat_dir[MAX_PATHLEN];
  char unique_read_dir[MAX_PATHLEN];
  char unique_rm_dir[MAX_PATHLEN];
  char unique_rm_uni_dir[MAX_PATHLEN];
  char *write_buffer;
  char *stoneWallingStatusFile;
  int gpu_memory_flags;


  int barriers;
  int create_only;
  int stat_only;
  int read_only;
  int verify_read;
  int verify_write;
  int verification_error;
  int remove_only;
  int rename_dirs;
  int leaf_only;
  unsigned branch_factor;
  int depth;
  int random_buffer_offset; /* user settable value, otherwise random */

  /*
   * This is likely a small value, but it's sometimes computed by
   * branch_factor^(depth+1), so we'll make it a larger variable,
   * just in case.
   */
  uint64_t num_dirs_in_tree;
  /*
   * As we start moving towards Exascale, we could have billions
   * of files in a directory. Make room for that possibility with
   * a larger variable.
   */
  uint64_t items;
  uint64_t items_per_dir;
  uint64_t num_dirs_in_tree_calc; /* this is a workaround until the overal code is refactored */
  int directory_loops;
  int print_time;
  int print_rate_and_time;
  int print_all_proc;
  ior_dataPacketType_e dataPacketType;
  int random_seed;
  int shared_file;
  int files_only;
  int dirs_only;
  int pre_delay;
  int unique_dir_per_task;
  int time_unique_dir_overhead;
  int collective_creates;
  size_t write_bytes;
  int stone_wall_timer_seconds;
  size_t read_bytes;
  int sync_file;
  int call_sync;
  int path_count;
  int nstride; /* neighbor stride */
  int make_node;
  #ifdef HAVE_LUSTRE_LUSTREAPI
  int global_dir_layout;
  #endif /* HAVE_LUSTRE_LUSTREAPI */
  char * saveRankDetailsCSV;       /* save the details about the performance to a file */
  const char *prologue;
  const char *epilogue;

  mdtest_results_t * summary_table;
  pid_t pid;
  uid_t uid;

  /* Use the POSIX backend by default */
  const ior_aiori_t *backend;
  void * backend_options;
  aiori_xfer_hint_t hints;
  char * api;
} mdtest_options_t;

static mdtest_options_t o;


/* This structure describes the processing status for stonewalling */
typedef struct{
  double start_time;

  int stone_wall_timer_seconds;

  uint64_t items_start;
  uint64_t items_done;

  uint64_t items_per_dir;
} rank_progress_t;

#define CHECK_STONE_WALL(p) (((p)->stone_wall_timer_seconds != 0) && ((GetTimeStamp() - (p)->start_time) > (p)->stone_wall_timer_seconds))

/* for making/removing unique directory && stating/deleting subdirectory */
enum {MK_UNI_DIR, STAT_SUB_DIR, READ_SUB_DIR, RM_SUB_DIR, RM_UNI_DIR};

/* a helper function for passing debug and verbose messages.
   use the MACRO as it will insert __LINE__ for you.
   Pass the verbose level for root to print, then the verbose level for anyone to print.
   Pass -1 to suppress the print for anyone.
   Then do the standard printf stuff.  This function adds the newline for you.
*/
#define VERBOSE(root,any,...) VerboseMessage(root,any,__LINE__,__VA_ARGS__)
void VerboseMessage (int root_level, int any_level, int line, char * format, ...) {
    if ((rank==0 && verbose >= root_level) || (any_level > 0 && verbose >= any_level)) {
        char buffer[1024];
        va_list args;
        va_start (args, format);
        vsnprintf (buffer, 1024, format, args);
        va_end (args);
        if (root_level == 0 && any_level == -1) {
            /* No header when it is just the standard output */
            fprintf( out_logfile, "%s\n", buffer );
        } else {
            /* add a header when the verbose is greater than 0 */
            fprintf( out_logfile, "V-%d: Rank %3d Line %5d %s\n", root_level, rank, line, buffer );
        }
        fflush(out_logfile);
    }
}

void parse_dirpath(char *dirpath_arg) {
    char * tmp, * token;
    char delimiter_string[3] = { '@', '\n', '\0' };
    int i = 0;


    VERBOSE(1,-1, "Entering parse_dirpath on %s...", dirpath_arg );

    tmp = dirpath_arg;

    if (* tmp != '\0') o.path_count++;
    while (* tmp != '\0') {
        if (* tmp == '@') {
            o.path_count++;
        }
        tmp++;
    }
    // prevent changes to the original dirpath_arg
    dirpath_arg = strdup(dirpath_arg);
    o.filenames = (char **) safeMalloc(o.path_count * sizeof(char **));

    token = strtok(dirpath_arg, delimiter_string);
    while (token != NULL) {
        o.filenames[i] = token;
        token = strtok(NULL, delimiter_string);
        i++;
    }
}

static void prep_testdir(int j, int dir_iter){
  int pos = sprintf(o.testdir, "%s", o.testdirpath);
  if ( o.testdir[strlen( o.testdir ) - 1] != '/' ) {
      pos += sprintf(& o.testdir[pos], "/");
  }
  pos += sprintf(& o.testdir[pos], "%s", TEST_DIR);
  pos += sprintf(& o.testdir[pos], ".%d-%d", j, dir_iter);
}

static void phase_prepare(){
  if (*o.prologue){
    VERBOSE(0,5,"calling prologue: \"%s\"", o.prologue);
    system(o.prologue);
    if (o.barriers) {
      MPI_Barrier(testComm);
    }
  }
}

static void phase_end(){
  if (o.call_sync){
    if(! o.backend->sync){
      FAIL("Error, backend does not provide the sync method, but you requested to use sync.\n");
    }
    o.backend->sync(o.backend_options);
  }
  if (*o.epilogue){
    VERBOSE(0,5,"calling epilogue: \"%s\"", o.epilogue);
    system(o.epilogue);
  }

  if (o.barriers) {
    MPI_Barrier(testComm);
  }
}

/*
 * This function copies the unique directory name for a given option to
 * the "to" parameter. Some memory must be allocated to the "to" parameter.
 */

void unique_dir_access(int opt, char *to) {
    if (opt == MK_UNI_DIR) {
        MPI_Barrier(testComm);
        sprintf( to, "%s/%s", o.testdir, o.unique_chdir_dir );
    } else if (opt == STAT_SUB_DIR) {
        sprintf( to, "%s/%s", o.testdir, o.unique_stat_dir );
    } else if (opt == READ_SUB_DIR) {
        sprintf( to, "%s/%s", o.testdir, o.unique_read_dir );
    } else if (opt == RM_SUB_DIR) {
        sprintf( to, "%s/%s", o.testdir, o.unique_rm_dir );
    } else if (opt == RM_UNI_DIR) {
        sprintf( to, "%s/%s", o.testdir, o.unique_rm_uni_dir );
    }
    VERBOSE(1,-1,"Entering unique_dir_access, set it to %s", to );
}

static void create_remove_dirs (const char *path, bool create, uint64_t itemNum) {
    char curr_item[MAX_PATHLEN];
    const char *operation = create ? "create" : "remove";

    if ( (itemNum % ITEM_COUNT==0 && (itemNum != 0))) {
        VERBOSE(3,5,"dir: "LLU"", operation, itemNum);
    }

    //create dirs
    sprintf(curr_item, "%s/dir.%s%" PRIu64, path, create ? o.mk_name : o.rm_name, itemNum);
    VERBOSE(3,5,"create_remove_items_helper (dirs %s): curr_item is '%s'", operation, curr_item);

    if (create) {
        if (o.backend->mkdir(curr_item, DIRMODE, o.backend_options) == -1) {
            EWARNF("unable to create directory %s", curr_item);
        }
    } else {
        if (o.backend->rmdir(curr_item, o.backend_options) == -1) {
            EWARNF("unable to remove directory %s", curr_item);
        }
    }
}

static void remove_file (const char *path, uint64_t itemNum) {
    char curr_item[MAX_PATHLEN];

    if ( (itemNum % ITEM_COUNT==0 && (itemNum != 0))) {
        VERBOSE(3,5,"remove file: "LLU"\n", itemNum);
    }

    //remove files
    sprintf(curr_item, "%s/file.%s"LLU"", path, o.rm_name, itemNum);
    VERBOSE(3,5,"create_remove_items_helper (non-dirs remove): curr_item is '%s'", curr_item);
    if (!(o.shared_file && rank != 0)) {
        o.backend->delete (curr_item, o.backend_options);
    }
}


static void create_file (const char *path, uint64_t itemNum) {
    char curr_item[MAX_PATHLEN];
    aiori_fd_t *aiori_fh = NULL;

    if ( (itemNum % ITEM_COUNT==0 && (itemNum != 0))) {
        VERBOSE(3,5,"create file: "LLU"", itemNum);
    }

    //create files
    sprintf(curr_item, "%s/file.%s"LLU"", path, o.mk_name, itemNum);
    VERBOSE(3,5,"create_remove_items_helper (non-dirs create): curr_item is '%s'", curr_item);

    if (o.make_node) {
        int ret;
        VERBOSE(3,5,"create_remove_items_helper : mknod..." );

        ret = o.backend->mknod (curr_item);
        if (ret != 0)
            EWARNF("unable to mknode file %s", curr_item);

        return;
    } else if (o.collective_creates) {
        VERBOSE(3,5,"create_remove_items_helper (collective): open..." );

        aiori_fh = o.backend->open (curr_item, IOR_WRONLY | IOR_CREAT, o.backend_options);
        if (NULL == aiori_fh){
            EWARNF("unable to open file %s", curr_item);
            return;
        }

        /*
         * !collective_creates
         */
    } else {
        o.hints.filePerProc = ! o.shared_file;
        VERBOSE(3,5,"create_remove_items_helper (non-collective, shared): open..." );

        aiori_fh = o.backend->create (curr_item, IOR_WRONLY | IOR_CREAT, o.backend_options);
        if (NULL == aiori_fh){
          EWARNF("unable to create file %s", curr_item);
          return;
        }
    }

    if (o.write_bytes > 0) {
        VERBOSE(3,5,"create_remove_items_helper: write..." );

        o.hints.fsyncPerWrite = o.sync_file;
        update_write_memory_pattern(itemNum, o.write_buffer, o.write_bytes, o.random_buffer_offset, rank, o.dataPacketType);

        if ( o.write_bytes != (size_t) o.backend->xfer(WRITE, aiori_fh, (IOR_size_t *) o.write_buffer, o.write_bytes, 0, o.backend_options)) {
            EWARNF("unable to write file %s", curr_item);
        }

        if (o.verify_write) {
            o.write_buffer[0] = 42;
            if (o.write_bytes != (size_t) o.backend->xfer(READ, aiori_fh, (IOR_size_t *) o.write_buffer, o.write_bytes, 0, o.backend_options)) {
                EWARNF("unable to verify write (read/back) file %s", curr_item);
            }
            o.verification_error += verify_memory_pattern(itemNum, o.write_buffer, o.write_bytes, o.random_buffer_offset, rank, o.dataPacketType);
        }
    }

    VERBOSE(3,5,"create_remove_items_helper: close..." );
    o.backend->close (aiori_fh, o.backend_options);
}

/* helper for creating/removing items */
void create_remove_items_helper(const int dirs, const int create, const char *path,
                                uint64_t itemNum, rank_progress_t * progress) {

    VERBOSE(1,-1,"Entering create_remove_items_helper on %s", path );

    for (uint64_t i = progress->items_start; i < progress->items_per_dir ; ++i) {
        if (!dirs) {
            if (create) {
                create_file (path, itemNum + i);
            } else {
                remove_file (path, itemNum + i);
            }
        } else {
            create_remove_dirs (path, create, itemNum + i);
        }
        if(CHECK_STONE_WALL(progress)){
          if(progress->items_done == 0){
            progress->items_done = i + 1;
          }
          return;
        }
    }
    progress->items_done = progress->items_per_dir;
}

/* helper function to do collective operations */
void collective_helper(const int dirs, const int create, const char* path, uint64_t itemNum, rank_progress_t * progress) {
    char curr_item[MAX_PATHLEN];

    VERBOSE(1,-1,"Entering collective_helper on %s", path );
    for (uint64_t i = progress->items_start ; i < progress->items_per_dir ; ++i) {
        if (dirs) {
            create_remove_dirs (path, create, itemNum + i);
            continue;
        }

        sprintf(curr_item, "%s/file.%s"LLU"", path, create ? o.mk_name : o.rm_name, itemNum+i);
        VERBOSE(3,5,"create file: %s", curr_item);

        if (create) {
            aiori_fd_t *aiori_fh;

            //create files
            aiori_fh = o.backend->create (curr_item, IOR_WRONLY | IOR_CREAT, o.backend_options);
            if (NULL == aiori_fh) {
                EWARNF("unable to create file %s", curr_item);
            }else{
              o.backend->close (aiori_fh, o.backend_options);
            }
        } else if (!(o.shared_file && rank != 0)) {
            //remove files
            o.backend->delete (curr_item, o.backend_options);
        }
        if(CHECK_STONE_WALL(progress)){
          progress->items_done = i + 1;
          return;
        }
    }
    progress->items_done = progress->items_per_dir;
}

/* recursive function to create and remove files/directories from the
   directory tree */
void create_remove_items(int currDepth, const int dirs, const int create, const int collective, const char *path, uint64_t dirNum, rank_progress_t * progress) {
    unsigned i;
    char dir[MAX_PATHLEN];
    char temp_path[MAX_PATHLEN];
    unsigned long long currDir = dirNum;


    VERBOSE(1,-1,"Entering create_remove_items on %s, currDepth = %d...", path, currDepth );


    memset(dir, 0, MAX_PATHLEN);
    strcpy(temp_path, path);

    VERBOSE(3,5,"create_remove_items (start): temp_path is '%s'", temp_path );

    if (currDepth == 0) {
        /* create items at this depth */
        if (! o.leaf_only || (o.depth == 0 && o.leaf_only)) {
            if (collective) {
                collective_helper(dirs, create, temp_path, 0, progress);
            } else {
                create_remove_items_helper(dirs, create, temp_path, 0, progress);
            }
        }

        if (o.depth > 0) {
            create_remove_items(++currDepth, dirs, create,
                                collective, temp_path, ++dirNum, progress);
        }

    } else if (currDepth <= o.depth) {
        /* iterate through the branches */
        for (i=0; i< o.branch_factor; i++) {

            /* determine the current branch and append it to the path */
            sprintf(dir, "%s.%llu/", o.base_tree_name, currDir);
            strcat(temp_path, "/");
            strcat(temp_path, dir);

            VERBOSE(3,5,"create_remove_items (for loop): temp_path is '%s'", temp_path );

            /* create the items in this branch */
            if (! o.leaf_only || (o.leaf_only && currDepth == o.depth)) {
                if (collective) {
                    collective_helper(dirs, create, temp_path, currDir* o.items_per_dir, progress);
                } else {
                    create_remove_items_helper(dirs, create, temp_path, currDir*o.items_per_dir, progress);
                }
            }

            /* make the recursive call for the next level below this branch */
            create_remove_items(
                ++currDepth,
                dirs,
                create,
                collective,
                temp_path,
                ( currDir * ( unsigned long long ) o.branch_factor ) + 1,
                progress
               );
            currDepth--;

            /* reset the path */
            strcpy(temp_path, path);
            currDir++;
        }
    }
}

/* stats all of the items created as specified by the input parameters */
void mdtest_stat(const int random, const int dirs, const long dir_iter, const char *path, rank_progress_t * progress) {
    struct stat buf;
    uint64_t parent_dir, item_num = 0;
    char item[MAX_PATHLEN], temp[MAX_PATHLEN];

    VERBOSE(1,-1,"Entering mdtest_stat on %s", path );

    uint64_t stop_items = o.items;

    if( o.directory_loops != 1 ){
      stop_items = o.items_per_dir;
    }

    /* iterate over all of the item IDs */
    for (uint64_t i = 0 ; i < stop_items ; ++i) {
        /*
         * It doesn't make sense to pass the address of the array because that would
         * be like passing char **. Tested it on a Cray and it seems to work either
         * way, but it seems that it is correct without the "&".
         *
         memset(&item, 0, MAX_PATHLEN);
        */
        memset(item, 0, MAX_PATHLEN);
        memset(temp, 0, MAX_PATHLEN);


        /* determine the item number to stat */
        if (random) {
            item_num = o.rand_array[i];
        } else {
            item_num = i;
        }

        /* make adjustments if in leaf only mode*/
        if (o.leaf_only) {
            item_num += o.items_per_dir *
                (o.num_dirs_in_tree - (uint64_t) pow( o.branch_factor, o.depth ));
        }

        /* create name of file/dir to stat */
        if (dirs) {
            if ( (i % ITEM_COUNT == 0) && (i != 0)) {
                VERBOSE(3,5,"stat dir: "LLU"", i);
            }
            sprintf(item, "dir.%s"LLU"", o.stat_name, item_num);
        } else {
            if ( (i % ITEM_COUNT == 0) && (i != 0)) {
                VERBOSE(3,5,"stat file: "LLU"", i);
            }
            sprintf(item, "file.%s"LLU"", o.stat_name, item_num);
        }

        /* determine the path to the file/dir to be stat'ed */
        parent_dir = item_num / o.items_per_dir;

        if (parent_dir > 0) {        //item is not in tree's root directory

            /* prepend parent directory to item's path */
            sprintf(temp, "%s."LLU"/%s", o.base_tree_name, parent_dir, item);
            strcpy(item, temp);

            //still not at the tree's root dir
            while (parent_dir > o.branch_factor) {
                parent_dir = (uint64_t) ((parent_dir-1) / o.branch_factor);
                sprintf(temp, "%s."LLU"/%s", o.base_tree_name, parent_dir, item);
                strcpy(item, temp);
            }
        }

        /* Now get item to have the full path */
        sprintf( temp, "%s/%s", path, item );
        strcpy( item, temp );

        /* below temp used to be hiername */
        VERBOSE(3,5,"mdtest_stat %4s: %s", (dirs ? "dir" : "file"), item);
        if (-1 == o.backend->stat (item, &buf, o.backend_options)) {
            EWARNF("unable to stat %s %s", dirs ? "directory" : "file", item);
        }
    }
}

/* reads all of the items created as specified by the input parameters */
void mdtest_read(int random, int dirs, const long dir_iter, char *path) {
    uint64_t parent_dir, item_num = 0;
    char item[MAX_PATHLEN], temp[MAX_PATHLEN];
    aiori_fd_t *aiori_fh;

    VERBOSE(1,-1,"Entering mdtest_read on %s", path );
    char *read_buffer;

    /* allocate read buffer */
    if (o.read_bytes > 0) {
        read_buffer = aligned_buffer_alloc(o.read_bytes, o.gpu_memory_flags);
        memset(read_buffer, -1, o.read_bytes);
    }

    uint64_t stop_items = o.items;

    if( o.directory_loops != 1 ){
      stop_items = o.items_per_dir;
    }

    /* iterate over all of the item IDs */
    for (uint64_t i = 0 ; i < stop_items ; ++i) {
        /*
         * It doesn't make sense to pass the address of the array because that would
         * be like passing char **. Tested it on a Cray and it seems to work either
         * way, but it seems that it is correct without the "&".
         *
         * NTH: Both are technically correct in C.
         *
         * memset(&item, 0, MAX_PATHLEN);
         */
        memset(item, 0, MAX_PATHLEN);
        memset(temp, 0, MAX_PATHLEN);

        /* determine the item number to read */
        if (random) {
            item_num = o.rand_array[i];
        } else {
            item_num = i;
        }

        /* make adjustments if in leaf only mode*/
        if (o.leaf_only) {
            item_num += o.items_per_dir *
                (o.num_dirs_in_tree - (uint64_t) pow (o.branch_factor, o.depth));
        }

        /* create name of file to read */
        if (!dirs) {
            if ((i%ITEM_COUNT == 0) && (i != 0)) {
                VERBOSE(3,5,"read file: "LLU"", i);
            }
            sprintf(item, "file.%s"LLU"", o.read_name, item_num);
        }

        /* determine the path to the file/dir to be read'ed */
        parent_dir = item_num / o.items_per_dir;

        if (parent_dir > 0) {        //item is not in tree's root directory

            /* prepend parent directory to item's path */
            sprintf(temp, "%s."LLU"/%s", o.base_tree_name, parent_dir, item);
            strcpy(item, temp);

            /* still not at the tree's root dir */
            while (parent_dir > o.branch_factor) {
                parent_dir = (unsigned long long) ((parent_dir-1) / o.branch_factor);
                sprintf(temp, "%s."LLU"/%s", o.base_tree_name, parent_dir, item);
                strcpy(item, temp);
            }
        }

        /* Now get item to have the full path */
        sprintf( temp, "%s/%s", path, item );
        strcpy( item, temp );

        /* below temp used to be hiername */
        VERBOSE(3,5,"mdtest_read file: %s", item);

        /* open file for reading */
        aiori_fh = o.backend->open (item, O_RDONLY, o.backend_options);
        if (NULL == aiori_fh) {
            EWARNF("unable to open file %s", item);
            continue;
        }

        /* read file */
        if (o.read_bytes > 0) {
            read_buffer[0] = 42;
            if (o.read_bytes != (size_t) o.backend->xfer(READ, aiori_fh, (IOR_size_t *) read_buffer, o.read_bytes, 0, o.backend_options)) {
                EWARNF("unable to read file %s", item);
                continue;
            }
            int pretend_rank = (2 * o.nstride + rank) % o.size;
            if(o.verify_read){
              if (o.shared_file) {
                pretend_rank = rank;
              }
              o.verification_error += verify_memory_pattern(item_num, read_buffer, o.read_bytes, o.random_buffer_offset, pretend_rank, o.dataPacketType);
            }
        }

        /* close file */
        o.backend->close (aiori_fh, o.backend_options);
    }
    if(o.read_bytes){
      aligned_buffer_free(read_buffer, o.gpu_memory_flags);
    }
}

/* This method should be called by rank 0.  It subsequently does all of
   the creates and removes for the other ranks */
void collective_create_remove(const int create, const int dirs, const int ntasks, const char *path, rank_progress_t * progress) {
    char temp[MAX_PATHLEN];

    VERBOSE(1,-1,"Entering collective_create_remove on %s", path );

    /* rank 0 does all of the creates and removes for all of the ranks */
    for (int i = 0 ; i < ntasks ; ++i) {
        memset(temp, 0, MAX_PATHLEN);

        strcpy(temp, o.testdir);
        strcat(temp, "/");

        /* set the base tree name appropriately */
        if (o.unique_dir_per_task) {
            sprintf(o.base_tree_name, "mdtest_tree.%d", i);
        } else {
            sprintf(o.base_tree_name, "mdtest_tree");
        }

        /* Setup to do I/O to the appropriate test dir */
        strcat(temp, o.base_tree_name);
        strcat(temp, ".0");

        /* set all item names appropriately */
        if (! o.shared_file) {
            sprintf(o.mk_name, "mdtest.%d.", (i+(0*o.nstride))%ntasks);
            sprintf(o.stat_name, "mdtest.%d.", (i+(1*o.nstride))%ntasks);
            sprintf(o.read_name, "mdtest.%d.", (i+(2*o.nstride))%ntasks);
            sprintf(o.rm_name, "mdtest.%d.", (i+(3*o.nstride))%ntasks);
        }
        if (o.unique_dir_per_task) {
            VERBOSE(3,5,"i %d nstride %d ntasks %d", i, o.nstride, ntasks);
            sprintf(o.unique_mk_dir, "%s/mdtest_tree.%d.0", o.testdir,
                    (i+(0*o.nstride))%ntasks);
            sprintf(o.unique_chdir_dir, "%s/mdtest_tree.%d.0", o.testdir,
                    (i+(1*o.nstride))%ntasks);
            sprintf(o.unique_stat_dir, "%s/mdtest_tree.%d.0", o.testdir,
                    (i+(2*o.nstride))%ntasks);
            sprintf(o.unique_read_dir, "%s/mdtest_tree.%d.0", o.testdir,
                    (i+(3*o.nstride))%ntasks);
            sprintf(o.unique_rm_dir, "%s/mdtest_tree.%d.0", o.testdir,
                    (i+(4*o.nstride))%ntasks);
            sprintf(o.unique_rm_uni_dir, "%s", o.testdir);
        }

        /* Now that everything is set up as it should be, do the create or remove */
        VERBOSE(3,5,"collective_create_remove (create_remove_items): temp is '%s'", temp);

        create_remove_items(0, dirs, create, 1, temp, 0, progress);
    }

    /* reset all of the item names */
    if (o.unique_dir_per_task) {
        sprintf(o.base_tree_name, "mdtest_tree.0");
    } else {
        sprintf(o.base_tree_name, "mdtest_tree");
    }
    if (! o.shared_file) {
        sprintf(o.mk_name, "mdtest.%d.", (0+(0*o.nstride))%ntasks);
        sprintf(o.stat_name, "mdtest.%d.", (0+(1*o.nstride))%ntasks);
        sprintf(o.read_name, "mdtest.%d.", (0+(2*o.nstride))%ntasks);
        sprintf(o.rm_name, "mdtest.%d.", (0+(3*o.nstride))%ntasks);
    }
    if (o.unique_dir_per_task) {
        sprintf(o.unique_mk_dir, "%s/mdtest_tree.%d.0", o.testdir,
                (0+(0*o.nstride))%ntasks);
        sprintf(o.unique_chdir_dir, "%s/mdtest_tree.%d.0", o.testdir,
                (0+(1*o.nstride))%ntasks);
        sprintf(o.unique_stat_dir, "%s/mdtest_tree.%d.0", o.testdir,
                (0+(2*o.nstride))%ntasks);
        sprintf(o.unique_read_dir, "%s/mdtest_tree.%d.0", o.testdir,
                (0+(3*o.nstride))%ntasks);
        sprintf(o.unique_rm_dir, "%s/mdtest_tree.%d.0", o.testdir,
                (0+(4*o.nstride))%ntasks);
        sprintf(o.unique_rm_uni_dir, "%s", o.testdir);
    }
}

void rename_dir_test(const int dirs, const long dir_iter, const char *path, rank_progress_t * progress) {
    uint64_t parent_dir, item_num = 0;
    char item[MAX_PATHLEN], temp[MAX_PATHLEN];
    char item_last[MAX_PATHLEN];

    if(o.backend->rename == NULL){
      WARN("Backend doesn't support rename\n");
      return;
    }

    VERBOSE(1,-1,"Entering mdtest_rename on %s", path );

    uint64_t stop_items = o.items;

    if( o.directory_loops != 1 ){
      stop_items = o.items_per_dir;
    }

    if(stop_items == 1) return;

    /* iterate over all of the item IDs */
    char first_item_name[MAX_PATHLEN];
    for (uint64_t i = 0 ; i < stop_items; ++i) {
        item_num = i;
        /* make adjustments if in leaf only mode*/
        if (o.leaf_only) {
            item_num += o.items_per_dir * (o.num_dirs_in_tree - (uint64_t) pow( o.branch_factor, o.depth ));
        }

        /* create name of file/dir to stat */
        if (dirs) {
            sprintf(item, "dir.%s"LLU"", o.stat_name, item_num);
        } else {
            sprintf(item, "file.%s"LLU"", o.stat_name, item_num);
        }

        /* determine the path to the file/dir to be stat'ed */
        parent_dir = item_num / o.items_per_dir;

        if (parent_dir > 0) {        //item is not in tree's root directory
            /* prepend parent directory to item's path */
            sprintf(temp, "%s."LLU"/%s", o.base_tree_name, parent_dir, item);
            strcpy(item, temp);

            //still not at the tree's root dir
            while (parent_dir > o.branch_factor) {
                parent_dir = (uint64_t) ((parent_dir-1) / o.branch_factor);
                sprintf(temp, "%s."LLU"/%s", o.base_tree_name, parent_dir, item);
                strcpy(item, temp);
            }
        }

        /* Now get item to have the full path */
        sprintf( temp, "%s/%s", path, item );
        strcpy( item, temp );

        VERBOSE(3,5,"mdtest_rename %4s: %s", (dirs ? "dir" : "file"), item);
        if(i == 0){
          sprintf(first_item_name, "%s-XX", item);
          strcpy(item_last, first_item_name);
        }else if(i == stop_items - 1){
          strcpy(item, first_item_name);
        }
        if (-1 == o.backend->rename(item, item_last, o.backend_options)) {
            EWARNF("unable to rename %s %s", dirs ? "directory" : "file", item);
        }

        strcpy(item_last, item);
    }
}

static void updateResult(mdtest_results_t * res, mdtest_test_num_t test, uint64_t item_count, double t_start, double t_end, double t_end_before_barrier){
  res->time[test] = t_end - t_start;
  if(isfinite(t_end_before_barrier)){
    res->time_before_barrier[test] = t_end_before_barrier - t_start;
  }else{
    res->time_before_barrier[test] = res->time[test];
  }
  res->rate[test] = item_count/res->time[test];
  res->rate_before_barrier[test] = item_count/res->time_before_barrier[test];
  res->items[test] = item_count;
  res->stonewall_last_item[test] = o.items;
}

void directory_test(const int iteration, const int ntasks, const char *path, rank_progress_t * progress) {
    int size;
    double t_start, t_end, t_end_before_barrier;
    char temp_path[MAX_PATHLEN];
    mdtest_results_t * res = & o.summary_table[iteration];

    MPI_Comm_size(testComm, &size);

    VERBOSE(1,-1,"Entering directory_test on %s", path );

    MPI_Barrier(testComm);

    /* create phase */
    if(o.create_only) {
      phase_prepare();
      t_start = GetTimeStamp();
      progress->stone_wall_timer_seconds = o.stone_wall_timer_seconds;
      progress->items_done = 0;
      progress->start_time = GetTimeStamp();
      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(iteration, dir_iter);
        if (o.unique_dir_per_task) {
            unique_dir_access(MK_UNI_DIR, temp_path);
            if (! o.time_unique_dir_overhead) {
                t_start = GetTimeStamp();
            }
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,-1,"directory_test: create path is '%s'", temp_path );

        /* "touch" the files */
        if (o.collective_creates) {
            if (rank == 0) {
                collective_create_remove(1, 1, ntasks, temp_path, progress);
            }
        } else {
            /* create directories */
            create_remove_items(0, 1, 1, 0, temp_path, 0, progress);
        }
      }
      progress->stone_wall_timer_seconds = 0;
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_DIR_CREATE_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }

    /* stat phase */
    if (o.stat_only) {
      phase_prepare();
      t_start = GetTimeStamp();
      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(iteration, dir_iter);
        if (o.unique_dir_per_task) {
            unique_dir_access(STAT_SUB_DIR, temp_path);
            if (! o.time_unique_dir_overhead) {
                t_start = GetTimeStamp();
            }
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,5,"stat path is '%s'", temp_path );

        /* stat directories */
        if (o.random_seed > 0) {
            mdtest_stat(1, 1, dir_iter, temp_path, progress);
        } else {
            mdtest_stat(0, 1, dir_iter, temp_path, progress);
        }
      }
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_DIR_STAT_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }

    /* read phase */
    if (o.read_only) {
      phase_prepare();
      t_start = GetTimeStamp();
      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(iteration, dir_iter);
        if (o.unique_dir_per_task) {
            unique_dir_access(READ_SUB_DIR, temp_path);
            if (! o.time_unique_dir_overhead) {
                t_start = GetTimeStamp();
            }
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,5,"directory_test: read path is '%s'", temp_path );

        /* read directories */
        if (o.random_seed > 0) {
            ;        /* N/A */
        } else {
            ;        /* N/A */
        }
      }
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_DIR_READ_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }

    /* rename phase */
    if(o.rename_dirs && o.items > 1){
      phase_prepare();
      t_start = GetTimeStamp();
      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(iteration, dir_iter);
        if (o.unique_dir_per_task) {
            unique_dir_access(STAT_SUB_DIR, temp_path);
            if (! o.time_unique_dir_overhead) {
                t_start = GetTimeStamp();
            }
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,5,"rename path is '%s'", temp_path );

        rename_dir_test(1, dir_iter, temp_path, progress);
      }
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_DIR_RENAME_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }

    /* remove phase */
    if (o.remove_only) {
      phase_prepare();
      t_start = GetTimeStamp();
      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(iteration, dir_iter);
        if (o.unique_dir_per_task) {
            unique_dir_access(RM_SUB_DIR, temp_path);
            if (!o.time_unique_dir_overhead) {
                t_start = GetTimeStamp();
            }
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,5,"directory_test: remove directories path is '%s'", temp_path );

        /* remove directories */
        if (o.collective_creates) {
            if (rank == 0) {
                collective_create_remove(0, 1, ntasks, temp_path, progress);
            }
        } else {
            create_remove_items(0, 1, 0, 0, temp_path, 0, progress);
        }
      }
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_DIR_REMOVE_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }

    if (o.remove_only) {
        if (o.unique_dir_per_task) {
            unique_dir_access(RM_UNI_DIR, temp_path);
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,5,"directory_test: remove unique directories path is '%s'\n", temp_path );
    }

    VERBOSE(1,-1,"   Directory creation: %14.3f sec, %14.3f ops/sec", res->time[MDTEST_DIR_CREATE_NUM], o.summary_table[iteration].rate[MDTEST_DIR_CREATE_NUM]);
    VERBOSE(1,-1,"   Directory stat    : %14.3f sec, %14.3f ops/sec", res->time[MDTEST_DIR_STAT_NUM], o.summary_table[iteration].rate[MDTEST_DIR_STAT_NUM]);
    VERBOSE(1,-1,"   Directory rename : %14.3f sec, %14.3f ops/sec", res->time[MDTEST_DIR_RENAME_NUM], o.summary_table[iteration].rate[MDTEST_DIR_RENAME_NUM]);
    VERBOSE(1,-1,"   Directory removal : %14.3f sec, %14.3f ops/sec", res->time[MDTEST_DIR_REMOVE_NUM], o.summary_table[iteration].rate[MDTEST_DIR_REMOVE_NUM]);
}

/* Returns if the stonewall was hit */
int updateStoneWallIterations(int iteration, uint64_t items_done, double tstart, uint64_t * out_max_iter){
  int hit = 0;
  long long unsigned max_iter = 0;

  VERBOSE(1,1,"stonewall hit with %lld items", (long long) items_done );
  MPI_Allreduce(& items_done, & max_iter, 1, MPI_LONG_LONG_INT, MPI_MAX, testComm);
  o.summary_table[iteration].stonewall_time[MDTEST_FILE_CREATE_NUM] = GetTimeStamp() - tstart;
  o.summary_table[iteration].stonewall_last_item[MDTEST_FILE_CREATE_NUM] = items_done;
  *out_max_iter = max_iter;

  // continue to the maximum...
  long long min_accessed = 0;
  MPI_Reduce(& items_done, & min_accessed, 1, MPI_LONG_LONG_INT, MPI_MIN, 0, testComm);
  long long sum_accessed = 0;
  MPI_Reduce(& items_done, & sum_accessed, 1, MPI_LONG_LONG_INT, MPI_SUM, 0, testComm);
  o.summary_table[iteration].stonewall_item_sum[MDTEST_FILE_CREATE_NUM] = sum_accessed;
  o.summary_table[iteration].stonewall_item_min[MDTEST_FILE_CREATE_NUM] = min_accessed * o.size;

  if(o.items != (sum_accessed / o.size)){
    VERBOSE(0,-1, "Continue stonewall hit min: %lld max: %lld avg: %.1f \n", min_accessed, max_iter, ((double) sum_accessed) / o.size);
    hit = 1;
  }

  return hit;
}

void file_test_create(const int iteration, const int ntasks, const char *path, rank_progress_t * progress, double *t_start){
  char temp_path[MAX_PATHLEN];
  for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
    prep_testdir(iteration, dir_iter);

    if (o.unique_dir_per_task) {
        unique_dir_access(MK_UNI_DIR, temp_path);
        VERBOSE(5,5,"operating on %s", temp_path);
        if (! o.time_unique_dir_overhead) {
            *t_start = GetTimeStamp();
        }
    } else {
        sprintf( temp_path, "%s/%s", o.testdir, path );
    }

    VERBOSE(3,-1,"file_test: create path is '%s'", temp_path );
    /* "touch" the files */
    if (o.collective_creates) {
        if (rank == 0) {
            collective_create_remove(1, 0, ntasks, temp_path, progress);
        }
        MPI_Barrier(testComm);
    }

    /* create files */
    create_remove_items(0, 0, 1, 0, temp_path, 0, progress);
    if(o.stone_wall_timer_seconds){
      // hit the stonewall
      uint64_t max_iter = 0;
      uint64_t items_done = progress->items_done + dir_iter * o.items_per_dir;
      int hit = updateStoneWallIterations(iteration, items_done, *t_start, & max_iter);
      progress->items_start = items_done;
      progress->items_per_dir = max_iter;
      if (hit){
        progress->stone_wall_timer_seconds = 0;
        VERBOSE(1,1,"stonewall: %lld of %lld", (long long) progress->items_start, (long long) progress->items_per_dir);
        create_remove_items(0, 0, 1, 0, temp_path, 0, progress);
        // now reset the values
        progress->stone_wall_timer_seconds = o.stone_wall_timer_seconds;
        o.items = progress->items_done;
      }
      if (o.stoneWallingStatusFile){
        StoreStoneWallingIterations(o.stoneWallingStatusFile, max_iter);
      }
      // reset stone wall timer to allow proper cleanup
      progress->stone_wall_timer_seconds = 0;
      // at the moment, stonewall can be done only with one directory_loop, so we can return here safely
      break;
    }
  }
}

void file_test(const int iteration, const int ntasks, const char *path, rank_progress_t * progress) {
    int size;
    double t_start, t_end, t_end_before_barrier;
    char temp_path[MAX_PATHLEN];
    mdtest_results_t * res = & o.summary_table[iteration];

    MPI_Comm_size(testComm, &size);

    VERBOSE(3,5,"Entering file_test on %s", path);

    MPI_Barrier(testComm);

    /* create phase */
    if (o.create_only ) {
      phase_prepare();
      t_start = GetTimeStamp();
      progress->stone_wall_timer_seconds = o.stone_wall_timer_seconds;
      progress->items_done = 0;
      progress->start_time = GetTimeStamp();
      file_test_create(iteration, ntasks, path, progress, &t_start);
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_FILE_CREATE_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }else{
      if (o.stoneWallingStatusFile){
        int64_t expected_items;
        /* The number of items depends on the stonewalling file */
        expected_items = ReadStoneWallingIterations(o.stoneWallingStatusFile, testComm);
        if(expected_items >= 0){
          if(o.directory_loops > 1){
            o.directory_loops = expected_items / o.items_per_dir;
            o.items = o.items_per_dir;
          }else{
            o.items = expected_items;
            progress->items_per_dir = o.items;
          }
        }
        if (rank == 0) {
          if(expected_items == -1){
            WARN("Could not read stonewall status file");
          }else {
            VERBOSE(1,1, "Read stonewall status; items: "LLU"\n", o.items);
          }
        }
      }
    }

    /* stat phase */
    if (o.stat_only ) {
      phase_prepare();
      t_start = GetTimeStamp();
      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(iteration, dir_iter);
        if (o.unique_dir_per_task) {
            unique_dir_access(STAT_SUB_DIR, temp_path);
            if (!o.time_unique_dir_overhead) {
                t_start = GetTimeStamp();
            }
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,5,"file_test: stat path is '%s'", temp_path );

        /* stat files */
        mdtest_stat((o.random_seed > 0 ? 1 : 0), 0, dir_iter, temp_path, progress);
      }
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_FILE_STAT_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }

    /* read phase */
    if (o.read_only ) {
      phase_prepare();
      t_start = GetTimeStamp();
      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(iteration, dir_iter);
        if (o.unique_dir_per_task) {
            unique_dir_access(READ_SUB_DIR, temp_path);
            if (! o.time_unique_dir_overhead) {
                t_start = GetTimeStamp();
            }
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,5,"file_test: read path is '%s'", temp_path );

        /* read files */
        if (o.random_seed > 0) {
                mdtest_read(1,0, dir_iter, temp_path);
        } else {
                mdtest_read(0,0, dir_iter, temp_path);
        }
      }
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_FILE_READ_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }

    /* remove phase */
    if (o.remove_only) {
      phase_prepare();
      t_start = GetTimeStamp();
      progress->items_start = 0;

      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(iteration, dir_iter);
        if (o.unique_dir_per_task) {
            unique_dir_access(RM_SUB_DIR, temp_path);
            if (! o.time_unique_dir_overhead) {
                t_start = GetTimeStamp();
            }
        } else {
            sprintf( temp_path, "%s/%s", o.testdir, path );
        }

        VERBOSE(3,5,"file_test: rm directories path is '%s'", temp_path );

        if (o.collective_creates) {
            if (rank == 0) {
                collective_create_remove(0, 0, ntasks, temp_path, progress);
            }
        } else {
            VERBOSE(3,5,"gonna create %s", temp_path);
            create_remove_items(0, 0, 0, 0, temp_path, 0, progress);
        }
      }
      t_end_before_barrier = GetTimeStamp();
      phase_end();
      t_end = GetTimeStamp();
      updateResult(res, MDTEST_FILE_REMOVE_NUM, o.items, t_start, t_end, t_end_before_barrier);
    }

    if (o.remove_only) {
        if (o.unique_dir_per_task) {
            unique_dir_access(RM_UNI_DIR, temp_path);
        } else {
            strcpy( temp_path, path );
        }

        VERBOSE(3,5,"file_test: rm unique directories path is '%s'", temp_path );
    }

    if(o.num_dirs_in_tree_calc){ /* this is temporary fix needed when using -n and -i together */
      o.items *= o.num_dirs_in_tree_calc;
    }

    VERBOSE(1,-1,"  File creation     : %14.3f sec, %14.3f ops/sec", res->time[MDTEST_FILE_CREATE_NUM], o.summary_table[iteration].rate[4]);
    if(o.summary_table[iteration].stonewall_time[MDTEST_FILE_CREATE_NUM]){
      VERBOSE(1,-1,"  File creation (stonewall): %14.3f sec, %14.3f ops/sec", o.summary_table[iteration].stonewall_time[MDTEST_FILE_CREATE_NUM], o.summary_table[iteration].stonewall_item_sum[MDTEST_FILE_CREATE_NUM]);
    }
    VERBOSE(1,-1,"  File stat         : %14.3f sec, %14.3f ops/sec", res->time[MDTEST_FILE_STAT_NUM], o.summary_table[iteration].rate[5]);
    VERBOSE(1,-1,"  File read         : %14.3f sec, %14.3f ops/sec", res->time[MDTEST_FILE_READ_NUM], o.summary_table[iteration].rate[6]);
    VERBOSE(1,-1,"  File removal      : %14.3f sec, %14.3f ops/sec", res->time[MDTEST_FILE_REMOVE_NUM], o.summary_table[iteration].rate[7]);
}

char const * mdtest_test_name(int i){
  switch (i) {
  case MDTEST_DIR_CREATE_NUM: return "Directory creation";
  case MDTEST_DIR_STAT_NUM:   return "Directory stat";
  case MDTEST_DIR_READ_NUM:   return "Directory read";
  case MDTEST_DIR_REMOVE_NUM: return "Directory removal";
  case MDTEST_DIR_RENAME_NUM: return "Directory rename";
  case MDTEST_FILE_CREATE_NUM: return "File creation";
  case MDTEST_FILE_STAT_NUM:   return "File stat";
  case MDTEST_FILE_READ_NUM:   return "File read";
  case MDTEST_FILE_REMOVE_NUM: return "File removal";
  case MDTEST_TREE_CREATE_NUM: return "Tree creation";
  case MDTEST_TREE_REMOVE_NUM: return "Tree removal";
  default: return "ERR INVALID TESTNAME      :";
  }
  return NULL;
}

/*
 * Store the results of each process in a file
 */
static void StoreRankInformation(int iterations, mdtest_results_t * agg){
  const size_t size = sizeof(mdtest_results_t) * iterations;
  if(rank == 0){
    FILE* fd = fopen(o.saveRankDetailsCSV, "a");
    if (fd == NULL){
      FAIL("Cannot open saveRankPerformanceDetails file for writes!");
    }

    mdtest_results_t * results = safeMalloc(size * o.size);
    MPI_Gather(o.summary_table, size / sizeof(double), MPI_DOUBLE, results, size / sizeof(double), MPI_DOUBLE, 0, testComm);

    char buff[4096];
    char * cpos = buff;
    cpos += sprintf(cpos, "all,%llu", (long long unsigned) o.items);
    for(int e = 0; e < MDTEST_LAST_NUM; e++){
      if(agg->items[e] == 0){
        cpos += sprintf(cpos, ",,");
      }else{
        cpos += sprintf(cpos, ",%.10e,%.10e", agg->items[e] / agg->time[e], agg->time[e]);
      }
    }
    cpos += sprintf(cpos, "\n");
    int ret = fwrite(buff, cpos - buff, 1, fd);

    for(int iter = 0; iter < iterations; iter++){
      for(int i=0; i < o.size; i++){
        mdtest_results_t * cur = & results[i * iterations + iter];
        cpos = buff;
        cpos += sprintf(cpos, "%d,", i);
        for(int e = 0; e < MDTEST_TREE_CREATE_NUM; e++){
          if(cur->items[e] == 0){
            cpos += sprintf(cpos, ",,");
          }else{
            cpos += sprintf(cpos, ",%.10e,%.10e", cur->items[e] / cur->time_before_barrier[e], cur->time_before_barrier[e]);
          }
        }
        cpos += sprintf(cpos, "\n");
        ret = fwrite(buff, cpos - buff, 1, fd);
        if(ret != 1){
          WARN("Couln't append to saveRankPerformanceDetailsCSV file\n");
          break;
        }
      }
    }
    fclose(fd);
    free(results);
  }else{
    /* this is a hack for now assuming all datatypes in the structure are double */
    MPI_Gather(o.summary_table, size / sizeof(double), MPI_DOUBLE, NULL, size / sizeof(double), MPI_DOUBLE, 0, testComm);
  }
}

static mdtest_results_t* get_result_index(mdtest_results_t* all_results, int proc, int iter, int interation_count){
  return & all_results[proc * interation_count + iter];
}

static void summarize_results_rank0(int iterations,  mdtest_results_t * all_results, int print_time) {
  int start, stop;
  double min, max, mean, sd, sum, var, curr = 0;
  double imin, imax, imean, isum, icur; // calculation per iteration
  char const * access;
  /* if files only access, skip entries 0-3 (the dir tests) */
  if (o.files_only && ! o.dirs_only) {
      start = MDTEST_FILE_CREATE_NUM;
  } else {
      start = 0;
  }

  /* if directories only access, skip entries 4-7 (the file tests) */
  if (o.dirs_only && !o.files_only) {
      stop = MDTEST_FILE_CREATE_NUM;
  } else {
      stop = MDTEST_TREE_CREATE_NUM;
  }

  /* special case: if no directory or file tests, skip all */
  if (!o.dirs_only && !o.files_only) {
      start = stop = 0;
  }

  if(o.print_all_proc){
    fprintf(out_logfile, "\nPer process result (%s):\n", print_time ? "time" : "rate");
    for (int j = 0; j < iterations; j++) {
      fprintf(out_logfile, "iteration: %d\n", j);
      for (int i = start; i < MDTEST_LAST_NUM; i++) {
        access = mdtest_test_name(i);
        if(access == NULL){
          continue;
        }
        fprintf(out_logfile, "Test %s", access);
        for (int k=0; k < o.size; k++) {
          mdtest_results_t * cur = get_result_index(all_results, k, j, iterations);
          if(print_time){
            curr = cur->time_before_barrier[i];
          }else{
            curr = cur->rate_before_barrier[i];
          }
          fprintf(out_logfile, "%c%e", (k==0 ? ' ': ','), curr);
        }
        fprintf(out_logfile, "\n");
      }
    }
  }

  VERBOSE(0, -1, "\nSUMMARY %s: (of %d iterations)", print_time ? "time" : "rate", iterations);
  VERBOSE(0, -1,
          "   Operation         per Rank: Max            Min           Mean    "
          "   per Iteration: Max            Min           Mean         Std Dev");
  VERBOSE(0, -1,
          "   ---------                   ---            ---           ----    "
          "                  ---            ---           ----         -------");
  for (int i = start; i < stop; i++) {
    min = 1e308;
    max = 0;
    sum = var = 0;
    imin = 1e308;
    isum = imax = 0;
    double iter_result[iterations];
    for (int j = 0; j < iterations; j++) {
      icur = print_time ? 0 : 1e308;
      for (int k = 0; k < o.size; k++) {
        mdtest_results_t * cur = get_result_index(all_results, k, j, iterations);
        if(print_time){
          curr = cur->time_before_barrier[i];
        }else{
          curr = cur->rate_before_barrier[i];
        }
        if (min > curr) {
          min = curr;
        }
        if (max < curr) {
          max = curr;
        }
        sum += curr;

        if (print_time) {
          curr = cur->time[i];
          if (icur < curr) {
            icur = curr;
          }
        } else {
          curr = cur->rate[i];
          if (icur > curr) {
            icur = curr;
          }
        }
      }

      if (icur > imax) {
        imax = icur;
      }
      if (icur < imin) {
        imin = icur;
      }
      isum += icur;
      if(print_time){
        iter_result[j] = icur;
      }else{
        iter_result[j] = icur * o.size;
      }
    }
    mean = sum / iterations / o.size;
    imean = isum / iterations;
    if(! print_time){
      imax *= o.size;
      imin *= o.size;
      isum *= o.size;
      imean *= o.size;
    }
    for (int j = 0; j < iterations; j++) {
      var += (imean - iter_result[j]) * (imean - iter_result[j]);
    }
    var = var / (iterations - 1);
    sd = sqrt(var);
    access = mdtest_test_name(i);
    if (i != 2) {
      fprintf(out_logfile, "   %-22s ", access);
      fprintf(out_logfile, "%14.3f ", max);
      fprintf(out_logfile, "%14.3f ", min);
      fprintf(out_logfile, "%14.3f ", mean);
      fprintf(out_logfile, "%18.3f ", imax);
      fprintf(out_logfile, "%14.3f ", imin);
      fprintf(out_logfile, "%14.3f ", imean);
      fprintf(out_logfile, "%14.3f\n", iterations == 1 ? 0 : sd);
      fflush(out_logfile);
    }
  }

  /* calculate tree create/remove rates, applies only to Rank 0 */
  for (int i = MDTEST_TREE_CREATE_NUM; i < MDTEST_LAST_NUM; i++) {
      min = imin = 1e308;
      max = imax = 0;
      sum = var = 0;
      for (int j = 0; j < iterations; j++) {
          if(print_time){
            curr = o.summary_table[j].time[i];
          }else{
            curr = o.summary_table[j].rate[i];
          }
          if (min > curr) {
            min = curr;
          }
          if (max < curr) {
            max =  curr;
          }
          sum += curr;
          if(curr > imax){
            imax = curr;
          }
          if(curr < imin){
            imin = curr;
          }
      }

      mean = sum / (iterations);

      for (int j = 0; j < iterations; j++) {
          if(print_time){
            curr = o.summary_table[j].time[i];
          }else{
            curr = o.summary_table[j].rate[i];
          }
          var += (mean -  curr)*(mean -  curr);
      }
      var = var / (iterations - 1);
      sd = sqrt(var);
      access = mdtest_test_name(i);
      fprintf(out_logfile, "   %-22s ", access);
      fprintf(out_logfile, "%14.3f ", max);
      fprintf(out_logfile, "%14.3f ", min);
      fprintf(out_logfile, "%14.3f ", mean);
      fprintf(out_logfile, "%18.3f ", imax);
      fprintf(out_logfile, "%14.3f ", imin);
      fprintf(out_logfile, "%14.3f ", sum / iterations);
      fprintf(out_logfile, "%14.3f\n", iterations == 1 ? 0 : sd);
      fflush(out_logfile);
  }
}

/*
 Output the results and summarize them into rank 0's o.summary_table
 */
void summarize_results(int iterations, mdtest_results_t * results) {
  const size_t size = sizeof(mdtest_results_t) * iterations;
  mdtest_results_t * all_results = NULL;
  if(rank == 0){
    all_results = safeMalloc(size * o.size);
    memset(all_results, 0, size * o.size);
    MPI_Gather(o.summary_table, size / sizeof(double), MPI_DOUBLE, all_results, size / sizeof(double), MPI_DOUBLE, 0, testComm);
    // calculate the aggregated values for all processes
    for(int j=0; j < iterations; j++){
      for(int i=0; i < MDTEST_LAST_NUM; i++){
        //double sum_rate = 0;
        double max_time = 0;
        double max_stonewall_time = 0;
        uint64_t sum_items = 0;

        // reduce over the processes
        for(int p=0; p < o.size; p++){
          mdtest_results_t * cur = get_result_index(all_results, p, j, iterations);
          //sum_rate += all_results[p + j*p]->rate[i];
          double t = cur->time[i];
          max_time = max_time < t ? t : max_time;

          sum_items += cur->items[i];

          t = cur->stonewall_time[i];
          max_stonewall_time = max_stonewall_time < t ? t : max_stonewall_time;
        }

        results[j].items[i] = sum_items;
        results[j].time[i] = max_time;
        results[j].stonewall_time[i] = max_stonewall_time;
        if(sum_items == 0){
          results[j].rate[i] = 0.0;
        }else{
          results[j].rate[i] = sum_items / max_time;
        }

        /* These results have already been reduced to Rank 0 */
        results[j].stonewall_item_sum[i] = o.summary_table[j].stonewall_item_sum[i];
        results[j].stonewall_item_min[i] = o.summary_table[j].stonewall_item_min[i];
        results[j].stonewall_time[i] = o.summary_table[j].stonewall_time[i];
      }
    }
  }else{
    MPI_Gather(o.summary_table, size / sizeof(double), MPI_DOUBLE, NULL, size / sizeof(double), MPI_DOUBLE, 0, testComm);
  }

  /* share global results across processes as these are returned by the API */
  MPI_Bcast(results, size / sizeof(double), MPI_DOUBLE, 0, testComm);

  /* update relevant result values with local values as these are returned by the API */
  for(int j=0; j < iterations; j++){
    for(int i=0; i < MDTEST_LAST_NUM; i++){
      results[j].time_before_barrier[i] = o.summary_table[j].time_before_barrier[i];
      results[j].stonewall_last_item[i] = o.summary_table[j].stonewall_last_item[i];
    }
  }

  if(rank != 0){
    return;
  }

  if (o.print_rate_and_time){
    summarize_results_rank0(iterations, all_results, 0);
    summarize_results_rank0(iterations, all_results, 1);
  }else{
    summarize_results_rank0(iterations, all_results, o.print_time);
  }

  free(all_results);
}

/* Checks to see if the test setup is valid.  If it isn't, fail. */
void md_validate_tests() {

    if (((o.stone_wall_timer_seconds > 0) && (o.branch_factor > 1)) || ! o.barriers) {
        FAIL( "Error, stone wall timer does only work with a branch factor <= 1 (current is %d) and with barriers\n", o.branch_factor);
    }

    if (!o.create_only && ! o.stat_only && ! o.read_only && !o.remove_only && !o.rename_dirs) {
        o.create_only = o.stat_only = o.read_only = o.remove_only = o.rename_dirs = 1;
        VERBOSE(1,-1,"main: Setting create/stat/read/remove_only to True" );
    }

    VERBOSE(1,-1,"Entering md_validate_tests..." );

    /* if dirs_only and files_only were both left unset, set both now */
    if (!o.dirs_only && !o.files_only) {
        o.dirs_only = o.files_only = 1;
    }

    /* if shared file 'S' access, no directory tests */
    if (o.shared_file) {
        o.dirs_only = 0;
    }

    /* check for no barriers with shifting processes for different phases.
       that is, one may not specify both -B and -N as it will introduce
       race conditions that may cause errors stat'ing or deleting after
       creates.
    */
    if (( o.barriers == 0 ) && ( o.nstride != 0 ) && ( rank == 0 )) {
        FAIL( "Possible race conditions will occur: -B not compatible with -N");
    }

    /* check for collective_creates incompatibilities */
    if (o.shared_file && o.collective_creates && rank == 0) {
        FAIL("-c not compatible with -S");
    }
    if (o.path_count > 1 && o.collective_creates && rank == 0) {
        FAIL("-c not compatible with multiple test directories");
    }
    if (o.collective_creates && !o.barriers) {
        FAIL("-c not compatible with -B");
    }

    /* check for shared file incompatibilities */
    if (o.unique_dir_per_task && o.shared_file && rank == 0) {
        FAIL("-u not compatible with -S");
    }

    /* check multiple directory paths and strided option */
    if (o.path_count > 1 && o.nstride > 0) {
        FAIL("cannot have multiple directory paths with -N strides between neighbor tasks");
    }

    /* check for shared directory and multiple directories incompatibility */
    if (o.path_count > 1 && o.unique_dir_per_task != 1) {
        FAIL("shared directory mode is not compatible with multiple directory paths");
    }

    /* check if more directory paths than ranks */
    if (o.path_count > o.size) {
        FAIL("cannot have more directory paths than MPI tasks");
    }

    /* check depth */
    if (o.depth < 0) {
            FAIL("depth must be greater than or equal to zero");
    }
    /* check branch_factor */
    if (o.branch_factor < 1 && o.depth > 0) {
            FAIL("branch factor must be greater than or equal to zero");
    }
    /* check for valid number of items */
    if ((o.items > 0) && (o.items_per_dir > 0)) {
       if(o.unique_dir_per_task){
         FAIL("only specify the number of items or the number of items per directory");
       }else if( o.items % o.items_per_dir != 0){
         FAIL("items must be a multiple of items per directory");
       }
    }
    /* check for using mknod */
    if (o.write_bytes > 0 && o.make_node) {
        FAIL("-k not compatible with -w");
    }

    if(o.verify_read && ! o.read_only)
      FAIL("Verify read requires that the read test is used");

    if(o.verify_read && o.read_bytes <= 0)
      FAIL("Verify read requires that read bytes is > 0");

    if(o.read_only && o.read_bytes <= 0)
      WARN("Read bytes is 0, thus, a read test will actually just open/close");

    if(o.create_only && o.read_only && o.read_bytes > o.write_bytes)
      FAIL("When writing and reading files, read bytes must be smaller than write bytes");

    if (rank == 0 && o.saveRankDetailsCSV){
      // check that the file is writeable, truncate it and add header
      FILE* fd = fopen(o.saveRankDetailsCSV, "w");
      if (fd == NULL){
        FAIL("Cannot open saveRankPerformanceDetails file for write!");
      }
      char * head = "rank,items";
      int ret = fwrite(head, strlen(head), 1, fd);
      for(int e = 0; e < MDTEST_LAST_NUM; e++){
        char buf[1024];
        const char * str = mdtest_test_name(e);

        sprintf(buf, ",rate-%s,time-%s", str, str);
        ret = fwrite(buf, strlen(buf), 1, fd);
        if(ret != 1){
          FAIL("Cannot write header to saveRankPerformanceDetails file");
        }
      }
      fwrite("\n", 1, 1, fd);
      fclose(fd);
    }
}

void show_file_system_size(char *file_system) {
    char          real_path[MAX_PATHLEN];
    char          file_system_unit_str[MAX_PATHLEN] = "GiB";
    char          inode_unit_str[MAX_PATHLEN]       = "Mi";
    int64_t       file_system_unit_val          = 1024 * 1024 * 1024;
    int64_t       inode_unit_val                = 1024 * 1024;
    int64_t       total_file_system_size,
        free_file_system_size,
        total_inodes,
        free_inodes;
    double        total_file_system_size_hr,
        used_file_system_percentage,
        used_inode_percentage;
    ior_aiori_statfs_t stat_buf;
    int ret;

    VERBOSE(1,-1,"Entering show_file_system_size on %s", file_system );

    ret = o.backend->statfs (file_system, &stat_buf, o.backend_options);
    if (0 != ret) {
        FAIL("unable to stat file system %s", file_system);
    }

    total_file_system_size = stat_buf.f_blocks * stat_buf.f_bsize;
    free_file_system_size = stat_buf.f_bfree * stat_buf.f_bsize;

    used_file_system_percentage = (1 - ((double)free_file_system_size
                                        / (double)total_file_system_size)) * 100;
    total_file_system_size_hr = (double)total_file_system_size
        / (double)file_system_unit_val;
    if (total_file_system_size_hr > 1024) {
        total_file_system_size_hr = total_file_system_size_hr / 1024;
        strcpy(file_system_unit_str, "TiB");
    }

    /* inodes */
    total_inodes = stat_buf.f_files;
    free_inodes = stat_buf.f_ffree;

    used_inode_percentage = (1 - ((double)free_inodes/(double)total_inodes))
        * 100;

    if (realpath(file_system, real_path) == NULL) {
        WARN("unable to use realpath() on file system");
    }


    /* show results */
    VERBOSE(0,-1,"Path: %s", real_path);
    VERBOSE(0,-1,"FS: %.1f %s   Used FS: %2.1f%%   Inodes: %.1f %s   Used Inodes: %2.1f%%\n",
            total_file_system_size_hr, file_system_unit_str, used_file_system_percentage,
            (double)total_inodes / (double)inode_unit_val, inode_unit_str, used_inode_percentage);

    return;
}

void create_remove_directory_tree(int create,
                                  int currDepth, char* path, int dirNum, rank_progress_t * progress) {

    unsigned i;
    char dir[MAX_PATHLEN];


    VERBOSE(1,5,"Entering create_remove_directory_tree on %s, currDepth = %d...", path, currDepth );

    if (currDepth == 0) {
        sprintf(dir, "%s/%s.%d/", path, o.base_tree_name, dirNum);

        if (create) {
            VERBOSE(2,5,"Making directory '%s'", dir);
            if (-1 == o.backend->mkdir (dir, DIRMODE, o.backend_options)) {
                EWARNF("unable to create tree directory '%s'", dir);
            }
#ifdef HAVE_LUSTRE_LUSTREAPI
            /* internal node for branching, can be non-striped for children */
            if (o.global_dir_layout && \
                llapi_dir_set_default_lmv_stripe(dir, -1, 0,
                                                 LMV_HASH_TYPE_FNV_1A_64,
                                                 NULL) == -1) {
                FAIL("Unable to reset to global default directory layout");
            }
#endif /* HAVE_LUSTRE_LUSTREAPI */
        }

        create_remove_directory_tree(create, ++currDepth, dir, ++dirNum, progress);

        if (!create) {
            VERBOSE(2,5,"Remove directory '%s'", dir);
            if (-1 == o.backend->rmdir(dir, o.backend_options)) {
                EWARNF("Unable to remove directory %s", dir);
            }
        }
    } else if (currDepth <= o.depth) {

        char temp_path[MAX_PATHLEN];
        strcpy(temp_path, path);
        int currDir = dirNum;

        for (i=0; i < o.branch_factor; i++) {
            sprintf(dir, "%s.%d/", o.base_tree_name, currDir);
            strcat(temp_path, dir);

            if (create) {
                VERBOSE(2,5,"Making directory '%s'", temp_path);
                if (-1 == o.backend->mkdir(temp_path, DIRMODE, o.backend_options)) {
                    EWARNF("Unable to create directory %s", temp_path);
                }
            }

            create_remove_directory_tree(create, ++currDepth,
                                         temp_path, (o.branch_factor*currDir)+1, progress);
            currDepth--;

            if (!create) {
                VERBOSE(2,5,"Remove directory '%s'", temp_path);
                if (-1 == o.backend->rmdir(temp_path, o.backend_options)) {
                    EWARNF("Unable to remove directory %s", temp_path);
                }
            }

            strcpy(temp_path, path);
            currDir++;
        }
    }
}

static void mdtest_iteration(int i, int j, MPI_Group testgroup, mdtest_results_t * summary_table){
  rank_progress_t progress_o;
  memset(& progress_o, 0 , sizeof(progress_o));
  progress_o.stone_wall_timer_seconds = 0;
  progress_o.items_per_dir = o.items_per_dir;
  rank_progress_t * progress = & progress_o;

  /* start and end times of directory tree create/remove */
  double startCreate, endCreate;
  int k;

  VERBOSE(1,-1,"main: * iteration %d *", j+1);

  if(o.create_only){
    for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
      if (rank >= o.path_count) {
        continue;
      }
      prep_testdir(j, dir_iter);

      VERBOSE(2,5,"main (for j loop): making o.testdir, '%s'", o.testdir );
      if (o.backend->access(o.testdir, F_OK, o.backend_options) != 0) {
          if (o.backend->mkdir(o.testdir, DIRMODE, o.backend_options) != 0) {
              EWARNF("Unable to create test directory %s", o.testdir);
          }
#ifdef HAVE_LUSTRE_LUSTREAPI
          /* internal node for branching, can be non-striped for children */
          if (o.global_dir_layout && o.unique_dir_per_task && llapi_dir_set_default_lmv_stripe(o.testdir, -1, 0, LMV_HASH_TYPE_FNV_1A_64, NULL) == -1) {
              EWARNF("Unable to reset to global default directory layout", 0);
          }
#endif /* HAVE_LUSTRE_LUSTREAPI */
      }
    }

    /* create hierarchical directory structure */
    MPI_Barrier(testComm);

    startCreate = GetTimeStamp();
    for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
      prep_testdir(j, dir_iter);

      if (o.unique_dir_per_task) {
        if (o.collective_creates && (rank == 0)) {
          /*
           * This is inside two loops, one of which already uses "i" and the other uses "j".
           * I don't know how this ever worked. I'm changing this loop to use "k".
           */
          for (k=0; k < o.size; k++) {
            sprintf(o.base_tree_name, "mdtest_tree.%d", k);

            VERBOSE(3,5,"main (create hierarchical directory loop-collective): Calling create_remove_directory_tree with '%s'", o.testdir );
            /*
             * Let's pass in the path to the directory we most recently made so that we can use
             * full paths in the other calls.
             */
            create_remove_directory_tree(1, 0, o.testdir, 0, progress);
            if(CHECK_STONE_WALL(progress)){
              o.size = k;
              break;
            }
          }
        } else if (! o.collective_creates) {
          VERBOSE(3,5,"main (create hierarchical directory loop-!collective_creates): Calling create_remove_directory_tree with '%s'", o.testdir );
          /*
           * Let's pass in the path to the directory we most recently made so that we can use
           * full paths in the other calls.
           */
          create_remove_directory_tree(1, 0, o.testdir, 0, progress);
        }
      } else {
        if (rank == 0) {
          VERBOSE(3,5,"main (create hierarchical directory loop-!unque_dir_per_task): Calling create_remove_directory_tree with '%s'", o.testdir );

          /*
           * Let's pass in the path to the directory we most recently made so that we can use
           * full paths in the other calls.
           */
          create_remove_directory_tree(1, 0 , o.testdir, 0, progress);
        }
      }
    }
    MPI_Barrier(testComm);
    endCreate = GetTimeStamp();
    summary_table->rate[MDTEST_TREE_CREATE_NUM] = o.num_dirs_in_tree / (endCreate - startCreate);
    summary_table->time[MDTEST_TREE_CREATE_NUM] = (endCreate - startCreate);
    summary_table->items[MDTEST_TREE_CREATE_NUM] = o.num_dirs_in_tree;
    summary_table->stonewall_last_item[MDTEST_TREE_CREATE_NUM] = o.num_dirs_in_tree;
    VERBOSE(1,-1,"V-1: main:   Tree creation     : %14.3f sec, %14.3f ops/sec", (endCreate - startCreate), summary_table->rate[MDTEST_TREE_CREATE_NUM]);
  }

  sprintf(o.unique_mk_dir, "%s.0", o.base_tree_name);
  sprintf(o.unique_chdir_dir, "%s.0", o.base_tree_name);
  sprintf(o.unique_stat_dir, "%s.0", o.base_tree_name);
  sprintf(o.unique_read_dir, "%s.0", o.base_tree_name);
  sprintf(o.unique_rm_dir, "%s.0", o.base_tree_name);
  o.unique_rm_uni_dir[0] = 0;

  if (! o.unique_dir_per_task) {
    VERBOSE(3,-1,"V-3: main: Using unique_mk_dir, '%s'", o.unique_mk_dir );
  }

  if (rank < i) {
      if (! o.shared_file) {
          sprintf(o.mk_name, "mdtest.%d.", (rank+(0*o.nstride))%i);
          sprintf(o.stat_name, "mdtest.%d.", (rank+(1*o.nstride))%i);
          sprintf(o.read_name, "mdtest.%d.", (rank+(2*o.nstride))%i);
          sprintf(o.rm_name, "mdtest.%d.", (rank+(3*o.nstride))%i);
      }
      if (o.unique_dir_per_task) {
          VERBOSE(3,5,"i %d nstride %d", i, o.nstride);
          sprintf(o.unique_mk_dir, "mdtest_tree.%d.0",  (rank+(0*o.nstride))%i);
          sprintf(o.unique_chdir_dir, "mdtest_tree.%d.0", (rank+(1*o.nstride))%i);
          sprintf(o.unique_stat_dir, "mdtest_tree.%d.0", (rank+(2*o.nstride))%i);
          sprintf(o.unique_read_dir, "mdtest_tree.%d.0", (rank+(3*o.nstride))%i);
          sprintf(o.unique_rm_dir, "mdtest_tree.%d.0", (rank+(4*o.nstride))%i);
          o.unique_rm_uni_dir[0] = 0;
          VERBOSE(5,5,"mk_dir %s chdir %s stat_dir %s read_dir %s rm_dir %s\n", o.unique_mk_dir, o.unique_chdir_dir, o.unique_stat_dir, o.unique_read_dir, o.unique_rm_dir);
      }

      VERBOSE(3,-1,"V-3: main: Copied unique_mk_dir, '%s', to topdir", o.unique_mk_dir );

      if (o.dirs_only && ! o.shared_file) {
          if (o.pre_delay) {
              DelaySecs(o.pre_delay);
          }
          directory_test(j, i, o.unique_mk_dir, progress);
      }
      if (o.files_only) {
          if (o.pre_delay) {
              DelaySecs(o.pre_delay);
          }
          VERBOSE(3,5,"will file_test on %s", o.unique_mk_dir);

          file_test(j, i, o.unique_mk_dir, progress);
      }
  }

  /* remove directory structure */
  if (! o.unique_dir_per_task) {
      VERBOSE(3,-1,"main: Using o.testdir, '%s'", o.testdir );
  }

  MPI_Barrier(testComm);
  if (o.remove_only) {
      progress->items_start = 0;
      startCreate = GetTimeStamp();
      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(j, dir_iter);
        if (o.unique_dir_per_task) {
            if (o.collective_creates && (rank == 0)) {
                /*
                 * This is inside two loops, one of which already uses "i" and the other uses "j".
                 * I don't know how this ever worked. I'm changing this loop to use "k".
                 */
                for (k=0; k < o.size; k++) {
                    sprintf(o.base_tree_name, "mdtest_tree.%d", k);

                    VERBOSE(3,-1,"main (remove hierarchical directory loop-collective): Calling create_remove_directory_tree with '%s'", o.testdir );

                    /*
                     * Let's pass in the path to the directory we most recently made so that we can use
                     * full paths in the other calls.
                     */
                    create_remove_directory_tree(0, 0, o.testdir, 0, progress);
                    if(CHECK_STONE_WALL(progress)){
                      o.size = k;
                      break;
                    }
                }
            } else if (! o.collective_creates) {
                VERBOSE(3,-1,"main (remove hierarchical directory loop-!collective): Calling create_remove_directory_tree with '%s'", o.testdir );

                /*
                 * Let's pass in the path to the directory we most recently made so that we can use
                 * full paths in the other calls.
                 */
                create_remove_directory_tree(0, 0, o.testdir, 0, progress);
            }
        } else {
            if (rank == 0) {
                VERBOSE(3,-1,"V-3: main (remove hierarchical directory loop-!unique_dir_per_task): Calling create_remove_directory_tree with '%s'", o.testdir );

                /*
                 * Let's pass in the path to the directory we most recently made so that we can use
                 * full paths in the other calls.
                 */
                create_remove_directory_tree(0, 0 , o.testdir, 0, progress);
            }
        }
      }

      MPI_Barrier(testComm);
      endCreate = GetTimeStamp();
      summary_table->rate[MDTEST_TREE_REMOVE_NUM] = o.num_dirs_in_tree / (endCreate - startCreate);
      summary_table->time[MDTEST_TREE_REMOVE_NUM] = endCreate - startCreate;
      summary_table->items[MDTEST_TREE_REMOVE_NUM] = o.num_dirs_in_tree;
      summary_table->stonewall_last_item[MDTEST_TREE_REMOVE_NUM] = o.num_dirs_in_tree;
      VERBOSE(1,-1,"main   Tree removal      : %14.3f sec, %14.3f ops/sec", (endCreate - startCreate), summary_table->rate[MDTEST_TREE_REMOVE_NUM]);
      VERBOSE(2,-1,"main (at end of for j loop): Removing o.testdir of '%s'\n", o.testdir );

      for (int dir_iter = 0; dir_iter < o.directory_loops; dir_iter ++){
        prep_testdir(j, dir_iter);
        if ((rank < o.path_count) && o.backend->access(o.testdir, F_OK, o.backend_options) == 0) {
            //if (( rank == 0 ) && access(o.testdir, F_OK) == 0) {
            if (o.backend->rmdir(o.testdir, o.backend_options) == -1) {
                EWARNF("unable to remove directory %s", o.testdir);
            }
        }
      }
  } else {
      summary_table->rate[MDTEST_TREE_REMOVE_NUM] = 0;
  }
}

void mdtest_init_args(){
  o = (mdtest_options_t) {
     .barriers = 1,
     .branch_factor = 1,
     .random_buffer_offset = -1,
     .prologue = "",
     .epilogue = "",
  };
}

mdtest_results_t * mdtest_run(int argc, char **argv, MPI_Comm world_com, FILE * world_out) {
    testComm = world_com;
    out_logfile = world_out;
    out_resultfile = world_out;

    init_clock(world_com);

    mdtest_init_args();
    int i, j;
    int numNodes;
    int numTasksOnNode0 = 0;
    MPI_Group worldgroup, testgroup;
    struct {
        int first;
        int last;
        int stride;
    } range = {0, 0, 1};
    int first = 1;
    int last = 0;
    int stride = 1;
    int iterations = 1;
    int created_root_dir = 0; // was the root directory existing or newly created

    verbose = 0;
    int no_barriers = 0;
    char * path = "./out";
    int randomize = 0;
    char APIs[1024];
    char APIs_legacy[1024];
    aiori_supported_apis(APIs, APIs_legacy, MDTEST);
    char apiStr[1024];
    sprintf(apiStr, "API for I/O [%s]", APIs);
    memset(& o.hints, 0, sizeof(o.hints));
    
    char * packetType = "t";

    option_help options [] = {
      {'a', NULL,        apiStr, OPTION_OPTIONAL_ARGUMENT, 's', & o.api},
      {'b', NULL,        "branching factor of hierarchical directory structure", OPTION_OPTIONAL_ARGUMENT, 'd', & o.branch_factor},
      {'d', NULL,        "the directory in which the tests will run", OPTION_OPTIONAL_ARGUMENT, 's', & path},
      {'B', NULL,        "no barriers between phases", OPTION_OPTIONAL_ARGUMENT, 'd', & no_barriers},
      {'C', NULL,        "only create files/dirs", OPTION_FLAG, 'd', & o.create_only},
      {'T', NULL,        "only stat files/dirs", OPTION_FLAG, 'd', & o.stat_only},
      {'E', NULL,        "only read files/dir", OPTION_FLAG, 'd', & o.read_only},
      {'r', NULL,        "only remove files or directories left behind by previous runs", OPTION_FLAG, 'd', & o.remove_only},
      {'D', NULL,        "perform test on directories only (no files)", OPTION_FLAG, 'd', & o.dirs_only},
      {'e', NULL,        "bytes to read from each file", OPTION_OPTIONAL_ARGUMENT, 'l', & o.read_bytes},
      {'f', NULL,        "first number of tasks on which the test will run", OPTION_OPTIONAL_ARGUMENT, 'd', & first},
      {'F', NULL,        "perform test on files only (no directories)", OPTION_FLAG, 'd', & o.files_only},
#ifdef HAVE_LUSTRE_LUSTREAPI
      {'g', NULL,        "global default directory layout for test subdirectories (deletes inherited striping layout)", OPTION_FLAG, 'd', & o.global_dir_layout},
#endif /* HAVE_LUSTRE_LUSTREAPI */
      {'G', NULL,        "Offset for the data in the read/write buffer, if not set, a random value is used", OPTION_OPTIONAL_ARGUMENT, 'd', & o.random_buffer_offset},
      {'i', NULL,        "number of iterations the test will run", OPTION_OPTIONAL_ARGUMENT, 'd', & iterations},
      {'I', NULL,        "number of items per directory in tree", OPTION_OPTIONAL_ARGUMENT, 'l', & o.items_per_dir},
      {'k', NULL,        "use mknod to create file", OPTION_FLAG, 'd', & o.make_node},
      {'l', NULL,        "last number of tasks on which the test will run", OPTION_OPTIONAL_ARGUMENT, 'd', & last},
      {'L', NULL,        "files only at leaf level of tree", OPTION_FLAG, 'd', & o.leaf_only},
      {'n', NULL,        "every process will creat/stat/read/remove # directories and files", OPTION_OPTIONAL_ARGUMENT, 'l', & o.items},
      {'N', NULL,        "stride # between tasks for file/dir operation (local=0; set to 1 to avoid client cache)", OPTION_OPTIONAL_ARGUMENT, 'd', & o.nstride},
      {'p', NULL,        "pre-iteration delay (in seconds)", OPTION_OPTIONAL_ARGUMENT, 'd', & o.pre_delay},
      {'P', NULL,        "print rate AND time", OPTION_FLAG, 'd', & o.print_rate_and_time},
      {0, "print-all-procs", "all processes print an excerpt of their results", OPTION_FLAG, 'd', & o.print_all_proc},
      {'R', NULL,        "random access to files (only for stat)", OPTION_FLAG, 'd', & randomize},
      {0, "random-seed", "random seed for -R", OPTION_OPTIONAL_ARGUMENT, 'd', & o.random_seed},
      {'s', NULL,        "stride between the number of tasks for each test", OPTION_OPTIONAL_ARGUMENT, 'd', & stride},
      {'S', NULL,        "shared file access (file only, no directories)", OPTION_FLAG, 'd', & o.shared_file},
      {'c', NULL,        "collective creates: task 0 does all creates", OPTION_FLAG, 'd', & o.collective_creates},
      {'t', NULL,        "time unique working directory overhead", OPTION_FLAG, 'd', & o.time_unique_dir_overhead},
      {'u', NULL,        "unique working directory for each task", OPTION_FLAG, 'd', & o.unique_dir_per_task},
      {'v', NULL,        "verbosity (each instance of option increments by one)", OPTION_FLAG, 'd', & verbose},
      {'V', NULL,        "verbosity value", OPTION_OPTIONAL_ARGUMENT, 'd', & verbose},
      {'w', NULL,        "bytes to write to each file after it is created", OPTION_OPTIONAL_ARGUMENT, 'l', & o.write_bytes},
      {'W', NULL,        "number in seconds; stonewall timer, write as many seconds and ensure all processes did the same number of operations (currently only stops during create phase and files)", OPTION_OPTIONAL_ARGUMENT, 'd', & o.stone_wall_timer_seconds},
      {'x', NULL,        "StoneWallingStatusFile; contains the number of iterations of the creation phase, can be used to split phases across runs", OPTION_OPTIONAL_ARGUMENT, 's', & o.stoneWallingStatusFile},
      {'X', "verify-read", "Verify the data read", OPTION_FLAG, 'd', & o.verify_read},
      {0, "verify-write", "Verify the data after a write by reading it back immediately", OPTION_FLAG, 'd', & o.verify_write},
      {'y', NULL,        "sync file after writing", OPTION_FLAG, 'd', & o.sync_file},
      {'Y', NULL,        "call the sync command after each phase (included in the timing; note it causes all IO to be flushed from your node)", OPTION_FLAG, 'd', & o.call_sync},
      {'z', NULL,        "depth of hierarchical directory structure", OPTION_OPTIONAL_ARGUMENT, 'd', & o.depth},
      {'Z', NULL,        "print time instead of rate", OPTION_FLAG, 'd', & o.print_time},
      {0, "run-cmd-before-phase", "call this external command before each phase (excluded from the timing)", OPTION_OPTIONAL_ARGUMENT, 's', & o.prologue},
      {0, "run-cmd-after-phase",  "call this external command after each phase (included in the timing)", OPTION_OPTIONAL_ARGUMENT, 's', & o.epilogue},
      {0, "dataPacketType", "type of packet that will be created [offset|incompressible|timestamp|o|i|t]", OPTION_OPTIONAL_ARGUMENT, 's', & packetType},
      {0, "allocateBufferOnGPU", "Allocate the buffer on the GPU.", OPTION_FLAG, 'd', & o.gpu_memory_flags},
      {0, "warningAsErrors",        "Any warning should lead to an error.", OPTION_FLAG, 'd', & aiori_warning_as_errors},
      {0, "saveRankPerformanceDetails", "Save the individual rank information into this CSV file.", OPTION_OPTIONAL_ARGUMENT, 's', & o.saveRankDetailsCSV},
      LAST_OPTION
    };
    options_all_t * global_options = airoi_create_all_module_options(options);
    option_parse(argc, argv, global_options);
    o.backend = aiori_select(o.api);
    if (o.backend == NULL)
        ERR("Unrecognized I/O API");
    if (! o.backend->enable_mdtest)
        ERR("Backend doesn't support MDTest");
    o.backend_options = airoi_update_module_options(o.backend, global_options);

    free(global_options->modules);
    free(global_options);
    
    o.dataPacketType = parsePacketType(packetType[0]);

    MPI_Comm_rank(testComm, &rank);
    MPI_Comm_size(testComm, &o.size);

    if(o.backend->xfer_hints){
      o.backend->xfer_hints(& o.hints);
    }
    if(o.backend->check_params){
      o.backend->check_params(o.backend_options);
    }
    if (o.backend->initialize){
	    o.backend->initialize(o.backend_options);
    }

    o.pid = getpid();
    o.uid = getuid();

    numNodes = GetNumNodes(testComm);
    numTasksOnNode0 = GetNumTasksOnNode0(testComm);

    char cmd_buffer[4096];
    strncpy(cmd_buffer, argv[0], 4096);
    for (i = 1; i < argc; i++) {
        snprintf(&cmd_buffer[strlen(cmd_buffer)], 4096-strlen(cmd_buffer), " '%s'", argv[i]);
    }

    VERBOSE(0,-1,"-- started at %s --\n", PrintTimestamp());
    VERBOSE(0,-1,"mdtest-%s was launched with %d total task(s) on %d node(s)", RELEASE_VERS, o.size, numNodes);
    VERBOSE(0,-1,"Command line used: %s", cmd_buffer);

    /* adjust special variables */
    o.barriers = ! no_barriers;
    if (path != NULL){
      parse_dirpath(path);
    }
    if( randomize > 0 ){
      if (o.random_seed == 0) {
        /* Ensure all procs have the same random number */
          o.random_seed = time(NULL);
          MPI_Barrier(testComm);
          MPI_Bcast(& o.random_seed, 1, MPI_INT, 0, testComm);
      }
      o.random_seed += rank;
    }
    if( o.random_buffer_offset == -1 ){
        o.random_buffer_offset = time(NULL);
        MPI_Bcast(& o.random_buffer_offset, 1, MPI_INT, 0, testComm);
    }
    if ((o.items > 0) && (o.items_per_dir > 0) && (! o.unique_dir_per_task)) {
      o.directory_loops = o.items / o.items_per_dir;
    }else{
      o.directory_loops = 1;
    }
    md_validate_tests();

    // option_print_current(options);
    VERBOSE(1,-1, "api                     : %s", o.api);
    VERBOSE(1,-1, "barriers                : %s", ( o.barriers ? "True" : "False" ));
    VERBOSE(1,-1, "collective_creates      : %s", ( o.collective_creates ? "True" : "False" ));
    VERBOSE(1,-1, "create_only             : %s", ( o.create_only ? "True" : "False" ));
    VERBOSE(1,-1, "dirpath(s):" );
    for ( i = 0; i < o.path_count; i++ ) {
        VERBOSE(1,-1, "\t%s", o.filenames[i] );
    }
    VERBOSE(1,-1, "dirs_only               : %s", ( o.dirs_only ? "True" : "False" ));
    VERBOSE(1,-1, "read_bytes              : "LLU"", o.read_bytes );
    VERBOSE(1,-1, "read_only               : %s", ( o.read_only ? "True" : "False" ));
    VERBOSE(1,-1, "first                   : %d", first );
    VERBOSE(1,-1, "files_only              : %s", ( o.files_only ? "True" : "False" ));
#ifdef HAVE_LUSTRE_LUSTREAPI
    VERBOSE(1,-1, "global_dir_layout       : %s", ( o.global_dir_layout ? "True" : "False" ));
#endif /* HAVE_LUSTRE_LUSTREAPI */
    VERBOSE(1,-1, "iterations              : %d", iterations );
    VERBOSE(1,-1, "items_per_dir           : "LLU"", o.items_per_dir );
    VERBOSE(1,-1, "last                    : %d", last );
    VERBOSE(1,-1, "leaf_only               : %s", ( o.leaf_only ? "True" : "False" ));
    VERBOSE(1,-1, "items                   : "LLU"", o.items );
    VERBOSE(1,-1, "nstride                 : %d", o.nstride );
    VERBOSE(1,-1, "pre_delay               : %d", o.pre_delay );
    VERBOSE(1,-1, "remove_only             : %s", ( o.leaf_only ? "True" : "False" ));
    VERBOSE(1,-1, "random_seed             : %d", o.random_seed );
    VERBOSE(1,-1, "stride                  : %d", stride );
    VERBOSE(1,-1, "shared_file             : %s", ( o.shared_file ? "True" : "False" ));
    VERBOSE(1,-1, "time_unique_dir_overhead: %s", ( o.time_unique_dir_overhead ? "True" : "False" ));
    VERBOSE(1,-1, "stone_wall_timer_seconds: %d", o.stone_wall_timer_seconds);
    VERBOSE(1,-1, "stat_only               : %s", ( o.stat_only ? "True" : "False" ));
    VERBOSE(1,-1, "unique_dir_per_task     : %s", ( o.unique_dir_per_task ? "True" : "False" ));
    VERBOSE(1,-1, "write_bytes             : "LLU"", o.write_bytes );
    VERBOSE(1,-1, "sync_file               : %s", ( o.sync_file ? "True" : "False" ));
    VERBOSE(1,-1, "call_sync               : %s", ( o.call_sync ? "True" : "False" ));
    VERBOSE(1,-1, "depth                   : %d", o.depth );
    VERBOSE(1,-1, "make_node               : %d", o.make_node );

    /* setup total number of items and number of items per dir */
    if (o.depth <= 0) {
        o.num_dirs_in_tree = 1;
    } else {
        if (o.branch_factor < 1) {
            o.num_dirs_in_tree = 1;
        } else if (o.branch_factor == 1) {
            o.num_dirs_in_tree = o.depth + 1;
        } else {
            o.num_dirs_in_tree = (pow(o.branch_factor, o.depth+1) - 1) / (o.branch_factor - 1);
        }
    }
    if (o.items_per_dir > 0) {
        if(o.items == 0){
          if (o.leaf_only) {
              o.items = o.items_per_dir * (uint64_t) pow(o.branch_factor, o.depth);
          } else {
              o.items = o.items_per_dir * o.num_dirs_in_tree;
          }
        }else{
          o.num_dirs_in_tree_calc = o.num_dirs_in_tree;
        }
    } else {
        if (o.leaf_only) {
            if (o.branch_factor <= 1) {
                o.items_per_dir = o.items;
            } else {
                o.items_per_dir = (uint64_t) (o.items / pow(o.branch_factor, o.depth));
                o.items = o.items_per_dir * (uint64_t) pow(o.branch_factor, o.depth);
            }
        } else {
            o.items_per_dir = o.items / o.num_dirs_in_tree;
            o.items = o.items_per_dir * o.num_dirs_in_tree;
        }
    }

    /* initialize rand_array */
    if (o.random_seed > 0) {
        srand(o.random_seed);

        uint64_t s;

        o.rand_array = (uint64_t *) safeMalloc( o.items * sizeof(*o.rand_array));

        for (s=0; s < o.items; s++) {
            o.rand_array[s] = s;
        }

        /* shuffle list randomly */
        uint64_t n = o.items;
        while (n>1) {
            n--;

            /*
             * Generate a random number in the range 0 .. n
             *
             * rand() returns a number from 0 .. RAND_MAX. Divide that
             * by RAND_MAX and you get a floating point number in the
             * range 0 .. 1. Multiply that by n and you get a number in
             * the range 0 .. n.
             */
            uint64_t k = ( uint64_t ) ((( double )rand() / ( double )RAND_MAX ) * ( double )n );

            /*
             * Now move the nth element to the kth (randomly chosen)
             * element, and the kth element to the nth element.
             */

            uint64_t tmp = o.rand_array[k];
            o.rand_array[k] = o.rand_array[n];
            o.rand_array[n] = tmp;
        }
    }

    /* allocate and initialize write buffer with # */
    if (o.write_bytes > 0) {
        o.write_buffer = aligned_buffer_alloc(o.write_bytes, o.gpu_memory_flags);
        generate_memory_pattern(o.write_buffer, o.write_bytes, o.random_buffer_offset, rank, o.dataPacketType);
    }

    /* setup directory path to work in */
    if (o.path_count == 0) { /* special case where no directory path provided with '-d' option */
        char *ret = getcwd(o.testdirpath, MAX_PATHLEN);
        if (ret == NULL) {
            FAIL("Unable to get current working directory on %s", o.testdirpath);
        }
        o.path_count = 1;
    } else {
        strcpy(o.testdirpath, o.filenames[rank % o.path_count]);
    }

    /*   if directory does not exist, create it */
    if ((rank < o.path_count) && o.backend->access(o.testdirpath, F_OK, o.backend_options) != 0) {
        if (o.backend->mkdir(o.testdirpath, DIRMODE, o.backend_options) != 0) {
            EWARNF("Unable to create test directory path %s", o.testdirpath);
        }
        created_root_dir = 1;
    }

    /* display disk usage */
    VERBOSE(3,-1,"main (before display_freespace): o.testdirpath is '%s'", o.testdirpath );

    if (rank == 0) ShowFileSystemSize(o.testdirpath, o.backend, o.backend_options);
    int tasksBlockMapping = QueryNodeMapping(testComm, true);

    /* set the shift to mimic IOR and shift by procs per node */
    if (o.nstride > 0) {
        if ( numNodes > 1 && tasksBlockMapping ) {
            /* the user set the stride presumably to get the consumer tasks on a different node than the producer tasks
               however, if the mpirun scheduler placed the tasks by-slot (in a contiguous block) then we need to adjust the shift by ppn */
            o.nstride *= numTasksOnNode0;
        }
        VERBOSE(0,5,"Shifting ranks by %d for each phase.", o.nstride);
    }

    VERBOSE(3,-1,"main (after display_freespace): o.testdirpath is '%s'", o.testdirpath );

    if (rank == 0) {
        if (o.random_seed > 0) {
            VERBOSE(0,-1,"random seed: %d", o.random_seed);
        }
    }

    if (gethostname(o.hostname, MAX_PATHLEN) == -1) {
        perror("gethostname");
        MPI_Abort(testComm, 2);
    }

    if (last == 0) {
        first = o.size;
        last = o.size;
    }

    /* setup summary table for recording results */
    o.summary_table = (mdtest_results_t *) safeMalloc(iterations * sizeof(mdtest_results_t));
    memset(o.summary_table, 0, iterations * sizeof(mdtest_results_t));

    if (o.unique_dir_per_task) {
        sprintf(o.base_tree_name, "mdtest_tree.%d", rank);
    } else {
        sprintf(o.base_tree_name, "mdtest_tree");
    }

    mdtest_results_t * aggregated_results = safeMalloc(iterations * sizeof(mdtest_results_t));

    /* default use shared directory */
    strcpy(o.mk_name, "mdtest.shared.");
    strcpy(o.stat_name, "mdtest.shared.");
    strcpy(o.read_name, "mdtest.shared.");
    strcpy(o.rm_name, "mdtest.shared.");

    MPI_Comm_group(testComm, &worldgroup);

    /* Run the tests */
    for (i = first; i <= last && i <= o.size; i += stride) {
        range.last = i - 1;
        MPI_Group_range_incl(worldgroup, 1, (void *)&range, &testgroup);
        MPI_Comm_create(testComm, testgroup, &testComm);
        if (rank == 0) {
            uint64_t items_all = i * o.items;
            if(o.num_dirs_in_tree_calc){
              items_all *= o.num_dirs_in_tree_calc;
            }
            if (o.files_only && o.dirs_only) {
                VERBOSE(0,-1,"%d tasks, "LLU" files/directories", i, items_all);
            } else if (o.files_only) {
                if (! o.shared_file) {
                    VERBOSE(0,-1,"%d tasks, "LLU" files", i, items_all);
                }
                else {
                    VERBOSE(0,-1,"%d tasks, 1 file", i);
                }
            } else if (o.dirs_only) {
                VERBOSE(0,-1,"%d tasks, "LLU" directories", i, items_all);
            }
        }
        VERBOSE(1,-1,"");
        VERBOSE(1,-1,"   Operation               Duration              Rate");
        VERBOSE(1,-1,"   ---------               --------              ----");

        for (j = 0; j < iterations; j++) {
            // keep track of the current status for stonewalling
            mdtest_iteration(i, j, testgroup, & o.summary_table[j]);
        }
        summarize_results(iterations, aggregated_results);
        if(o.saveRankDetailsCSV){
          StoreRankInformation(iterations, aggregated_results);
        }
        if (i == 1 && stride > 1) {
            i = 0;
        }

        int total_errors = 0;
        MPI_Reduce(& o.verification_error, & total_errors, 1, MPI_INT, MPI_SUM, 0,  testComm);
        if(rank == 0 && total_errors){
            VERBOSE(0, -1, "\nERROR: verifying the data on read (%lld errors)! Take the performance values with care!\n", total_errors);
        }

        MPI_Comm_free(&testComm);
        MPI_Group_free(&testgroup);
    }

    MPI_Group_free(&worldgroup);
    testComm = world_com;

    if (created_root_dir && o.remove_only && o.backend->rmdir(o.testdirpath, o.backend_options) != 0) {
        FAIL("Unable to remove test directory path %s", o.testdirpath);
    }

    VERBOSE(0,-1,"-- finished at %s --\n", PrintTimestamp());

    if (o.random_seed > 0) {
        free(o.rand_array);
    }

    if (o.backend->finalize){
      o.backend->finalize(o.backend_options);
    }

    if (o.write_bytes > 0) {
      aligned_buffer_free(o.write_buffer, o.gpu_memory_flags);
    }
    free(o.summary_table);

    return aggregated_results;
}

#ifndef __BP_READ_H__
#define __BP_READ_H__

#ifdef NOMPI
#   include "mpidummy.h"
#else
#   include "mpi.h"
#endif

#include <stdint.h>
/*
header file for the subsetting read routines
March 2009, ORNL
*/

// Types used in the API

typedef struct {
	uint16_t namelist_true;
	uint16_t vars_count;
	char 	 ** var_namelist;
}BP_GROUP_INFO;

typedef struct {
	uint16_t namelist_true;
	uint16_t groups_count;
	uint16_t vars_count;
	uint16_t attrs_count;
	uint32_t tidx_start;
	uint32_t tidx_stop;
	uint32_t version;
	uint32_t file_size;
	char 	 ** group_namelist;
}BP_FILE_INFO;


// C interface
/*
	IN:  fname
	     comm
	OUT: fh_p
*/
int bp_fopen ( int64_t * fh_p,
               const char * fname,
               MPI_Comm comm
             );

/*
	IN: fh 
*/
int bp_fclose ( int64_t fh);

/*
	IN:  fh_p 
	OUT: ngroup
	     nvar
	     nattr
	     nt
	     gnamelist
 */ 
int bp_inq_file ( int64_t fh_p, BP_FILE_INFO * pfinfo); 
void bp_print_fileinfo (BP_FILE_INFO * pfinfo);
void bp_init_fileinfo  (BP_FILE_INFO * pfinfo, int flag);
void bp_free_fileinfo  (BP_FILE_INFO * pfinfo);
/*
 * int bp_inq_file ( int64_t fh_p, int *ngroup, 
		  int *nvar, int *nattr, int *nt, char **gnamelist);
*/ 
void bp_print_groupinfo (BP_GROUP_INFO * pginfo);
void bp_init_groupinfo  (BP_GROUP_INFO * pginfo, int flag);
void bp_free_groupinfo  (BP_GROUP_INFO * pginfo);
/*
	IN:  fh
	     grpname 
	OUT: gh_p 
*/
int bp_gopen ( int64_t * gh_p,
		int64_t fh, 
		char * grpname);

/*
	IN:  fh
*/
int bp_gclose ( int64_t gh);

/*
	IN:  gh_p
*/
int bp_inq_group (int64_t gh, BP_GROUP_INFO *);
//int bp_inq_group (int64_t gh, int *nvar, char ** vnamelist);

/*
	IN:  gh
	     varname 
	     start 
	     readsize
	     timestep 
	OUT: var 
*/
int bp_get_var (int64_t gh,
		 char * varname,
		 void * var, 
		 int  * start,
		 int  * readsize, 
		 int    timestep);

/*
	IN:  gh
	     varname 
	OUT: type 
	     ndim 
	     is_timebased
	     dims  
*/
int bp_inq_var (int64_t gh, char * varname,
		 int * type,
		 int * ndim,
		 int * is_timebased,
		 int * dims);
const char * bp_type_to_string (int type);

// Fortran interface

void bp_fopen_ ( int64_t * fh,
        char * fname,
        void * comm,
        int * err,
        int fname_len
        );

void bp_fclose_ ( int64_t * fh, int * err);

void bp_inq_file_ ( int64_t * fh_p,
                    int * groups_count,
                    int * vars_count,
                    int * attrs_count,
                    int * tstart,
                    int * tstop,
                    void * gnamelist,
                    int * err,
                    int gnamelist_len);

void bp_gopen_ (int64_t * gh_p,int64_t * fh, 
		char * grpname, int *err, int grpname_len);

void bp_gclose_ ( int64_t * gh, int * err);

void bp_inq_group_ (int64_t * gh, int *vcnt, void *v, int * err, int);

void bp_get_var_ (int64_t * gh,
		 char * varname,
		 void * var, 
		 int  * start,
		 int  * readsize, 
		 int  * timestep,
         int  * err,
         int  varname_len);

void bp_inq_var_ (int64_t * gh_p, char * varname,
		 int * type,
		 int * ndim,
		 int * is_timebased,
		 int * dims,
         int * err,
         int varname_len);

#endif

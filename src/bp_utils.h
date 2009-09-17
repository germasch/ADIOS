#ifndef __BP_UTILS_H__
#define __BP_UTILS_H__

#include <stdio.h>
#include <sys/types.h>
#ifdef NOMPI
#   include "mpidummy.h"
#else
#   include "mpi.h"
#endif
#include "bp_types.h"
#define VARS_MINIHEADER_SIZE 10
#define ERRMSG_MAXLEN 256

void error(int errno, char *fmt, ...);
void bp_alloc_aligned (struct adios_bp_buffer_struct_v1 * b, uint64_t size);
void bp_realloc_aligned (struct adios_bp_buffer_struct_v1 * b, uint64_t size);
int bp_parse_characteristics (struct adios_bp_buffer_struct_v1 * b,
		  	      struct adios_index_var_struct_v1 ** root,
			      uint64_t j);
int bp_get_characteristics_data (void ** ptr_data,
				 void * buffer,
				 int data_size,
				 enum ADIOS_DATATYPES type);
int bp_read_close (struct adios_bp_buffer_struct_v1 * b);
int bp_read_open (const char * filename,
	 	  MPI_Comm comm, 
		  struct BP_FILE * fh);

const char * value_to_string (enum ADIOS_DATATYPES type, void * data);
//void bp_grouping ( struct BP_FILE * fh, uint64_t * gh);
uint64_t bp_get_type_size (enum ADIOS_DATATYPES type, void * var);

void print_process_group_index (
                         struct adios_index_process_group_struct_v1 * pg_root
                         );

void copy_data (void *dst, void *src,
        int idim,
        int ndim,
        uint64_t* size_in_dset,
        uint64_t* ldims,
        const uint64_t * readsize,
        uint64_t dst_stride,
        uint64_t src_stride,
        uint64_t dst_offset,
        uint64_t src_offset,
        uint64_t ele_num,
                int      size_of_type
                );

/* Return 1 if a < b wrt. the given type, otherwise 0 */
int adios_lt(int type, void *a, void *b);

#endif

/*
 * ADIOS is freely available under the terms of the BSD license described
 * in the COPYING file in the top level directory of this source distribution.
 *
 * Copyright (c) 2008 - 2009.  UT-BATTELLE, LLC. All rights reserved.
 */

/* ADIOS C Example: read global arrays from a BP file
 *
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
//#include "adios_read.h"
#include "adios_query.h"

void oneDefinedBox(ADIOS_FILE* bf ){

	  printf("\n=============== testing one single bounding box ===========\n");
	  int ndim = 3;
	  uint64_t start1[] = {0, 0, 0};
	  uint64_t count1[] = {64, 32,32};


	  ADIOS_SELECTION* box1 = adios_selection_boundingbox(ndim, start1, count1);
	  // rdm data is in the range btw 100 and 200, and this query constraint should return zero
	  const char* varName1 = "rdm";
	  enum ADIOS_PREDICATE_MODE op1 = ADIOS_GT;
      const char* value1 = "201";

      ADIOS_QUERY* q1 = adios_query_create(bf, varName1, box1, op1, value1);

      int timestep = 0;
      int64_t batchSize = 50;

      while (1) {
        ADIOS_SELECTION* currBatch = NULL;
        int hasMore =  adios_query_get_selection(q1, batchSize, box1, &currBatch);

        assert(currBatch->type ==ADIOS_SELECTION_POINTS);
        const ADIOS_SELECTION_POINTS_STRUCT * retrievedPts = &(currBatch->u.points);
        printf("retrieved points %" PRIu64 " \n",  retrievedPts->npoints);
        adios_selection_delete(currBatch);

        if (hasMore == 0) {
          break;
        }
      }

      adios_query_free(q1);
      adios_selection_delete(box1);
}


int main (int argc, char ** argv)
{

    char        filename [256];
    int         i, j, datasize, if_any;
    MPI_Comm    comm = MPI_COMM_WORLD;
    enum ADIOS_READ_METHOD method = ADIOS_READ_METHOD_BP;
    ADIOS_SELECTION * sel1, * sel2;
    ADIOS_VARCHUNK * chunk = 0;
    double * data = NULL;
    uint64_t start[2], count[2], npoints, * points;

    MPI_Init (&argc, &argv);

    if (argc < 2 ){
    	printf(" usage: %s {input bp file} \n", argv[0]);
    	return 1;
    }
    adios_read_init_method (method, comm, NULL);

    ADIOS_FILE * f = adios_read_open_file (argv[1], method, comm);

    if ( f == NULL){
    	printf(" can not open file %s \n", argv[1]);
    	return 1;
    }


    adios_query_init(ADIOS_QUERY_TOOL_ALACRITY);

    //====================== start to test ===================//
    oneDefinedBox(f );


    adios_read_close (f);

    adios_read_finalize_method (ADIOS_READ_METHOD_BP);


    MPI_Finalize ();
    return 0;
}
/*
    read_flexpath.c       
    Goal: to create evpath io connection layer in conjunction with 
    write/adios_flexpath.c

*/
// system libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>

// evpath libraries
#include <ffs.h>
#include <atl.h>
//#include <gen_thread.h>
#include <evpath.h>

// local libraries
#include "config.h"
#include "public/adios.h"
#include "public/adios_types.h"
#include "public/adios_read_v2.h"
#include "core/adios_read_hooks.h"
#include "core/adios_logger.h"
#include "public/adios_error.h"
#include "core/flexpath.h"

// conditional libraries
#ifdef DMALLOC
#include "dmalloc.h"
#endif

/*
 * Contains start & counts for each dimension for a writer_rank.
 */
typedef struct _array_displ
{
    int writer_rank;
    int ndims;
    uint64_t *start;
    uint64_t *count;    
}array_displacements;

typedef struct _bridge_info
{
    EVstone bridge_stone;
    EVsource flush_source;
    EVsource var_source;
    EVsource op_source;
    int their_num;
    char * contact;
    int created;
    int opened;
    int step;
    int scheduled;
}bridge_info;

typedef struct _flexpath_var_chunk
{
    int has_data;
    int rank;
    void *data;
    void *user_buf;
    uint64_t *local_bounds; // nodims
    uint64_t *global_bounds; // ndims
    uint64_t *global_offsets; // ndims
} flexpath_var_chunk, *flexpath_var_chunk_p;

typedef struct _flexpath_var
{
    int id;
    char *varname;
    char *varpath;
    enum ADIOS_DATATYPES type;
    uint64_t type_size; // type size, not arrays size
    int time_dim; // -1 means no time dimension
    int ndims;
    uint64_t *dims; // ndims size (if ndims>0)
    uint64_t array_size; // not relevant for scalars
    int num_chunks;
    flexpath_var_chunk *chunks;
    int num_displ;
    array_displacements *displ;
    ADIOS_SELECTION *sel;
    uint64_t start_position;
    struct _flexpath_var *next;
} flexpath_var;

typedef struct _flexpath_reader_file
{
    char * file_name;
    char * group_name; // assuming one group per file right now.

    EVstone stone;

    MPI_Comm comm;
    int rank;
    int size;
    int valid;

    int num_bridges;
    bridge_info *bridges;
    int writer_coordinator;

    int num_vars;
    flexpath_var * var_list;
    int num_gp; 
    evgroup * gp;

    int writer_finalized;
    int last_writer_step;
    int mystep;
    int num_sendees;
    int *sendees;
    int ackCondition;    
} flexpath_reader_file;

typedef struct _local_read_data
{
    // MPI stuff
    MPI_Comm fp_comm;
    int fp_comm_rank;
    int fp_comm_size;

    // EVPath stuff
    CManager fp_cm;
    EVstone stone;
    atom_t CM_TRANSPORT;
} flexpath_read_data;

flexpath_read_data* fp_read_data = NULL;

/********** Helper functions. **********/
void build_bridge(bridge_info* bridge) {
    attr_list contact_list = attr_list_from_string(bridge->contact);
    if(bridge->created == 0){
	bridge->bridge_stone =
	    EVcreate_bridge_action(fp_read_data->fp_cm,
				   contact_list,
				   (EVstone)bridge->their_num);

	bridge->flush_source =
	    EVcreate_submit_handle(fp_read_data->fp_cm,
				   bridge->bridge_stone,
				   flush_format_list);

	bridge->var_source =
	    EVcreate_submit_handle(fp_read_data->fp_cm,
				   bridge->bridge_stone,
				   var_format_list);

	bridge->op_source =
	    EVcreate_submit_handle(fp_read_data->fp_cm,
				   bridge->bridge_stone,
				   op_format_list);

	bridge->created = 1;
    }
}

void
free_displacements(array_displacements *displ, int num)
{
    if(displ){
	int i;
	for(i=0; i<num; i++){
	    free(displ[i].start);
	    free(displ[i].count);
	}
	free(displ);
    }
}

void
free_evgroup(evgroup *gp)
{
    EVreturn_event_buffer(fp_read_data->fp_cm, gp);
}

flexpath_var*
new_flexpath_var(const char * varname, int id, uint64_t type_size)
{
    flexpath_var* var = malloc(sizeof(flexpath_var));
    if(var == NULL){
	log_error("Error creating new var: %s\n", varname);
	return NULL;
    }
    
    memset(var, 0, sizeof(flexpath_var));
    var->varname = strdup(varname);
    var->time_dim = -1;
    var->id = id;
    var->type_size = type_size;
    var->displ = NULL;
    return var;
}

flexpath_reader_file*
new_flexpath_reader_file(const char * fname)
{
    flexpath_reader_file * fp = malloc(sizeof(flexpath_reader_file));
    if(fp == NULL){
	log_error("Cannot create data for new file.\n");
	exit(1);
    }
    memset(fp, 0, sizeof(flexpath_reader_file));
    fp->file_name = strdup(fname);
    fp->writer_coordinator = -1;
    fp->last_writer_step = -1;
    return fp;        
}

enum ADIOS_DATATYPES
ffs_type_to_adios_type(const char *ffs_type)
{
    if(!strcmp("integer", ffs_type))
	return adios_integer;
    else if(!strcmp("float", ffs_type))
	return adios_real;
    else if(!strcmp("string", ffs_type))
	return adios_string;
    else if(!strcmp("double", ffs_type))
	return adios_double;
    else if(!strcmp("char", ffs_type))
	return adios_byte;
    else
	return adios_unknown;
}

ADIOS_VARINFO* 
convert_var_info(flexpath_var * fpvar,
		 ADIOS_VARINFO * v, 
		 const char* varname,
		 const ADIOS_FILE *adiosfile)
{
    int i;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;    
    v->type = fpvar->type;
    v->ndim = fpvar->ndims;
    // needs to change. Has to get information from write.
    v->nsteps = 1;
    v->nblocks = malloc(sizeof(int)*v->nsteps);
    v->sum_nblocks = 1;    
    v->nblocks[0] = 1;
    v->statistics = NULL;
    v->blockinfo = NULL;

    if(v->ndim == 0){    
	int value_size = fpvar->type_size;
	v->value = malloc(value_size);
	if(!v->value) {
	    adios_error(err_no_memory, "Cannot allocate buffer in adios_read_datatap_inq_var()");
	    return NULL;
	}
	flexpath_var_chunk * chunk = &fpvar->chunks[0];
	memcpy(v->value, chunk->data, value_size);
	v->global = 0;	
    }else{ // arrays
	v->dims = (uint64_t*)malloc(v->ndim * sizeof(uint64_t));
	if(!v->dims) {
	    adios_error(err_no_memory, "Cannot allocate buffer in adios_read_datatap_inq_var()");
	    return NULL;
	}
	// broken.  fix.
	int cpysize = fpvar->ndims*sizeof(uint64_t);
	memcpy(v->dims, fpvar->dims, cpysize);
    }
    return v;
}

// compare used-provided varname with the full path name of variable
static int
compare_var_name (const char *varname, const flexpath_var *v)
{
    if (varname[0] == '/') { // varname is full path
        char fullpath[256];
        if(!strcmp(v->varpath, "/")) {
            sprintf(fullpath, "/%s", v->varname);
        }
        else {
            sprintf(fullpath, "%s/%s", v->varpath, v->varname);
        }
        return strcmp(fullpath, varname);
    }
    else { // varname doesn't include path
        return strcmp(v->varname, varname);
    }
}

flexpath_var *
find_fp_var(flexpath_var * var_list, const char * varname)
{
    while(var_list){
	if(!compare_var_name(varname, var_list)){
	    return var_list;
	}
	var_list = var_list->next;
    }
    return NULL;
}

global_var* 
find_gbl_var(global_var *vars, const char *name, int num_vars)
{
    int i;
    for(i=0; i<num_vars; i++){
	if(!strcmp(vars[i].name, name))
	    return &vars[i];
    }
    return NULL;
}

static FMField*
find_field_by_name (const char *name, const FMFieldList flist)
{
    FMField *f = flist;
    while (f->field_name != NULL)
    {
        if(!strcmp(name, f->field_name))
            return f;
        else
            f++;
    }
    return NULL;
}

/*
 * Finds the array displacements for a writer identified by its rank.
 */
array_displacements*
find_displacement(array_displacements* list, int rank, int num_displ){
    int i;
    for(i=0; i<num_displ; i++){
	if(list[i].writer_rank == rank)
	    return &list[i];	
    }
    return NULL;
}

uint64_t
linearize(uint64_t *sizes, int ndim)
{
    int size = 1;
    int i;
    for(i = 0; i<ndim - 1; i++){
	size *= sizes[i];
    }   
    return size;
}


uint64_t
copyarray(
    uint64_t *sizes, 
    uint64_t *sel_start, 
    uint64_t *sel_count, 
    int ndim,
    int elem_size,
    int writer_pos,
    char *writer_array,
    char *reader_array)
{
    if(ndim == 1){
	int start = elem_size * (writer_pos + sel_start[ndim-1]);
	int end = (start + (elem_size)*(sel_count[ndim-1]));
	memcpy(reader_array, writer_array + start, end-start);
	return end-start;
    }
    else{
	int end = sel_start[ndim-1] + sel_count[ndim-1];
	int i;
	int amt_copied = 0;
	for(i = sel_start[ndim-1]; i<end; i++){
	    int pos = linearize(sizes, ndim);    
	    pos *=i;    
	    amt_copied += copyarray(sizes, sel_start, sel_count, ndim-1,
				    elem_size, writer_pos+pos, writer_array, 
				    reader_array+amt_copied);
	}
	return amt_copied;
    }
}

array_displacements*
get_writer_displacements(
    int writer_rank, 
    const ADIOS_SELECTION * sel, 
    global_var* gvar)
{
    int ndims = sel->u.bb.ndim;
    array_displacements * displ = malloc(sizeof(array_displacements));
    displ->writer_rank = writer_rank;

    displ->start = malloc(sizeof(uint64_t) * ndims);
    displ->count = malloc(sizeof(uint64_t) * ndims);  
    memset(displ->start, 0, sizeof(uint64_t) * ndims);
    memset(displ->count, 0, sizeof(uint64_t) * ndims);

    displ->ndims = ndims;
    uint64_t *offsets = gvar->offsets[0].local_offsets;
    uint64_t *local_dims = gvar->offsets[0].local_dimensions;
    uint64_t pos = writer_rank * gvar->offsets[0].offsets_per_rank;

    int i;   
    for(i=0; i<ndims; i++){	
	if(sel->u.bb.start[i] >= offsets[pos+i]){
	    int start = sel->u.bb.start[i] - offsets[pos+i];
	    displ->start[i] = start;
	}
	if((sel->u.bb.start[i] + sel->u.bb.count[i] - 1) <= 
	   (offsets[pos+i] + local_dims[pos+i] - 1)){
	    int count = ((sel->u.bb.start[i] + sel->u.bb.count[i] - 1) - 
			 offsets[pos+i]) - displ->start[i] + 1;
	    displ->count[i] = count;

	    
	}else{
	    int count = (local_dims[pos+i] - 1) - displ->start[i] + 1;
	    displ->count[i] = count;
	}
      /* fp_log("DISP","start:%d\n", displ->start[i]);	 */
      /* fp_log("DISP","count %d\n",displ->count[i]); */
    }
    fp_log("DISP","\n");
    return displ;
}

int
need_writer(
    flexpath_reader_file *fp, 
    int writer, 
    const ADIOS_SELECTION* sel, 
    evgroup_ptr gp, 
    char* varname) 
{    
    //select var from group
    global_var * gvar = find_gbl_var(gp->vars, varname, gp->num_vars);

    //for each dimension
    int i=0;
    offset_struct var_offsets = gvar->offsets[0];
    for(i=0; i< var_offsets.offsets_per_rank; i++){      
	int pos = writer*(var_offsets.offsets_per_rank) + i;

        uint64_t sel_offset = sel->u.bb.start[i];
        uint64_t sel_size = sel->u.bb.count[i];        
	
        uint64_t rank_offset = var_offsets.local_offsets[pos];
        uint64_t rank_size = var_offsets.local_dimensions[pos];        
	
        if((rank_offset <= sel_offset) && (rank_offset + rank_size - 1 >=sel_offset)) {
	     log_debug("matched overlap type 1\n");
        }

        else if((rank_offset <= sel_offset + sel_size - 1) && \
		(rank_offset+rank_size>=sel_offset+sel_size-1)) {
        }else if((sel_offset <= rank_offset) && (rank_offset+rank_size<= sel_offset+sel_size-1)) {
        }else {
            return 0;
        }
    }
    return 1;
}

/*****************Messages to writer procs**********************/

void
send_open_msg(flexpath_reader_file *fp, int destination)
{

    if(!fp->bridges[destination].created){
	build_bridge(&(fp->bridges[destination]));
    }
    op_msg msg;
    msg.process_id = fp->rank;
    msg.file_name = strdup(fp->file_name);
    msg.step = fp->mystep;
    msg.type = OPEN_MSG;
    msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL);

    EVsubmit(fp->bridges[destination].op_source, &msg, NULL);    
    CMCondition_wait(fp_read_data->fp_cm, msg.condition);
    fp->bridges[destination].opened = 1;
}

void
send_close_msg(flexpath_reader_file *fp, int destination)
{
    if(!fp->bridges[destination].created){
	build_bridge(&(fp->bridges[destination]));
    }
    op_msg msg;
    msg.process_id = fp->rank;
    msg.file_name = strdup(fp->file_name);
    msg.step = fp->mystep;
    msg.type = CLOSE_MSG;
    msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL);

    EVsubmit(fp->bridges[destination].op_source, &msg, NULL);  
    CMCondition_wait(fp_read_data->fp_cm, msg.condition);
    fp->bridges[destination].opened = 0;
}

void
send_flush_msg(flexpath_reader_file *fp, int destination, Flush_type type)
{
    Flush_msg msg;
    msg.type = type;
    msg.rank = fp->rank;
    msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
    // maybe check to see if the bridge is create first.
    EVsubmit(fp->bridges[destination].flush_source, &msg, NULL);
    CMCondition_wait(fp_read_data->fp_cm, msg.condition);
}

void 
send_var_message(flexpath_reader_file *fp, int destination, char *varname)
{
        int i = 0;
        int found = 0;
        for(i=0; i<fp->num_sendees; i++) {
            if(fp->sendees[i]==destination) {
                found=1;
                break;
            }
        }
        if(!found) {
            fp->num_sendees+=1;
            fp->sendees=realloc(fp->sendees, fp->num_sendees*sizeof(int));
            fp->sendees[fp->num_sendees-1] = destination;
        }
        if(!fp->bridges[destination].created) {
            build_bridge(&(fp->bridges[destination]));
	}
	if(!fp->bridges[destination].opened){
	    fp->bridges[destination].opened = 1;
	    send_open_msg(fp, destination);
	}
	Var_msg var;
	var.rank = fp->rank;
	var.var_name = strdup(varname);
	EVsubmit(fp->bridges[destination].var_source, &var, NULL);    
}

/********** EVPath Handlers **********/

static int
update_step_msg_handler(
    CManager cm,
    void *vevent,
    void *client_data,
    attr_list attrs)
{
    fp_log("HANDLER", "update_step_msg_handler enter\n");
    update_step_msg *msg = (update_step_msg*)vevent;
    fprintf(stderr, "got update_step msg: %d\n", msg->step);
    ADIOS_FILE *adiosfile = (ADIOS_FILE*)client_data;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;    
    fprintf(stderr, "mystep: %d, last_writer_step: %d\n", 
	    fp->mystep, msg->step);
    fp->last_writer_step = msg->step;
    fp->writer_finalized = msg->finalized;
    adiosfile->last_step = msg->step;
    fprintf(stderr, "update_step_msg_handler, waiting on condition: %d\n", msg->condition);
    CMCondition_signal(fp_read_data->fp_cm, msg->condition);
    fp_log("HANDLER", "update_step_msg_handler depart\n");
    return 0;
}

static int 
op_msg_handler(CManager cm, void *vevent, void *client_data, attr_list attrs) {
    fp_log("HANDLER", "op_msg_handler enter\n");
    op_msg* msg = (op_msg*)vevent;    
    ADIOS_FILE *adiosfile = (ADIOS_FILE*)client_data;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    if(msg->type==ACK_MSG) {
	if(msg->condition != -1){
	    fp_log("HANDLER", "op_msg_handler signaling on condition: %d\n", msg->condition);
	    CMCondition_signal(fp_read_data->fp_cm, msg->condition);
	}
        //ackCondition = CMCondition_get(fp_read_data->fp_cm, NULL);
    }
    if(msg->type == EOS_MSG){	
	adios_errno = err_end_of_stream;
	CMCondition_signal(fp_read_data->fp_cm, msg->condition);
    }       
    fp_log("HANDLER", "op_msg_handler depart\n");
    return 0;
}

static int
group_msg_handler(CManager cm, void *vevent, void *client_data, attr_list attrs)
{
    fp_log("HANDLER", "\tgroup_msg_handler enter\n");
    EVtake_event_buffer(fp_read_data->fp_cm, vevent);
    evgroup * msg = (evgroup*)vevent;
    ADIOS_FILE *adiosfile = client_data;
    flexpath_reader_file * fp = (flexpath_reader_file*)adiosfile->fh;
    fp->gp = msg;
    int i;
    for(i = 0; i<msg->num_vars; i++){
	global_var *gblvar = &msg->vars[i];
	flexpath_var *fpvar = find_fp_var(fp->var_list, gblvar->name);
	if(fpvar){
	    offset_struct *offset = &gblvar->offsets[0];
	    uint64_t *local_dimensions = offset->local_dimensions;
	    uint64_t *local_offsets = offset->local_offsets;
	    uint64_t *global_dimensions = offset->global_dimensions;
	    fpvar->ndims = offset->offsets_per_rank;
	    fpvar->dims = malloc(sizeof(uint64_t)*fpvar->ndims);
	    int j;
	    for(j=0; j<fpvar->ndims; j++){
		fpvar->dims[j] = global_dimensions[j];
	    }
	}else{
	    adios_error(err_corrupted_variable, 
			"Mismatch between global variables and variables specified %s.",
			gblvar->name);
	}
    }

    CMCondition_signal(fp_read_data->fp_cm, msg->condition);    
    fp_log("HANDLER", "\tgroup_msg_handler depart\n");
    return 0;
}

static int
raw_handler(CManager cm, void *vevent, int len, void *client_data, attr_list attrs)
{
    fp_log("HANDLER", "raw_handler enter\n");
    ADIOS_FILE *adiosfile = client_data;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    FMContext context = CMget_FMcontext(cm);
    void *base_data = FMheader_skip(context, vevent);
    FMFormat format = FMformat_from_ID(context, vevent);  
    
    FMStructDescList struct_list = 
	FMcopy_struct_list(format_list_of_FMFormat(format));
    FMField *f = struct_list[0].field_list;

    /* setting up initial vars from the format list that comes along with the
       message. Message contains both an FFS description and the data. */
    if(fp->num_vars == 0){
	int var_count = 0;
	int i=0;       
	while(f->field_name != NULL){           
	    flexpath_var *curr_var = new_flexpath_var(f->field_name, 
						      var_count, 
						      f->field_size);
	    curr_var->num_chunks = 1;
	    curr_var->chunks = malloc(sizeof(flexpath_var_chunk)*curr_var->num_chunks);
	    memset(curr_var->chunks, 0, sizeof(flexpath_var_chunk)*curr_var->num_chunks);

	    curr_var->sel = NULL;
	    curr_var->type = ffs_type_to_adios_type(f->field_type);
	    flexpath_var * temp = fp->var_list;	
	    curr_var->next = temp;
	    fp->var_list = curr_var;
	    var_count++;
	    f++;
	}
		
	adiosfile->var_namelist = malloc(var_count * sizeof(char *));
	f = struct_list[0].field_list;  // f is top-level field list 	
	while(f->field_name != NULL) {
	    adiosfile->var_namelist[i++] = strdup(f->field_name);
	    f++;
	}
	adiosfile->nvars = var_count;
	fp->num_vars = var_count;       
    }

    int condition;
    int writer_rank;          
    get_int_attr(attrs, attr_atom_from_string("fp_dst_condition"), &condition);   
    get_int_attr(attrs, attr_atom_from_string(FP_RANK_ATTR_NAME), &writer_rank); 
    f = struct_list[0].field_list;
    char *curr_offset = NULL;
    int i = 0, j = 0;

    while(f->field_name){
        char atom_name[200] = "";
    	flexpath_var * var = find_fp_var(fp->var_list, strdup(f->field_name));	

    	if(!var){
    	    adios_error(err_file_open_error,
    			"file not opened correctly.  var does not match format.\n");
    	    return err_file_open_error;
    	}

        strcat(atom_name, f->field_name);
        strcat(atom_name, "_");
        strcat(atom_name, FP_NDIMS_ATTR_NAME);
        int num_dims;
        int i;
        get_int_attr(attrs, attr_atom_from_string(strdup(atom_name)), &num_dims);
    	flexpath_var_chunk * curr_chunk = &var->chunks[0];

    	// scalar
    	if(num_dims == 0){	   
	    curr_chunk->global_offsets = NULL;
	    curr_chunk->global_bounds = NULL;
	    curr_chunk->local_bounds = NULL;

	    void *tmp_data = get_FMfieldAddr_by_name(f, f->field_name, base_data);
	    if(var->sel){
		if(var->sel->type == ADIOS_SELECTION_WRITEBLOCK){
		    if(var->sel->u.block.index == writer_rank){
			memcpy(var->chunks[0].user_buf, tmp_data, f->field_size);
		    }
		}
		else{
		    adios_error(err_offset_required, 
				"Only scalars can be scheduled with write_block selection.\n");
		}
	    }
	    else{
		var->chunks[0].data = malloc(f->field_size);
		memcpy(var->chunks[0].data, tmp_data, f->field_size);	
	    }
	    
    	    // else it's an array
    	}else{
            if(!var->sel){ 
    		fp_log("SEL", "Variable has not yet been scheduled.  Cannot recieve data for it.\n");
    	    }
    	    else if(var->sel->type == ADIOS_SELECTION_WRITEBLOCK){
    		var->ndims = num_dims;
    		var->dims = malloc(sizeof(uint64_t)*num_dims);
    		if(var->sel->u.block.index == writer_rank){
    		    var->array_size = var->type_size;
    		    for(i=0; i<num_dims; i++){
    			char *dim;
    			atom_name[0] ='\0';
    			strcat(atom_name, f->field_name);
    			strcat(atom_name, "_");
    			strcat(atom_name, FP_DIM_ATTR_NAME);
    			strcat(atom_name, "_");
    			char dim_num[10] = "";
    			sprintf(dim_num, "%d", i+1);
    			strcat(atom_name, dim_num);
    			get_string_attr(attrs, attr_atom_from_string(atom_name), &dim);
    			FMField * temp_field = find_field_by_name(dim, f);
    			if(!temp_field){
    			    adios_error(err_invalid_varname,
    					"Could not find fieldname: %s\n",
    					dim);
    			}
    			else{    			    
    			    int *temp_data = get_FMfieldAddr_by_name(temp_field,
    								     temp_field->field_name,
    								     base_data);			    
			    uint64_t dim = (uint64_t)(*temp_data);
			    var->dims[i] = dim;
    			    var->array_size = var->array_size * var->dims[i];
    			}
    		    }    	       
		    void *arrays_data  = get_FMPtrField_by_name(f, f->field_name, base_data, 1);
		    memcpy(var->chunks[0].user_buf, arrays_data, var->array_size);
    		}
    	    }
    	    else if(var->sel->type == ADIOS_SELECTION_BOUNDINGBOX){
    		int i;
                global_var *gv = find_gbl_var(fp->gp->vars,
    					      var->varname,
    					      fp->gp->num_vars);                
    		array_displacements * disp = find_displacement(var->displ,
    							       writer_rank,
    							       var->num_displ);
		if(disp){
		    uint64_t *temp = gv->offsets[0].local_dimensions;
		    int offsets_per_rank = gv->offsets[0].offsets_per_rank;
		    uint64_t *writer_sizes = &temp[offsets_per_rank * writer_rank];
		    uint64_t *sel_start = disp->start;
		    uint64_t *sel_count = disp->count;
		    char *writer_array = (char*)get_FMPtrField_by_name(f, f->field_name, base_data, 1);
		    char *reader_array = (char*)var->chunks[0].user_buf;
		    uint64_t reader_start_pos = var->start_position;
		    var->start_position += copyarray(writer_sizes,
						     sel_start,
						     sel_count,
						     disp->ndims,
						     f->field_size,
						     0,
						     writer_array,
						     reader_array+reader_start_pos);		    
		}
    	    }
    	}
        j++;
        f++;
    }

    CMCondition_signal(fp_read_data->fp_cm, condition);
    fp_log("HANDLER", "raw_handler depart\n");
    return 0; 
}

/********** Core ADIOS Read functions. **********/

/*
 * Gathers basic MPI information; sets up reader CM.
 */
int
adios_read_flexpath_init_method (MPI_Comm comm, PairStruct* params)
{    
    setenv("CMSelfFormats", "1", 1);
    fp_read_data = malloc(sizeof(flexpath_read_data));     
    if(!fp_read_data) {
        adios_error(err_no_memory, "Cannot allocate memory for flexpath.");
        return -1;
    }
    memset(fp_read_data, 0, sizeof(flexpath_read_data));
    
    fp_read_data->CM_TRANSPORT = attr_atom_from_string("CM_TRANSPORT");
    attr_list listen_list = NULL;
    char * transport = NULL;
    transport = getenv("CMTransport");

    // setup MPI stuffs
    fp_read_data->fp_comm = comm;
    MPI_Comm_size(fp_read_data->fp_comm, &(fp_read_data->fp_comm_size));
    MPI_Comm_rank(fp_read_data->fp_comm, &(fp_read_data->fp_comm_rank));

    fp_read_data->fp_cm = CManager_create();
    if(transport == NULL){
	if(CMlisten(fp_read_data->fp_cm) == 0) {
	    log_error( "Flexpath ERROR: unable to initialize connection manager.\n");
	}
    }else{
	listen_list = create_attr_list();
	add_attr(listen_list, fp_read_data->CM_TRANSPORT, Attr_String, 
		 (attr_value)strdup(transport));
	CMlisten_specific(fp_read_data->fp_cm, listen_list);
    }
    if(CMfork_comm_thread(fp_read_data->fp_cm)) {/*log_debug( "forked\n");*/}
    return 0;
}

ADIOS_FILE*
adios_read_flexpath_open_file(const char * fname, MPI_Comm comm)
{
    adios_error (err_operation_not_supported,
                 "FLEXPATH staging method does not support file mode for reading. "
                 "Use adios_read_open() to open a staged dataset.\n");
    return NULL;
}

/*
 * Still have work to do here.  
 * Change it so that we can support the timeouts and lock_modes.
 */
/*
 * Sets up local data structure for series of reads on an adios file
 * - create evpath graph and structures
 * -- create evpath control stone (outgoing)
 * -- create evpath data stone (incoming)
 * -- rank 0 dumps contact info to file
 * -- create connections using contact info from file
 */
ADIOS_FILE*
adios_read_flexpath_open(const char * fname,
			 MPI_Comm comm,
			 enum ADIOS_LOCKMODE lock_mode,
			 float timeout_sec)
{
    ADIOS_FILE *adiosfile = malloc(sizeof(ADIOS_FILE));        
    if(!adiosfile){
	adios_error (err_no_memory, 
		     "Cannot allocate memory for file info.\n");
	return NULL;
    }    
    
    flexpath_reader_file *fp = new_flexpath_reader_file(fname);
	
    adios_errno = 0;
    fp->stone = EValloc_stone(fp_read_data->fp_cm);	
    fp->comm = comm;

    MPI_Comm_size(fp->comm, &(fp->size));
    MPI_Comm_rank(fp->comm, &(fp->rank));

    EVassoc_terminal_action(fp_read_data->fp_cm,
			    fp->stone,
			    op_format_list,
			    op_msg_handler,
			    adiosfile);       

    EVassoc_terminal_action(fp_read_data->fp_cm,
			    fp->stone,
			    update_step_msg_format_list,
			    update_step_msg_handler,
			    adiosfile);       


    EVassoc_terminal_action(fp_read_data->fp_cm,
			    fp->stone,
			    evgroup_format_list,
			    group_msg_handler,
			    adiosfile);

    EVassoc_raw_terminal_action(fp_read_data->fp_cm,
				fp->stone,
				raw_handler,
				adiosfile);

    /* Gather the contact info from the other readers
       and write it to a file. Create a ready file so
       that the writer knows it can parse this file. */
    char writer_ready_filename[200];
    char writer_info_filename[200];
    char reader_ready_filename[200];
    char reader_info_filename[200];
	
    sprintf(reader_ready_filename, "%s_%s", fname, READER_READY_FILE);
    sprintf(reader_info_filename, "%s_%s", fname, READER_CONTACT_FILE);
    sprintf(writer_ready_filename, "%s_%s", fname, WRITER_READY_FILE);
    sprintf(writer_info_filename, "%s_%s", fname, WRITER_CONTACT_FILE);
	
    char * string_list;
    char data_contact_info[50];
    string_list = attr_list_to_string(CMget_contact_list(fp_read_data->fp_cm));
    sprintf(&data_contact_info[0], "%d:%s", fp->stone, string_list);
    char * recvbuf;
    if(fp->rank == 0){	
	recvbuf = (char*)malloc(sizeof(char)*50*(fp->size));
    }

    MPI_Gather(data_contact_info, 50, MPI_CHAR, recvbuf,
	       50, MPI_CHAR, 0, fp->comm);

    if(fp->rank == 0){	
	// print our own contact information
	FILE * fp_out = fopen(reader_info_filename, "w");
	int i;
	if(!fp_out){	    
	    adios_error(err_file_open_error,
			"File for contact info could not be opened for writing.\n");
	    exit(1);
	}
	for(i=0; i<fp->size; i++) {
	    fprintf(fp_out,"%s\n", &recvbuf[i*50]);
	}
	fclose(fp_out);
	free(recvbuf);

	FILE * read_ready = fopen(reader_ready_filename, "w");
	fprintf(read_ready, "ready");
	fclose(read_ready);
    }
    MPI_Barrier(fp->comm);

    FILE * fp_in = fopen(writer_ready_filename,"r");
    while(!fp_in) {
	CMsleep(fp_read_data->fp_cm, 1);
	fp_in = fopen(writer_ready_filename, "r");
    }
    fclose(fp_in);

    fp_in = fopen(writer_info_filename, "r");
    while(!fp_in){
	CMsleep(fp_read_data->fp_cm, 1);
	fp_in = fopen(writer_info_filename, "r");
    }

    char in_contact[50] = "";
    //fp->bridges = malloc(sizeof(bridge_info));
    int num_bridges = 0;
    int their_stone;

    // change to read all numbers, dont create stones, turn bridge array into linked list
    while(fscanf(fp_in, "%d:%s", &their_stone, in_contact) != EOF){	
	fp->bridges = realloc(fp->bridges,
					  sizeof(bridge_info) * (num_bridges+1));
	fp->bridges[num_bridges].their_num = their_stone;
	fp->bridges[num_bridges].contact = strdup(in_contact);
	fp->bridges[num_bridges].created = 0;
	fp->bridges[num_bridges].step = 0;
	fp->bridges[num_bridges].opened = 0;
	fp->bridges[num_bridges].scheduled = 0;
	num_bridges++;
    }
    fclose(fp_in);
    fp->num_bridges = num_bridges;
    // clean up of writer's files
    MPI_Barrier(fp->comm);
    if(fp->rank == 0){
	unlink(writer_info_filename);
	unlink(writer_ready_filename);
    }	    
    adiosfile->fh = (uint64_t)fp;
    adiosfile->current_step = 0;
    
    /* Init with a writer to get initial scalar
       data so we can handle inq_var calls and
       also populate the ADIOS_FILE struct. */
    if(fp->size < num_bridges){
    	int mystart = (num_bridges/fp->size) * fp->rank;
    	int myend = (num_bridges/fp->size) * (fp->rank+1);
    	fp->writer_coordinator = mystart;
    	int z;
    	for(z=mystart; z<myend; z++){
    	    build_bridge(&fp->bridges[z]);
    	}
    }
    else{
	int writer_rank = fp->rank % num_bridges;
	build_bridge(&fp->bridges[writer_rank]);
	fp->writer_coordinator = writer_rank;
    }

    // requesting initial data.
    send_open_msg(fp, fp->writer_coordinator);
    send_flush_msg(fp, fp->writer_coordinator, DATA);
    send_flush_msg(fp, fp->writer_coordinator, EVGROUP);

    // this has to change. Writer needs to have some way of
    // taking the attributes out of the xml document
    // and sending them over ffs encoded. Not yet implemented.
    // the rest of this info for adiosfile gets filled in raw_handler.
    adiosfile->nattrs = 0;
    adiosfile->attr_namelist = NULL;
    // first step is at least one, otherwise raw_handler will not execute.
    // in reality, writer might be further along, so we might have to make
    // the writer explitly send across messages each time it calls close, to
    // indicate which timesteps are available. 
    adiosfile->last_step = 1;
    adiosfile->path = strdup(fname);
    // verifies these two fields. It's not BP, so no BP version.
    // It's a stream, so how can the file size be known?
    adiosfile->version = -1;
    adiosfile->file_size = 0;
    adios_errno = err_no_error;        
    return adiosfile;
}

int adios_read_flexpath_finalize_method ()
{
    return 0;
}

void adios_read_flexpath_release_step(ADIOS_FILE *adiosfile) {
    int i;
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    for(i=0; i<fp->num_bridges; i++) {
        if(fp->bridges[i].created && !fp->bridges[i].opened) {
	    send_open_msg(fp, i);
        }
    }
    free_evgroup(fp->gp);
    fp->gp = NULL;

    flexpath_var *tmpvars = fp->var_list;
    while(tmpvars){
	free_displacements(tmpvars->displ, tmpvars->num_displ);
	tmpvars->displ = NULL;
	if(tmpvars->sel)
	    free_selection(tmpvars->sel);
	tmpvars->sel = NULL;	
	for(i=0; i<tmpvars->num_chunks; i++){	   
	    flexpath_var_chunk chunk = tmpvars->chunks[i];	    
	    free(chunk.data);
	    free(chunk.local_bounds);
	    free(chunk.global_offsets);
	    free(chunk.global_bounds);
	    chunk.data = NULL;
	    chunk.global_offsets = NULL;
	    chunk.global_bounds = NULL;
	    chunk.has_data = 0;
	    chunk.rank = 0;
	}
	tmpvars = tmpvars->next;
    }
}

int 
adios_read_flexpath_advance_step(ADIOS_FILE *adiosfile, int last, float timeout_sec) 
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    MPI_Barrier(fp->comm);
    int i=0;
    for(i=0; i<fp->num_bridges; i++) {
        if(fp->bridges[i].created && fp->bridges[i].opened) {
	    send_close_msg(fp, i);           
	}
    }
    MPI_Barrier(fp->comm);

    fprintf(stderr, "old step: %d new step: %d\n", fp->mystep, fp->mystep+1);
    adiosfile->current_step++;
    fp->mystep = adiosfile->current_step;

    for(i=0; i<fp->num_bridges; i++){
	if(fp->bridges[i].created && !fp->bridges[i].opened){	    
	    send_open_msg(fp, i);
        }
    }   

    /* fprintf(stderr, "sending flush step\n"); */
    /* send_flush_msg(fp, fp->writer_coordinator, STEP); */
    
    /* //put this on a timer, so to speak, for timeout_sec */
    /* while(fp->mystep == fp->last_writer_step){ */
    /* 	if(fp->writer_finalized){ */
    /* 	    adios_errno = err_end_of_stream; */
    /* 	    return err_end_of_stream; */
    /* 	} */
    /* 	CMsleep(fp_read_data->fp_cm, 1); */
    /* 	fprintf(stderr, "sending flush step 123123123\n"); */
    /* 	send_flush_msg(fp, fp->writer_coordinator, STEP); */
    /* } */
	
    // need to remove selectors from each var now.

// should only happen if there are more steps available.
    // writer should have advanced.
    send_flush_msg(fp, fp->writer_coordinator, EVGROUP);
    send_flush_msg(fp, fp->writer_coordinator, DATA);
         
    return 0;
}

int adios_read_flexpath_close(ADIOS_FILE * fp)
{
    flexpath_reader_file *file = (flexpath_reader_file*)fp->fh;
    //send to each opened link
    int i;
    for(i = 0; i<file->num_bridges; i++){
        if(file->bridges[i].created && file->bridges[i].opened) {
	    send_close_msg(file, i);
        }
    }
    /*
    start to cleanup.  Clean up var_lists for now, as the
    data has already been copied over to ADIOS_VARINFO structs
    that the user maintains a copy of. 
    */
    flexpath_var * v = file->var_list;
    while(v){        	
    	// free chunks; data has already been copied to user
    	int i;	
    	for(i = 0; i<v->num_chunks; i++){    		    
    	    flexpath_var_chunk * c = &v->chunks[i];	    
	    if(!c)
		log_error("FLEXPATH: %s This should not happen! line %d\n",__func__,__LINE__);
	    free(c->global_bounds);		
	    free(c->global_offsets);
	    free(c->local_bounds);	    
	}
    	v=v->next;
    }
    return 0;
}

ADIOS_FILE *adios_read_flexpath_fopen(const char *fname, MPI_Comm comm) {
   return 0;
}

int adios_read_flexpath_is_var_timed(const ADIOS_FILE* fp, int varid) { return 0; }

void adios_read_flexpath_get_groupinfo(const ADIOS_FILE *fp, int *ngroups, char ***group_namelist, int **nvars_per_group, int **nattrs_per_group) {}

int adios_read_flexpath_check_reads(const ADIOS_FILE* fp, ADIOS_VARCHUNK** chunk) { log_debug( "flexpath:adios function check reads\n"); return 0; }

int adios_read_flexpath_perform_reads(const ADIOS_FILE *adiosfile, int blocking)
{
    flexpath_reader_file * fp = (flexpath_reader_file*)adiosfile->fh;
    int i;
    int num_sendees = fp->num_sendees;
    for(i = 0; i<num_sendees; i++)
    {
	int sendee = fp->sendees[i];	
	// need solution for blocking vs. non
	send_flush_msg(fp, sendee, DATA);
    }
   
    free(fp->sendees);
    fp->sendees = NULL;    
    fp->num_sendees = 0;
    return 0;
}
int
adios_read_flexpath_inq_var_blockinfo(const ADIOS_FILE* fp,
				      ADIOS_VARINFO* varinfo)
{ /*log_debug( "flexpath:adios function inq var block info\n");*/ return 0; }
int
adios_read_flexpath_inq_var_stat(const ADIOS_FILE* fp,
				 ADIOS_VARINFO* varinfo,
				 int per_step_stat,
				 int per_block_stat)
{ /*log_debug( "flexpath:adios function inq var stat\n");*/ return 0; }

//void adiosread_flexpath_release_step (ADIOS_FILE *fp);

int adios_read_flexpath_schedule_read_byid(const ADIOS_FILE *adiosfile,
					   const ADIOS_SELECTION *sel,
					   int varid,
					   int from_steps,
					   int nsteps,
					   void *data)
{   
    flexpath_reader_file * fp = (flexpath_reader_file*)adiosfile->fh;
    flexpath_var * var = fp->var_list;
    while(var){
        if(var->id == varid)
        	break;
        else
	    var=var->next;
    }
    if(!var){
        adios_error(err_invalid_varid,
		    "Invalid variable id: %d\n",
		    varid);
        return err_invalid_varid;
    }    
    //store the user allocated buffer.
    flexpath_var_chunk *chunk = &var->chunks[0];  
    chunk->user_buf = data;
    var->start_position = 0;
    if(nsteps != 1){
	adios_error (err_invalid_timestep,
                     "Only one step can be read from a stream at a time. "
                     "You requested % steps in adios_schedule_read()\n", 
		     nsteps);
        return err_invalid_timestep;
    }
    // this is done so that the user can do multiple schedule_read/perform_reads
    // within before doing release/advance step. Might need a better way to 
    // manage the ADIOS selections.
    if(var->sel)
	free_selection(var->sel);
    var->sel = copy_selection(sel);

    switch(var->sel->type)
    {
    case ADIOS_SELECTION_WRITEBLOCK:
    {
	int writer_index = var->sel->u.block.index;
	if(writer_index > fp->num_bridges){
	    adios_error(err_out_of_bound,
			"No process exists on the writer side matching the index.\n");
	    return err_out_of_bound;
	}
	send_var_message(fp, writer_index, var->varname);
	break;
    }
    case ADIOS_SELECTION_BOUNDINGBOX:
    {   
	free_displacements(var->displ, var->num_displ);
	var->displ = NULL;
        int j=0;
	int need_count = 0;
	array_displacements * all_disp = NULL;	
        for(j=0; j<fp->num_bridges; j++) {
            int destination=0;	    	    
            if(need_writer(fp, j, var->sel, fp->gp, var->varname)==1){           
		need_count++;
                destination = j;
		global_var *gvar = find_gbl_var(fp->gp->vars, var->varname, fp->gp->num_vars);
		// TODO: memory leak here. have to free these at some point.
		array_displacements * displ = get_writer_displacements(j, var->sel, gvar);
		all_disp = realloc(all_disp, sizeof(array_displacements)*need_count);
		all_disp[need_count-1] = *displ;
		send_var_message(fp, j, var->varname);
            }
	}
	var->displ = all_disp;
	var->num_displ = need_count;
        break;
    }
    case ADIOS_SELECTION_AUTO:
    {
	adios_error(err_operation_not_supported,
		    "ADIOS_SELECTION_AUTO not yet supported by flexpath.");
	break;
    }
    case ADIOS_SELECTION_POINTS:
    {
	adios_error(err_operation_not_supported,
		    "ADIOS_SELECTION_POINTS not yet supported by flexpath.");
	break;
    }
    }
    return 0;
}

int adios_read_flexpath_schedule_read(const ADIOS_FILE *adiosfile,
			const ADIOS_SELECTION * sel,
			const char * varname,
			int from_steps,
			int nsteps,
			void * data)
{
    fprintf(stderr, "schedule_read is called\n");
    return 0;
}

int adios_read_flexpath_fclose(ADIOS_FILE *adiosfile)
{
    //log_debug( "debug: adios_read_flexpath_fclose\n");
    return 0;
}

int * adios_read_flexpath_gopen (ADIOS_FILE *adiosfile, const char *grpname)
{
    //log_debug( "debug: adios_read_flexpath_gopen\n");
    return NULL;
}

int * adios_read_flexpath_gopen_byid (ADIOS_FILE *adiosfile, int grpid)
{
    //log_debug( "debug: adios_read_flexpath_gopen_byid\n");
    return NULL;
}

int adios_read_flexpath_gclose (int *gp)
{
    log_debug( "adios_read_flexpath_gclose\n");
    adios_errno = 0;
    int i;
//    free_namelist ((gp->var_namelist),gp->vars_count);
    free(gp);
    return 0;

}

int adios_read_flexpath_get_attr (int *gp, const char *attrname,
                                 enum ADIOS_DATATYPES *type,
                                 int *size, void **data)
{
    //log_debug( "debug: adios_read_flexpath_get_attr\n");
    // TODO: borrowed from dimes
    adios_error(err_invalid_read_method, 
		"adios_read_flexpath_get_attr is not implemented.");
    *size = 0;
    *type = adios_unknown;
    *data = 0;
    return adios_errno;
}

int 
adios_read_flexpath_get_attr_byid (const ADIOS_FILE *adiosfile, int attrid,
                                      enum ADIOS_DATATYPES *type,
                                      int *size, void **data)
{
//    log_debug( "debug: adios_read_flexpath_get_attr_byid\n");
    // TODO: borrowed from dimes
    adios_error(err_invalid_read_method, 
		"adios_read_flexpath_get_attr_byid is not implemented.");
    *size = 0;
    *type = adios_unknown;
    *data = 0;
    return adios_errno;
}

ADIOS_VARINFO* 
adios_read_flexpath_inq_var(const ADIOS_FILE * adiosfile, const char* varname)
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    ADIOS_VARINFO* v = malloc(sizeof(ADIOS_VARINFO));
    if(!v) {
        adios_error(err_no_memory, 
		    "Cannot allocate buffer in adios_read_datatap_inq_var()");
        return NULL;
    }
    memset(v, 0, sizeof(ADIOS_VARINFO));
    
    flexpath_var *fpvar = find_fp_var(fp->var_list, varname);
    if(fpvar) {
	v = convert_var_info(fpvar, v, varname, adiosfile);
	return v;
    }
    else {
        adios_error(err_invalid_varname, "Cannot find var %s\n", varname);
        return NULL;
    }
}

ADIOS_VARINFO* 
adios_read_flexpath_inq_var_byid (const ADIOS_FILE * adiosfile, int varid)
{
    flexpath_reader_file *fp = (flexpath_reader_file*)adiosfile->fh;
    if(varid >= 0 && varid < adiosfile->nvars) {
        return adios_read_flexpath_inq_var(adiosfile, adiosfile->var_namelist[varid]);
    }
    else {
        adios_error(err_invalid_varid, "FLEXPATH method: Cannot find var %d\n", varid);
        return NULL;
    }
}

void adios_read_flexpath_free_varinfo (ADIOS_VARINFO *adiosvar)
{
    //log_debug( "debug: adios_read_flexpath_free_varinfo\n");
    return;
}

int64_t adios_read_flexpath_read_var (int *gp, const char *varname,
                                     const uint64_t *start, const uint64_t *count,
                                     void *data)
{
    //log_debug( "debug: adios_read_flexpath_read_var\n");
    return (int64_t)0;
}

int64_t adios_read_flexpath_read_var_byid (int *gp, int varid,
                                          const uint64_t *start,
                                          const uint64_t *count,
                                          void *data)
{
    //log_debug( "debug: adios_read_flexpath_read_var_byid\n");
    return (int64_t)0;
}

void adios_read_flexpath_reset_dimension_order (const ADIOS_FILE *adiosfile, int is_fortran)
{
    //log_debug( "debug: adios_read_flexpath_reset_dimension_order\n");
    adios_error(err_invalid_read_method, "adios_read_flexpath_reset_dimension_order is not implemented.");
}

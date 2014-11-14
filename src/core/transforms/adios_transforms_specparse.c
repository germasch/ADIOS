/*
 * adios_transforms_specparse.c
 *
 *  Created on: May 20, 2013
 *      Author: David A. Boyuka II
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "core/transforms/adios_transforms_specparse.h"
#include "core/transforms/adios_transforms_hooks.h"
#include "core/util.h"

inline static char * strsplit(char *input, char split) {
    char *pos = strchr(input, split);
    if (!pos)
        return NULL;

    *pos = '\0';
    return pos + 1;
}

inline static int strcount(char *input, char chr) {
    int count = 0;
    while ((input = strchr(input, chr))) {
        count++;
        input++;
    }
    return count;
}

// var is a pointer type
#define MALLOC_ARRAY(var, type, count) ((var) = (type *)malloc(sizeof(type) * (count)))
#define MALLOC_VAR(var, type) MALLOC_ARRAY(var, type, 1)
#define CALLOC_ARRAY(var, type, count) ((var) = (type *)calloc((count), sizeof(type)))
#define CALLOC_VAR(var, type) CALLOC_ARRAY(var, type, 1)

//struct adios_transform_spec * adios_transform_parse_spec(const char *spec_str) {
struct adios_transform_spec * adios_transform_parse_spec(const char *spec_str, 
                                                         struct adios_transform_spec *spec_in) 
{
    //struct adios_transform_spec *spec = (struct adios_transform_spec *)malloc(sizeof(struct adios_transform_spec));
    struct adios_transform_spec *spec = spec_in;
    if (!spec_in) {
        MALLOC_VAR(spec, struct adios_transform_spec);
    }

    *spec = (struct adios_transform_spec){
        .transform_type = adios_transform_none,
        .transform_type_str = "",
        .param_count = 0,
        .params = NULL,
        .backing_str = NULL,
        .backing_str_len = 0,
    };

    // If the spec string is null/empty, stop now with a blank spec
    if (!spec_str || strcmp(spec_str, "") == 0)
        return spec;
    assert(spec_str && strcmp(spec_str, "") != 0);

    // Duplicate the spec string so we can chop it up
    char *new_spec_str = strdup(spec_str);
    spec->backing_str = new_spec_str;
    spec->backing_str_len = strlen(new_spec_str);

    // Mark the transform method string in the spec string (the beginning)
    spec->transform_type_str = new_spec_str;

    // Split off the parameters if present
    char *param_list = strsplit(new_spec_str, ':');

    // Parse the transform method string
    spec->transform_type = adios_transform_find_type_by_xml_alias(spec->transform_type_str);

    // If the transform type is unknown (error) or none, stop now and return
    if (spec->transform_type == adios_transform_unknown ||
        spec->transform_type == adios_transform_none)
        return spec;
    assert(spec->transform_type != adios_transform_unknown &&
           spec->transform_type != adios_transform_none);

    // If there is no parameter list, we are done
    if (!param_list)
        return spec;
    assert(param_list);

    spec->param_count = strcount(param_list, ',') + 1;
    MALLOC_ARRAY(spec->params, struct adios_transform_spec_kv_pair, spec->param_count);
    //spec->params = (typeof(spec->params))malloc(sizeof(*spec->params));

    struct adios_transform_spec_kv_pair *cur_kv = spec->params;
    char *cur_param;
    while (param_list) {
        cur_param = param_list;
        param_list = strsplit(param_list, ',');

        cur_kv->key = cur_param;
        cur_kv->value = strsplit(cur_param, '='); // NULL if no =

        cur_kv++;
    }

    return spec;
}

struct adios_transform_spec * adios_transform_spec_copy(const struct adios_transform_spec *src) {
	// Allocate a new transform spec struct
	struct adios_transform_spec *dst;
	MALLOC_VAR(dst, struct adios_transform_spec);

	// Copy some non-pointer fields
	dst->transform_type = src->transform_type;
	dst->backing_str_len = src->backing_str_len;

	// If there is a "backing string" field, copy it according to its recorded length
	// (note: strlen/strcpy won't work, as it probably contains \0s in the middle)
	dst->backing_str = src->backing_str ? (char*)bufdup(src->backing_str, 1, src->backing_str_len + 1) : NULL;

	// REBASE_STR: return a pointer into dst->backing_str with the same offset as the old pointer had into src->backing_str
	#define REBASE_STR(str) ((str) - src->backing_str + dst->backing_str)
	// REBASE_OR_DUP: if src->backing_str exists, return a REBASE_STR pointer; otherwise, duplicate the string and return that
	#define REBASE_OR_DUP(str) ((src->backing_str) ? REBASE_STR(str) : strdup(str))
	// REBASE_OR_DUP_OR_NULL: if str is non-NULL, same as REBASE_OR_DUP(str); otherwise, returns NULL
	#define REBASE_OR_DUP_OR_NULL(str) ((str) ? REBASE_OR_DUP(str) : NULL)

	// Copy the content of transform_type_str (either pointer-into-backing-str, a
	// duplicated string when no backing string exists, or NULL if it was NULL)
	dst->transform_type_str = REBASE_OR_DUP_OR_NULL(src->transform_type_str);

	// Copy transform user parameters if they exist
	if (src->params) {
        int i;

        // Initialize the key-value pair array
        dst->param_count = src->param_count;
        MALLOC_ARRAY(dst->params, struct adios_transform_spec_kv_pair, dst->param_count);

        // Copy each key-value pair
        for (i = 0; i < dst->param_count; i++) {
            const struct adios_transform_spec_kv_pair *src_kv = &src->params[i];
            struct adios_transform_spec_kv_pair *dst_kv = &dst->params[i];

            // Copy the key and/or value content
            dst_kv->key = REBASE_OR_DUP_OR_NULL(src_kv->key);
            dst_kv->value = REBASE_OR_DUP_OR_NULL(src_kv->value);
        }
	} else {
		dst->params = NULL;
	}

#undef REBASE_STR
#undef REBASE_OR_DUP
#undef REBASE_OR_DUP_OR_NULL

	// Finally, return the copied structure
    return dst;
}

#define FREE(x) {if(x)free(x);(x)=NULL;}
void adios_transform_free_spec(struct adios_transform_spec **spec_ptr) {
    struct adios_transform_spec *spec = *spec_ptr;
    FREE(spec->params);
    FREE(spec->backing_str);
    FREE(*spec_ptr)
}
#undef FREE

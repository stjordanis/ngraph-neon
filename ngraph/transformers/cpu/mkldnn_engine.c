/*******************************************************************************
* Copyright 2016 Nervana Systems Inc.
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "mkldnn.h"
#include "mkldnn_engine.h"
#include "mkldnn_util.h"

mkldnn_engine_t init_mkldnn_engine(void) {
  mkldnn_engine_t engine;
  MKL_CHECK(mkldnn_engine_create(&engine, mkldnn_cpu, 0 /* idx */));
  return engine;
}

size_t product(int *arr, size_t size) {
  size_t prod = 1;
  for (size_t i = 0; i < size; ++i) prod *= arr[i];
  return prod;
}

void set_mkl_dimensions(char *primitive_name, int *primitive_src_sizes,
                        int *primitive_dst_sizes, int *primitive_weights_sizes,
                        int *primitive_strides, int *primitive_padding,
                        int *mkl_src_sizes, int *mkl_dst_sizes,
                        int *mkl_weights_sizes, int *mkl_strides,
                        int *mkl_padding) {
  /* Flatten out the depth (D, M) dimension and reorder logical dimensions to
   * match MKLDNN */

  /* Input: C, D, H, W, N -> N, C, H, W */
  mkl_src_sizes[0] = primitive_src_sizes[4];
  mkl_src_sizes[1] = primitive_src_sizes[0];
  mkl_src_sizes[2] = primitive_src_sizes[2];
  mkl_src_sizes[3] = primitive_src_sizes[3];

  /* Output: K, M, P, Q, N -> N, K, P, Q */
  mkl_dst_sizes[0] = primitive_dst_sizes[4];
  mkl_dst_sizes[1] = primitive_dst_sizes[0];
  mkl_dst_sizes[2] = primitive_dst_sizes[2];
  mkl_dst_sizes[3] = primitive_dst_sizes[3];

  if (!strcmp(primitive_name, "convolution")) {
    /* Weights: C, T, R, S, K ->  O, I, H, W */
    mkl_weights_sizes[0] = primitive_weights_sizes[4];
    mkl_weights_sizes[1] = primitive_weights_sizes[0];
    mkl_weights_sizes[2] = primitive_weights_sizes[2];
    mkl_weights_sizes[3] = primitive_weights_sizes[3];
  }

  if (!strcmp(primitive_name, "pooling")){
     /* Kernel: J, T, R, S -> R, S */
    mkl_weights_sizes[0] = primitive_weights_sizes[2];
    mkl_weights_sizes[1] = primitive_weights_sizes[3];
  }

  mkl_strides[0] = primitive_strides[1];
  mkl_strides[1] = primitive_strides[2];

  mkl_padding[0] = primitive_padding[1];
  mkl_padding[1] = primitive_padding[2];
}

void destroy_mkldnn_engine(mkldnn_engine_t engine) {
  MKL_CHECK(mkldnn_engine_destroy(engine));
}


void create_mkldnn_tensor(int ndims, const int* dim_sizes,
                          mkldnn_data_type_t data_type,
                          mkldnn_memory_format_t fmt,
                          mkldnn_engine_t engine,
                          mkldnn_tensor* tensor) {
    tensor->ndims = ndims;
    for (int i = 0; i < ndims; i++) tensor->sizes[i] = dim_sizes[i];

    mkldnn_memory_desc_t md;
    MKL_CHECK(mkldnn_memory_desc_init(&md, ndims, dim_sizes, data_type, fmt));
    MKL_CHECK(mkldnn_memory_primitive_desc_create(&(tensor->desc), &md, engine));
    MKL_CHECK(mkldnn_primitive_create(&(tensor->prim), tensor->desc, NULL, NULL));
}

void create_mkldnn_tensor_from_pd(int ndims, const int* dim_sizes,
                        mkldnn_memory_desc_t* md,
                        mkldnn_engine_t engine,
                        mkldnn_tensor* tensor) {
    tensor->ndims = ndims;
    for (int i = 0; i < ndims; i++) tensor->sizes[i] = dim_sizes[i];

    MKL_CHECK(mkldnn_memory_primitive_desc_create(&(tensor->desc), md, engine));
    MKL_CHECK(mkldnn_primitive_create(&(tensor->prim), tensor->desc, NULL, NULL));
}

/* Create MKLDNN memory primitives */
void create_mkldnn_memory_primitive(uint32_t n_dim, const int *dims,
                                    mkldnn_memory_format_t user_fmt,
                                    mkldnn_data_type_t data_type,
                                    mkldnn_engine_t engine, float *data,
                                    mkldnn_primitive_t *memory) {
  mkldnn_memory_desc_t prim_md;
  mkldnn_primitive_desc_t user_pd;
  MKL_CHECK(
      mkldnn_memory_desc_init(&prim_md, n_dim, dims, data_type, user_fmt));
  MKL_CHECK(mkldnn_memory_primitive_desc_create(&user_pd, &prim_md, engine));
  MKL_CHECK(mkldnn_primitive_create(memory, user_pd, NULL, NULL));
  MKL_CHECK(mkldnn_memory_set_data_handle(*memory, data));
  MKL_CHECK(mkldnn_primitive_desc_destroy(user_pd));
}

void create_mkldnn_reorder_primitive(
    mkldnn_primitive_t *user_memory,               /** in */
    const_mkldnn_primitive_desc_t *prim_memory_pd, /** in */
    int dir_is_user_to_prim,         /** in: user -> prim or prim -> user */
    mkldnn_primitive_t *prim_memory, /** out: memory primitive created */
    mkldnn_primitive_t *reorder      /** out: reorder primitive created */
    ) {
  const_mkldnn_primitive_desc_t user_memory_pd;
  mkldnn_primitive_get_primitive_desc(*user_memory, &user_memory_pd);

  if (!mkldnn_memory_primitive_desc_equal(user_memory_pd, *prim_memory_pd)) {
    MKL_CHECK(
        mkldnn_primitive_create(prim_memory, *prim_memory_pd, NULL, NULL));
    mkldnn_primitive_desc_t reorder_pd;
    if (dir_is_user_to_prim) {
      MKL_CHECK(mkldnn_reorder_primitive_desc_create(
          &reorder_pd, user_memory_pd, *prim_memory_pd));
      mkldnn_primitive_at_t inputs = {*user_memory};
      const_mkldnn_primitive_t outputs[] = {*prim_memory};
      MKL_CHECK(mkldnn_primitive_create(reorder, reorder_pd, &inputs, outputs));
    } else {
      MKL_CHECK(mkldnn_reorder_primitive_desc_create(
          &reorder_pd, *prim_memory_pd, user_memory_pd));
      mkldnn_primitive_at_t inputs = {*prim_memory};
      const_mkldnn_primitive_t outputs[] = {*user_memory};
      MKL_CHECK(mkldnn_primitive_create(reorder, reorder_pd, &inputs, outputs));
    }
  } else {
    *prim_memory = NULL;
    *reorder = NULL;
  }
}

mkldnn_opkernel_t create_empty_kernel(int id) {
    mkldnn_opkernel_t op_kernel =
        (mkldnn_opkernel_t) malloc(sizeof(struct mkldnn_opkernel));
    op_kernel->id = id;
    op_kernel->num_inputs = 0;
    op_kernel->num_outputs = 0;
    op_kernel->net_size = 0;

    return op_kernel;
}

mkldnn_netlist_t create_mkldnn_netlist(void) {
  mkldnn_netlist_t mkldnn_net =
      (mkldnn_netlist_t)malloc(sizeof(struct mkldnn_netlist));
  mkldnn_net->net_size = 0;
  mkldnn_net->prim_desc_count = 0;
  mkldnn_net->prim_layouts_count = 0;
  mkldnn_net->prim_count = 0;
  mkldnn_net->buffer_count = 0;

  return mkldnn_net;
}

void destroy_mkldnn_netlist(mkldnn_netlist_t mkldnn_net) {
  for (int i = 0; i < mkldnn_net->prim_desc_count; i++) {
    MKL_CHECK(mkldnn_primitive_desc_destroy(mkldnn_net->prim_desc_list[i]));
  }

  for (int i = 0; i < mkldnn_net->prim_count; i++) {
    MKL_CHECK(mkldnn_primitive_destroy(mkldnn_net->prim_list[i]));
  }

  for (int i = 0; i < mkldnn_net->buffer_count; i++) {
    free(mkldnn_net->buffer_list[i]);
  }

  free(mkldnn_net);
}

void delete_mkldnn_tensor(mkldnn_tensor* tensor) {
    MKL_CHECK(mkldnn_primitive_desc_destroy(tensor->desc));
    MKL_CHECK(mkldnn_primitive_destroy(tensor->prim));
}

void delete_mkldnn_opkernel(mkldnn_opkernel_t opkernel) {
    for (int i = 0; i < opkernel->num_inputs; i++) {
        delete_mkldnn_tensor(&opkernel->inputs[i]);
        if (opkernel->reorder_i[i]) {
            delete_mkldnn_tensor(&opkernel->internal_inputs[i]);
            MKL_CHECK(mkldnn_primitive_destroy(opkernel->reorder_i[i]));
            free(opkernel->internal_inputs[i].buffer);
        }
    }
    for (int i = 0; i < opkernel->num_outputs; i++) {
        delete_mkldnn_tensor(&opkernel->outputs[i]);
        if (opkernel->reorder_o[i]) {
            delete_mkldnn_tensor(&opkernel->internal_outputs[i]);
            MKL_CHECK(mkldnn_primitive_destroy(opkernel->reorder_o[i]));
            free(opkernel->internal_outputs[i].buffer);
        }
    }
    MKL_CHECK(mkldnn_primitive_desc_destroy(opkernel->op_desc));
    MKL_CHECK(mkldnn_primitive_destroy(opkernel->op_prim));
}

void set_input_tensor_data_handle(mkldnn_opkernel_t opkernel, void* buffer, int index) {
    MKL_CHECK(mkldnn_memory_set_data_handle(opkernel->inputs[index].prim, buffer));
}

void set_output_tensor_data_handle(mkldnn_opkernel_t opkernel, void* buffer, int index) {
    MKL_CHECK(mkldnn_memory_set_data_handle(opkernel->outputs[index].prim, buffer));
}

void print_mkldnn_opkernel(mkldnn_opkernel_t opkernel) {
    void *buf;
    printf("ID: %d\n", opkernel->id);
    printf(" INPUTS\n");
    for (int i = 0; i < opkernel->num_inputs; i++) {
        mkldnn_memory_desc_t md = *mkldnn_primitive_desc_query_memory_d(opkernel->inputs[i].desc);
        mkldnn_memory_get_data_handle(opkernel->inputs[i].prim, &buf);
        printf("  Input %d (%p) md.format: %d", i, buf, md.format);
        if (opkernel->reorder_i[i]) {
            mkldnn_memory_desc_t i_md = *mkldnn_primitive_desc_query_memory_d(opkernel->internal_inputs[i].desc);
            mkldnn_memory_get_data_handle(opkernel->internal_inputs[i].prim, &buf);
            printf(" -> (%p) md.format: %d", buf, i_md.format);
        }
        printf("\n");
    }
    printf(" OUTPUTS\n");
    for (int i = 0; i < opkernel->num_outputs; i++) {
        mkldnn_memory_desc_t md = *mkldnn_primitive_desc_query_memory_d(opkernel->outputs[i].desc);
        mkldnn_memory_get_data_handle(opkernel->outputs[i].prim, &buf);
        printf("  Output %d (%p) md.format: %d", i, buf, md.format);
        if (opkernel->reorder_o[i]) {
            mkldnn_memory_desc_t i_md = *mkldnn_primitive_desc_query_memory_d(opkernel->internal_outputs[i].desc);
            mkldnn_memory_get_data_handle(opkernel->internal_outputs[i].prim, &buf);
            printf(" <- (%p) md.format: %d", buf, i_md.format);
        }
        printf("\n");
    }
}

void run_mkldnn_opkernel(mkldnn_opkernel_t opkernel) {
  //print_mkldnn_opkernel(opkernel);
  MKL_CHECK(mkldnn_stream_create(&opkernel->stream, mkldnn_eager));
  mkldnn_primitive_t error_primitive;
  mkldnn_status_t s =
      mkldnn_stream_submit(opkernel->stream, opkernel->net_size,
                           opkernel->net, &error_primitive);
  if (s != mkldnn_success) {
    printf(
        "[%s:%d] error: mkldnn_stream_submit returns %d, error_primitive: %p\n",
        __FILE__, __LINE__, s, error_primitive);
    exit(2);
  }
  MKL_CHECK(mkldnn_stream_wait(opkernel->stream, opkernel->net_size, NULL));
  MKL_CHECK(mkldnn_stream_destroy(opkernel->stream));
}

void run_mkldnn_netlist(mkldnn_netlist_t mkldnn_net) {
  MKL_CHECK(mkldnn_stream_create(&mkldnn_net->stream, mkldnn_eager));
  mkldnn_primitive_t error_primitive;
  mkldnn_status_t s =
      mkldnn_stream_submit(mkldnn_net->stream, mkldnn_net->net_size,
                           mkldnn_net->net, &error_primitive);
  if (s != mkldnn_success) {
    printf(
        "[%s:%d] error: mkldnn_stream_submit returns %d, error_primitive: %p\n",
        __FILE__, __LINE__, s, error_primitive);
    exit(2);
  }
  MKL_CHECK(mkldnn_stream_wait(mkldnn_net->stream, mkldnn_net->net_size, NULL));
  MKL_CHECK(mkldnn_stream_destroy(mkldnn_net->stream));
}

void cleanup_mkldnn(mkldnn_netlist_t mkldnn_net) {
  destroy_mkldnn_netlist(mkldnn_net);
}

mkldnn_primitive_desc_t query_opkernel_layout(mkldnn_opkernel_t opkernel, int index) {
    assert (index < opkernel->num_outputs);
    mkldnn_memory_desc_t md = *mkldnn_primitive_desc_query_memory_d(opkernel->outputs[index].desc);
    if (md.format == mkldnn_x || md.format == mkldnn_ihwo || md.format == mkldnn_chwn) { // Native formats
        return NULL;
    } else {
        return opkernel->outputs[index].desc;
    }
}

mkldnn_primitive_desc_t query_prim_layout(mkldnn_netlist_t mkldnn_net, int index) {
  return mkldnn_net->prim_layouts[index];
}

int compare_layouts(mkldnn_primitive_desc_t a, mkldnn_primitive_desc_t b) {
  if (mkldnn_memory_primitive_desc_equal(a, b))
    return 1;
  else
    return 0;
}

void create_mkldnn_reorder_kernel(
        mkldnn_engine_t engine,
        int ndims, int* dims, 
        mkldnn_data_type_t data_type,
        mkldnn_memory_format_t format,
        mkldnn_primitive_desc_t input_pd,
        mkldnn_primitive_desc_t output_pd,
        mkldnn_opkernel_t opkernel
        )
{
    mkldnn_memory_desc_t input_md, output_md;
    if (input_pd && output_pd) {
        input_md = *(mkldnn_primitive_desc_query_memory_d(input_pd));
        output_md = *(mkldnn_primitive_desc_query_memory_d(output_pd));
    } else if (input_pd) {
        input_md = *(mkldnn_primitive_desc_query_memory_d(input_pd));
        MKL_CHECK(mkldnn_memory_desc_init(&output_md, ndims, dims, data_type, format));
    } else if (output_pd) {
        output_md = *(mkldnn_primitive_desc_query_memory_d(output_pd));
        MKL_CHECK(mkldnn_memory_desc_init(&input_md, ndims, dims, data_type, format));
    } else {
        assert(0);
    }

    create_mkldnn_tensor_from_pd(ndims, dims, &input_md, engine, &(opkernel->inputs[0]));
    create_mkldnn_tensor_from_pd(ndims, dims, &output_md, engine, &(opkernel->outputs[0]));
    MKL_CHECK(mkldnn_reorder_primitive_desc_create(&opkernel->op_desc, opkernel->inputs[0].desc, opkernel->outputs[0].desc));
    mkldnn_primitive_at_t inputs[] = {opkernel->inputs[0].prim};
    const_mkldnn_primitive_t outputs[] = {opkernel->outputs[0].prim};
    MKL_CHECK(mkldnn_primitive_create(&opkernel->op_prim, opkernel->op_desc, inputs, outputs));
    opkernel->num_inputs = 1;
    opkernel->num_outputs = 1;
    opkernel->reorder_i[0] = NULL;
    opkernel->reorder_o[0] = NULL;
    opkernel->net[opkernel->net_size++] = opkernel->op_prim;
}

void* alloc_memory(size_t size, mkldnn_data_type_t data_type) {
    void* buf;
    switch (data_type) {
        case mkldnn_f32:
        case mkldnn_s32:
            buf = malloc(size*4);
            if (buf == NULL) {
                printf("Memory allocation failure. Could not allocate %lld bytes\n", size*4);
                exit(2);
            }
            return buf;
        default:
            assert(0);
            ;
    }
}

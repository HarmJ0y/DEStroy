#include "opencl_host.h"
#include "opencl_dyn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int gpu_init(gpu_context *ctx) {
    cl_int err;
    cl_uint num_platforms;
    cl_platform_id platforms[16];
    
    memset(ctx, 0, sizeof(gpu_context));
    
    if (opencl_load() != 0) {
        return -1;
    }
    
    err = p_clGetPlatformIDs(16, platforms, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        fprintf(stderr, "No OpenCL platforms found\n");
        return -1;
    }
    
    for (cl_uint i = 0; i < num_platforms; i++) {
        cl_uint num_devices;
        err = p_clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU, 1, &ctx->device, &num_devices);
        if (err == CL_SUCCESS && num_devices > 0) {
            ctx->platform = platforms[i];
            break;
        }
    }
    
    if (ctx->device == NULL) {
        fprintf(stderr, "No GPU device found\n");
        return -1;
    }
    
    p_clGetDeviceInfo(ctx->device, CL_DEVICE_NAME, sizeof(ctx->device_name), ctx->device_name, NULL);
    p_clGetDeviceInfo(ctx->device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(ctx->compute_units), &ctx->compute_units, NULL);
    p_clGetDeviceInfo(ctx->device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(ctx->max_work_group_size), &ctx->max_work_group_size, NULL);
    
    ctx->context = p_clCreateContext(NULL, 1, &ctx->device, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create context: %d\n", err);
        return -1;
    }
    
    ctx->queue = p_clCreateCommandQueue(ctx->context, ctx->device, 0, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create command queue: %d\n", err);
        return -1;
    }
    
    return 0;
}

int gpu_load_kernel(gpu_context *ctx, const char *filename, const char *kernel_name) {
    cl_int err;
    FILE *f;
    char *source;
    size_t source_len;
    
    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open kernel file: %s\n", filename);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    source_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    source = malloc(source_len + 1);
    if (!source) {
        fclose(f);
        return -1;
    }
    
    fread(source, 1, source_len, f);
    source[source_len] = '\0';
    fclose(f);
    
    ctx->program = p_clCreateProgramWithSource(ctx->context, 1, (const char **)&source, &source_len, &err);
    free(source);
    
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create program: %d\n", err);
        return -1;
    }
    
    err = p_clBuildProgram(ctx->program, 1, &ctx->device, "-cl-fast-relaxed-math -cl-mad-enable", NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t log_size;
        p_clGetProgramBuildInfo(ctx->program, ctx->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        char *log = malloc(log_size);
        p_clGetProgramBuildInfo(ctx->program, ctx->device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
        fprintf(stderr, "Build error:\n%s\n", log);
        free(log);
        return -1;
    }
    
    ctx->kernel = p_clCreateKernel(ctx->program, kernel_name, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create kernel '%s': %d\n", kernel_name, err);
        return -1;
    }
    
    return 0;
}

void gpu_cleanup(gpu_context *ctx) {
    gpu_free_verify_buffers(ctx);
    if (ctx->kernel) p_clReleaseKernel(ctx->kernel);
    if (ctx->program) p_clReleaseProgram(ctx->program);
    if (ctx->fa_kernel) p_clReleaseKernel(ctx->fa_kernel);
    if (ctx->fa_program) p_clReleaseProgram(ctx->fa_program);
    if (ctx->queue) p_clReleaseCommandQueue(ctx->queue);
    if (ctx->context) p_clReleaseContext(ctx->context);
    memset(ctx, 0, sizeof(gpu_context));
    opencl_unload();
}

int gpu_precompute(gpu_context *ctx, const uint8_t *hash,
                   uint32_t chain_len, uint32_t reduction_offset,
                   uint64_t plaintext_space_total,
                   uint64_t *output) {
    cl_int err;
    cl_mem hash_buf, output_buf;
    size_t global_work_size;
    
    uint32_t num_indices = chain_len - 1;
    global_work_size = num_indices;
    
    hash_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                8, (void *)hash, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create hash buffer: %d\n", err);
        return -1;
    }
    
    output_buf = p_clCreateBuffer(ctx->context, CL_MEM_WRITE_ONLY,
                                  num_indices * sizeof(cl_ulong), NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create output buffer: %d\n", err);
        p_clReleaseMemObject(hash_buf);
        return -1;
    }
    
    cl_ulong plaintext_space_mask = plaintext_space_total - 1;
    p_clSetKernelArg(ctx->kernel, 0, sizeof(cl_mem), &hash_buf);
    p_clSetKernelArg(ctx->kernel, 1, sizeof(cl_uint), &chain_len);
    p_clSetKernelArg(ctx->kernel, 2, sizeof(cl_uint), &reduction_offset);
    p_clSetKernelArg(ctx->kernel, 3, sizeof(cl_ulong), &plaintext_space_mask);
    p_clSetKernelArg(ctx->kernel, 4, sizeof(cl_mem), &output_buf);

    size_t local_work_size = 256;
    global_work_size = ((global_work_size + local_work_size - 1) / local_work_size) * local_work_size;
    err = p_clEnqueueNDRangeKernel(ctx->queue, ctx->kernel, 1, NULL,
                                    &global_work_size, &local_work_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to enqueue kernel: %d\n", err);
        p_clReleaseMemObject(hash_buf);
        p_clReleaseMemObject(output_buf);
        return -1;
    }
    
    p_clFinish(ctx->queue);
    
    err = p_clEnqueueReadBuffer(ctx->queue, output_buf, CL_TRUE, 0,
                                 num_indices * sizeof(cl_ulong), output, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to read output buffer: %d\n", err);
        p_clReleaseMemObject(hash_buf);
        p_clReleaseMemObject(output_buf);
        return -1;
    }
    
    p_clReleaseMemObject(hash_buf);
    p_clReleaseMemObject(output_buf);
    
    return num_indices;
}

int gpu_precompute_chunked(gpu_context *ctx, const uint8_t *hash,
                           uint32_t chain_len, uint32_t reduction_offset,
                           uint64_t plaintext_space_total,
                           uint64_t *output,
                           precompute_progress_fn cb, void *cb_data) {
    cl_int err;
    uint32_t num_indices = chain_len - 1;
    cl_ulong plaintext_space_mask = plaintext_space_total - 1;

    cl_mem hash_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        8, (void *)hash, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create hash buffer: %d\n", err);
        return -1;
    }

    cl_mem indices_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_WRITE,
                                           num_indices * sizeof(cl_ulong), NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create indices buffer: %d\n", err);
        p_clReleaseMemObject(hash_buf);
        return -1;
    }

    /* Create init kernel */
    cl_kernel init_kernel = p_clCreateKernel(ctx->program, "precompute_init", &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create precompute_init kernel: %d\n", err);
        p_clReleaseMemObject(hash_buf);
        p_clReleaseMemObject(indices_buf);
        return -1;
    }

    /* Create step kernel */
    cl_kernel step_kernel = p_clCreateKernel(ctx->program, "precompute_step", &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create precompute_step kernel: %d\n", err);
        p_clReleaseKernel(init_kernel);
        p_clReleaseMemObject(hash_buf);
        p_clReleaseMemObject(indices_buf);
        return -1;
    }

    /* Run init kernel */
    cl_uint pos_offset_zero = 0;
    p_clSetKernelArg(init_kernel, 0, sizeof(cl_mem), &hash_buf);
    p_clSetKernelArg(init_kernel, 1, sizeof(cl_uint), &chain_len);
    p_clSetKernelArg(init_kernel, 2, sizeof(cl_uint), &reduction_offset);
    p_clSetKernelArg(init_kernel, 3, sizeof(cl_ulong), &plaintext_space_mask);
    p_clSetKernelArg(init_kernel, 4, sizeof(cl_mem), &indices_buf);
    p_clSetKernelArg(init_kernel, 5, sizeof(cl_uint), &pos_offset_zero);

    size_t local_work_size = 256;
    size_t global_work_size = ((num_indices + local_work_size - 1) / local_work_size) * local_work_size;
    err = p_clEnqueueNDRangeKernel(ctx->queue, init_kernel, 1, NULL,
                                    &global_work_size, &local_work_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to enqueue init kernel: %d\n", err);
        p_clReleaseKernel(init_kernel);
        p_clReleaseKernel(step_kernel);
        p_clReleaseMemObject(hash_buf);
        p_clReleaseMemObject(indices_buf);
        return -1;
    }
    p_clFinish(ctx->queue);

    /* Set step kernel args that don't change */
    p_clSetKernelArg(step_kernel, 0, sizeof(cl_mem), &indices_buf);
    /* arg 1 = num_positions */
    p_clSetKernelArg(step_kernel, 1, sizeof(cl_uint), &num_indices);
    /* arg 2 = round_num: changes each iteration */
    /* arg 3 = stride */
    /* arg 4 = chain_len */
    p_clSetKernelArg(step_kernel, 4, sizeof(cl_uint), &chain_len);
    p_clSetKernelArg(step_kernel, 5, sizeof(cl_uint), &reduction_offset);
    p_clSetKernelArg(step_kernel, 6, sizeof(cl_ulong), &plaintext_space_mask);
    p_clSetKernelArg(step_kernel, 7, sizeof(cl_uint), &pos_offset_zero);

    uint32_t stride = 1024;
    p_clSetKernelArg(step_kernel, 3, sizeof(cl_uint), &stride);

    /* Position 0 needs chain_len-2 ops total -> ceil((chain_len-2)/stride) rounds.
     * Position pos needs chain_len-2-pos ops.
     * After round r, positions where (r+1)*stride >= chain_len-2-pos are done,
     * i.e., pos >= chain_len-2 - (r+1)*stride are done.
     * So active_count = min(num_indices, chain_len - 2 - round_num * stride). */
    uint32_t max_ops = chain_len - 2;
    uint32_t total_rounds = (max_ops + stride - 1) / stride;

    for (uint32_t round_num = 0; round_num < total_rounds; round_num++) {
        /* How many positions still have work this round? */
        uint32_t remaining = max_ops - round_num * stride;
        uint32_t active_count = remaining < num_indices ? remaining : num_indices;

        p_clSetKernelArg(step_kernel, 1, sizeof(cl_uint), &active_count);
        p_clSetKernelArg(step_kernel, 2, sizeof(cl_uint), &round_num);

        global_work_size = ((active_count + local_work_size - 1) / local_work_size) * local_work_size;
        err = p_clEnqueueNDRangeKernel(ctx->queue, step_kernel, 1, NULL,
                                        &global_work_size, &local_work_size, 0, NULL, NULL);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "Failed to enqueue step kernel round %u: %d\n", round_num, err);
            p_clReleaseKernel(init_kernel);
            p_clReleaseKernel(step_kernel);
            p_clReleaseMemObject(hash_buf);
            p_clReleaseMemObject(indices_buf);
            return -1;
        }

        /* Flush periodically, finish for progress reporting */
        if ((round_num + 1) % 50 == 0) {
            p_clFinish(ctx->queue);
            if (cb) cb(round_num + 1, total_rounds, cb_data);
        } else {
            p_clFlush(ctx->queue);
        }
    }
    p_clFinish(ctx->queue);
    if (cb) cb(total_rounds, total_rounds, cb_data);

    /* Read results */
    err = p_clEnqueueReadBuffer(ctx->queue, indices_buf, CL_TRUE, 0,
                                 num_indices * sizeof(cl_ulong), output, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to read output buffer: %d\n", err);
        p_clReleaseKernel(init_kernel);
        p_clReleaseKernel(step_kernel);
        p_clReleaseMemObject(hash_buf);
        p_clReleaseMemObject(indices_buf);
        return -1;
    }

    p_clReleaseKernel(init_kernel);
    p_clReleaseKernel(step_kernel);
    p_clReleaseMemObject(hash_buf);
    p_clReleaseMemObject(indices_buf);

    return num_indices;
}

int gpu_precompute_chunked_range(gpu_context *ctx, const uint8_t *hash,
                                  uint32_t chain_len, uint32_t reduction_offset,
                                  uint64_t plaintext_space_total,
                                  uint64_t *output, uint32_t pos_offset, uint32_t count,
                                  precompute_progress_fn cb, void *cb_data) {
    cl_int err;
    cl_ulong plaintext_space_mask = plaintext_space_total - 1;

    cl_mem hash_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        8, (void *)hash, &err);
    if (err != CL_SUCCESS) return -1;

    cl_mem indices_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_WRITE,
                                           count * sizeof(cl_ulong), NULL, &err);
    if (err != CL_SUCCESS) { p_clReleaseMemObject(hash_buf); return -1; }

    cl_kernel init_kernel = p_clCreateKernel(ctx->program, "precompute_init", &err);
    if (err != CL_SUCCESS) { p_clReleaseMemObject(hash_buf); p_clReleaseMemObject(indices_buf); return -1; }

    cl_kernel step_kernel = p_clCreateKernel(ctx->program, "precompute_step", &err);
    if (err != CL_SUCCESS) {
        p_clReleaseKernel(init_kernel); p_clReleaseMemObject(hash_buf);
        p_clReleaseMemObject(indices_buf); return -1;
    }

    /* Init kernel: compute initial index for each position in [pos_offset, pos_offset+count) */
    p_clSetKernelArg(init_kernel, 0, sizeof(cl_mem), &hash_buf);
    p_clSetKernelArg(init_kernel, 1, sizeof(cl_uint), &chain_len);
    p_clSetKernelArg(init_kernel, 2, sizeof(cl_uint), &reduction_offset);
    p_clSetKernelArg(init_kernel, 3, sizeof(cl_ulong), &plaintext_space_mask);
    p_clSetKernelArg(init_kernel, 4, sizeof(cl_mem), &indices_buf);
    p_clSetKernelArg(init_kernel, 5, sizeof(cl_uint), &pos_offset);

    size_t local_work_size = 256;
    size_t global_work_size = ((count + local_work_size - 1) / local_work_size) * local_work_size;
    err = p_clEnqueueNDRangeKernel(ctx->queue, init_kernel, 1, NULL,
                                    &global_work_size, &local_work_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) goto cleanup;
    p_clFinish(ctx->queue);

    /* Step kernel args */
    p_clSetKernelArg(step_kernel, 0, sizeof(cl_mem), &indices_buf);
    p_clSetKernelArg(step_kernel, 4, sizeof(cl_uint), &chain_len);
    p_clSetKernelArg(step_kernel, 5, sizeof(cl_uint), &reduction_offset);
    p_clSetKernelArg(step_kernel, 6, sizeof(cl_ulong), &plaintext_space_mask);
    p_clSetKernelArg(step_kernel, 7, sizeof(cl_uint), &pos_offset);

    uint32_t stride = 1024;
    p_clSetKernelArg(step_kernel, 3, sizeof(cl_uint), &stride);

    /* Position pos_offset needs chain_len-2-pos_offset ops (most in this range) */
    uint32_t max_ops = chain_len - 2 - pos_offset;
    uint32_t total_rounds = (max_ops + stride - 1) / stride;

    for (uint32_t round_num = 0; round_num < total_rounds; round_num++) {
        uint32_t remaining = max_ops - round_num * stride;
        uint32_t active_count = remaining < count ? remaining : count;

        p_clSetKernelArg(step_kernel, 1, sizeof(cl_uint), &active_count);
        p_clSetKernelArg(step_kernel, 2, sizeof(cl_uint), &round_num);

        global_work_size = ((active_count + local_work_size - 1) / local_work_size) * local_work_size;
        err = p_clEnqueueNDRangeKernel(ctx->queue, step_kernel, 1, NULL,
                                        &global_work_size, &local_work_size, 0, NULL, NULL);
        if (err != CL_SUCCESS) goto cleanup;

        if ((round_num + 1) % 50 == 0) {
            p_clFinish(ctx->queue);
            if (cb) cb(round_num + 1, total_rounds, cb_data);
        } else {
            p_clFlush(ctx->queue);
        }
    }
    p_clFinish(ctx->queue);
    if (cb) cb(total_rounds, total_rounds, cb_data);

    err = p_clEnqueueReadBuffer(ctx->queue, indices_buf, CL_TRUE, 0,
                                 count * sizeof(cl_ulong), output, 0, NULL, NULL);

cleanup:
    p_clReleaseKernel(init_kernel);
    p_clReleaseKernel(step_kernel);
    p_clReleaseMemObject(hash_buf);
    p_clReleaseMemObject(indices_buf);
    return (err == CL_SUCCESS) ? (int)count : -1;
}

int gpu_load_false_alarm_kernel(gpu_context *ctx, const char *filename) {
    cl_int err;
    FILE *f;
    char *source;
    size_t source_len;
    
    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open kernel file: %s\n", filename);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    source_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    source = malloc(source_len + 1);
    if (!source) {
        fclose(f);
        return -1;
    }
    
    fread(source, 1, source_len, f);
    source[source_len] = '\0';
    fclose(f);
    
    ctx->fa_program = p_clCreateProgramWithSource(ctx->context, 1, (const char **)&source, &source_len, &err);
    free(source);
    
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create false alarm program: %d\n", err);
        return -1;
    }
    
    err = p_clBuildProgram(ctx->fa_program, 1, &ctx->device, "-cl-fast-relaxed-math -cl-mad-enable", NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t log_size;
        p_clGetProgramBuildInfo(ctx->fa_program, ctx->device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        char *log = malloc(log_size);
        p_clGetProgramBuildInfo(ctx->fa_program, ctx->device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
        fprintf(stderr, "False alarm kernel build error:\n%s\n", log);
        free(log);
        return -1;
    }
    
    ctx->fa_kernel = p_clCreateKernel(ctx->fa_program, "check_false_alarms", &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create false alarm kernel: %d\n", err);
        return -1;
    }
    
    return 0;
}

int gpu_check_false_alarms(gpu_context *ctx,
                           const uint8_t *target_hash,
                           uint64_t *start_indices,
                           uint32_t *positions,
                           uint32_t num_candidates,
                           uint32_t reduction_offset,
                           uint64_t plaintext_space_total,
                           uint8_t *found_key) {
    cl_int err;
    
    if (num_candidates == 0) {
        return 0;
    }
    
    cl_mem hash_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        8, (void *)target_hash, &err);
    if (err != CL_SUCCESS) return -1;
    
    cl_mem start_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         num_candidates * sizeof(uint64_t), start_indices, &err);
    if (err != CL_SUCCESS) { p_clReleaseMemObject(hash_buf); return -1; }
    
    cl_mem pos_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       num_candidates * sizeof(uint32_t), positions, &err);
    if (err != CL_SUCCESS) { p_clReleaseMemObject(hash_buf); p_clReleaseMemObject(start_buf); return -1; }
    
    int found_idx_init = -1;
    cl_mem found_idx_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                             sizeof(int), &found_idx_init, &err);
    if (err != CL_SUCCESS) { 
        p_clReleaseMemObject(hash_buf); 
        p_clReleaseMemObject(start_buf); 
        p_clReleaseMemObject(pos_buf); 
        return -1; 
    }
    
    cl_mem found_key_buf = p_clCreateBuffer(ctx->context, CL_MEM_WRITE_ONLY, 7, NULL, &err);
    if (err != CL_SUCCESS) { 
        p_clReleaseMemObject(hash_buf); 
        p_clReleaseMemObject(start_buf); 
        p_clReleaseMemObject(pos_buf); 
        p_clReleaseMemObject(found_idx_buf);
        return -1; 
    }
    
    cl_ulong ps_mask = plaintext_space_total - 1;
    p_clSetKernelArg(ctx->fa_kernel, 0, sizeof(cl_mem), &hash_buf);
    p_clSetKernelArg(ctx->fa_kernel, 1, sizeof(cl_mem), &start_buf);
    p_clSetKernelArg(ctx->fa_kernel, 2, sizeof(cl_mem), &pos_buf);
    p_clSetKernelArg(ctx->fa_kernel, 3, sizeof(cl_uint), &num_candidates);
    p_clSetKernelArg(ctx->fa_kernel, 4, sizeof(cl_uint), &reduction_offset);
    p_clSetKernelArg(ctx->fa_kernel, 5, sizeof(cl_ulong), &ps_mask);
    p_clSetKernelArg(ctx->fa_kernel, 6, sizeof(cl_mem), &found_idx_buf);
    p_clSetKernelArg(ctx->fa_kernel, 7, sizeof(cl_mem), &found_key_buf);

    size_t global_work_size = num_candidates;
    size_t fa_local_work_size = 256;
    global_work_size = ((global_work_size + fa_local_work_size - 1) / fa_local_work_size) * fa_local_work_size;
    err = p_clEnqueueNDRangeKernel(ctx->queue, ctx->fa_kernel, 1, NULL,
                                    &global_work_size, &fa_local_work_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to enqueue false alarm kernel: %d\n", err);
        p_clReleaseMemObject(hash_buf);
        p_clReleaseMemObject(start_buf);
        p_clReleaseMemObject(pos_buf);
        p_clReleaseMemObject(found_idx_buf);
        p_clReleaseMemObject(found_key_buf);
        return -1;
    }
    
    p_clFinish(ctx->queue);
    
    int found_idx;
    p_clEnqueueReadBuffer(ctx->queue, found_idx_buf, CL_TRUE, 0, sizeof(int), &found_idx, 0, NULL, NULL);
    
    int result = 0;
    if (found_idx >= 0) {
        p_clEnqueueReadBuffer(ctx->queue, found_key_buf, CL_TRUE, 0, 7, found_key, 0, NULL, NULL);
        result = 1;
    }
    
    p_clReleaseMemObject(hash_buf);
    p_clReleaseMemObject(start_buf);
    p_clReleaseMemObject(pos_buf);
    p_clReleaseMemObject(found_idx_buf);
    p_clReleaseMemObject(found_key_buf);
    
    return result;
}

int gpu_alloc_verify_buffers(gpu_context *ctx, uint32_t capacity) {
    cl_int err;

    gpu_free_verify_buffers(ctx);

    ctx->fa_hash_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY, 8, NULL, &err);
    if (err != CL_SUCCESS) return -1;

    ctx->fa_start_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY,
                                          capacity * sizeof(uint64_t), NULL, &err);
    if (err != CL_SUCCESS) goto fail;

    ctx->fa_pos_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_ONLY,
                                        capacity * sizeof(uint32_t), NULL, &err);
    if (err != CL_SUCCESS) goto fail;

    ctx->fa_found_idx_buf = p_clCreateBuffer(ctx->context, CL_MEM_READ_WRITE,
                                              sizeof(int), NULL, &err);
    if (err != CL_SUCCESS) goto fail;

    ctx->fa_found_key_buf = p_clCreateBuffer(ctx->context, CL_MEM_WRITE_ONLY, 7, NULL, &err);
    if (err != CL_SUCCESS) goto fail;

    ctx->fa_buf_capacity = capacity;
    return 0;

fail:
    gpu_free_verify_buffers(ctx);
    return -1;
}

void gpu_free_verify_buffers(gpu_context *ctx) {
    if (ctx->fa_hash_buf) { p_clReleaseMemObject(ctx->fa_hash_buf); ctx->fa_hash_buf = NULL; }
    if (ctx->fa_start_buf) { p_clReleaseMemObject(ctx->fa_start_buf); ctx->fa_start_buf = NULL; }
    if (ctx->fa_pos_buf) { p_clReleaseMemObject(ctx->fa_pos_buf); ctx->fa_pos_buf = NULL; }
    if (ctx->fa_found_idx_buf) { p_clReleaseMemObject(ctx->fa_found_idx_buf); ctx->fa_found_idx_buf = NULL; }
    if (ctx->fa_found_key_buf) { p_clReleaseMemObject(ctx->fa_found_key_buf); ctx->fa_found_key_buf = NULL; }
    ctx->fa_buf_capacity = 0;
}

int gpu_check_false_alarms_fast(gpu_context *ctx,
                                const uint8_t *target_hash,
                                uint64_t *start_indices,
                                uint32_t *positions,
                                uint32_t num_candidates,
                                uint32_t reduction_offset,
                                uint64_t plaintext_space_total,
                                uint8_t *found_key) {
    cl_int err;

    if (num_candidates == 0) return 0;

    if (!ctx->fa_hash_buf || num_candidates > ctx->fa_buf_capacity) {
        /* Fallback to original if buffers not allocated or too small */
        return gpu_check_false_alarms(ctx, target_hash, start_indices, positions,
                                       num_candidates, reduction_offset,
                                       plaintext_space_total, found_key);
    }

    /* Write data to pre-allocated buffers */
    err = p_clEnqueueWriteBuffer(ctx->queue, ctx->fa_hash_buf, CL_FALSE, 0,
                                  8, target_hash, 0, NULL, NULL);
    if (err != CL_SUCCESS) return -1;

    err = p_clEnqueueWriteBuffer(ctx->queue, ctx->fa_start_buf, CL_FALSE, 0,
                                  num_candidates * sizeof(uint64_t), start_indices, 0, NULL, NULL);
    if (err != CL_SUCCESS) return -1;

    err = p_clEnqueueWriteBuffer(ctx->queue, ctx->fa_pos_buf, CL_FALSE, 0,
                                  num_candidates * sizeof(uint32_t), positions, 0, NULL, NULL);
    if (err != CL_SUCCESS) return -1;

    int found_idx_init = -1;
    err = p_clEnqueueWriteBuffer(ctx->queue, ctx->fa_found_idx_buf, CL_FALSE, 0,
                                  sizeof(int), &found_idx_init, 0, NULL, NULL);
    if (err != CL_SUCCESS) return -1;

    cl_ulong ps_mask = plaintext_space_total - 1;
    p_clSetKernelArg(ctx->fa_kernel, 0, sizeof(cl_mem), &ctx->fa_hash_buf);
    p_clSetKernelArg(ctx->fa_kernel, 1, sizeof(cl_mem), &ctx->fa_start_buf);
    p_clSetKernelArg(ctx->fa_kernel, 2, sizeof(cl_mem), &ctx->fa_pos_buf);
    p_clSetKernelArg(ctx->fa_kernel, 3, sizeof(cl_uint), &num_candidates);
    p_clSetKernelArg(ctx->fa_kernel, 4, sizeof(cl_uint), &reduction_offset);
    p_clSetKernelArg(ctx->fa_kernel, 5, sizeof(cl_ulong), &ps_mask);
    p_clSetKernelArg(ctx->fa_kernel, 6, sizeof(cl_mem), &ctx->fa_found_idx_buf);
    p_clSetKernelArg(ctx->fa_kernel, 7, sizeof(cl_mem), &ctx->fa_found_key_buf);

    size_t local_work_size = 256;
    size_t global_work_size = ((num_candidates + local_work_size - 1) / local_work_size) * local_work_size;
    err = p_clEnqueueNDRangeKernel(ctx->queue, ctx->fa_kernel, 1, NULL,
                                    &global_work_size, &local_work_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) return -1;

    p_clFinish(ctx->queue);

    int found_idx;
    p_clEnqueueReadBuffer(ctx->queue, ctx->fa_found_idx_buf, CL_TRUE, 0,
                           sizeof(int), &found_idx, 0, NULL, NULL);

    if (found_idx >= 0) {
        p_clEnqueueReadBuffer(ctx->queue, ctx->fa_found_key_buf, CL_TRUE, 0,
                               7, found_key, 0, NULL, NULL);
        return 1;
    }

    return 0;
}

int gpu_list(void) {
    if (opencl_load() != 0) {
        return -1;
    }
    
    cl_platform_id platforms[8];
    cl_uint num_platforms;
    p_clGetPlatformIDs(8, platforms, &num_platforms);
    
    int gpu_index = 0;
    for (cl_uint p = 0; p < num_platforms; p++) {
        cl_device_id devices[16];
        cl_uint num_devices;
        if (p_clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 16, devices, &num_devices) != CL_SUCCESS)
            continue;
        
        for (cl_uint d = 0; d < num_devices; d++) {
            char name[256];
            cl_uint cus;
            p_clGetDeviceInfo(devices[d], CL_DEVICE_NAME, sizeof(name), name, NULL);
            p_clGetDeviceInfo(devices[d], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cus), &cus, NULL);
            printf("  [%d] %s (%u CUs)\n", gpu_index, name, cus);
            gpu_index++;
        }
    }
    
    if (gpu_index == 0) {
        printf("  (none found)\n");
    }
    
    opencl_unload();
    return gpu_index;
}

int gpu_init_index(gpu_context *ctx, int target_index) {
    cl_int err;
    cl_platform_id platforms[8];
    cl_uint num_platforms;
    
    memset(ctx, 0, sizeof(gpu_context));
    
    if (opencl_load() != 0) {
        return -1;
    }
    
    p_clGetPlatformIDs(8, platforms, &num_platforms);
    
    int gpu_index = 0;
    for (cl_uint p = 0; p < num_platforms; p++) {
        cl_device_id devices[16];
        cl_uint num_devices;
        if (p_clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 16, devices, &num_devices) != CL_SUCCESS)
            continue;
        
        for (cl_uint d = 0; d < num_devices; d++) {
            if (gpu_index == target_index) {
                ctx->platform = platforms[p];
                ctx->device = devices[d];
                goto found;
            }
            gpu_index++;
        }
    }
    
    fprintf(stderr, "GPU index %d not found (have %d)\n", target_index, gpu_index);
    return -1;

found:
    p_clGetDeviceInfo(ctx->device, CL_DEVICE_NAME, sizeof(ctx->device_name), ctx->device_name, NULL);
    p_clGetDeviceInfo(ctx->device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(ctx->compute_units), &ctx->compute_units, NULL);
    p_clGetDeviceInfo(ctx->device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(ctx->max_work_group_size), &ctx->max_work_group_size, NULL);
    
    ctx->context = p_clCreateContext(NULL, 1, &ctx->device, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create context: %d\n", err);
        return -1;
    }
    
    ctx->queue = p_clCreateCommandQueue(ctx->context, ctx->device, 0, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Failed to create command queue: %d\n", err);
        return -1;
    }
    
    return 0;
}

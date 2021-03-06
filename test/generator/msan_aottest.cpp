#ifdef __MINGW32__
#include <stdio.h>
// Mingw doesn't support weak linkage
int main(int argc, char **argv) {
    printf("Skipping test on mingw");
    return 0;
}
#else
#include "HalideRuntime.h"
#include "HalideBuffer.h"

#include <iostream>
#include <limits>
#include <type_traits>
#include <vector>

#include "msan.h"

using namespace std;
using namespace Halide::Runtime;

// Just copies in -> out.
extern "C" int msan_extern_stage(buffer_t *in, buffer_t *out) {
    if (in->host == nullptr) {
        in->extent[0] = 4;
        in->extent[1] = 4;
        in->extent[2] = 3;
        in->min[0] = 0;
        in->min[1] = 0;
        in->min[2] = 0;
        return 0;
    }
    if (!out->host) {
        fprintf(stderr, "msan_extern_stage failure\n");
        return -1;
    }
    if (in->elem_size != out->elem_size) {
        return -1;
    }
    for (int c = 0; c < in->extent[2]; c++) {
        for (int y = 0; y < in->extent[1]; y++) {
            for (int x = 0; x < in->extent[0]; x++) {
                const uint64_t in_off = (x * in->stride[0] +
                                         y * in->stride[1] +
                                         c * in->stride[2]) * in->elem_size;
                const uint64_t out_off = (x * out->stride[0] +
                                          y * out->stride[1] +
                                          c * out->stride[2]) * out->elem_size;
                memcpy(out->host + out_off, in->host + in_off, in->elem_size);
            }
        }
    }
    out->host_dirty = true;
    return 0;
}

extern "C" void halide_error(void *user_context, const char *msg) {
    fprintf(stderr, "Saw error: %s\n", msg);
    // Do not exit.
}

// Must provide a stub for this since we aren't compiling with LLVM MSAN
// enabled, and the default implementation of halide_msan_annotate_memory_is_initialized()
// expects this to be present
extern "C" void AnnotateMemoryIsInitialized(const char *file, int line,
                                            const void *mem, size_t size) {
    fprintf(stderr, "Impossible\n");
    exit(-1);
}

enum {
  expect_bounds_inference_buffer,
  expect_intermediate_buffer,
  expect_output_buffer,
  expect_intermediate_contents,
  expect_output_contents,
} annotate_stage = expect_bounds_inference_buffer;
const void* output_base = nullptr;
const void* output_previous = nullptr;
int bounds_inference_count = 0;

void reset_state(const void* base) {
    annotate_stage = expect_bounds_inference_buffer;
    output_base = base;
    output_previous = nullptr;
    bounds_inference_count = 0;
}

extern "C" void halide_msan_annotate_memory_is_initialized(void *user_context, const void *ptr, uint64_t len) {
    printf("%d:%p:%08x\n", (int)annotate_stage, ptr, (unsigned int) len);
    if (annotate_stage == expect_bounds_inference_buffer) {
        if (output_previous != nullptr || len != sizeof(buffer_t)) {
            fprintf(stderr, "Failure: Expected sizeof(buffer_t), saw %d\n", (unsigned int) len);
            exit(-1);
        }
        bounds_inference_count += 1;
        if (bounds_inference_count == 4) {
            annotate_stage = expect_intermediate_buffer;
        }
    } else if (annotate_stage == expect_intermediate_buffer) {
        if (output_previous != nullptr || len != sizeof(buffer_t)) {
            fprintf(stderr, "Failure: Expected sizeof(buffer_t), saw %d\n", (unsigned int) len);
            exit(-1);
        }
        annotate_stage = expect_output_buffer;
    } else if (annotate_stage == expect_output_buffer) {
        if (output_previous != nullptr || len != sizeof(buffer_t)) {
            fprintf(stderr, "Failure: Expected sizeof(buffer_t), saw %d\n", (unsigned int) len);
            exit(-1);
        }
        annotate_stage = expect_intermediate_contents;
    } else if (annotate_stage == expect_intermediate_contents) {
        if (output_previous != nullptr || len != 4 * 4 * 3 * 4) {
            fprintf(stderr, "Failure: Expected %d, saw %d\n", 4 * 4 * 3 * 4, (unsigned int) len);
            exit(-1);
        }
        annotate_stage = expect_output_contents;
    } else if (annotate_stage == expect_output_contents) {
        if (output_previous == nullptr) {
            if (ptr != output_base) {
                fprintf(stderr, "Failure: Expected base p %p but saw %p\n", output_base, ptr);
                exit(-1);
            }
            if (ptr <= output_previous) {
                fprintf(stderr, "Failure: Expected monotonic increase but saw %p -> %p\n", output_previous, ptr);
                exit(-1);
            }
            output_previous = ptr;
        }
    } else {
        fprintf(stderr, "Failure: bad enum\n");
        exit(-1);
    }
}

template<typename T>
void verify(const T &image) {
    image.for_each_element([&](int x, int y, int c) {
        int expected = 3;
        for (int i = 0; i < 4; ++i) {
            expected += (int32_t)(i + y + c);
        }
        int actual = image(x, y, c);
        if (actual != expected) {
            fprintf(stderr, "Failure @ %d %d %d: expected %d, got %d\n", x, y, c, expected, actual);
            exit(-1);
        }
    });
}

//-----------------------------------------------------------------------------

int main()
{
    printf("Testing interleaved...\n");
    {
        auto out = Buffer<int32_t>::make_interleaved(4, 4, 3);
        reset_state(out.data());
        if (msan(out) != 0) {
            fprintf(stderr, "Failure!\n");
            exit(-1);
        }
        verify(out);
        if (output_previous == nullptr) {
            fprintf(stderr, "Failure: Expected to see annotations.\n");
            exit(-1);
        }
    }

    printf("Testing sparse chunky...\n");
    {
        const int kPad = 1;
        halide_dimension_t shape[3] = {
            { 0, 4, 3 },
            { 0, 4, (4 * 3) + kPad },
            { 0, 3, 1 },
        };
        std::vector<int32_t> data(((4 * 3) + kPad) * 4);
        auto out = Buffer<int32_t>(data.data(), 3, shape);
        reset_state(out.data());
        if (msan(out) != 0) {
            fprintf(stderr, "Failure!\n");
            exit(-1);
        }
        if (output_previous == nullptr) {
            fprintf(stderr, "Failure: Expected to see annotations.\n");
            exit(-1);
        }
    }

    printf("Testing planar...\n");
    {
        auto out = Buffer<int32_t>(4, 4, 3);
        reset_state(out.data());
        if (msan(out) != 0) {
            fprintf(stderr, "Failure!\n");
            exit(-1);
        }
        if (output_previous == nullptr) {
            fprintf(stderr, "Failure: Expected to see annotations.\n");
            exit(-1);
        }
    }

    printf("Testing sparse planar...\n");
    {
        const int kPad = 1;
        halide_dimension_t shape[3] = {
            { 0, 4, 1 },
            { 0, 4, 4 + kPad },
            { 0, 3, (4 + kPad) * 4 },
        };
        std::vector<int32_t> data((4 + kPad) * 4 * 3);
        auto out = Buffer<int32_t>(data.data(), 3, shape);
        reset_state(out.data());
        if (msan(out) != 0) {
            fprintf(stderr, "Failure!\n");
            exit(-1);
        }
        if (output_previous == nullptr) {
            fprintf(stderr, "Failure: Expected to see annotations.\n");
            exit(-1);
        }
    }
    // Buffers should not be marked as "initialized" if the filter fails with an error.
    printf("Testing error case...\n");
    {
        auto out = Buffer<int32_t>(1, 1, 1);
        reset_state(out.data());
        if (msan(out) == 0) {
            fprintf(stderr, "Failure (expected failure but did not)!\n");
            exit(-1);
        }
        if (output_previous != nullptr) {
            fprintf(stderr, "Failure: Expected NOT to see annotations.\n");
            exit(-1);
        }
    }

    printf("Success!\n");
    return 0;
}

#endif

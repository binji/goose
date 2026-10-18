/* The test package's C API, in the shape of the blessed packages' layers
   (gfx_api.h): one X-macro list, every parameter shape the foreign boundary
   admits. Goose structs are packed, so these are. */
#ifndef INTERPTEST_API_H
#define INTERPTEST_API_H

#include <stdint.h>

#pragma pack(push, 1)

typedef struct { uint8_t *data; int64_t len; } it_bytes;          /* u8[:] */
typedef struct { uint32_t id; } it_handle;                         /* a handle struct by value */
typedef struct { int32_t a, b; } it_pair;                          /* a flat struct, 8 bytes */
typedef struct { uint8_t tag; int64_t value; float weight; } it_item;   /* packed to 13 bytes */
typedef struct { const it_item *data; int64_t len; } it_items;    /* a slice of flat structs */

#pragma pack(pop)

#define IT_API(X) \
    X(int64_t, it_scalars, (int8_t a, uint8_t b, int16_t c, uint16_t d, int32_t e, uint32_t f, \
                            int64_t g, uint64_t h, float x, double y, uint8_t flag)) \
    X(int64_t, it_sum_bytes, (it_bytes data)) \
    X(int64_t, it_fill_bytes, (it_bytes out, uint8_t value)) \
    X(double, it_sum_items, (it_items items)) \
    X(it_handle, it_make_handle, (uint32_t id)) \
    X(uint32_t, it_handle_id, (it_handle h)) \
    X(it_pair, it_pair_swap, (it_pair p)) \
    X(void, it_pair_out, (int32_t a, int32_t b, it_pair *out)) \
    X(int64_t, it_pair_in, (const it_pair *p)) \
    X(void, it_item_scale, (it_item *item, float k)) \
    X(int64_t, it_counter, (void)) \
    X(void, it_yield, (void)) \
    X(int64_t, it_many, (int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, \
                         int64_t g, int64_t h, int64_t i, int64_t j)) \
    X(void, it_nothing, (void)) \
    X(float, it_half, (float x)) \
    X(uint8_t, it_bytes_equal, (it_bytes a, it_bytes b))

#define X(ret, name, params) ret name params;
IT_API(X)
#undef X

#endif

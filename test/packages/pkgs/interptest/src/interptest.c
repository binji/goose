/* The test package's native part: each function returns something that
   tells its arguments arrived whole. */
#include "interptest_api.h"

static int64_t calls;

int64_t it_scalars(int8_t a, uint8_t b, int16_t c, uint16_t d, int32_t e, uint32_t f,
                   int64_t g, uint64_t h, float x, double y, uint8_t flag) {
    calls++;
    return a + b + c + d + e + f + g + (int64_t)(h / 2) + (int64_t)(x * 10) + (int64_t)(y * 100) +
           (flag ? 1000000 : 0);
}

int64_t it_sum_bytes(it_bytes data) {
    calls++;
    int64_t sum = 0;
    for (int64_t i = 0; i < data.len; i++) sum += data.data[i];
    return sum;
}

int64_t it_fill_bytes(it_bytes out, uint8_t value) {
    calls++;
    for (int64_t i = 0; i < out.len; i++) out.data[i] = (uint8_t)(value + i);
    return out.len;
}

double it_sum_items(it_items items) {
    calls++;
    double sum = 0;
    for (int64_t i = 0; i < items.len; i++)
        sum += items.data[i].tag * 1000 + (double)items.data[i].value + items.data[i].weight;
    return sum;
}

it_handle it_make_handle(uint32_t id) {
    calls++;
    it_handle h = { id * 2 + 1 };
    return h;
}

uint32_t it_handle_id(it_handle h) { calls++; return h.id; }

it_pair it_pair_swap(it_pair p) {
    calls++;
    it_pair q = { p.b, p.a };
    return q;
}

void it_pair_out(int32_t a, int32_t b, it_pair *out) {
    calls++;
    out->a = a * 10;
    out->b = b * 10;
}

int64_t it_pair_in(const it_pair *p) { calls++; return (int64_t)p->a * 1000 + p->b; }

void it_item_scale(it_item *item, float k) {
    calls++;
    item->value *= 2;
    item->weight *= k;
    item->tag++;
}

int64_t it_counter(void) { return calls; }

void it_yield(void) { calls++; }

int64_t it_many(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g,
                int64_t h, int64_t i, int64_t j) {
    calls++;
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}

void it_nothing(void) { calls++; }

float it_half(float x) { calls++; return x / 2; }

uint8_t it_bytes_equal(it_bytes a, it_bytes b) {
    calls++;
    if (a.len != b.len) return 0;
    for (int64_t i = 0; i < a.len; i++)
        if (a.data[i] != b.data[i]) return 0;
    return 1;
}

/* qsort and bsearch run interpreted, calling the comparison through a
   function pointer; library functions called through pointers work too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int by_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static int by_int_desc(const void *a, const void *b) { return by_int(b, a); }

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

typedef struct { char name[12]; int score; double weight; } Entry;

static int by_score_then_name(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->score != y->score) return x->score - y->score;
    return strcmp(x->name, y->name);
}

int main(void) {
    int v[] = { 5, 3, 9, 1, 5, 7, 2, 8, 0, 6, 4 };
    int n = sizeof v / sizeof v[0];
    qsort(v, n, sizeof v[0], by_int);
    printf("ints:");
    for (int i = 0; i < n; i++) printf(" %d", v[i]);
    printf("\n");
    qsort(v, n, sizeof v[0], by_int_desc);
    printf("desc:");
    for (int i = 0; i < n; i++) printf(" %d", v[i]);
    printf("\n");

    const char *names[] = { "pear", "apple", "fig", "banana", "cherry", "date" };
    int nn = sizeof names / sizeof names[0];
    qsort(names, nn, sizeof names[0], by_name);
    printf("names:");
    for (int i = 0; i < nn; i++) printf(" %s", names[i]);
    printf("\n");

    Entry es[] = {
        { "carol", 70, 1.5 }, { "alice", 90, 2.5 }, { "bob", 70, 3.5 }, { "dave", 85, 4.5 }, { "erin", 90, 5.5 },
    };
    int ne = sizeof es / sizeof es[0];
    qsort(es, ne, sizeof es[0], by_score_then_name);
    for (int i = 0; i < ne; i++) printf("%s %d %.1f\n", es[i].name, es[i].score, es[i].weight);

    /* Large enough that the heap sort's shape matters, with duplicates. */
    enum { N = 2000 };
    int *big = malloc(N * sizeof *big);
    srand(7);
    for (int i = 0; i < N; i++) big[i] = rand() % 100;
    qsort(big, N, sizeof *big, by_int);
    int sorted = 1;
    long sum = 0;
    for (int i = 0; i < N; i++) {
        if (i && big[i] < big[i - 1]) sorted = 0;
        sum += big[i];
    }
    printf("big: sorted %d, first %d, last %d, sum %ld\n", sorted, big[0], big[N - 1], sum);
    free(big);

    qsort(v, 0, sizeof v[0], by_int);
    qsort(v, 1, sizeof v[0], by_int);
    printf("empty and single: fine\n");

    qsort(v, n, sizeof v[0], by_int);
    int key = 7;
    int *found = bsearch(&key, v, n, sizeof v[0], by_int);
    printf("bsearch 7: index %d\n", found ? (int)(found - v) : -1);
    key = 11;
    printf("bsearch 11: %s\n", bsearch(&key, v, n, sizeof v[0], by_int) ? "found" : "absent");

    /* A library function through a pointer. */
    size_t (*len)(const char *) = strlen;
    int (*cmp)(const void *, const void *) = by_int;
    printf("strlen via pointer: %d, by_int via pointer: %d\n", (int)len("twelve chars"), cmp(&v[0], &v[1]));
    printf("rand after srand(1):");
    srand(1);
    for (int i = 0; i < 3; i++) printf(" %d", rand());
    printf("\n");
    return 0;
}

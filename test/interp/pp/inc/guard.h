#ifndef GUARD_H
#define GUARD_H
#ifdef GUARD_COUNT
#error "the guard failed"
#endif
#define GUARD_COUNT 1
int guarded;
#endif

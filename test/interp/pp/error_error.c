#define GS_STACK_RESERVE (1ull << 49)
#if GS_STACK_RESERVE > (1ull << 48)
#error "GS_STACK_RESERVE exceeds the 2^48 limit (goose_spec.md 10.4)"
#endif

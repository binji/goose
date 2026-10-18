/* A pointer before an object may be formed, but not read through. */
static int table[4] = { 1, 2, 3, 4 };
int main(void) {
    int *origin = table - 1;
    return *origin;
}

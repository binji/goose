/* main with arguments: argv is an array of handles to strings. */
#include "inc/print.h"
int main(int argc, char **argv) {
    show("argc", argc);
    show_s("argv[0]", argv[0]);
    show("argv[argc] is null", argv[argc] == 0);
    return argc - 1;
}

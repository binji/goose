/* main with arguments: argv is an array of handles to strings. The runner
   passes the fixture's scratch directory as argv[1]. */
#include "inc/print.h"
int main(int argc, char **argv) {
    show("argc", argc);
    show_s("argv[0]", argv[0]);
    show("argv[1] is a path", argv[1][0] == '/' || argv[1][1] == ':');
    show("argv[argc] is null", argv[argc] == 0);
    return argc - 2;
}

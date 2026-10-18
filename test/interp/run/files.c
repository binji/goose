/* Files and directories in the fixture's scratch directory (argv[1]): the
   stdio and POSIX subsets runtime_os.h is written against, and errno. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char path[4096];
static const char *in(const char *dir, const char *name) {
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return path;
}

static int kind(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *dir = argv[1];
    printf("scratch dir is a directory: %d\n", kind(dir) == 2);

    /* stdio: write, append, read back, seek. */
    FILE *f = fopen(in(dir, "a.txt"), "wb");
    printf("fopen for writing: %d\n", f != NULL);
    int n = fprintf(f, "line %d\n", 1);
    n += (int)fwrite("line 2\n", 1, 7, f);
    fputs("line 3\n", f);
    fputc('!', f);
    printf("fclose: %d, wrote %d\n", fclose(f), n);
    f = fopen(in(dir, "a.txt"), "ab");
    fputs("\nappended\n", f);
    fclose(f);
    f = fopen(in(dir, "a.txt"), "rb");
    char buf[128];
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    buf[got] = 0;
    printf("read %d bytes:\n%s", (int)got, buf);
    printf("feof %d ferror %d\n", feof(f), ferror(f));
    printf("fseek %d ftell %ld\n", fseek(f, 5, SEEK_SET), ftell(f));
    printf("fgetc after seek: %c\n", fgetc(f));
    fseek(f, 0, SEEK_SET);
    while (fgets(buf, sizeof buf, f)) printf("fgets: %s", buf);
    printf("fgetc at end: %d\n", fgetc(f));
    fclose(f);
    struct stat st;
    printf("stat: %d, regular %d, size %ld\n", stat(in(dir, "a.txt"), &st), S_ISREG(st.st_mode), (long)st.st_size);

    /* A missing file. */
    errno = 0;
    f = fopen(in(dir, "missing.txt"), "rb");
    printf("fopen missing: %d, ENOENT %d, %s\n", f == NULL, errno == ENOENT, strerror(errno));
    printf("stat missing: %d\n", stat(in(dir, "missing.txt"), &st));

    /* POSIX: an exclusive create, write, fsync, rename. */
    int fd = open(in(dir, "b.tmp"), O_WRONLY | O_CREAT | O_EXCL, 0666);
    printf("open O_EXCL: %d\n", fd >= 0);
    printf("write: %d\n", (int)write(fd, "posix bytes", 11));
    printf("fsync: %d, close: %d\n", fsync(fd), close(fd));
    errno = 0;
    int again = open(in(dir, "b.tmp"), O_WRONLY | O_CREAT | O_EXCL, 0666);
    printf("open O_EXCL again: %d, EEXIST %d\n", again, errno == EEXIST);
    char from[4096];
    strcpy(from, in(dir, "b.tmp"));
    printf("rename: %d\n", rename(from, in(dir, "b.txt")));
    printf("old gone %d, new there %d\n", kind(from) == 0, kind(in(dir, "b.txt")) == 1);
    fd = open(in(dir, "b.txt"), O_RDONLY);
    n = (int)read(fd, buf, sizeof buf);
    buf[n] = 0;
    printf("read back: %d \"%s\"\n", n, buf);
    close(fd);
    fd = open(in(dir, "b.txt"), O_WRONLY | O_APPEND);
    write(fd, "+", 1);
    close(fd);
    stat(in(dir, "b.txt"), &st);
    printf("after append: size %ld\n", (long)st.st_size);
    fd = open(in(dir, "b.txt"), O_WRONLY | O_TRUNC);
    close(fd);
    stat(in(dir, "b.txt"), &st);
    printf("after truncate: size %ld\n", (long)st.st_size);

    /* Directories: make, list sorted, remove. */
    printf("mkdir: %d, is dir %d\n", mkdir(in(dir, "sub"), 0777), kind(in(dir, "sub")) == 2);
    errno = 0;
    printf("mkdir again: %d, EEXIST %d\n", mkdir(in(dir, "sub"), 0777), errno == EEXIST);
    fclose(fopen(in(dir, "sub/z"), "wb"));
    fclose(fopen(in(dir, "sub/y"), "wb"));
    fclose(fopen(in(dir, "sub/x"), "wb"));
    DIR *d = opendir(in(dir, "sub"));
    printf("opendir: %d\n", d != NULL);
    const char *names[16];
    int count = 0;
    errno = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (e->d_name[0] == '.') continue;
        names[count++] = strdup(e->d_name);
    }
    printf("readdir end with errno %d, %d entries\n", errno, count);
    closedir(d);
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(names[i], names[j]) > 0) { const char *t = names[i]; names[i] = names[j]; names[j] = t; }
    for (int i = 0; i < count; i++) printf("  %s\n", names[i]);
    printf("opendir missing: %d, ENOENT %d\n", opendir(in(dir, "nowhere")) == NULL, errno == ENOENT);
    errno = 0;
    printf("rmdir nonempty: %d, ENOTEMPTY or EEXIST %d\n", rmdir(in(dir, "sub")), errno == ENOTEMPTY || errno == EEXIST);
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof buf, "sub/%s", names[i]);
        printf("unlink %s: %d\n", names[i], unlink(in(dir, buf)));
    }
    printf("rmdir: %d, gone %d\n", rmdir(in(dir, "sub")), kind(in(dir, "sub")) == 0);
    printf("remove: %d %d, unlink missing: %d\n", remove(in(dir, "a.txt")), remove(in(dir, "b.txt")),
           unlink(in(dir, "missing")));
    printf("errno is ENOENT: %d\n", errno == ENOENT);

    /* The standard streams are streams too. */
    printf("fwrite to stderr: %d\n", (int)fwrite("stderr bytes\n", 1, 13, stderr));
    printf("fflush(stdout): %d, fflush(NULL): %d\n", fflush(stdout), fflush(NULL));
    printf("setvbuf: %d\n", setvbuf(stdout, NULL, _IOFBF, 4096));
    return 0;
}

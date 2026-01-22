#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <io.h>
    #include <direct.h>
    #include <sys/stat.h>
    #define PATH_SEP '\\'
    #define PATH_SEP_STR "\\"
    #define strdup _strdup
    #ifndef S_ISDIR
        #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
    #endif
    #ifndef S_ISREG
        #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #endif
#else
    #include <dirent.h>
    #include <unistd.h>
    #include <dlfcn.h>
    #include <time.h>
    #include <sys/stat.h>
    #define PATH_SEP '/'
    #define PATH_SEP_STR "/"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Directory iteration */
typedef struct {
    char name[260];
    int is_dir;
} dir_entry_t;

typedef struct dir_iter dir_iter_t;

dir_iter_t *dir_open(const char *path);
int dir_next(dir_iter_t *iter, dir_entry_t *entry);
void dir_close(dir_iter_t *iter);

/* Dynamic library loading */
void *lib_open(const char *path);
void *lib_sym(void *handle, const char *name);
void lib_close(void *handle);

#endif /* PLATFORM_H */
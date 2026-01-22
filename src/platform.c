#include "platform.h"

#ifdef _WIN32

/* === Windows implementation === */

struct dir_iter {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    int first;
};

dir_iter_t *dir_open(const char *path) {
    dir_iter_t *iter = malloc(sizeof(dir_iter_t));
    if (!iter) return NULL;

    char search[MAX_PATH];
    snprintf(search, MAX_PATH, "%s\\*", path);

    iter->handle = FindFirstFileA(search, &iter->data);
    if (iter->handle == INVALID_HANDLE_VALUE) {
        free(iter);
        return NULL;
    }
    iter->first = 1;
    return iter;
}

int dir_next(dir_iter_t *iter, dir_entry_t *entry) {
    if (!iter) return 0;
    
    /* Skip . and .. */
    while (1) {
        if (iter->first) {
            iter->first = 0;
        } else {
            if (!FindNextFileA(iter->handle, &iter->data))
                return 0;
        }
        
        if (strcmp(iter->data.cFileName, ".") != 0 &&
            strcmp(iter->data.cFileName, "..") != 0)
            break;
    }
    
    strncpy(entry->name, iter->data.cFileName, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->is_dir = (iter->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 1;
}

void dir_close(dir_iter_t *iter) {
    if (iter) {
        FindClose(iter->handle);
        free(iter);
    }
}

void *lib_open(const char *path) {
    return LoadLibraryA(path);
}

void *lib_sym(void *handle, const char *name) {
    return (void *)GetProcAddress((HMODULE)handle, name);
}

void lib_close(void *handle) {
    if (handle) FreeLibrary((HMODULE)handle);
}

#else

/* === Linux/POSIX implementation === */

struct dir_iter {
    DIR *dir;
};

dir_iter_t *dir_open(const char *path) {
    dir_iter_t *iter = malloc(sizeof(dir_iter_t));
    if (!iter) return NULL;

    iter->dir = opendir(path);
    if (!iter->dir) {
        free(iter);
        return NULL;
    }
    return iter;
}

int dir_next(dir_iter_t *iter, dir_entry_t *entry) {
    if (!iter) return 0;
    
    struct dirent *de;
    
    /* Skip . and .. */
    while ((de = readdir(iter->dir)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 &&
            strcmp(de->d_name, "..") != 0)
            break;
    }
    
    if (!de) return 0;

    strncpy(entry->name, de->d_name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->is_dir = (de->d_type == DT_DIR);
    return 1;
}

void dir_close(dir_iter_t *iter) {
    if (iter) {
        closedir(iter->dir);
        free(iter);
    }
}

void *lib_open(const char *path) {
    return dlopen(path, RTLD_LAZY);
}

void *lib_sym(void *handle, const char *name) {
    return dlsym(handle, name);
}

void lib_close(void *handle) {
    if (handle) dlclose(handle);
}

#endif
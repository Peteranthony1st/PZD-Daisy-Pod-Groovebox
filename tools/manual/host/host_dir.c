/* Folder listing for host_ff.cpp, kept apart because the computer's DIR
   and FatFs's DIR are different types with the same name. */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

struct HostDir
{
    DIR* d;
};
void* host_dir_open(const char* path)
{
    DIR* d = opendir(path);
    if(!d)
        return 0;
    struct HostDir* h = malloc(sizeof(*h));
    h->d              = d;
    return h;
}
/* The next name, skipping "." and the computer's hidden files; 0 at the end. */
const char* host_dir_next(void* h)
{
    struct dirent* e;
    while((e = readdir(((struct HostDir*)h)->d)))
        if(e->d_name[0] != '.')
            return e->d_name;
    return 0;
}
void host_dir_close(void* h)
{
    closedir(((struct HostDir*)h)->d);
    free(h);
}

// FatFs over a folder on the computer, for the host renderer. The folder
// is $PZD_HOST_SD; without it there's no card (f_mount fails).
#include "ff.h"
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void*       host_dir_open(const char* path);
extern "C" const char* host_dir_next(void* h);
extern "C" void        host_dir_close(void* h);

static std::string Host(const TCHAR* p)
{
    const char* root = getenv("PZD_HOST_SD");
    std::string s    = p ? p : "";
    if(s.rfind("0:", 0) == 0)
        s = s.substr(2);
    while(!s.empty() && s[0] == '/')
        s = s.substr(1);
    return std::string(root ? root : "") + "/" + s;
}
static FRESULT Err()
{
    return errno == ENOENT ? FR_NO_FILE : errno == EEXIST ? FR_EXIST : FR_DENIED;
}

FRESULT f_mount(FATFS*, const TCHAR*, BYTE)
{
    struct stat st;
    const char* root = getenv("PZD_HOST_SD");
    return root && stat(root, &st) == 0 && S_ISDIR(st.st_mode) ? FR_OK : FR_NOT_READY;
}
FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode)
{
    std::string h = Host(path);
    struct stat st;
    bool exists = stat(h.c_str(), &st) == 0;
    const char* m;
    if(mode & FA_CREATE_NEW)
    {
        if(exists)
            return FR_EXIST;
        m = "w+b";
    }
    else if(mode & FA_CREATE_ALWAYS)
        m = "w+b";
    else if(mode & FA_OPEN_ALWAYS)
        m = exists ? "r+b" : "w+b";
    else
    {
        if(!exists)
            return FR_NO_FILE;
        m = (mode & FA_WRITE) ? "r+b" : "rb";
    }
    fp->fp = fopen(h.c_str(), m);
    if(!fp->fp)
        return Err();
    if((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND)
        fseeko(fp->fp, 0, SEEK_END);
    return FR_OK;
}
FRESULT f_close(FIL* fp)
{
    if(!fp->fp)
        return FR_INVALID_OBJECT;
    fclose(fp->fp);
    fp->fp = nullptr;
    return FR_OK;
}
FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br)
{
    size_t n = fread(buff, 1, btr, fp->fp);
    if(br)
        *br = (UINT)n;
    return ferror(fp->fp) ? FR_DISK_ERR : FR_OK;
}
FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
{
    size_t n = fwrite(buff, 1, btw, fp->fp);
    if(bw)
        *bw = (UINT)n;
    return FR_OK;
}
FRESULT f_lseek(FIL* fp, FSIZE_t ofs)
{
    return fseeko(fp->fp, (off_t)ofs, SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR;
}
FRESULT f_sync(FIL* fp)
{
    fflush(fp->fp);
    return FR_OK;
}
FSIZE_t f_tell(FIL* fp) { return (FSIZE_t)ftello(fp->fp); }
FSIZE_t f_size(FIL* fp)
{
    off_t pos = ftello(fp->fp);
    fseeko(fp->fp, 0, SEEK_END);
    off_t end = ftello(fp->fp);
    fseeko(fp->fp, pos, SEEK_SET);
    return (FSIZE_t)end;
}
// FatFs's DIR has one field; it holds the listing and, after it, the path.
struct HostListing
{
    void*       d;
    std::string path;
};
FRESULT f_opendir(DIR* dp, const TCHAR* path)
{
    std::string h = Host(path);
    void*       d = host_dir_open(h.c_str());
    if(!d)
        return FR_NO_PATH;
    dp->d = new HostListing{d, h};
    return FR_OK;
}
FRESULT f_closedir(DIR* dp)
{
    HostListing* l = (HostListing*)dp->d;
    if(l)
    {
        host_dir_close(l->d);
        delete l;
    }
    dp->d = nullptr;
    return FR_OK;
}
static void Fill(const char* full, const char* name, FILINFO* fno)
{
    struct stat st;
    memset(fno, 0, sizeof(*fno));
    strncpy(fno->fname, name, sizeof(fno->fname) - 1);
    if(stat(full, &st) == 0)
    {
        fno->fsize   = S_ISDIR(st.st_mode) ? 0 : (FSIZE_t)st.st_size;
        fno->fattrib = S_ISDIR(st.st_mode) ? AM_DIR : AM_ARC;
    }
}
FRESULT f_readdir(DIR* dp, FILINFO* fno)
{
    HostListing* l    = (HostListing*)dp->d;
    const char*  name = l ? host_dir_next(l->d) : nullptr;
    if(!name)
    {
        fno->fname[0] = 0;
        return FR_OK;
    }
    Fill((l->path + "/" + name).c_str(), name, fno);
    return FR_OK;
}
FRESULT f_mkdir(const TCHAR* path)
{
    return mkdir(Host(path).c_str(), 0755) == 0 ? FR_OK : Err();
}
FRESULT f_unlink(const TCHAR* path)
{
    std::string h = Host(path);
    return (unlink(h.c_str()) == 0 || rmdir(h.c_str()) == 0) ? FR_OK : Err();
}
FRESULT f_rename(const TCHAR* a, const TCHAR* b)
{
    struct stat st;
    if(stat(Host(b).c_str(), &st) == 0)
        return FR_EXIST;
    return rename(Host(a).c_str(), Host(b).c_str()) == 0 ? FR_OK : Err();
}
FRESULT f_stat(const TCHAR* path, FILINFO* fno)
{
    std::string h = Host(path);
    struct stat st;
    if(stat(h.c_str(), &st) != 0)
        return FR_NO_FILE;
    if(fno)
    {
        const char* slash = strrchr(h.c_str(), '/');
        Fill(h.c_str(), slash ? slash + 1 : h.c_str(), fno);
    }
    return FR_OK;
}

#include "state_save_branch.h"
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

int StateSaveBranch_Create(const char *canonical, const char *source,
                           size_t expected_size, char *branch, size_t capacity)
{
    if (!canonical || !source || !branch || !capacity || !expected_size) return -1;
    branch[0] = 0;
    FILE *in = fopen(source, "rb");
    if (!in) return -1;
    struct stat st;
    if (fstat(fileno(in), &st) || st.st_size != (off_t)expected_size) {
        fclose(in); return -1;
    }
    int fd = -1;
    for (unsigned i=1; i<=9999; ++i) {
        int n = snprintf(branch, capacity, "%s.state-%04u.sav", canonical, i);
        // Reserve room for the existing publisher's .tmp and .old sidecars.
        if (n < 0 || (size_t)n + 5 > capacity) break;
        fd = open(branch, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { fclose(in); branch[0]=0; return -1; }
    // Refuse orphaned publication sidecars instead of reusing their history.
    char sidecar[384];
    int ok = 1;
    for (unsigned i=0; i<2; ++i) {
        int n=snprintf(sidecar,sizeof(sidecar),"%s.%s",branch,i?"old":"tmp");
        if(n<0 || (size_t)n>=sizeof(sidecar) || stat(sidecar,&st)==0 || errno!=ENOENT) ok=0;
    }
    unsigned char buffer[1024];
    size_t left=expected_size;
    while(ok && left) {
        size_t n=left<sizeof(buffer)?left:sizeof(buffer);
        if(fread(buffer,1,n,in)!=n) { ok=0;break; }
        size_t offset=0;
        while(offset<n) {
            ssize_t written=write(fd,buffer+offset,n-offset);
            if(written<0 && errno==EINTR) continue;
            if(written<=0) { ok=0;break; }
            offset+=(size_t)written;
        }
        left-=n;
    }
    if(fgetc(in)!=EOF || ferror(in)) ok=0;
    if(fclose(in)) ok=0;
    if(ok && fsync(fd)) ok=0;
    if(close(fd)) ok=0;
    if(!ok) { unlink(branch);branch[0]=0;return -1; }
    return 0;
}

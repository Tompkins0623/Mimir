#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#include "log.h"
#include "memory.h"
#include "const.h"

#include <malloc.h>
#ifdef BGQ
#include <spi/include/kernel/memory.h>
#endif

int64_t peakmem=0;

// Get max maps
inline int64_t get_max_mmap(){
#define BUFSIZE 1024
    int stderr_save;
    char buffer[BUFSIZE]={'\0'};
    fflush(stderr);
    stderr_save = dup(STDERR_FILENO); 
    freopen("/dev/null", "a", stderr);
    setvbuf(stderr, buffer, _IOFBF, BUFSIZE);
    malloc_stats();
    freopen("/dev/null", "a", stderr);
    dup2(stderr_save, STDERR_FILENO);
    setvbuf(stderr, NULL, _IONBF, BUFSIZE);

    char *p, *temp=NULL;
    int64_t maxmmap=0;
    p = strtok_r(buffer, "\n", &temp);
    do{
        if(strncmp(p, "max mmap bytes   =", 18) == 0){
            char *word = p + 18;
            while(isspace(*word)) ++word;
            maxmmap=strtoull(word, NULL, 0);
        }
      p = strtok_r(NULL, "\n", &temp);
    } while (p != NULL);

    return maxmmap;
}

/// Get vm size
inline int64_t get_vmsize(){
    pid_t pid=getpid();

    int64_t vmsize=0;
    char procname[100], line[100];
    sprintf(procname,"/proc/%ld/status", (long)pid);
    FILE *fp=fopen(procname,"r");

    while(fgets(line, 100, fp)){
        if(strncmp(line, "VmSize:", 7) == 0){
            char *p = line + 7;
            while(isspace(*p)) ++p;
            vmsize=strtoull(p, NULL, 0);
        }
    }

    fclose(fp);
  
    return vmsize;
}

inline int64_t get_mem_usage(){

    int64_t memsize = 0;

#ifdef BGQ

    long unsigned int shared = 0;
    long unsigned int persist = 0;
    long unsigned int heapavail = 0;
    long unsigned int stackavail = 0;
    long unsigned int stacksize = 0;
    long unsigned int heap = 0;
    long unsigned int guard = 0;
    long unsigned int mmap = 0;

    Kernel_GetMemorySize(KERNEL_MEMSIZE_GUARD, &guard);
    Kernel_GetMemorySize(KERNEL_MEMSIZE_SHARED, &shared);
    Kernel_GetMemorySize(KERNEL_MEMSIZE_PERSIST, &persist);
    Kernel_GetMemorySize(KERNEL_MEMSIZE_HEAPAVAIL, &heapavail);
    Kernel_GetMemorySize(KERNEL_MEMSIZE_STACKAVAIL, &stackavail);
    Kernel_GetMemorySize(KERNEL_MEMSIZE_STACK, &stacksize);
    Kernel_GetMemorySize(KERNEL_MEMSIZE_HEAP, &heap);
    Kernel_GetMemorySize(KERNEL_MEMSIZE_MMAP, &mmap);

    memsize = heap + stacksize;

#else

    memsize=get_max_mmap();

#endif

    return memsize;
}

void *mem_aligned_malloc(size_t alignment, size_t size){
    void *ptr=NULL;

    size_t align_size = (size+alignment-1)/alignment*alignment;
    posix_memalign(&ptr, alignment, align_size);

    //ptr=malloc(align_size);

    if(ptr == NULL){
        int64_t memsize=get_mem_usage();
        LOG_ERROR("Error: malloc memory error (align=%ld; size=%ld; \
aligned_size=%ld; memsize=%ld)\n", \
         alignment, size, align_size, memsize);
        return NULL;
    }

    if(RECORD_PEAKMEM==1){
        int64_t vmsize=get_mem_usage();
        if(vmsize>peakmem) peakmem=vmsize;
    }

    return ptr;
}

void *mem_aligned_free(void *ptr)
{
    free(ptr);

    if(RECORD_PEAKMEM==1){
        int64_t vmsize=get_mem_usage();
        if(vmsize>peakmem) peakmem=vmsize;
    }

    return NULL;
}

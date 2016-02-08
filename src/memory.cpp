#include "log.h"
#include "memory.h"

void *mem_aligned_malloc(size_t alignment, size_t size){
  void *ptr=NULL;

  //printf("mem_aligned_mal")
  
  //void *ptr = aligned_alloc(alignment, size);
  if(posix_memalign(&ptr, alignment, size)){
    LOG_ERROR("malloc memory with alignment %ld and size %ld error!", alignment, size);
    return NULL;
  }

  //printf("ptr=%p\n", ptr);

  return ptr;
}

void *mem_aligned_free(void *ptr){
  free(ptr);
  return NULL;
}

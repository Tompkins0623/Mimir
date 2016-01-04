#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <dirent.h>

#include <iostream>
#include <string>
#include <list>
#include <vector>

#include <sys/stat.h>

#include <mpi.h>
#include <omp.h>

#include "mapreduce.h"
#include "dataobject.h"
#include "alltoall.h"
#include "ptop.h"
#include "spool.h"

#include "log.h"
#include "config.h"

#include "const.h"

using namespace MAPREDUCE_NS;

#if GATHER_STAT
#include "stat.h"
#endif

MapReduce::MapReduce(MPI_Comm caller)
{
    comm = caller;
    MPI_Comm_rank(comm,&me);
    MPI_Comm_size(comm,&nprocs);

#pragma omp parallel
{
    tnum = omp_get_num_threads();    
}

    blocks = new int[tnum];   
    nitems = new uint64_t[tnum];
    for(int i = 0; i < tnum; i++){
      blocks[i] = -1;
      nitems[i] = 0;
    }

    data = NULL;
    c = NULL;

    mode = NoneMode;
  
    init();

    fprintf(stdout, "Process count=%d, thread count=%d\n", nprocs, tnum);

    LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: create. (thread number=%d)\n", me, nprocs, tnum);
}

MapReduce::~MapReduce()
{
    if(data) delete data;

    if(c) delete c;

    delete [] nitems;
    delete [] blocks;

    LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: destroy.\n", me, nprocs);
}

// configurable functions
/******************************************************/

/*
 * map: (no input) 
 * arguments:
 *   mymap: user defined map function
 *   ptr:   user defined pointer
 * return:
 *   global KV count
 */
uint64_t MapReduce::map(void (*mymap)(MapReduce *, void *), void *ptr){

  // create data object
  if(data) delete data;
  KeyValue *kv = new KeyValue(kvtype, 
                              blocksize, 
                              nmaxblock, 
                              maxmemsize, 
                              outofcore, 
                              tmpfpath);
  data=kv;
  kv->setKVsize(ksize,vsize);

  // create communicator
  if(commmode==0)
    c = new Alltoall(comm, tnum);
  else if(commmode==1)
    c = new Ptop(comm, tnum);
  c->setup(lbufsize, gbufsize, kvtype, ksize, vsize);
  c->init(kv);

  mode = MapMode;

  // begin traversal
#pragma omp parallel
{
  int tid = omp_get_thread_num();
  
  tinit(tid);
  
  // FIXME: should I invoke user-defined map in each thread?
  if(tid == 0)
    mymap(this, ptr);
  
  // wait all threads quit
  c->twait(tid);
}
  // wait all processes done
  c->wait();

  mode = NoneMode;

  // destroy communicator
  delete c;
  c = NULL;

  // sum kv count
  uint64_t sum = 0, count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  MPI_Allreduce(&count, &sum, 1, MPI_UINT64_T, MPI_SUM, comm);

  return sum;
}

/*
 * map_local: (no input) 
 * arguments:
 *   mymap: user defined map function
 *   ptr:   user defined pointer
 * return:
 *   local KV count
 */
uint64_t MapReduce::map_local(void (*mymap)(MapReduce *, void *), void* ptr){
  // create data object
  if(data) delete data;
  KeyValue *kv = new KeyValue(kvtype, 
                              blocksize, 
                              nmaxblock, 
                              maxmemsize, 
                              outofcore, 
                              tmpfpath);
  data=kv;
  kv->setKVsize(ksize, vsize);

  mode = MapLocalMode;


#pragma omp parallel
{
  int tid = omp_get_thread_num();
  tinit(tid);
  // FIXME: should invoke mymap in each thread?
  if(tid == 0)
    mymap(this, ptr);
}

  mode = NoneMode;

  uint64_t count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  return count;
}


/*
 * map: (files as input, main thread reads files)
 * arguments:
 *  filepath:   input file path
 *  sharedflag: 0 for local file system, 1 for global file system
 *  recurse:    1 for recurse
 *  readmode:   0 for by word, 1 for by line
 *  mymap:      user defined map function
 *  ptr:        user-defined pointer
 * return:
 *  global KV count
 */
uint64_t MapReduce::map(char *filepath, int sharedflag, int recurse, 
    char *whitespace, void (*mymap) (MapReduce *, char *, void *), void *ptr){

  //LOG_ERROR("%s", "map (files as input, main thread reads files) has been implemented!\n");
  // create data object
  if(data) delete data;
  ifiles.clear();

  KeyValue *kv = new KeyValue(kvtype, 
                              blocksize, 
                              nmaxblock, 
                              maxmemsize, 
                              outofcore, 
                              tmpfpath);
  data=kv;
  kv->setKVsize(ksize,vsize);

  // create communicator
  //c = new Alltoall(comm, tnum);
  if(commmode==0)
    c = new Alltoall(comm, tnum);
  else if(commmode==1)
    c = new Ptop(comm, tnum);

  c->setup(lbufsize, gbufsize, kvtype, ksize, vsize);
  c->init(kv);

  mode = MapMode;

  if(strlen(whitespace) == 0){
    LOG_ERROR("%s", "Error: the white space should not be empty string!\n");
  }
  
  // TODO: Finish it!!!!
  // distribute input files
  disinputfiles(filepath, sharedflag, recurse);

  struct stat stbuf;

  int input_buffer_size=BLOCK_SIZE*UNIT_SIZE;

  char *text = new char[input_buffer_size+1];

  int fcount = ifiles.size();
  for(int i = 0; i < fcount; i++){
    //std::cout << me << ":" << ifiles[i] << std::endl;
    int flag = stat(ifiles[i].c_str(), &stbuf);
    if( flag < 0){
      LOG_ERROR("Error: could not query file size of %s\n", ifiles[i].c_str());
    }

    FILE *fp = fopen(ifiles[i].c_str(), "r");
    int fsize = stbuf.st_size;
    int foff = 0, boff = 0;
    while(fsize > 0){
      // set file pointer
      fseek(fp, foff, SEEK_SET);
      // read a block
      int readsize = fread(text+boff, 1, input_buffer_size-boff, fp);
      text[boff+readsize+1] = '\0';

      // the last block
      //if(boff+readsize < input_buffer_size) 
      boff = 0;
      while(!strchr(whitespace, text[input_buffer_size-boff])) boff++;

#pragma omp parallel
{
      int tid = omp_get_thread_num();
      tinit(tid);

      int divisor = input_buffer_size / tnum;
      int remain  = input_buffer_size % tnum;

      int tstart = tid * divisor;
      if(tid < remain) tstart += tid;
      else tstart += remain;

      int tend = tstart + divisor;
      if(tid < remain) tend += 1;

      if(tid == tnum-1) tend -= boff;

      // start by the first none whitespace char
      int i;
      for(i = tstart; i < tend; i++){
        if(!strchr(whitespace, text[i])) break;
      }
      tstart = i;
      
      // end by the first whitespace char
      for(i = tend; i < input_buffer_size; i++){
        if(strchr(whitespace, text[i])) break;
      }
      tend = i;
      text[tend] = '\0';

      char *saveptr = NULL;
      char *word = strtok_r(text+tstart, whitespace, &saveptr);
      while(word){
        mymap(this, word, ptr);
        word = strtok_r(NULL,whitespace,&saveptr);
      }
}

      foff += readsize;
      fsize -= readsize;
      
      for(int i =0; i < boff; i++) text[i] = text[input_buffer_size-boff+i];
    }
    
    fclose(fp);
  }

  delete []  text;

   mode = NoneMode;

  // destroy communicator
  delete c;
  c = NULL;

  // sum kv count
  uint64_t sum = 0, count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  MPI_Allreduce(&count, &sum, 1, MPI_UINT64_T, MPI_SUM, comm);

  return sum;
}

/*
 * map_local: (files as input, main thread reads files)
 */
// TODO: finish it!!!
uint64_t MapReduce::map_local(char *filepath, int sharedflag, int recurse,
    char *whitespace, void (*mymap)(MapReduce *, char *, void *), void *ptr){

  if(strlen(whitespace) == 0){
    LOG_ERROR("%s", "Error: the white space should not be empty string!\n");
  }

  // create data object
  if(data) delete data;
  ifiles.clear();

  KeyValue *kv = new KeyValue(kvtype, 
                              blocksize, 
                              nmaxblock, 
                              maxmemsize, 
                              outofcore, 
                              tmpfpath);
  data=kv;
  kv->setKVsize(ksize, vsize);
  
  // TODO: Finish it!!!!
  // distribute input files
  disinputfiles(filepath, sharedflag, recurse);

  struct stat stbuf;

  int input_buffer_size=BLOCK_SIZE*UNIT_SIZE;

  char *text = new char[input_buffer_size+1];

  mode = MapLocalMode;

  int fcount = ifiles.size();
  for(int i = 0; i < fcount; i++){
    //std::cout << me << ":" << ifiles[i] << std::endl;
    int flag = stat(ifiles[i].c_str(), &stbuf);
    if( flag < 0){
      LOG_ERROR("Error: could not query file size of %s\n", ifiles[i].c_str());
    }

    FILE *fp = fopen(ifiles[i].c_str(), "r");
    int fsize = stbuf.st_size;
    int foff = 0, boff = 0;
    while(fsize > 0){
      // set file pointer
      fseek(fp, foff, SEEK_SET);
      // read a block
      int readsize = fread(text+boff, 1, input_buffer_size-boff, fp);
      text[boff+readsize+1] = '\0';

      // the last block
      //if(boff+readsize < input_buffer_size) 
      boff = 0;
      while(!strchr(whitespace, text[input_buffer_size-boff])) boff++;

#pragma omp parallel
{
      int tid = omp_get_thread_num();
      tinit(tid);

      int divisor = input_buffer_size / tnum;
      int remain  = input_buffer_size % tnum;

      int tstart = tid * divisor;
      if(tid < remain) tstart += tid;
      else tstart += remain;

      int tend = tstart + divisor;
      if(tid < remain) tend += 1;

      if(tid == tnum-1) tend -= boff;

      // start by the first none whitespace char
      int i;
      for(i = tstart; i < tend; i++){
        if(!strchr(whitespace, text[i])) break;
      }
      tstart = i;
      
      // end by the first whitespace char
      for(i = tend; i < input_buffer_size; i++){
        if(strchr(whitespace, text[i])) break;
      }
      tend = i;
      text[tend] = '\0';

      char *saveptr = NULL;
      char *word = strtok_r(text+tstart, whitespace, &saveptr);
      while(word){
        mymap(this, word, ptr);
        word = strtok_r(NULL,whitespace,&saveptr);
      }
}

      foff += readsize;
      fsize -= readsize;
      
      for(int i =0; i < boff; i++) text[i] = text[input_buffer_size-boff+i];
    }
    
    fclose(fp);
  }

  delete []  text;

  mode = NoneMode;

  uint64_t count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  return count;
}

/*
 * map: (files as input, user-defined map reads files)
 * argument:
 *  filepath:   input file path
 *  sharedflag: 0 for local file system, 1 for global file system
 *  recurse:    1 for resucse
 *  mymap:      user-defined map
 *  ptr:        user-defined pointer
 * return:
 *  global KV count
 */
uint64_t MapReduce::map(char *filepath, int sharedflag, int recurse, 
  void (*mymap) (MapReduce *, const char *, void *), void *ptr){
  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: map start. (File name to mymap)\n", me, nprocs);
  // create new data object
  if(data) delete data;
  ifiles.clear();
  KeyValue *kv = new KeyValue(kvtype, 
                              blocksize, 
                              nmaxblock, 
                              maxmemsize, 
                              outofcore, 
                              tmpfpath);

  // distribute input fil list
  disinputfiles(filepath, sharedflag, recurse);

  data = kv;
  kv->setKVsize(ksize, vsize);


  // create communicator
  //c = new Alltoall(comm, tnum);
  if(commmode==0)
    c = new Alltoall(comm, tnum);
  else if(commmode==1)
    c = new Ptop(comm, tnum);
  c->setup(lbufsize, gbufsize, kvtype, ksize, vsize);
  c->init(data);

  mode = MapMode;

#if GATHER_STAT
  //int tpar=st.init_timer("map parallel");
  //double t1 = MPI_Wtime();
#endif

  //printf("begin parallel\n");

#pragma omp parallel
{
  int tid = omp_get_thread_num();
  tinit(tid);
  
  int fcount = ifiles.size();
#pragma omp for nowait
  for(int i = 0; i < fcount; i++){
    mymap(this, ifiles[i].c_str(), ptr);
  }
  c->twait(tid);
}
  c->wait();

  //printf("end parallel\n");

#if GATHER_STAT
  //double t2 = MPI_Wtime();
  //st.inc_timer(tpar, t2-t1);
#endif

  // delete communicator
  delete c;
  c = NULL; 

  mode = NoneMode;

  // sum KV count
  uint64_t sum = 0, count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  MPI_Allreduce(&count, &sum, 1, MPI_UINT64_T, MPI_SUM, comm);

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: map end (output count: local=%ld, global=%ld).\n", me, nprocs, count, sum);

  return sum;
}

/*
 * map_local: (files as input, user-defined map reads files)
 * argument:
 *  filepath:   input file path
 *  sharedflag: 0 for local file system, 1 for global file system
 *  recurse:    1 for resucse
 *  mymap:      user-defined map
 *  ptr:        user-defined pointer
 * return:
 *  local KV count
 */
uint64_t MapReduce::map_local(char *filepath, int sharedflag, int recurse, 
  void (*mymap) (MapReduce *, const char *, void *), void *ptr){

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: map local start. (File name to mymap)\n", me, nprocs);

  // create data object
  if(data) delete data;
  ifiles.clear();

  KeyValue *kv = new KeyValue(kvtype, 
                              blocksize, 
                              nmaxblock, 
                              maxmemsize, 
                              outofcore, 
                              tmpfpath);
  data = kv;
  kv->setKVsize(ksize, vsize);

  // distribute input file list
  disinputfiles(filepath, sharedflag, recurse);

  mode = MapLocalMode;

#pragma omp parallel
{
  int tid = omp_get_thread_num();
  tinit(tid);

  int fcount = ifiles.size();
#pragma omp for
  for(int i = 0; i < fcount; i++){
    //std::cout << ifiles[i] << std::endl;
    mymap(this, ifiles[i].c_str(), ptr);
  }
}

  mode = NoneMode;

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: map local end.\n", me, nprocs);

  // sum KV count
  uint64_t count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  return count;
}

/*
 * map: (KV object as input)
 * argument:
 *  mr:     input mapreduce object
 *  mymap:  user-defined map
 *  ptr:    user-defined pointer
 * return:
 *  global KV count
 */
uint64_t MapReduce::map(MapReduce *mr, 
    void (*mymap)(MapReduce *, char *, int, char *, int, void*), void *ptr){

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: map start. (KV as input)\n", me, nprocs);

  // save original data object
  DataObject *data = mr->data;
  if(!data || data->getDatatype() != KVType){
    LOG_ERROR("%s","Error in map_local: input object of map must be KV object\n");
    return -1;
  }

  // create new data object
  KeyValue *kv = new KeyValue(kvtype, 
                              blocksize, 
                              nmaxblock, 
                              maxmemsize, 
                              outofcore, 
                              tmpfpath);
  kv->setKVsize(ksize, vsize);
  this->data = kv;


  // create communicator
  //c = new Alltoall(comm, tnum);
  if(commmode==0)
    c = new Alltoall(comm, tnum);
  else if(commmode==1)
    c = new Ptop(comm, tnum);

  c->setup(lbufsize, gbufsize, kvtype, ksize, vsize);
  c->init(kv);

  mode = MapMode;

  //printf("begin parallel\n");

  KeyValue *inputkv = (KeyValue*)data;
#pragma omp parallel
{
  int tid = omp_get_thread_num();
  tinit(tid);  

  char *key, *value;
  int keybytes, valuebytes;

  int i;
#pragma omp for nowait
  for(i = 0; i < inputkv->nblock; i++){
    int offset = 0;

    inputkv->acquireblock(i);

    offset = inputkv->getNextKV(i, offset, &key, keybytes, &value, valuebytes);
  
    while(offset != -1){

      mymap(this, key, keybytes, value, valuebytes, ptr);

      offset = inputkv->getNextKV(i, offset, &key, keybytes, &value, valuebytes);
    }
   
    inputkv->releaseblock(i);
  }

  c->twait(tid);
}
  //printf("end parallel\n");

  c->wait();


  //printf("begin delete\n");
  // delete data object
  delete inputkv;
  //printf("end delete\n");

  // delete communicator
  delete c;
  c = NULL;
 
  mode= NoneMode;

  // sum KV count
  uint64_t sum = 0, count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  MPI_Allreduce(&count, &sum, 1, MPI_UINT64_T, MPI_SUM, comm);

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: map end (output count: local=%ld, global=%ld).\n", me, nprocs, count, sum); 

  return sum;
}

/*
 * map_local: (KV object as input)
 * argument:
 *  mr:     input mapreduce object
 *  mymap:  user-defined map
 *  ptr:    user-defined pointer
 * return:
 *  local KV count
 */
uint64_t MapReduce::map_local(MapReduce *mr, 
    void (*mymap)(MapReduce *, char *, int, char *, int, void *), void *ptr){

  LOG_PRINT(DBG_GEN, "%s", "MapReduce: map local start. (KV as input)\n");

  // save original data object
  DataObject *data = mr->data;
  if(!data || data->getDatatype() != KVType){
    LOG_ERROR("%s","Error in map_local: input object of map must be KV object\n");
    return -1;
  }

  // create new data object
  KeyValue *kv = new KeyValue(kvtype, 
                              blocksize, 
                              nmaxblock, 
                              maxmemsize, 
                              outofcore, 
                              tmpfpath);
  this->data = kv;
  kv->setKVsize(ksize, vsize);

  mode = MapLocalMode;

  KeyValue *inputkv = (KeyValue*)data;

#pragma omp parallel
{
  int tid = omp_get_thread_num();
  tinit(tid);
  
  char *key, *value;
  int keybytes, valuebytes;

#pragma omp for nowait
  for(int i = 0; i < inputkv->nblock; i++){
    int offset = 0;

    inputkv->acquireblock(i);

    offset = inputkv->getNextKV(i, offset, &key, keybytes, &value, valuebytes);
  
    while(offset != -1){
      mymap(this, key, keybytes, value, valuebytes, ptr);

      offset = inputkv->getNextKV(i, offset, &key, keybytes, &value, valuebytes);
    }
    
    inputkv->releaseblock(i);
  }
}

  delete inputkv;

  mode = NoneMode;

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: map local end.\n", me, nprocs);

  uint64_t count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  return count;
}

/*
 * convert: convert KMV to KV
 *   return: local kmvs count
 */
uint64_t MapReduce::convert(){

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: convert start.\n", me, nprocs);

#if SAFE_CHECK
  if(!data || data->getDatatype() != KVType){
    LOG_ERROR("%s", "Error: input of convert must be KV data!\n");
    return -1;
  }
#endif

  nbucket = pow(2, BUCKET_SIZE);

  KeyValue *kv = (KeyValue*)data;
  int kvtype=kv->getKVtype();

#if 0
  // Convert KV to thread KV
  int istkv=0;
  KeyValue *tkv = NULL;
  if(kv->nblock > tnum){
    tkv = new KeyValue(KVType,
		blocksize,
		nmaxblock,
		nmaxblock*blocksize,
		outofcore,
		tmpfpath);
    tkv->setKVsize(ksize, vsize);

    KV_Block_info *block_info = new KV_Block_info[kv->nblock];
    kv2tkv(kv, tkv, block_info);
    delete kv;
    kv=tkv;
    istkv=1;
    tkv->print();
  }
#endif

  // Create KMV Object
  KeyMultiValue *kmv = new KeyMultiValue(kvtype,
                             blocksize, 
                             nmaxblock, 
                             maxmemsize,
                             outofcore, 
                             tmpfpath);
  kmv->setKVsize(ksize, vsize);

#pragma omp parallel
{
  int tid = omp_get_thread_num();
  tinit(tid);

  LOG_PRINT(DBG_CVT, "%d[%d] Convert: thread %d parallel start.\n", me, nprocs, tid);

  // initialize the unique list
  UniqueList *u=new UniqueList();
  u->ulist = new Unique*[nbucket];
  memset(u->ulist, 0, nbucket*sizeof(Unique*));
  u->unique_pool=new Spool(UNIQUE_SIZE*UNIT_SIZE);
  u->set_pool=new Spool((SET_COUNT)*sizeof(Set));
  u->nunique=0;
  u->ubuf=u->unique_pool->addblock();
  u->uoff=0;
  u->sets=NULL;
  u->nset=0;

  // initialize partition structure
  Partition *p=new Partition();
  p->set_id_pool = new Spool((KEY_COUNT)*sizeof(int));
  p->tmp_kv_pool = new Spool(blocksize*UNIT_SIZE);
  p->nkey=0;
  p->set_id=(int*)p->set_id_pool->addblock();
  p->tmp_kv=p->tmp_kv_pool->addblock();
  p->tmp_kv_off=0;
  p->index=0;

  // store results of local convert
  int tmpsize = TMP_BLOCK_SIZE*UNIT_SIZE;
  DataObject *tmpdata = NULL;
  tmpdata = new KeyValue(ByteType, 
                  TMP_BLOCK_SIZE, 
                  nmaxblock, 
                  nmaxblock*TMP_BLOCK_SIZE, 
                  outofcore, 
                  tmpfpath,
                  0);

  // <Key, Value> Variable
  char *key, *value;
  int keybytes, valuebytes, kvsize;

  int isfirst=1, start_block=0, mvbytes=0, kmvbytes=0;

  // scan all kvs to gain the thread kvs
  for(int i = 0; i < kv->nblock; i++){

    kv->acquireblock(i);

    int   datasize  = kv->getblocktail(i);
    char *kvbuf     = kv->getblockbuffer(i);
    char *kvbuf_end =kvbuf+datasize;

    // Scan KVs
    while(kvbuf < kvbuf_end){
      // Get Key and Value
      char *kvbuf_start=kvbuf;
      keybytes = *(int*)(kvbuf);
      valuebytes = *(int*)(kvbuf+oneintlen);
      kvbuf += twointlen;
      kvbuf = ROUNDUP(kvbuf, (ALIGNK-1));
      key = kvbuf;
      kvbuf += keybytes;
      kvbuf = ROUNDUP(kvbuf, (ALIGNV-1));
      value = kvbuf;
      kvbuf += valuebytes;
      kvbuf = ROUNDUP(kvbuf, (ALIGNT-1));
      kvsize=kvbuf-kvbuf_start;

      //printf("key=%s, value=%s\n", key, value);

      // Get hash
      uint32_t hid = hashlittle(key, keybytes, 0);
      if(hid%tnum != tid) continue;

      // Find the key
      int ibucket = hid % nbucket;
      Unique *ukey, *pre;
      int ret = findukey(u->ulist, ibucket, key, keybytes, &ukey, &pre);

      // Decide if we need a local convert 
      int mv_inc=sizeof(int)+valuebytes;
      if(isfirst){
        kmvbytes+=mv_inc;
        if(!ret) kmvbytes+=(keybytes+2*sizeof(int));

        if(kmvbytes > blocksize*UNIT_SIZE){
          LOG_ERROR("%s", "Single block support!\n");
          // convert KV to temp MV
          unique2tmp_first();
          isfirst=0;
        }
      }
      if(mvbytes + mv_inc > tmpsize){
        LOG_ERROR("%s", "Single block support!\n");
        unique2tmp();
        mvbytes=0;
      }
      mvbytes += mv_inc;

      // Key hit
      Set *pset=NULL;
      if(ret){
        ukey->nvalue++;
        ukey->mvbytes += valuebytes;
        if(!isfirst){
          pset=ukey->mysets;
          // Add a new block
          if(pset->mv_block_id != -1){
            // add a block
            pset=&(u->sets)[(u->nset)%SET_COUNT];

            pset->myid=u->nset;
            u->nset++;
            pset->nvalue=0;
            pset->mvbytes=0;
            pset->soffset=NULL;
            pset->voffset=NULL;
            pset->mv_block_id=-1;

            pset->next = ukey->mysets;
            ukey->mysets=pset;


            if(u->nset%SET_COUNT==0){
              u->sets=(Set*)u->set_pool->addblock();
            }
          }

          // add key information into block
          pset->nvalue++;
          pset->mvbytes += valuebytes;
        }
        
      // add unique key
      }else{
        //checkbuffer(sizeof(Unique)+keybytes, &ubuf, &uoff, unique_pool);
        ukey=(Unique*)(u->ubuf+u->uoff);
        u->uoff += sizeof(Unique);

        // add to the list
        ukey->next = NULL;
        if(pre == NULL)
          u->ulist[ibucket] = ukey;
        else
          pre->next = ukey;

        // copy key
        ukey->key = u->ubuf+u->uoff;
        memcpy(u->ubuf+u->uoff, key, keybytes);
        u->uoff += keybytes;
 
        ukey->keybytes=keybytes;
        ukey->nvalue=1;
        ukey->mvbytes=valuebytes;
                
        if(!isfirst){
          pset=&(u->sets)[(u->nset)%SET_COUNT];
          pset->next=NULL;

          ukey->mysets=pset;             
 
          pset->myid=u->nset;
          u->nset++;
          pset->nvalue=1;
          pset->mvbytes=valuebytes;
          pset->soffset=NULL;
          pset->voffset=NULL;
          pset->mv_block_id=-1;

          if((u->nset)%SET_COUNT==0){
            u->sets=(Set*)u->set_pool->addblock();
          }
        }

        u->nunique++;
      }

      //printf("tmp kv offset=%d, kvsize=%d\n", p->tmp_kv_off, kvsize);
      // Copy KV into KV pool
      if(p->tmp_kv_off+kvsize>p->tmp_kv_pool->blocksize){
        memset(p->tmp_kv+p->tmp_kv_off, 0, p->tmp_kv_pool->blocksize-p->tmp_kv_off);
        p->tmp_kv=p->tmp_kv_pool->addblock();
        p->tmp_kv_off=0;
      }
      memcpy(p->tmp_kv+p->tmp_kv_off, kvbuf_start, kvsize);
      p->tmp_kv_off += kvsize;
     
      if(isfirst)
        p->set_id[p->nkey%KEY_COUNT]=u->nunique-1;
      else
        p->set_id[p->nkey%KEY_COUNT]=pset->myid;

      p->nkey++;
      if(p->nkey%KEY_COUNT==0){
        p->set_id = (int*)p->set_id_pool->addblock();
      }
    }// end while(kvbuf < kvbuf_end)
    
    kv->releaseblock(i);
  } // scan kvs

#if 0
#if SAFE_CHECK
    if(mvbytes > tmpdata->getblocksize()){
      LOG_ERROR("The mvbytes %d is larger than the block size %d\n", mvbytes, tmpdata->getblocksize());
    }
#endif

    LOG_PRINT(DBG_CVT, "%d[%d] Convert: thread %d block %d stage 2.2 start.\n", me, nprocs, tid, i);

#if GATHER_STAT
    double tt2=omp_get_wtime();
    if(tid==TID) st.inc_timer(tstage21, tt2-tt1);
#endif

#if 1
// Stage 2.2
#if 1
    if(blockid==-1){
      blockid = tmpdata->addblock();
      //printf("tmp data add block=%d\n", blockid);
      tmpdata->acquireblock(blockid);
    }

    //printf("mvbytes=%d, space=%d\n", mvbytes, tmpdata->getblockspace(blockid));

    if(mvbytes > tmpdata->getblockspace(blockid)){
      tmpdata->releaseblock(blockid);
      blockid=tmpdata->addblock();
      //printf("tmp data add block=%d\n", blockid);
      tmpdata->acquireblock(blockid);
    }

    char *outbuf=tmpdata->getblockbuffer(blockid);
    int   oldoff=tmpdata->getblocktail(blockid);
    int   outoff=oldoff;

    // printf("start block=%d, block number=%d\n", start_block, nblock);

    // compute blocks
    for(int k=start_block; k<nblock; k++){
      char *buf = block_pool->getblockbuffer(k/BLOCK_COUNT);
      Block *block = (Block*)buf+(k%BLOCK_COUNT);

      //if(kvtype==0){
        block->soffset=(int*)(outbuf+outoff);
        block->s_off=outoff;
        outoff+=(block->nvalue)*sizeof(int);
      //}
      block->voffset=outbuf+outoff;
      block->v_off=outoff;
      outoff+=block->mvbytes;
      block->blockid=blockid;
      block->nvalue=0;
      block->mvbytes=0;
    }

#if SAFE_CHECK
    if(outoff > tmpdata->getblocksize())
      LOG_ERROR("The offset %d of tmp data is larger than block size %d!\n", outoff, tmpdata->getblocksize());
    if(outoff-oldoff != mvbytes){
      LOG_ERROR("The offset %d isn't equal to mv bytes %d!\n", outoff, mvbytes);
    }
#endif

    tmpdata->setblockdatasize(blockid, outoff);
#endif
#endif

#if GATHER_STAT
    double tt3=omp_get_wtime();
    if(tid==TID) st.inc_timer(tstage22, tt3-tt2);
#endif

#if 1
// Stage 2.3
    LOG_PRINT(DBG_CVT, "%d[%d] Convert: thread %d block %d stage 2.3 start.\n", me, nprocs, tid, i);
    //printf("nblock=%d\n", nblock); fflush(stdout);
    //offset=0;
    int k=0;
    kvbuf = tkv->getblockbuffer(i);
    offset=info->tkv_off[tid];
    //int datasize=offset+info->tkv_size[i];
    while(kvbuf < kvbuf_end){ 
      //printf("%d\n", k/KEY_COUNT);
      int key_block_id = *((int*)(block_id_pool->blocks[k/KEY_COUNT])+(k%KEY_COUNT));
      //printf("key_block")
#if SAFE_CHECK
     if(key_block_id >= nblock)
       LOG_ERROR("Error: block id=%d is larger than block count %d!\n", key_block_id, nblock);
#endif      

      Block *block = (Block*)(block_pool->blocks[key_block_id/BLOCK_COUNT])+key_block_id%BLOCK_COUNT;

      char *key, *value;
      int keybytes, valuebytes;

      char *start_buf=kvbuf;
      keybytes = *(int*)(kvbuf);
      valuebytes = *(int*)(kvbuf+oneintlen);
      kvbuf += twointlen;
      kvbuf = ROUNDUP(kvbuf, (ALIGNK-1));
      key = kvbuf;
      kvbuf += keybytes;
      kvbuf = ROUNDUP(kvbuf, (ALIGNV-1));
      value = kvbuf;
      kvbuf += valuebytes;
      kvbuf = ROUNDUP(kvbuf, (ALIGNT-1));
      kvsize=kvbuf-start_buf;

      //printf("%s,%s\n", key, value);

#if 0
      if(kvtype==0){
        keybytes = *(int*)(kvbuf+offset);
        valuebytes = *(int*)(kvbuf+offset+oneintlen);
        key = kvbuf+offset+twointlen;
        value=kvbuf+offset+twointlen+keybytes;
        kvsize = twointlen+keybytes+valuebytes;
      }else if(kvtype==1){
        key=kvbuf+offset;
        keybytes = strlen(key)+1;
        value=kvbuf+offset+keybytes;
        valuebytes=strlen(value)+1;
        kvsize=keybytes+valuebytes;
      }else if(kvtype==2){
        key=kvbuf+offset;
        keybytes = tkv->getkeysize();
        value=kvbuf+offset+keybytes;
        valuebytes=tkv->getvalsize();
        kvsize=keybytes+valuebytes;
      }else if(kvtype==3){
        key=kvbuf+offset;
        keybytes = strlen(key)+1;
        value=NULL;
        valuebytes=0;
        kvsize=keybytes;
      }
#endif

      //printf("%d (%s,%s)\n", tid, key, value);

      //printf("key=%s\n", key);

      /*if(kvtype==0)*/ block->soffset[block->nvalue++]=valuebytes;
      memcpy(block->voffset+block->mvbytes, value, valuebytes);
      block->mvbytes+=valuebytes;
  
      k++;
      offset+=kvsize;

      //printf("kvsize=%d\n", kvsize);
    }
#endif

#if GATHER_STAT
    double tt4=omp_get_wtime();
    if(tid==TID) st.inc_timer(tstage23, tt4-tt3);
#endif
#endif

  //  block_id_pool->clear();
  //  kv->releaseblock(i);
  //}

  //if(blockid!=-1) tmpdata->releaseblock(blockid);

#if 0
  delete block_id_pool;

#pragma omp barrier
  if(tid==0){
    for(int i=0; i<tkv->nblock; i++){
      delete [] block_info[i].tkv_off;
      delete [] block_info[i].tkv_size;
    }
    delete [] block_info;
    delete tmp_kv_pool;
    delete tkv;
  }
#endif

  LOG_PRINT(DBG_CVT, "%d[%d] Convert: thread %d stage 3 start\n", me, nprocs, tid);

#if 1
///////////// Stage: 3
  //printf("kmvbytes=%d\n", kmvbytes);
  memset(u->ubuf+u->uoff, 0, u->unique_pool->blocksize-u->uoff);
  //printf("isfirst=%d\n", isfirst);
  if(isfirst){
    unique2kmv(u, p, kmv);
    delete p->set_id_pool;
    delete p->tmp_kv_pool;
    delete p;
  }
  else{
    LOG_ERROR("%s", "Single block support!\n");
    delete p->set_id_pool;
    delete p->tmp_kv_pool;
    delete p;
    //merge(tmpdata, kmv, unique_pool);
  }
#endif 
 
  delete [] u->ulist;
  delete u->unique_pool;
  delete u->set_pool;
  delete u;
  delete tmpdata;

  LOG_PRINT(DBG_CVT, "%d[%d] Convert: thread %d convert end.\n", me, nprocs, tid);

  //nitems[tid]=nunique;
} 

  //for(int i=0; i<kv->nblock; i++){
  //  delete block_info[i].hid_pool;
  //  delete block_info[i].pos_pool;
  //}
  //delete [] block_info;

#if GATHER_STAT
  double t2 = MPI_Wtime();
  st.inc_timer(tpar, t2-t1);
#endif

  // set new data
  // delete data;
  data = kmv;

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: convert end.\n", me, nprocs);

  uint64_t count = 0, sum = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  MPI_Allreduce(&count, &sum, 1, MPI_UINT64_T, MPI_SUM, comm);

  return sum;
}

/*
 * reduce:
 * argument:
 *  myreduce: user-defined reduce function
 *  ptr:      user-defined pointer
 * return:
 *  local KV count
 */
uint64_t MapReduce::reduce(void (myreduce)(MapReduce *, char *, int, int, char *, 
    int *, void*), void* ptr){
  if(!data || data->getDatatype() != KMVType){
    LOG_ERROR("%s", "Error: the input of reduce function must be KMV object!\n");
    return -1;
  }

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: reduce start.\n", me, nprocs);

  KeyMultiValue *kmv = (KeyMultiValue*)data;

  // create new data object
  KeyValue *kv = new KeyValue(kvtype, 
                      blocksize, 
                      nmaxblock, 
                      maxmemsize, 
                      outofcore, 
                      tmpfpath);
  kv->setKVsize(ksize, vsize);
  data = kv;

  mode = ReduceMode;

#pragma omp parallel
{
  int tid = omp_get_thread_num();
  tinit(tid);

  char *key, *values;
  int keybytes, nvalue, *valuebytes;

#pragma omp for nowait
  for(int i = 0; i < kmv->nblock; i++){
     int offset = 0;
     kmv->acquireblock(i);
     offset = kmv->getNextKMV(i, offset, &key, keybytes, nvalue, &values, &valuebytes);
     while(offset != -1){
       //printf("keybytes=%d, valuebytes=%d\n", keybytes, nvalue);
       myreduce(this, key, keybytes, nvalue, values, valuebytes, ptr);        
       offset = kmv->getNextKMV(i, offset, &key, keybytes, nvalue, &values, &valuebytes);
     }
     kmv->releaseblock(i);
  }
}

  delete kmv;
 
  mode = NoneMode;

  // sum KV count
  uint64_t count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: reduce end.(Output count: local=%ld)\n", me, nprocs, count);

  return count;
}

/*
 * scan: (KMV object as input)
 * argument:
 *  myscan: user-defined scan function
 *  ptr:    user-defined pointer
 * return:
 *  local KMV count
 */
// FIXME: should I provide multi-thread scan function?
uint64_t MapReduce::scan(void (myscan)(char *, int, int, char *, int *,void *), void * ptr){

  if(!data || data->getDatatype() != KMVType){
    LOG_ERROR("%s", "Error: the input of scan (KMV) must be KMV object!\n");
  }

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: scan start.\n", me, nprocs);  

  KeyMultiValue *kmv = (KeyMultiValue*)data;

  char *key, *values;
  int keybytes, nvalue, *valuebytes;

  int tid = omp_get_thread_num();
  tinit(tid);  

  for(int i = 0; i < kmv->nblock; i++){
    int offset = 0;
    
    kmv->acquireblock(i);
    
    offset = kmv->getNextKMV(i, offset, &key, keybytes, nvalue, &values, &valuebytes);

    while(offset != -1){
      myscan(key, keybytes, nvalue, values, valuebytes, ptr);

      nitems[tid]++;

      offset = kmv->getNextKMV(i, offset, &key, keybytes, nvalue, &values, &valuebytes);
    }

    kmv->releaseblock(i);
  }

  LOG_PRINT(DBG_GEN, "%d[%d] MapReduce: scan end.\n", me, nprocs);

  // sum KV count
  uint64_t count = 0; 
  for(int i = 0; i < tnum; i++) count += nitems[i];

  return count;
}

/*
 * add a KV (invoked in user-defined map or reduce functions) 
 *  argument:
 *   key:        key 
 *   keybytes:   keysize
 *   value:      value
 *   valuebytes: valuesize
 */
void MapReduce::add(char *key, int keybytes, char *value, int valuebytes){

#if SAFE_CHECK
  if(!data || data->getDatatype() != KVType){
    LOG_ERROR("%s", "Error: add function only can be used to generate KV object!\n");
  }

  if(mode == NoneMode){
    LOG_ERROR("%s", "Error: add function only can be invoked in user-defined map and reduce functions\n");
  }
#endif

  int tid = omp_get_thread_num();
 
  // invoked in map function
  if(mode == MapMode){

    // get target process
    uint32_t hid = 0;
    int target = 0;
    if(myhash != NULL){
      target=myhash(key, keybytes);
    }
    else{
      hid = hashlittle(key, keybytes, nprocs);
      target = hid % (uint32_t)nprocs;
    }

    // send KV    
    c->sendKV(tid, target, key, keybytes, value, valuebytes);

    nitems[tid]++;
 
    return;
   }

  // invoked in map_local or reduce function
  if(mode == MapLocalMode || mode == ReduceMode){
    
    // add KV into data object 
    KeyValue *kv = (KeyValue*)data;

    if(blocks[tid] == -1){
      blocks[tid] = kv->addblock();
    }

    kv->acquireblock(blocks[tid]);

    while(kv->addKV(blocks[tid], key, keybytes, value, valuebytes) == -1){
      kv->releaseblock(blocks[tid]);
      blocks[tid] = kv->addblock();
      kv->acquireblock(blocks[tid]);
    }

    kv->releaseblock(blocks[tid]);
    nitems[tid]++;
  }

  return;
}

void MapReduce::print_stat(int verb, FILE *out){
#if GATHER_STAT
  st.print(verb, out);
#endif
}

void MapReduce::clear_stat(){
#if GATHER_STAT
  st.clear();
#endif
}

/*
 * Output data in this object
 *  type: 0 for string, 1 for int, 2 for int64_t
 *  fp:     file pointer
 *  format: hasn't been used
 */
void MapReduce::output(int type, FILE* fp, int format){
  if(data){
    data->print(type, fp, format);
  }else{
    LOG_ERROR("%s","Error to output empty data object\n");
  }
}

// private function
/*****************************************************************************/

// process init
void MapReduce::init(){
  blocksize = BLOCK_SIZE;
  nmaxblock = MAX_BLOCKS;
  maxmemsize = MAXMEM_SIZE;
  lbufsize = LOCAL_BUF_SIZE;
  gbufsize = GLOBAL_BUF_SIZE;

  kvtype = KV_TYPE;

  outofcore = OUT_OF_CORE; 
  tmpfpath = std::string(TMP_PATH);

  commmode=0;
  
  myhash = NULL;
}

// thread init
void MapReduce::tinit(int tid){
  blocks[tid] = -1;
  nitems[tid] = 0;
}

// distribute input file list
void MapReduce::disinputfiles(const char *filepath, int sharedflag, int recurse){
  getinputfiles(filepath, sharedflag, recurse);

  if(sharedflag){
    int fcount = ifiles.size();
    int div = fcount / nprocs;
    int rem = fcount % nprocs;
    int *send_count = new int[nprocs];
    int total_count = 0;

    if(me == 0){
      int j = 0, end=0;
      for(int i = 0; i < nprocs; i++){
        send_count[i] = 0;
        end += div;
        if(i < rem) end++;
        while(j < end){
          send_count[i] += strlen(ifiles[j].c_str())+1;
          j++;
        }
        total_count += send_count[i];
      }
    }

    int recv_count;
    MPI_Scatter(send_count, 1, MPI_INT, &recv_count, 1, MPI_INT, 0, comm);

    int *send_displs = new int[nprocs];
    if(me == 0){
      send_displs[0] = 0;
      for(int i = 1; i < nprocs; i++){   
        send_displs[i] = send_displs[i-1]+send_count[i-1];
      }
    }

    char *send_buf = new char[total_count];
    char *recv_buf = new char[recv_count];

    if(me == 0){
      int offset = 0;
      for(int i = 0; i < fcount; i++){
          memcpy(send_buf+offset, ifiles[i].c_str(), strlen(ifiles[i].c_str())+1);
          offset += strlen(ifiles[i].c_str())+1;
        }
    }

    MPI_Scatterv(send_buf, send_count, send_displs, MPI_BYTE, recv_buf, recv_count, MPI_BYTE, 0, comm);

    ifiles.clear();
    int off=0;
    while(off < recv_count){
      char *str = recv_buf+off;
      ifiles.push_back(std::string(str));
      off += strlen(str)+1;
    }

    delete [] send_count;
    delete [] send_displs;
    delete [] send_buf;
    delete [] recv_buf;
  }
}

// get input file list
void MapReduce::getinputfiles(const char *filepath, int sharedflag, int recurse){
  // if shared, only process 0 read file names
  if(!sharedflag || (sharedflag && me == 0)){
    
    struct stat inpath_stat;
    int err = stat(filepath, &inpath_stat);
    if(err) LOG_ERROR("Error in get input files, err=%d\n", err);
    
    // regular file
    if(S_ISREG(inpath_stat.st_mode)){
      ifiles.push_back(std::string(filepath));
    // dir
    }else if(S_ISDIR(inpath_stat.st_mode)){
      
      struct dirent *ep;
      DIR *dp = opendir(filepath);
      if(!dp) LOG_ERROR("%s", "Error in get input files\n");
      
      while(ep = readdir(dp)){
        
        if(ep->d_name[0] == '.') continue;
       
        char newstr[MAXLINE]; 
        sprintf(newstr, "%s/%s", filepath, ep->d_name);
        err = stat(newstr, &inpath_stat);
        if(err) LOG_ERROR("Error in get input files, err=%d\n", err);
        
        // regular file
        if(S_ISREG(inpath_stat.st_mode)){
          ifiles.push_back(std::string(newstr));
        // dir
        }else if(S_ISDIR(inpath_stat.st_mode) && recurse){
          getinputfiles(newstr, sharedflag, recurse);
        }
      }
    }
  }  
}

#if 0
void MapReduce::merge(DataObject *tmpdata, KeyMultiValue *kmv, Spool *unique_pool){
  int kmvtype=kmv->getKMVtype();
  int blocksize=unique_pool->getblocksize();

  char *inbuf=NULL, *outbuf=NULL;
  int inoff=0, outoff=0;
  
  int blockid = kmv->addblock();
  kmv->acquireblock(blockid);
  
  outbuf = kmv->getblockbuffer(blockid);
  outoff = 0;

  for(int i=0; i < unique_pool->nblock; i++){
    inbuf = unique_pool->getblockbuffer(i);
    while(1){
      Unique *ukey = (Unique*)(inbuf+inoff);
      if((blocksize-inoff < sizeof(Unique)) || (ukey->key==NULL))
        break;

      int kmvsize=ukey->keybytes+ukey->mvbytes+sizeof(int);
      if(kmvtype==0) kmvsize += (ukey->nvalue+1)*sizeof(int);

#if SAFE_CHECK
      if(kmvsize > kmv->getblocksize()){
        LOG_ERROR("Error: KMV size %d is larger than block size %d\n", kmvsize, kmv->getblocksize());
      }
#endif

      if(outoff+kmvsize>kmv->getblocksize()){

#if SAFE_CHECK
        if(outoff > kmv->getblocksize()){
          LOG_ERROR("Error: offset in KMV %d is larger than block size %d\n", outoff, kmv->getblocksize());
        }
#endif
        kmv->setblockdatasize(blockid, outoff);
        kmv->releaseblock(blockid);
        blockid=kmv->addblock();
        kmv->acquireblock(blockid);
        outbuf=kmv->getblockbuffer(blockid);
        outoff=0;
      }
     
      if(kmvtype==0){
        *(int*)(outbuf+outoff)=ukey->keybytes;
        outoff+=sizeof(int);
      }

      memcpy(outbuf+outoff, ukey->key, ukey->keybytes);
      outoff+=ukey->keybytes;
      *(int*)(outbuf+outoff)=ukey->nvalue;
      outoff+=sizeof(int);

      if(kmvtype==0){
        ukey->soffset=(int*)(outbuf+outoff);
        outoff+=(ukey->nvalue)*sizeof(int);
      }

      ukey->voffset=outbuf+outoff;
      outoff+=(ukey->mvbytes);

      ukey->nvalue=0;
      ukey->mvbytes=0;

      // copy block information
      Block *block = ukey->blocks;
      do{
        tmpdata->acquireblock(block->blockid);

        char *tmpbuf = tmpdata->getblockbuffer(block->blockid);
        block->soffset = (int*)(tmpbuf + block->s_off);
        block->voffset = tmpbuf + block->v_off;
 
        if(kmvtype==0){
          memcpy(ukey->soffset+ukey->nvalue, block->soffset, (block->nvalue)*sizeof(int));
          ukey->nvalue+=block->nvalue;
        }
 
        memcpy(ukey->voffset+ukey->mvbytes, block->voffset, block->mvbytes);
        ukey->mvbytes += block->mvbytes;

        tmpdata->releaseblock(block->blockid);
      }while(block = block->next);     

      inoff+=(ukey->keybytes+sizeof(Unique));

#if SAFE_CHECK
      if(inoff > blocksize)
        LOG_ERROR("Error: offset %d in unique pool is larger than block size %d\n", inoff, blocksize);
#endif

    }
    inoff=0;
  }

#if SAFE_CHECK
  if(outoff > kmv->getblocksize()){
    LOG_ERROR("Error: offset in KMV %d is larger than block size %d\n", outoff, kmv->getblocksize());
  }
#endif

  kmv->setblockdatasize(blockid, outoff);
  kmv->releaseblock(blockid);
}
#endif

void MapReduce::kv2tkv(KeyValue *kv, KeyValue *tkv, KV_Block_info *block_info){

#pragma omp parallel
{
  char *key, *value;
  int keybytes, valuebytes, kvsize;

  Spool *hid_pool = new Spool(KEY_COUNT*sizeof(uint32_t));
  int *insert_off = new int[tnum];

// Stage: 1.1
  // Gather KV information in block
#pragma omp for
  for(int i = 0; i < kv->nblock; i++){

    int tkv_blockid=tkv->addblock();
    KV_Block_info *info=&block_info[tkv_blockid];
    Spool *hid_pool = new Spool(KEY_COUNT*sizeof(uint32_t));

    uint32_t *key_hid = (uint32_t*)hid_pool->addblock();
    int  nkey = 0;

    info->tkv_off = new int[tnum];
    info->tkv_size = new int[tnum];
    memset(info->tkv_off, 0, tnum*sizeof(int));
    memset(info->tkv_size, 0, tnum*sizeof(int));

    kv->acquireblock(i);
 
    char *kvbuf = kv->getblockbuffer(i);
    int datasize = kv->getblocktail(i);
    char *kvbuf_end = kvbuf+datasize;

     //int offset=0;
    while(kvbuf < kvbuf_end){
      
        char *kvbuf_start=kvbuf;
        keybytes = *(int*)(kvbuf);
        valuebytes = *(int*)(kvbuf+oneintlen);

        kvbuf += twointlen;
        kvbuf = ROUNDUP(kvbuf, (ALIGNK-1));
        key = kvbuf;
        kvbuf += keybytes;
        kvbuf = ROUNDUP(kvbuf, (ALIGNV-1));
        value = kvbuf;
        kvbuf += valuebytes;
        kvbuf = ROUNDUP(kvbuf, (ALIGNT-1));
        kvsize=kvbuf-kvbuf_start;


      uint32_t hid = hashlittle(key, keybytes, 0);
      info->tkv_size[hid%tnum]+=kvsize;

      key_hid[nkey%KEY_COUNT]=hid;
      nkey++;
      if(nkey % KEY_COUNT==0){
        key_hid = (uint32_t*)hid_pool->addblock();
      }

      //offset+=kvsize;
    }

    insert_off[0]=info->tkv_off[0]=0;
    for(int j=1; j<tnum; j++){
      info->tkv_off[j]=info->tkv_off[j-1]+info->tkv_size[j-1];
      insert_off[j]=info->tkv_off[j];
    }

    tkv->acquireblock(tkv_blockid);
    char *tkvbuf = tkv->getblockbuffer(tkv_blockid);

    kvbuf = kv->getblockbuffer(i);
    for(int j=0; j<nkey; j++){  

      char *kvbuf_start=kvbuf;
      keybytes = *(int*)(kvbuf);
      valuebytes = *(int*)(kvbuf+oneintlen);

      kvbuf += twointlen;
      kvbuf = ROUNDUP(kvbuf, (ALIGNK-1));
      key = kvbuf;
      kvbuf += keybytes;
      kvbuf = ROUNDUP(kvbuf, (ALIGNV-1));
      value = kvbuf;
      kvbuf += valuebytes;
      kvbuf = ROUNDUP(kvbuf, (ALIGNT-1));
      kvsize=kvbuf-kvbuf_start;
      uint32_t hid = *((int*)hid_pool->blocks[j/KEY_COUNT]+j%KEY_COUNT);

      int tkvoff = insert_off[hid%tnum];
 
      memcpy(tkvbuf+tkvoff, kvbuf_start, kvsize);
      insert_off[hid%tnum]+=kvsize;
      //offset+=kvsize;
    }
#if GATHER_STAT
    double tt3=omp_get_wtime();
    if(tid==TID) st.inc_timer(tstage12, tt3-tt2);
#endif

    hid_pool->clear();

    tkv->setblockdatasize(tkv_blockid, datasize);
    tkv->releaseblock(tkv_blockid);
    kv->releaseblock(i);

  }

  delete insert_off;
  delete hid_pool;
}// end parallel

}

#if 0
void MapReduce::unique2tmp_first(int nunique, DataObject *mv, Spool *unique_pool){
  
  int blockid = mv->addblock();
  mv->acquireblock(blockid);

  int  uidx=0;
  char *ubuf=unique_pool->blocks[uidx++];
  for(int i = 0; i < nunique; i++){
    
  } 

  mv->releaseblock(blockid);

}
#endif

void MapReduce::unique2tmp_first(){
}
void MapReduce::unique2tmp(){
}

void MapReduce::unique2kmv(UniqueList *u, Partition *p, KeyMultiValue *kmv){
  //printf("unique2kmv start\n") ; 

  int kmv_block_id=kmv->addblock();
  kmv->acquireblock(kmv_block_id);
  
  char *kmv_buf=kmv->getblockbuffer(kmv_block_id);
  int kmv_off=0;

  char *key, *val;
  int keybytes, valbytes, kvsize;
  Spool *unique_pool=u->unique_pool;
  for(int i=0; i<unique_pool->nblock; i++){
    char *ubuf=unique_pool->blocks[i];
    int uoff=0;
    while(uoff < unique_pool->blocksize){
      Unique *ukey = (Unique*)(ubuf+uoff);
      if((unique_pool->blocksize-uoff < sizeof(Unique)) || (ukey->key==NULL))
        break;
      *(int*)(kmv_buf+kmv_off)=ukey->keybytes;
      kmv_off+=sizeof(int);
      memcpy(kmv_buf+kmv_off, ukey->key, ukey->keybytes);
      kmv_off+=ukey->keybytes;
      *(int*)(kmv_buf+kmv_off)=ukey->nvalue;
      kmv_off+=sizeof(int);
      ukey->soffset=(int*)(kmv_buf+kmv_off);
      kmv_off+=ukey->nvalue*sizeof(int);
      ukey->voffset=kmv_buf+kmv_off;
      kmv_off+=ukey->mvbytes;
      uoff+=sizeof(Unique)+ukey->keybytes;
      
      //printf("key=%s, nvalue=%d, mvbytes=%d\n", ukey->key, ukey->nvalue, ukey->mvbytes);
     
      ukey->nvalue=0;
      ukey->mvbytes=0;

      //printf("kmv offset=%d\n", kmv_off);
    }
  }

  //printf("kmv_off=%d\n", kmv_off);
  kmv->setblockdatasize(kmv_block_id, kmv_off);

  Spool *tmp_kv_pool=p->tmp_kv_pool;
  //printf("")
  for(int i=0; i<tmp_kv_pool->nblock; i++){
    char *kvbuf=tmp_kv_pool->blocks[i];
    char *kvbuf_end=kvbuf+tmp_kv_pool->blocksize;
    //int kvoff=0;
    while(kvbuf<kvbuf_end){
      //printf("kvbuf=%p, kvbuf_end=%p\n", kvbuf, kvbuf_end) ;     
 
      if(kvbuf_end-kvbuf<twointlen) break;
      char *kvbuf_start=kvbuf;
      keybytes = *(int*)kvbuf;
      valbytes = *(int*)(kvbuf+oneintlen);
      //printf("keybytes=%d, valytes=%d\n", keybytes, valbytes);
      
      if(keybytes+valbytes<=0) break;

      kvbuf += twointlen;
      kvbuf = ROUNDUP(kvbuf, (ALIGNK-1));
      key = kvbuf;
      kvbuf += keybytes;
      kvbuf = ROUNDUP(kvbuf, (ALIGNV-1));
      val = kvbuf;
      kvbuf += valbytes;
      kvbuf = ROUNDUP(kvbuf, (ALIGNT-1));
      kvsize = kvbuf-kvbuf_start;

      //printf("key=%s, value=%s, kvsize=%d\n", key, val, kvsize);    
 
      uint32_t hid = hashlittle(key, keybytes, 0);
      // Find the key
      int ibucket = hid % nbucket;
      Unique *ukey, *pre;
      int ret = findukey(u->ulist, ibucket, key, keybytes, &ukey, &pre);
      ukey->soffset[ukey->nvalue]=valbytes;
      memcpy(ukey->voffset+ukey->mvbytes, val, valbytes);
      ukey->nvalue++;
      ukey->mvbytes+=valbytes;

      //printf("key=%s\n", ukey->key);
    }
  }

  kmv->releaseblock(kmv_block_id);
}

// find the key in the unique list
int MapReduce::findukey(Unique **unique_list, int ibucket, char *key, int keybytes, Unique **unique, Unique **pre){
  Unique *list = unique_list[ibucket];
  
  if(list == NULL){
    *unique = NULL;
    if(pre) *pre = NULL;
    return 0;
  }

  int ret = 0;
  if(pre) *pre = NULL;
  do{
    if(list->keybytes == keybytes && 
      memcmp(list->key, key, keybytes) == 0){
      ret = 1;
      break;
    }
    if(pre) *pre = list;
  }while(list = list->next);

  *unique = list;
  return ret;
}

// check if we need add another block
void MapReduce::checkbuffer(int bytes, char **buf, int *off, Spool *pool){
  int blocksize = pool->getblocksize();
  if(*off+bytes > blocksize){
    memset(*buf+*off, 0, blocksize-*off);
    *buf = pool->addblock();
    *off = 0;
  }
}

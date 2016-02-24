#include <string.h>
#include <string>
#include "keyvalue.h"

#include "log.h"

#include "const.h"

using namespace MAPREDUCE_NS;


KeyValue::KeyValue(
  int _kvtype,
  int blocksize, 
  int maxblock,
  int maxmemsize,
  int outofcore,
  std::string filename,
  int threadsafe):
  DataObject(KVType, blocksize, 
    maxblock, maxmemsize, outofcore, filename, threadsafe){
  kvtype = _kvtype;

  ksize = vsize = 0;

  LOG_PRINT(DBG_DATA, "%s", "DATA: KV Create.\n");
}

KeyValue::~KeyValue(){
  LOG_PRINT(DBG_DATA, "%s", "DATA: KV Destroy.\n");
}


int KeyValue::getNextKV(int blockid, int offset, char **key, int &keybytes, char **value, int &valuebytes, int *kff, int *vff){
  if(offset >= blocks[blockid].datasize) return -1;
  
  int bufferid = blocks[blockid].bufferid;
  char *buf = buffers[bufferid].buf + offset;

  int kvsize=0;
  GET_KV_VARS(kvtype,buf,*key,keybytes,*value,valuebytes,kvsize,this);

  offset+=kvsize;

  return offset;
}

/*
 * Add a KV
 * return 0 if success, else return -1
 */
int KeyValue::addKV(int blockid, char *key, int &keybytes, char *value, int &valuebytes){
  int kvbytes = 0;
  
  GET_KV_SIZE(kvtype, keybytes, valuebytes, kvbytes);

#if SAFE_CHECK
  if(kvbytes > blocksize){
     LOG_ERROR("Error: KV size is larger than block size. (KV size=%d, block size=%d)\n", kvbytes, blocksize);
  }
#endif

  int datasize = blocks[blockid].datasize;
  if(kvbytes+datasize > blocksize) return -1;

  int bufferid = blocks[blockid].bufferid;
  char *buf = buffers[bufferid].buf+datasize;

  PUT_KV_VARS(kvtype, buf, key, keybytes, value, valuebytes, kvbytes);
  blocks[blockid].datasize += kvbytes;

  return 0;
}

void KeyValue::print(int type, FILE *fp, int format){
  char *key, *value;
  int keybytes, valuebytes;

  fprintf(fp, "KV Object:\n");

  for(int i = 0; i < nblock; i++){
    int offset = 0;

    acquire_block(i);

    offset = getNextKV(i, offset, &key, keybytes, &value, valuebytes);

    while(offset != -1){
      if(type == 0) fprintf(fp, "%s", key);
      else if(type == 1) fprintf(fp, "%d", *(int*)key);
      else if(type == 2) fprintf(fp, "%ld", *(int64_t*)key);
      else LOG_ERROR("%s", "Error undefined output type\n");

      if(valuebytes != 0){
        if(type == 0) fprintf(fp, ",%s", value);
        else if(type == 1) fprintf(fp, ",%d", *(int*)value);
        else if(type == 2) fprintf(fp, ",%ld", *(int64_t*)value);
        else LOG_ERROR("%s", "Error undefined output type\n");
     }
     fprintf(fp, "\n");

      offset = getNextKV(i, offset, &key, keybytes, &value, valuebytes);
    }

    release_block(i);

  }
}

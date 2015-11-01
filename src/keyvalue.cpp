#include <string.h>
#include <string>
#include "keyvalue.h"

using namespace MAPREDUCE_NS;

KeyValue::KeyValue(
  int _kvtype,
  int blocksize, 
  int maxblock,
  int maxmemsize,
  int outofcore,
  std::string filename):
  DataObject(KVType, blocksize, 
    maxblock, maxmemsize, outofcore, filename){
  kvtype = _kvtype;
}

KeyValue::~KeyValue(){
}

int KeyValue::getNextKV(int blockid, int offset, char **key, int &keybytes, char **value, int &valuebytes, int *kff, int *vff){
  if(offset >= blocks[blockid].datasize) return -1;
  
  int bufferid = blocks[blockid].bufferid;
  char *buf = buffers[bufferid].buf + offset;

  if(kvtype == 0){
    *key = buf;
    keybytes = strlen(*key)+1;
    buf += keybytes;
    *value = buf;
    valuebytes = strlen(*value)+1;
    if(kff != NULL) *kff = offset;
    if(vff != NULL) *vff = (offset+keybytes);
    offset += keybytes+valuebytes;
  }else if(kvtype == 1){
    keybytes = *(int*)buf;
    buf += sizeof(int);
    *key = buf;
    buf += keybytes;
    valuebytes = *(int*)buf;
    buf += sizeof(int);
    *value = buf;
    if(kff != NULL) *kff = offset+sizeof(int);
    if(vff != NULL) *vff = offset+sizeof(int)+keybytes+sizeof(int);
    offset += 2*sizeof(int)+keybytes+valuebytes;
  }

  return offset;
}

/*
 * Add a KV
 * return 0 if success, else return -1
 */
int KeyValue::addKV(int blockid, char *key, int &keybytes, char *value, int &valuebytes){
  int kvbytes = 0;
  if(kvtype == 0) kvbytes = keybytes+valuebytes;
  else if(kvtype == 1) kvbytes = 2*sizeof(int)+keybytes+valuebytes;

  int datasize = blocks[blockid].datasize;
  if(kvbytes+datasize > blocksize) return -1;

  int bufferid = blocks[blockid].bufferid;
  char *buf = buffers[bufferid].buf;

  if(kvtype == 0){
    memcpy(buf+datasize, key, keybytes);
    datasize += keybytes;
    memcpy(buf+datasize, value, valuebytes);
    datasize += valuebytes;
  }else if(kvtype == 1){
    memcpy(buf+datasize, &keybytes, sizeof(int));
    datasize += sizeof(int);
    memcpy(buf+datasize, key, keybytes);
    datasize += keybytes;
    memcpy(buf+datasize, &valuebytes, sizeof(int));
    datasize += sizeof(int);
    memcpy(buf+datasize, value, valuebytes);
    datasize += valuebytes;
  }
  blocks[blockid].datasize = datasize;
  return 0;
}


void KeyValue::print(){
  char *key, *value;
  int keybytes, valuebytes;

  printf("Print KV:\n");

  for(int i = 0; i < nblock; i++){
    int offset = 0;

    acquireblock(i);

    offset = getNextKV(i, offset, &key, keybytes, &value, valuebytes);

    while(offset != -1){
      printf("%s:%s\n",key, value);

      offset = getNextKV(i, offset, &key, keybytes, &value, valuebytes);
    }

    releaseblock(i);

  }
}
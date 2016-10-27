#include <stdio.h>
#include <stdlib.h>
#include "log.h"
#include "config.h"
#include "alltoall.h"
#include "const.h"
#include "memory.h"
#include "keyvalue.h"

using namespace MIMIR_NS;

#include "stat.h"

#define SAVE_ALL_DATA(ii) \
{\
  int k=0;\
  int64_t spacesize=0;\
  char *src_buf=NULL, *dst_buf=NULL;\
  src_buf=recv_buf[ii];\
  if(blockid!=-1){\
    dst_buf=data->getblockbuffer(blockid);\
    int64_t datasize=data->getdatasize(blockid);\
    dst_buf += datasize;\
    spacesize=data->blocksize-datasize;\
  }else{\
    blockid=data->add_block();\
    data->acquire_block(blockid);\
    dst_buf=data->getblockbuffer(blockid);\
    spacesize=data->blocksize;\
  }\
  while(k<size){\
    int copysize=0;\
    int padding=0;\
    while(k<size && spacesize>=recv_count[ii][k]){\
      copysize+=recv_count[ii][k];\
      spacesize-=recv_count[ii][k];\
      padding=recv_count[ii][k]&((0x1<<type_log_bytes)-0x1);\
      k++;\
      if(padding !=0 ){\
        break;\
      }\
    }\
    LOG_PRINT(DBG_COMM, "%d[%d] DATA: data copy \
(blockid=%d, dst_buf=%p, src_buf=%p, copysize=%d)\n", me, nprocs, blockid, dst_buf, src_buf, copysize);\
    memcpy(dst_buf, src_buf, copysize);\
    int64_t datasize=data->getdatasize(blockid);\
    data->setblockdatasize(blockid,datasize+copysize);\
    dst_buf+=copysize;\
    src_buf+=copysize;\
    if(padding!=0){\
      src_buf+=padding;\
    }else if(k<size){\
      data->release_block(blockid);\
      blockid=data->add_block();\
      data->acquire_block(blockid);\
      dst_buf=data->getblockbuffer(blockid);\
      spacesize=data->blocksize;\
    }\
  }\
}

Alltoall::Alltoall(MPI_Comm _comm):Communicator(_comm, 0){
    switchflag=0;
    ibuf=0;
    buf=NULL;
    off=NULL;

    recv_count=NULL;
    recv_buf=NULL;
    recvcounts=NULL;
    type_log_bytes=0;

    reqs=NULL;

    LOG_PRINT(DBG_COMM, "%d[%d] Comm: alltoall create.\n", rank, size);
}

Alltoall::~Alltoall(){
    for(int i = 0; i < nbuf; i++){
      if(recv_buf != NULL && recv_buf[i] != NULL) mem_aligned_free(recv_buf[i]);
      if(recv_count !=NULL && recv_count[i] != NULL) mem_aligned_free(recv_count[i]);
    }

    if(recv_count != NULL) delete [] recv_count;
    if(recv_buf != NULL) delete [] recv_buf;
    if(recvcounts != NULL) delete [] recvcounts;
    if(reqs != NULL) delete [] reqs;

    LOG_PRINT(DBG_COMM, "%d[%d] Comm: alltoall destroy.\n", rank, size);
}

int Alltoall::setup(int64_t _sbufsize, DataObject *_data){
    Communicator::setup(_sbufsize, _data);

    int64_t total_send_buf_size=(int64_t)send_buf_size*size;

    type_log_bytes=0;
    int type_bytes=0x1;
    while((int64_t)type_bytes*(int64_t)MAX_COMM_SIZE<total_send_buf_size){
        type_bytes<<=1;
        type_log_bytes++;
    }

    recv_buf = new char*[nbuf];
    recv_count  = new int*[nbuf];

    for(int i = 0; i < nbuf; i++){
      recv_buf[i] = (char*)mem_aligned_malloc(MEMPAGE_SIZE, total_send_buf_size);
      recv_count[i] = (int*)mem_aligned_malloc(MEMPAGE_SIZE, size*sizeof(int));
    }

    reqs = new MPI_Request[nbuf];
    for(int i=0; i<nbuf; i++)
      reqs[i]=MPI_REQUEST_NULL;

    recvcounts = new int64_t[nbuf];
    for(int i = 0; i < nbuf; i++) recvcounts[i] = 0;

    switchflag=0;
    ibuf = 0;
    buf = send_buffers[0];
    off = send_offsets[0];

    for(int i=0; i<size; i++) off[i] = 0;

    LOG_PRINT(DBG_COMM, "%d[%d] Comm: alltoall setup. (\
comm buffer size=%ld, type_log_bytes=%d)\n", \
      rank, size, send_buf_size, type_log_bytes);

    return 0;
}

int Alltoall::sendKV(int target, char *key, int keysize, char *val, int valsize){
    if(target < 0 || target >= size){
      LOG_ERROR("Error: target process (%d) isn't correct!\n", target);
    }

    int kvsize = 0;
    KeyValue *kv = (KeyValue*)data;
    GET_KV_SIZE(kv->kvtype, keysize, valsize, kvsize);

    /* copy kv into local buffer */
    while(1){
        // communication
        if(switchflag != 0){
            exchange_kv();
            switchflag = 0;
        }

        int goff=off[target];
        if((int64_t)goff+(int64_t)kvsize<=send_buf_size){
            int64_t global_buf_off=target*(int64_t)send_buf_size+goff;
            char *gbuf=buf+global_buf_off;
            KeyValue *kv = (KeyValue*)data;
            PUT_KV_VARS(kv->kvtype,gbuf,key,keysize,val,valsize,kvsize);
            off[target]+=kvsize;
            break;
        }else{
            switchflag=1;
        }
    }

    return 0;
}

// wait all procsses done
void Alltoall::wait(){
    LOG_PRINT(DBG_COMM, "%d[%d] Comm: start wait.\n", rank, size);

    medone = 1;

    // do exchange kv until all processes done
    do{
        exchange_kv();
    }while(pdone < size);

#ifdef MIMIR_COMM_NONBLOCKING
   // wait all pending communication
   for(int i = 0; i < nbuf; i++){
       if(reqs[i] != MPI_REQUEST_NULL){

           TRACKER_RECORD_EVENT(0, EVENT_MAP_COMPUTING);

           MPI_Status mpi_st;
           MPI_Wait(&reqs[i], &mpi_st);
           reqs[i] = MPI_REQUEST_NULL;

           TRACKER_RECORD_EVENT(0, EVENT_COMM_WAIT);

           uint64_t recvcount = recvcounts[i];

           PROFILER_RECORD_COUNT(0, COUNTER_COMM_RECV_SIZE, recvcount);

           LOG_PRINT(DBG_COMM, "%d[%d] Comm: receive data. (count=%ld)\n", rank, size, recvcount);

           if(recvcount > 0) {
               SAVE_ALL_DATA(i);
           }
       }
   }
#endif

   LOG_PRINT(DBG_COMM, "%d[%d] Comm: finish wait.\n", rank, size);
}

void Alltoall::exchange_kv(){
    int i;
    int64_t sendcount=0;
    for(i=0; i<size; i++) sendcount += (int64_t)off[i];

    // exchange send count
    TRACKER_RECORD_EVENT(0, EVENT_MAP_COMPUTING);
    PROFILER_RECORD_COUNT(0, COUNTER_COMM_SEND_SIZE, sendcount);

    // exchange send and recv counts
    MPI_Alltoall(off, 1, MPI_INT, recv_count[ibuf], 1, MPI_INT, comm);

    TRACKER_RECORD_EVENT(0, EVENT_COMM_ALLTOALL);

    recvcounts[ibuf] = (int64_t)recv_count[ibuf][0];
    for(i = 1; i < size; i++){
        recvcounts[ibuf] += (int64_t)recv_count[ibuf][i];
    }

    int *a2a_s_count=new int[size];
    int *a2a_s_displs=new int[size];
    int *a2a_r_count=new int[size];
    int *a2a_r_displs= new int[size];

    for(i=0; i<size; i++){
        a2a_s_count[i]=(off[i]+(0x1<<type_log_bytes)-1)>>type_log_bytes;
        a2a_r_count[i]=(recv_count[ibuf][i]+(0x1<<type_log_bytes)-1)>>type_log_bytes;
        a2a_s_displs[i] = (i*(int)send_buf_size)>>type_log_bytes;
    }
    a2a_r_displs[0] = 0;
    for(i=1; i<size; i++)
        a2a_r_displs[i]=a2a_r_displs[i-1]+a2a_r_count[i-1];

    int64_t send_padding_bytes=a2a_s_count[0];
    int64_t recv_padding_bytes=a2a_r_count[0];
    for(i=1;i<size;i++){
      send_padding_bytes+=a2a_s_count[i];
      recv_padding_bytes+=a2a_r_count[i];
    }
    send_padding_bytes<<=type_log_bytes;
    recv_padding_bytes<<=type_log_bytes;
    send_padding_bytes-=sendcount;
    recv_padding_bytes-=recvcounts[ibuf];

    PROFILER_RECORD_COUNT(0, COUNTER_COMM_SEND_PAD, send_padding_bytes);
    PROFILER_RECORD_COUNT(0, COUNTER_COMM_RECV_PAD, recv_padding_bytes);

    MPI_Datatype comm_type;
    MPI_Type_contiguous((0x1<<type_log_bytes), MPI_BYTE, &comm_type);
    MPI_Type_commit(&comm_type);

#ifndef MIMIR_COMM_NONBLOCKING
    char *recvbuf=recv_buf[ibuf];

    MPI_Alltoallv(send_buffers[ibuf], \
      a2a_s_count, a2a_s_displs, comm_type, \
      recvbuf, a2a_r_count, a2a_r_displs, comm_type, comm);
    
    TRACKER_RECORD_EVENT(0, EVENT_COMM_ALLTOALLV);

    int64_t recvcount = recvcounts[ibuf];
    PROFILER_RECORD_COUNT(0, COUNTER_COMM_RECV_SIZE, recvcount);

    LOG_PRINT(DBG_COMM, "%d[%d] Comm: receive data. (count=%ld)\n", rank, size, recvcount);

    if(recvcount > 0) {
        SAVE_ALL_DATA(ibuf);
    }

    TRACKER_RECORD_EVENT(0, EVENT_MAP_COMPUTING);

#else
    char *recvbuf=recv_buf[ibuf];
    MPI_Ialltoallv(send_buffers[ibuf], a2a_s_count, a2a_s_displs, comm_type, \
        recvbuf, a2a_r_count, a2a_r_displs, comm_type, comm,  &reqs[ibuf]);

    TRACKER_RECORD_EVENT(0, EVENT_COMM_IALLTOALLV);

    // wait data
    ibuf = (ibuf+1)%nbuf;
    if(reqs[ibuf] != MPI_REQUEST_NULL) {

        MPI_Status mpi_st;
        MPI_Wait(&reqs[ibuf], &mpi_st);
        reqs[ibuf] = MPI_REQUEST_NULL;

        int64_t recvcount = recvcounts[ibuf];

        TRACKER_RECORD_EVENT(0, EVENT_COMM_WAIT);
        PROFILER_RECORD_COUNT(0, COUNTER_COMM_RECV_SIZE, recvcount);

        LOG_PRINT(DBG_COMM, "%d[%d] Comm: receive data. (count=%ld)\n", rank, size, recvcount);

        if(recvcount > 0) {
            SAVE_ALL_DATA(ibuf);
        }

        TRACKER_RECORD_EVENT(0, EVENT_MAP_COMPUTING);
    }

    // switch buffer
    buf = send_buffers[ibuf];
    off = send_offsets[ibuf];
#endif

    for(int i = 0; i < size; i++) off[i] = 0;
    MPI_Type_free(&comm_type);

    delete [] a2a_s_count;
    delete [] a2a_s_displs;
    delete [] a2a_r_count;
    delete [] a2a_r_displs;


    MPI_Allreduce(&medone, &pdone, 1, MPI_INT, MPI_SUM, comm);

    TRACKER_RECORD_EVENT(0, EVENT_COMM_ALLREDUCE);

    LOG_PRINT(DBG_COMM, "%d[%d] Comm: exchange KV. (send count=%ld, done count=%d)\n", rank, size, sendcount, pdone);
}

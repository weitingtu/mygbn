/*
 * mygbn.h
 */

#ifndef __mygbn_h__
#define __mygbn_h__

#include <pthread.h>
#include <list>
#include <queue>

#define MAX_PAYLOAD_SIZE 512

typedef enum Cmd_e
{
    DATA_PACKET = 0xA0,
    ACK_PACKET  = 0xA1,
    END_PACKET  = 0xA2
} Cmd;

struct MYGBN_Packet_s
{
    unsigned char protocol[3];                  /* protocol string (3 bytes) "gbn" */
    unsigned char type;                         /* type (1 byte) */
    unsigned int seqNum;                        /* sequence number (4 bytes) */
    unsigned int length;                        /* length(header+payload) (4 bytes) */
    unsigned char payload[MAX_PAYLOAD_SIZE];    /* payload data */
};

typedef struct MYGBN_Packet_s MYGBN_Packet;

struct MyGBN_Data
{
    MyGBN_Data( unsigned char* d, int l, unsigned int s ) : data( d ), len( l ), seq_num( s ) {}
    unsigned char* data;
    int len;
    unsigned int seq_num;
};

struct mygbn_sender
{
    int sd; // GBN sender socket
    // ... other member variables
    struct sockaddr_in address;
    unsigned base;
    unsigned nextseqnum;
    int N;
    int timeout;
    std::list<MyGBN_Data>  window;
    std::queue<MyGBN_Data> cache;
    bool send_end_packet;
    unsigned resend_end_packet_count;
    pthread_t ack_thread;
    pthread_t timeout_thread;
};

void mygbn_init_sender( struct mygbn_sender* sender, char* ip, int port, int N, int timeout );
int mygbn_send( struct mygbn_sender* sender, unsigned char* buf, int len );
void mygbn_close_sender( mygbn_sender* sender );

struct mygbn_receiver
{
    int sd; // GBN receiver socket
    // ... other member variables
    unsigned expected;
    unsigned acked;
};

void mygbn_init_receiver( mygbn_receiver* receiver, int port );
int mygbn_recv( struct mygbn_receiver* receiver, unsigned char* buf, int len );
void mygbn_close_receiver( struct mygbn_receiver* receiver );

#endif

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdarg.h>
#include <limits>
#include "mygbn.h"

#define IPADDR "127.0.0.1"

pthread_mutex_t sender_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ack_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ack_signal = PTHREAD_COND_INITIALIZER;

pthread_mutex_t time_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t time_signal = PTHREAD_COND_INITIALIZER;

void _io_debug( const char* format, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
void _io_debug( const char* format, ... )
{
#ifdef DEBUG
    printf( "<debug> " );
    va_list args;
    va_start( args, format );
    vprintf( format, args );
    va_end( args );
#endif
}

#define io_debug( format, ... )   \
    { \
        _io_debug( format, ##__VA_ARGS__); \
    }

void _print_packet( MYGBN_Packet* packet )
{
#ifdef DEBUG
    printf( "protocol: %c%c%c\n", packet->protocol[0], packet->protocol[1], packet->protocol[2] );
    printf( "type    : %x\n",     packet->type );
    printf( "seq num : %u\n",     packet->seqNum );
    printf( "length  : %u\n",     packet->length );
    printf( "payload :" );
    for ( int i = 0; i < packet->length - sizeof( MYGBN_Packet ); ++i )
    {
        printf( " %2x", packet->payload[i] );
    }
    printf( "\n" );
#endif
}

char* _create_data_packet( MYGBN_Packet* packet, int seq_num, unsigned char* data, int len )
{
    memcpy( packet->protocol, "gdn", 3 );
    packet->type = DATA_PACKET;
    packet->seqNum = seq_num;
    packet->length = sizeof( MYGBN_Packet ) + len;
    memcpy( packet->payload, data, len );

    char* buff = ( char* )malloc( sizeof( char ) * sizeof( MYGBN_Packet ) );
    memcpy( buff, packet, sizeof( MYGBN_Packet ) );

    return buff;
}

char* _create_ack_packet( MYGBN_Packet* packet, int seq_num )
{
    memcpy( packet->protocol, "gdn", 3 );
    packet->type = ACK_PACKET;
    packet->seqNum = seq_num;
    packet->length = sizeof( MYGBN_Packet );

    char* buff = ( char* )malloc( sizeof( char ) * packet->length );
    memcpy( buff, packet, packet->length );

    return buff;
}

char* _create_end_packet( MYGBN_Packet* packet, int seq_num )
{
    memcpy( packet->protocol, "gdn", 3 );
    packet->type = END_PACKET;
    packet->seqNum = seq_num;
    packet->length = sizeof( MYGBN_Packet );

    char* buff = ( char* )malloc( sizeof( char ) * packet->length );
    memcpy( buff, packet, packet->length );

    return buff;
}

int _recv_packet( int sd, MYGBN_Packet* packet, struct sockaddr* from, socklen_t* fromlen )
{
    char buf[sizeof( MYGBN_Packet )];
    if ( -1 == recvfrom( sd, buf, sizeof( MYGBN_Packet ), 0, from, fromlen ) )
    {
        fprintf( stderr, "error receiving, exit! %s\n", strerror( errno ) );
        return -1;
    }

    memcpy( packet, buf, sizeof( MYGBN_Packet ) );

    return 0;
}

int _send_data_packet( struct mygbn_sender* sender, unsigned char* buf, int data_len, unsigned seq_num )
{
    if ( seq_num - sender->base >= sender->N )
    {
        printf( "next seq num %u base %u N %u\n", seq_num, sender->base, sender->N );
        assert( seq_num - sender->base < sender->N );
    }

    MYGBN_Packet packet;
    char* data = _create_data_packet( &packet, seq_num, buf, data_len );
    _print_packet( &packet );
    int res = sendto( sender->sd, data, sizeof( MYGBN_Packet ), 0, ( struct sockaddr* )&sender->address, sizeof( sender->address ) );
    if ( -1 == res )
    {
        fprintf( stderr, "error sending data packet, exit! %s\n", strerror( errno ) );
    }
    free( data );
}

int _send_end_packet( struct mygbn_sender* sender, unsigned seq_num )
{
    if ( seq_num - sender->base >= sender->N )
    {
        printf( "next seq num %u base %u N %u\n", seq_num, sender->base, sender->N );
        assert( seq_num - sender->base < sender->N );
    }

    MYGBN_Packet packet;
    char* data = _create_end_packet( &packet, seq_num );
    _print_packet( &packet );
    int res = sendto( sender->sd, data, sizeof( MYGBN_Packet ), 0, ( struct sockaddr* )&sender->address, sizeof( sender->address ) );
    if ( -1 == res )
    {
        fprintf( stderr, "error sending data packet, exit! %s\n", strerror( errno ) );
    }
    free( data );
}

void _print_addr( struct sockaddr* from )
{
    struct sockaddr_in* addr_in = ( struct sockaddr_in* )from;
    char* s = inet_ntoa( addr_in->sin_addr );
    printf( "IP address: %s\n", s );
}

int _send_ack_packet( int sd, unsigned int seq_num, struct sockaddr* from, socklen_t fromlen )
{
    MYGBN_Packet packet;
    char* data = _create_ack_packet( &packet, seq_num );
    _print_packet( &packet );
    int res = sendto( sd, data, sizeof( MYGBN_Packet ), 0, from, fromlen );
    if ( -1 == res )
    {
        fprintf( stderr, "error sending ack packet, exit! %s\n", strerror( errno ) );
    }
    free( data );

    return res;
}

void* pthread_timeout_prog( void* sSender )
{
    mygbn_sender* sender = ( mygbn_sender* )sSender;

    struct timespec ts;
    struct timeval tp;

    while ( 1 )
    {
        io_debug( "timing::set timeout deadline\n" );
        pthread_mutex_lock( &time_lock );

        gettimeofday( &tp, NULL );

        ts.tv_sec = tp.tv_sec;
        ts.tv_nsec = tp.tv_usec * 1000;
        ts.tv_sec += sender->timeout;     // set wait deadline

        int rc;
        io_debug( "timing::wait for the signal or timeout!\n" );
        rc = pthread_cond_timedwait( &time_signal, &time_lock, &ts );

        if ( rc == ETIMEDOUT )
        {
            io_debug( "timing::timing thread timeout!\n" );
        }
        else
        {
            io_debug( "timing::timing thread is waked up by signal!\n" );
        }
        pthread_mutex_unlock( &time_lock );


        pthread_mutex_lock( &sender_lock );

        io_debug( "re-send all the packets in current window (size: %u)\n", ( unsigned )sender->window.size() );
        // re-send all the packets in current window
        std::list<MyGBN_Data>::iterator ite = sender->window.begin();
        std::list<MyGBN_Data>::iterator ite_end = sender->window.end();
        for ( ; ite != ite_end; ++ite )
        {
            if ( -1 == _send_data_packet( sender, ( *ite ).data, ( *ite ).len, ( *ite ).seq_num ) )
            {
                exit( 0 );
            }
        }

        bool finish = false;
        if ( sender->send_end_packet )
        {
            if ( ( sender->resend_end_packet_count++ ) < 3 )
            {
                io_debug( "re-send end packet %u\n", sender->resend_end_packet_count );
            }
            else
            {
                io_debug( "after 3 times retransmission, terminate and report an error" );
                finish = true;
            }
        }

        pthread_mutex_unlock( &sender_lock );

        if ( finish )
        {
            pthread_mutex_lock( &ack_lock );
            pthread_cond_signal( &ack_signal );
            pthread_mutex_unlock( &ack_lock );
        }

    }
    return NULL;
}

void* pthread_ack_prog( void* sSender )
{
    mygbn_sender* sender = ( mygbn_sender* )sSender;

    unsigned end_seq_num = std::numeric_limits<unsigned>::max();

    while ( 1 )
    {
        MYGBN_Packet packet;
        io_debug( "receive packet\n" );
        if ( -1 == _recv_packet( sender->sd, &packet, NULL, NULL ) )
        {
            exit( 0 );
        }
        _print_packet( &packet );
        if ( ACK_PACKET != packet.type )
        {
            printf( "ignore packet type %x\n", packet.type );
            continue;
        }

        pthread_mutex_lock( &sender_lock );
        // get base ack
        if ( packet.seqNum == sender->base )
        {
            io_debug( "get base %u ack seq num %u\n", sender->base, packet.seqNum );
            ++sender->base;
            free( sender->window.front().data );
            sender->window.pop_front();

            // move cache data to window
            while ( ( sender->nextseqnum - sender->base < sender->N ) && !sender->cache.empty() )
            {
                MyGBN_Data d = sender->cache.front();
                sender->cache.pop();
                d.seq_num = sender->nextseqnum++;
                sender->window.push_back( d );
                io_debug( "send cache packet\n" );
                if ( -1 == _send_data_packet( sender, d.data, d.len, d.seq_num ) )
                {
                    exit( 0 );
                }
            }

            if ( end_seq_num == std::numeric_limits<unsigned>::max() && sender->base == sender->nextseqnum )
            {
                io_debug( "send end packet base %u next seq num %u\n", sender->base, sender->nextseqnum );
                end_seq_num = sender->nextseqnum;
                // send end
                MyGBN_Data d( 0, 0, sender->nextseqnum++ );
                sender->window.push_back( d );
                if ( -1 == _send_end_packet( sender, d.seq_num ) )
                {
                    exit( 0 );
                }
                sender->send_end_packet = true;
            }
        }
        else
        {
            io_debug( "seq num %u base %u mismatch. drop it, it is not the expected ACK\n", packet.seqNum, sender->base );
        }

        pthread_mutex_unlock( &sender_lock );

        if ( packet.seqNum == end_seq_num )
        {
            break;
        }
    }
    pthread_mutex_lock( &ack_lock );
    pthread_cond_signal( &ack_signal );
    pthread_mutex_unlock( &ack_lock );

    return NULL;
}

void mygbn_init_sender( struct mygbn_sender* sender, char* ip, int port, int N, int timeout )
{
    // create an IPv4/UDP socket
    sender->sd = socket( AF_INET, SOCK_DGRAM, 0 );

    // initialize the address of server
    sender->address;
    memset( &sender->address, 0, sizeof( struct sockaddr_in ) );
    sender->address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &( sender->address.sin_addr ) );
    sender->address.sin_port = htons( port );
    bind( sender->sd, ( struct sockaddr* )&sender->address, sizeof( struct sockaddr ) );

    sender->base = 0;
    sender->nextseqnum = 0;
    sender->N = N;
    sender->timeout = timeout;
    sender->window.clear();
    while ( !sender->cache.empty() )
    {
        sender->cache.pop();
    }
    sender->send_end_packet = false;
    sender->resend_end_packet_count = 0;
    pthread_create( &sender->ack_thread, NULL, pthread_ack_prog, sender );
    pthread_create( &sender->timeout_thread, NULL, pthread_timeout_prog, sender );
}

int mygbn_send( struct mygbn_sender* sender, unsigned char* buf, int len )
{
    int size = len / MAX_PAYLOAD_SIZE + 1;
    int i = 0;
    pthread_mutex_lock( &sender_lock );
    for ( ; i < size; ++i )
    {
        int data_len = i == size - 1 ? len % MAX_PAYLOAD_SIZE :  MAX_PAYLOAD_SIZE;

        unsigned char* cache = ( unsigned char* ) malloc( sizeof( unsigned char ) * data_len );
        memcpy( cache, buf + i * MAX_PAYLOAD_SIZE, data_len );

        // Window full, put into queue
        if ( sender->nextseqnum - sender->base >= sender->N )
        {
            io_debug( "window full, put into queue nextseqnum %u base %u N %u\n", sender->nextseqnum, sender->base, sender->N );
            sender->cache.push( MyGBN_Data( cache, data_len, 0 ) );
            continue;
        }

        io_debug( "send packet\n" );
        MyGBN_Data d( cache, data_len, sender->nextseqnum++ );
        sender->window.push_back( d );
        if ( -1 == _send_data_packet( sender, d.data, d.len, d.seq_num ) )
        {
            pthread_mutex_unlock( &sender_lock );
            return -1;
        }
    }
    pthread_mutex_unlock( &sender_lock );

    return 0;
}

void mygbn_close_sender( mygbn_sender* sender )
{
    if ( sender->resend_end_packet_count  > 3 )
    {
        printf( "error: failed to receive end packget ack\n" );
    }

    pthread_mutex_lock( &ack_lock );
    pthread_cond_wait( &ack_signal, &ack_lock );
    pthread_mutex_unlock( &ack_lock );
}

void mygbn_init_receiver( mygbn_receiver* receiver, int port )
{
    // create a IPv4/UDP socket
    receiver->sd = socket( AF_INET, SOCK_DGRAM, 0 );

    // initialize the address
    struct sockaddr_in address;
    memset( &address, 0, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_port = htons( port );
    address.sin_addr.s_addr = htonl( INADDR_ANY );

    // Bind the socket to the address
    bind( receiver->sd, ( struct sockaddr* )&address, sizeof( struct sockaddr ) );

    receiver->expected = 0;
    receiver->acked = 0;
}

int mygbn_recv( struct mygbn_receiver* receiver, unsigned char* buf, int len )
{
    int size = len / MAX_PAYLOAD_SIZE;
    if ( len % MAX_PAYLOAD_SIZE != 0 )
    {
        ++size;
    }
    int recv_len = 0;
    int i = 0;
    while ( i < size )
    {
        MYGBN_Packet packet;
        struct sockaddr from;
        memset( &from, 0, sizeof( sockaddr ) );
        socklen_t fromlen = sizeof( sockaddr );
        if ( -1 == _recv_packet( receiver->sd, &packet, &from, &fromlen ) )
        {
            return -1;
        }
        io_debug( "receive packet\n" );
        _print_packet( &packet );
        if ( DATA_PACKET == packet.type )
        {
            if ( receiver->expected == packet.seqNum )
            {
                io_debug( "accept packet expected %u acked %u seq num %u\n", receiver->expected, receiver->acked, packet.seqNum );
                memcpy( buf + i * MAX_PAYLOAD_SIZE, packet.payload, packet.length - sizeof( MYGBN_Packet ) );
                recv_len += packet.length - sizeof( MYGBN_Packet );

                ++receiver->expected;
                receiver->acked = packet.seqNum;
                ++i;
            }

            io_debug( "send packet\n" );
            if ( -1 == _send_ack_packet( receiver->sd, receiver->acked, &from, fromlen ) )
            {
                return -1;
            }
        }
        else if ( END_PACKET == packet.type )
        {
            if ( -1 == _send_ack_packet( receiver->sd, packet.seqNum, &from, fromlen ) )
            {
                return -1;
            }

            receiver->expected = 0;
            receiver->acked = 0;

            if ( 0 != i )
            {
                break;
            }
        }
        else
        {
            printf( "ignore packet type %x\n", packet.type );
        }
    }

    return recv_len;
}

void mygbn_close_receiver( struct mygbn_receiver* receiver )
{
}

#ifndef RTP_H
#define RTP_H

#include <stdint.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTP_START 0
#define RTP_END   1
#define RTP_DATA  2
#define RTP_ACK   3

#define PAYLOAD_SIZE 1461

typedef struct __attribute__ ((__packed__)) RTP_header {
    uint8_t type;       // 0: START; 1: END; 2: DATA; 3: ACK
    uint16_t length;    // Length of data; 0 for ACK, START and END packets
    uint32_t seq_num;
    uint32_t checksum;  // 32-bit CRC
} rtp_header_t;


typedef struct __attribute__ ((__packed__)) RTP_packet {
    rtp_header_t rtp;
    char payload[];
} rtp_packet_t;

typedef struct RTP_sender{
    int seq_base; // First pkt waiting for ACK
    int seq_next; // Next pkt to be sent
    uint32_t window_size;
    char** send_buf; // Pkt cache
    size_t* send_length; // Pkt length in cache
} rtp_sender_t;

//TODO
typedef struct RTP_receiver{
    int seq_next; // Next expected pkt seq_num
    uint32_t window_size;
    char** recv_buf; // Pkt cache
    size_t* recv_length; //Pkt length in cache
    size_t* recv_ack; // 1 for acked pkt
} rtp_receiver_t;

/**
 * @brief Build RTP connection
 * @author Sheng Lin
 * @param sockfd Sender's socket fd
 * @param servaddr Receiver's address
 * @param addrlen A pointer to address length
 * @param sender_control Sender control unit to support sliding window
 * @return -1 means failure, 0 means success
 * @cite https://www.man7.org/linux/man-pages/man3/FD_SET.3.html
*/
int rtp_connect(int sockfd, struct sockaddr_in* servaddr, socklen_t* addrlen, rtp_sender_t* sender_control);

/**
 * @brief Create a RTP packet
 * @note Remember to free returned packet after use
 * @author Sheng Lin
 * @param type RTP segment type
 * @param length RTP message length (equals 0 for START, END, ACK type)
 * @param seq_num RTP sequence number for sequential reception
 * @param message RTP message (NULL for START, END, ACK type)
 * @return A pointer to one RTP packet
*/
rtp_packet_t* rtp_packet(uint8_t type, uint16_t length, uint32_t seq_num, char* message);

/**
 * @brief Receive a RTP packet and verify its checksum
 * @note Remember to free returned packet after use
 * @author Sheng Lin
 * @param sockfd Receiver's socket fd
 * @param from Sender's address
 * @param fromlen A pointer to sender address's length
 * @return A pointer to received RTP packet. NULL if verification failed.
*/
rtp_packet_t* rtp_recvfrom(int sockfd, struct sockaddr* from, socklen_t* fromlen);

/**
 * @brief Send END packet and wait for ACK with correct seq_num.
 * Return when time out or receive ACK
 * @note Remember to close connection after return
 * @author Sheng Lin
 * @param sockfd Sender's socket fd
 * @param to Receiver's address
 * @param tolen A pointer to receiver's address length
 * @param sender_control Sender control unit to support sliding window
*/
void rtp_sendEND(int sockfd, struct sockaddr* to, socklen_t* tolen, rtp_sender_t* sender_control);

/**
 * @brief Free sender_control's resources
 * @author Sheng Lin
 * @param sender_control Sender control unit to be freed
*/
void rtp_freeSenderControl(rtp_sender_t* sender_control);

/**
 * @brief Free receiver_control's resouces
 * @author Sheng Lin
 * @param receiver_control Receiver contol unit to be freed
*/
void rtp_freeReceiverControl(rtp_receiver_t* receiver_control);

#ifdef __cplusplus
}
#endif

#endif //RTP_H

#include <sys/select.h>
#include <time.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "util.h"
#include "rtp.h"

rtp_packet_t* rtp_packet(uint8_t type, uint16_t length, uint32_t seq_num, char* message){
    rtp_packet_t* pkt = malloc(sizeof(rtp_header_t) + length);
    pkt->rtp.type = type;
    pkt->rtp.length = length;
    pkt->rtp.seq_num = seq_num;
    pkt->rtp.checksum = 0;
    if(message != NULL){
        memcpy(pkt->payload, message, length);
    }
    pkt->rtp.checksum = compute_checksum((void*)pkt, sizeof(rtp_header_t) + length);
    return pkt;
}

int rtp_connect(int sockfd, struct sockaddr_in* servaddr, socklen_t* addrlen){
    // seq_num is a random value for connection.
    srand(time(NULL));    
    uint32_t seq = rand();

    // Create a START packet.
    rtp_packet_t* start_pkt = rtp_packet(RTP_START, 0, seq, NULL);

    // Send packet for connection.
    //printf("Sender: Sending START.\n");
    ssize_t send_length = sendto(sockfd, (void*)start_pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)servaddr, *addrlen);
    if(send_length != sizeof(rtp_header_t)){
        free(start_pkt);
        perror("Send failure");
        return -1;
    }

    free(start_pkt);

    // Check whether ACK time out.
    struct timeval timeout = {5, 0}; // 20s
    fd_set wait_fd;
    FD_ZERO(&wait_fd);
    FD_SET(sockfd, &wait_fd);
    int res = select(sockfd + 1, &wait_fd, NULL, NULL, &timeout);
    if(res == -1)
        return -1;
    else if(res == 0){
        // Handle timeout.
        perror("Timeout");
        rtp_sendEND(sockfd, (struct sockaddr*)servaddr, addrlen, NULL);
        return -1;
    }
    else if(FD_ISSET(sockfd, &wait_fd)){
        //printf("Sender: Received ACK.\n");
        // Receive ACK and check its checksum.
        rtp_packet_t* recv_ack = rtp_recvfrom(sockfd, (struct sockaddr*)servaddr, addrlen);
        if(!recv_ack){
            rtp_sendEND(sockfd, (struct sockaddr*)servaddr, addrlen, NULL);
            return -1;
        }
        else if(recv_ack->rtp.type == RTP_ACK){
            free(recv_ack);
            return 0;
        }
    }
    return 0;
}

rtp_packet_t* rtp_recvfrom(int sockfd, struct sockaddr* from, socklen_t* fromlen){
    rtp_packet_t* recv_pkt = malloc(sizeof(rtp_header_t) + PAYLOAD_SIZE);
    ssize_t recv_length = recvfrom(sockfd, (void*)recv_pkt, sizeof(rtp_header_t) + PAYLOAD_SIZE, 0, from, fromlen);
    
    if(recv_length == -1){
        free(recv_pkt);
        perror("Receive failure");
        return NULL;
    }
    uint32_t checksum = recv_pkt->rtp.checksum;
    recv_pkt->rtp.checksum = 0;
    if(checksum != compute_checksum(recv_pkt, recv_length)){
        free(recv_pkt);
        return NULL;
    }
    else
        return recv_pkt;
}

void rtp_sendEND(int sockfd, struct sockaddr* to, socklen_t* tolen, rtp_sender_t* sender_control){
    uint32_t seq_next = 0;
    if(sender_control != NULL)
        seq_next = sender_control->seq_next;

    //perror("Sending END...");
    // Send End packet.
    rtp_packet_t* end_pkt = rtp_packet(RTP_END, 0, seq_next, NULL);
    ssize_t end_length = sendto(sockfd, (void*)end_pkt, sizeof(rtp_header_t), 0, to, *tolen);
    if(end_length != sizeof(rtp_header_t)){
        free(end_pkt);
        perror("End send failure");
        return;
    }
    free(end_pkt);

    // Check whether ACK time out.
    // If time out, return and close connection.
    // Else, check seq_num
    
    //perror("Waiting for ACK");
    fd_set wait_fd;
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(sockfd, &wait_fd);
        struct timeval timeout = {0, 100000}; //100ms
        int res = select(sockfd + 1, &wait_fd, NULL, NULL, &timeout);
        //perror("res");
        if(res == -1 || res == 0)
            return;
        else if(FD_ISSET(sockfd, &wait_fd)){
            // Receive ACK and check its checksum.
            //perror("recvfrom");
            rtp_packet_t* recv_ack = rtp_recvfrom(sockfd, to, tolen);
            //TODO: What if END loss or wrong END ACK.
            if(!recv_ack){
                //perror("ACK wrong");
                rtp_sendEND(sockfd, to, tolen, sender_control);
                return;
            }
            else if(recv_ack->rtp.seq_num != seq_next){
                //TODO: Handle response with different seq_num
                //perror("Invalid seq_num");
                free(recv_ack);
                continue;
            }
            else{
                //perror("Success");
                free(recv_ack);
                break;
            }
        }
    }
    return;
}

void rtp_freeSenderControl(rtp_sender_t* sender_control){
    if(!sender_control) return;
    if(sender_control->send_buf){
        for(int i=0; i < sender_control->window_size; i++)
            if(sender_control->send_buf[i])
                free(sender_control->send_buf[i]);
        free(sender_control->send_buf);
        free(sender_control->send_ack);
        free(sender_control->send_length);
    }
    free(sender_control);
}

void rtp_freeReceiverControl(rtp_receiver_t* receiver_control){
    if(!receiver_control) return;
    if(receiver_control->recv_buf){
        for(int i = 0; i < receiver_control->window_size; i++)
            if(receiver_control->recv_buf[i])
                free(receiver_control->recv_buf[i]);
        free(receiver_control->recv_buf);
        free(receiver_control->recv_ack);
        free(receiver_control->recv_length);
    }
    free(receiver_control);
}
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "rtp.h"
#include "receiver_def.h"

int sockfd;
struct sockaddr_in addr;
rtp_receiver_t* receiver_control = NULL;

int initReceiver(uint16_t port, uint32_t window_size){
    // Create a socket.
    //printf("Receiver: Creating socket...\n");
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd == -1){
        perror("Socket failure");
        return -1;
    }

    // Initialize server control.
    //printf("Receiver: Initializing receiver control...\n");
    receiver_control = malloc(sizeof(rtp_receiver_t));
    receiver_control->window_size = window_size;
    receiver_control->seq_next = 0;
    receiver_control->recv_buf = malloc(window_size * sizeof(char*));
    receiver_control->recv_length = malloc(window_size * sizeof(size_t));
    receiver_control->recv_ack = malloc(window_size * sizeof(size_t));
    for(int i=0; i < window_size; ++i){
        receiver_control->recv_length[i] = 0;
        receiver_control->recv_ack[i] = 0;
        receiver_control->recv_buf[i] = malloc(PAYLOAD_SIZE);
        memset(receiver_control->recv_buf[i], 0, PAYLOAD_SIZE);
    } 

    // Initialize sockaddr.
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    //printf("Receiver: Binding...\n");
    if(bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1){
        rtp_freeReceiverControl(receiver_control);
        perror("Bind failure");
        return -1;
    }

    // Wait for START
    //printf("Receiver: Waiting for START pkt...\n");
    fd_set wait_fd;
    FD_ZERO(&wait_fd);
    FD_SET(sockfd, &wait_fd);
    struct timeval timeout = {20, 0}; // 15s
    int res = select(sockfd + 1, &wait_fd, NULL, NULL, &timeout);
    if(res == -1){
        rtp_freeReceiverControl(receiver_control);
        return -1;
    }
    else if(res == 0){
        // Timeout
        rtp_freeReceiverControl(receiver_control);
        close(sockfd);
        return -1;
    }
    else if(FD_ISSET(sockfd, &wait_fd)){
        // Receive START and check its checksum.
        //printf("Receiver: START pkt received.\n");
        socklen_t addrlen = sizeof(addr);
        rtp_packet_t* recv_ack = rtp_recvfrom(sockfd, (struct sockaddr*)&addr, &addrlen);
        if(!recv_ack){
            rtp_freeReceiverControl(receiver_control);
            close(sockfd);
            return -1;
        }
        else if(recv_ack->rtp.type == RTP_START){
            // Send ACK.
            //printf("Receiver: Receive RTP_START.\n");
            rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_ack->rtp.seq_num, NULL);
            ssize_t send_length = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
            if(send_length != sizeof(rtp_header_t)){
                free(pkt);
                free(recv_ack);
                rtp_freeReceiverControl(receiver_control);
                perror("Send failure");
                return -1;
            }
            free(recv_ack);
            free(pkt);
            return 0;
        }
        else if(recv_ack->rtp.type == RTP_END){
            // Send ACK.
            rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, receiver_control->seq_next, NULL);
            ssize_t send_length = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
            if(send_length != sizeof(rtp_header_t))
                perror("Send failure");
            free(pkt);
            free(recv_ack);
            rtp_freeReceiverControl(receiver_control);
            return -1;
        }
        else{
            free(recv_ack);
            rtp_freeReceiverControl(receiver_control);
            perror("Type failure");
            return -1;
        }
    }
    return -1;
}

int recvMessage(char* filename){
    //printf("Receiver: receiving data...\n");
    // Open file whose name is filename.
    FILE* recv_file = fopen(filename, "wb");
    if(!recv_file){
        perror("Open file failure");
        return -1;
    }

    int recv_byte = 0;

    // Wait for data.
    fd_set wait_fd;
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(sockfd, &wait_fd);
        struct timeval timeout = {10, 0};
        //printf("Receiver: Waiting for data...\n");
        int res = select(sockfd + 1, &wait_fd, NULL, NULL, &timeout);
        if(res == -1){
            perror("Select failure");
            fclose(recv_file);
            return -1;
        }
        else if(res == 0){
            perror("Time out");
            fclose(recv_file);
            return recv_byte;
        }
        else if(FD_ISSET(sockfd, &wait_fd)){
            // Receive data pkt.
            //printf("Receiver: receive data...\n");
            socklen_t addrlen = sizeof(addr);
            rtp_packet_t* recv_pkt = rtp_recvfrom(sockfd, (struct sockaddr*)&addr, &addrlen);
            if(!recv_pkt)
                continue;
            else{
                if(recv_pkt->rtp.type == RTP_START || recv_pkt->rtp.type == RTP_END){
                    //printf("Receiver: receive START or END.\n");
                    rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_pkt->rtp.seq_num, NULL);
                    ssize_t send_length = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                    if(send_length != sizeof(rtp_header_t)){
                        free(pkt);
                        free(recv_pkt);
                        // rtp_freeReceiverControl(receiver_control);
                        perror("Send failure");
                        return -1;
                    }
                    free(pkt);

                    if(recv_pkt->rtp.type == RTP_END && recv_pkt->rtp.seq_num == receiver_control->seq_next){
                        printf("Receiver: END and return.\n");
                        printf("Received byte = %d\n", recv_byte);
                        fclose(recv_file);
                        free(recv_pkt);
                        return recv_byte;
                    }
                    else
                        free(recv_pkt);
                }
                else if(recv_pkt->rtp.type == RTP_DATA){
                    //printf("Receiver: receive DATA.\n");
                    //printf("recv_pkt->rtp.seq_num = %d\n",recv_pkt->rtp.seq_num);
                    //printf("receiver_control->seq_next = %d\n", receiver_control->seq_next);
                    // printf("receiver_control->window_size = %d\n", receiver_control->window_size);
                    if(recv_pkt->rtp.seq_num > receiver_control->seq_next){
                        if(recv_pkt->rtp.seq_num > receiver_control->seq_next + receiver_control->window_size){
                            //printf("Receiver: I'll free..\n");
                            free(recv_pkt);
                            continue;
                        }

                        //printf("Receiver: caching data... seq_num = %d\n", recv_pkt->rtp.seq_num);
                        //printf("Receiver: Now seq_next = %d\n", receiver_control->seq_next);
                        // Cache data.
                        if(recv_pkt->rtp.length == 0)
                            continue;
                        int ack_num = recv_pkt->rtp.seq_num - receiver_control->seq_next;
                        receiver_control->recv_ack[ack_num] = 1;
                        receiver_control->recv_length[ack_num] = recv_pkt->rtp.length;
                        memcpy(receiver_control->recv_buf[ack_num], recv_pkt->payload, recv_pkt->rtp.length);

                        //printf("Receiver: Sending ACK...\n");
                        // Send ACK.
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, receiver_control->seq_next, NULL);
                        ssize_t send_length = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            //rtp_freeReceiverControl(receiver_control);
                            perror("Send failure");
                            return -1;
                        }
                        free(recv_pkt);
                        free(pkt);
                        //printf("Receiver: End...\n");
                    }
                    else if(recv_pkt->rtp.seq_num == receiver_control->seq_next){
                        //printf("Receiver: update sliding window...\n");
                        // Cache data.
                        if(recv_pkt->rtp.length == 0)
                            continue;
                        int ack_num = 0;
                        receiver_control->recv_ack[ack_num] = 1;
                        receiver_control->recv_length[ack_num] = recv_pkt->rtp.length;
                        memcpy(receiver_control->recv_buf[ack_num], recv_pkt->payload, recv_pkt->rtp.length);

                        // Update seq_next.
                        int slide_step = 0;
                        for(slide_step = 0; slide_step < receiver_control->window_size; slide_step++){
                            if(receiver_control->recv_ack[slide_step] == 0)
                                break;
                        }
                        receiver_control->seq_next += slide_step;
                        //printf("Receiver: slide_step = %d\n", slide_step);
                        //printf("Receiver: Now new seq_next = %d\n", receiver_control->seq_next);
                        
                        // Write to file.
                        for(int i = 0; i < slide_step; i++){
                            size_t write_byte = fwrite(receiver_control->recv_buf[i], 1, receiver_control->recv_length[i], recv_file);
                            if(write_byte != receiver_control->recv_length[i]){
                                free(recv_pkt);
                                perror("Write failure");
                                return -1;
                            }
                            recv_byte += write_byte;
                        }
                        //printf("Receiver: recv_byte = %d\n", recv_byte);
                        
                        // Update sliding window.
                        for(int i = slide_step; i < receiver_control->window_size; i++){
                            memset(receiver_control->recv_buf[i - slide_step], 0, PAYLOAD_SIZE);
                            memcpy(receiver_control->recv_buf[i - slide_step], receiver_control->recv_buf[i], receiver_control->recv_length[i]);
                            // memset(receiver_control->recv_buf[i], 0, PAYLOAD_SIZE);
                            receiver_control->recv_length[i - slide_step] = receiver_control->recv_length[i];
                            // receiver_control->recv_length[i] = 0;
                            receiver_control->recv_ack[i - slide_step] = receiver_control->recv_ack[i];
                            // receiver_control->recv_ack[i] = 0;
                        }
                        for(int i = 0; i < slide_step; i++){
                            int j = receiver_control->window_size - 1 - i;
                            memset(receiver_control->recv_buf[j], 0, PAYLOAD_SIZE);
                            receiver_control->recv_length[j] = 0;
                            receiver_control->recv_ack[j] = 0;
                        }

                        // Send ACK.
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, receiver_control->seq_next, NULL);
                        ssize_t send_length = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            //rtp_freeReceiverControl(receiver_control);
                            perror("Send failure");
                            return -1;
                        }
                        free(recv_pkt);
                        free(pkt);
                    }
                    else{
                        // Send ACK.
                        //printf("Receiver: Sending ACK... seq_next = %d\n", receiver_control->seq_next);
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, receiver_control->seq_next, NULL);
                        ssize_t send_length = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            //rtp_freeReceiverControl(receiver_control);
                            perror("Send failure");
                            return -1;
                        }
                        free(recv_pkt);
                        free(pkt);
                    }
                }
            }
        }
    }
    return -1;
}

void terminateReceiver(){
    //printf("Receiver: terminate...\n");
    rtp_freeReceiverControl(receiver_control);
    close(sockfd);
    return;
}

int recvMessageOpt(char* filename){
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "rtp.h"
#include "receiver_def.h"

int recvfd;
struct sockaddr_in addr;
rtp_receiver_t* receiver_control = NULL;

int initReceiver(uint16_t port, uint32_t window_size){
    // Create a socket.
    recvfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(recvfd == -1){
        perror("[Receiver] Socket failure");
        return -1;
    }

    // Initialize server control.
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

    if(bind(recvfd, (struct sockaddr *)&addr, sizeof(addr)) == -1){
        rtp_freeReceiverControl(receiver_control);
        perror("[Receiver] Bind failure");
        return -1;
    }

    // Wait for START
    fd_set wait_fd;
    FD_ZERO(&wait_fd);
    FD_SET(recvfd, &wait_fd);
    struct timeval timeout = {10, 0}; // 10s
    int res = select(recvfd + 1, &wait_fd, NULL, NULL, &timeout);
    if(res == -1){
        rtp_freeReceiverControl(receiver_control);
        return -1;
    }
    else if(res == 0){
        // Timeout
        rtp_freeReceiverControl(receiver_control);
        close(recvfd);
        return -1;
    }
    else if(FD_ISSET(recvfd, &wait_fd)){
        // Receive START and check its checksum.
        socklen_t addrlen = sizeof(addr);
        rtp_packet_t* recv_ack = rtp_recvfrom(recvfd, (struct sockaddr*)&addr, &addrlen);
        if(!recv_ack){
            rtp_freeReceiverControl(receiver_control);
            close(recvfd);
            return -1;
        }
        else if(recv_ack->rtp.type == RTP_START){
            // Send ACK.
            rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_ack->rtp.seq_num, NULL);
            ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
            if(send_length != sizeof(rtp_header_t)){
                free(pkt);
                free(recv_ack);
                rtp_freeReceiverControl(receiver_control);
                perror("[Receiver] Start ACK send failure");
                return -1;
            }
            free(recv_ack);
            free(pkt);
            return 0;
        }
        else if(recv_ack->rtp.type == RTP_END){
            // Send ACK.
            // rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, receiver_control->seq_next, NULL);
            rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_ack->rtp.seq_num, NULL);
            ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
            if(send_length != sizeof(rtp_header_t))
                perror("[Receiver] End ACK send failure");
            free(pkt);
            free(recv_ack);
            rtp_freeReceiverControl(receiver_control);
            return -1;
        }
        else{
            free(recv_ack);
            rtp_freeReceiverControl(receiver_control);
            perror("[Receiver] Type failure");
            return -1;
        }
    }
    return -1;
}

int recvMessage(char* filename){
    // Open file whose name is filename.
    FILE* recv_file = fopen(filename, "wb");
    if(!recv_file){
        perror("[Receiver] Open file failure");
        return -1;
    }

    int recv_byte = 0;

    // Wait for data.
    fd_set wait_fd;
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(recvfd, &wait_fd);
        struct timeval timeout = {10, 0}; // 10s
        int res = select(recvfd + 1, &wait_fd, NULL, NULL, &timeout);
        if(res == -1){
            fclose(recv_file);
            return -1;
        }
        else if(res == 0){
            fclose(recv_file);
            return recv_byte;
        }
        else if(FD_ISSET(recvfd, &wait_fd)){
            // Receive data pkt.
            socklen_t addrlen = sizeof(addr);
            rtp_packet_t* recv_pkt = rtp_recvfrom(recvfd, (struct sockaddr*)&addr, &addrlen);
            if(!recv_pkt)
                continue;
            else{
                if(recv_pkt->rtp.type == RTP_START || recv_pkt->rtp.type == RTP_END){
                    rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_pkt->rtp.seq_num, NULL);
                    ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                    if(send_length != sizeof(rtp_header_t)){
                        free(pkt);
                        free(recv_pkt);
                        perror("[Receiver] Send failure");
                        return -1;
                    }
                    free(pkt);

                    if(recv_pkt->rtp.type == RTP_END && recv_pkt->rtp.seq_num == receiver_control->seq_next){
                        fclose(recv_file);
                        free(recv_pkt);
                        return recv_byte;
                    }
                    else
                        free(recv_pkt);
                }
                else if(recv_pkt->rtp.type == RTP_DATA){
                    if(recv_pkt->rtp.seq_num > receiver_control->seq_next){
                        if(recv_pkt->rtp.seq_num > receiver_control->seq_next + receiver_control->window_size){
                            free(recv_pkt);
                            continue;
                        }

                        // Cache data.
                        if(recv_pkt->rtp.length == 0)
                            continue;
                        int ack_num = recv_pkt->rtp.seq_num - receiver_control->seq_next;
                        receiver_control->recv_ack[ack_num] = 1;
                        receiver_control->recv_length[ack_num] = recv_pkt->rtp.length;
                        memcpy(receiver_control->recv_buf[ack_num], recv_pkt->payload, recv_pkt->rtp.length);

                        // Send ACK.
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, receiver_control->seq_next, NULL);
                        ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            perror("[Receiver] Send failure");
                            return -1;
                        }
                        free(recv_pkt);
                        free(pkt);
                    }
                    else if(recv_pkt->rtp.seq_num == receiver_control->seq_next){
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
                        
                        // Write to file.
                        for(int i = 0; i < slide_step; i++){
                            size_t write_byte = fwrite(receiver_control->recv_buf[i], 1, receiver_control->recv_length[i], recv_file);
                            if(write_byte != receiver_control->recv_length[i]){
                                free(recv_pkt);
                                perror("[Receiver] Write failure");
                                return -1;
                            }
                            recv_byte += write_byte;
                        }
                        
                        // Update sliding window.
                        for(int i = slide_step; i < receiver_control->window_size; i++){
                            memset(receiver_control->recv_buf[i - slide_step], 0, PAYLOAD_SIZE);
                            memcpy(receiver_control->recv_buf[i - slide_step], receiver_control->recv_buf[i], receiver_control->recv_length[i]);
                            receiver_control->recv_length[i - slide_step] = receiver_control->recv_length[i];
                            receiver_control->recv_ack[i - slide_step] = receiver_control->recv_ack[i];
                        }
                        for(int i = 0; i < slide_step; i++){
                            int j = receiver_control->window_size - 1 - i;
                            memset(receiver_control->recv_buf[j], 0, PAYLOAD_SIZE);
                            receiver_control->recv_length[j] = 0;
                            receiver_control->recv_ack[j] = 0;
                        }

                        // Send ACK.
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, receiver_control->seq_next, NULL);
                        ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            perror("[Receiver] Send failure");
                            return -1;
                        }
                        free(recv_pkt);
                        free(pkt);
                    }
                    else{
                        // Send ACK.
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, receiver_control->seq_next, NULL);
                        ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            perror("[Receiver] Send failure");
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
    rtp_freeReceiverControl(receiver_control);
    close(recvfd);
    return;
}

int recvMessageOpt(char* filename){
    // Open file whose name is filename.
    FILE* recv_file = fopen(filename, "wb");
    if(!recv_file){
        perror("[Receiver] Open file failure");
        return -1;
    }

    int recv_byte = 0;

    // Wait for data.
    fd_set wait_fd;
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(recvfd, &wait_fd);
        struct timeval timeout = {10, 0}; // 10s
        int res = select(recvfd + 1, &wait_fd, NULL, NULL, &timeout);
        if(res == -1){
            fclose(recv_file);
            return -1;
        }
        else if(res == 0){
            fclose(recv_file);
            return recv_byte;
        }
        else if(FD_ISSET(recvfd, &wait_fd)){
            // Receive data pkt.
            socklen_t addrlen = sizeof(addr);
            rtp_packet_t* recv_pkt = rtp_recvfrom(recvfd, (struct sockaddr*)&addr, &addrlen);
            if(!recv_pkt)
                continue;
            else{
                if(recv_pkt->rtp.type == RTP_START || recv_pkt->rtp.type == RTP_END){
                    rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_pkt->rtp.seq_num, NULL);
                    ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                    if(send_length != sizeof(rtp_header_t)){
                        free(pkt);
                        free(recv_pkt);
                        perror("[Receiver] Send failure");
                        return -1;
                    }
                    free(pkt);

                    if(recv_pkt->rtp.type == RTP_END && recv_pkt->rtp.seq_num == receiver_control->seq_next){
                        fclose(recv_file);
                        free(recv_pkt);
                        return recv_byte;
                    }
                    else
                        free(recv_pkt);
                }
                else if(recv_pkt->rtp.type == RTP_DATA){
                    if(recv_pkt->rtp.seq_num > receiver_control->seq_next){
                        if(recv_pkt->rtp.seq_num > receiver_control->seq_next + receiver_control->window_size){
                            free(recv_pkt);
                            continue;
                        }

                        // Cache data.
                        if(recv_pkt->rtp.length == 0)
                            continue;
                        int ack_num = recv_pkt->rtp.seq_num - receiver_control->seq_next;
                        receiver_control->recv_ack[ack_num] = 1;
                        receiver_control->recv_length[ack_num] = recv_pkt->rtp.length;
                        memcpy(receiver_control->recv_buf[ack_num], recv_pkt->payload, recv_pkt->rtp.length);

                        // Send ACK.
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_pkt->rtp.seq_num, NULL);
                        ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            perror("[Receiver] Send failure");
                            return -1;
                        }
                        free(recv_pkt);
                        free(pkt);
                    }
                    else if(recv_pkt->rtp.seq_num == receiver_control->seq_next){
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
                        
                        // Write to file.
                        for(int i = 0; i < slide_step; i++){
                            size_t write_byte = fwrite(receiver_control->recv_buf[i], 1, receiver_control->recv_length[i], recv_file);
                            if(write_byte != receiver_control->recv_length[i]){
                                free(recv_pkt);
                                perror("[Receiver] Write failure");
                                return -1;
                            }
                            recv_byte += write_byte;
                        }
                        
                        // Update sliding window.
                        for(int i = slide_step; i < receiver_control->window_size; i++){
                            memset(receiver_control->recv_buf[i - slide_step], 0, PAYLOAD_SIZE);
                            memcpy(receiver_control->recv_buf[i - slide_step], receiver_control->recv_buf[i], receiver_control->recv_length[i]);
                            receiver_control->recv_length[i - slide_step] = receiver_control->recv_length[i];
                            receiver_control->recv_ack[i - slide_step] = receiver_control->recv_ack[i];
                        }
                        for(int i = 0; i < slide_step; i++){
                            int j = receiver_control->window_size - 1 - i;
                            memset(receiver_control->recv_buf[j], 0, PAYLOAD_SIZE);
                            receiver_control->recv_length[j] = 0;
                            receiver_control->recv_ack[j] = 0;
                        }

                        // Send ACK.
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_pkt->rtp.seq_num, NULL);
                        ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            perror("[Receiver] Send failure");
                            return -1;
                        }
                        free(recv_pkt);
                        free(pkt);
                    }
                    else{
                        // Send ACK.
                        rtp_packet_t* pkt = rtp_packet(RTP_ACK, 0, recv_pkt->rtp.seq_num, NULL);
                        ssize_t send_length = sendto(recvfd, (void*)pkt, sizeof(rtp_header_t), 0, (struct sockaddr*)&addr, addrlen);
                        if(send_length != sizeof(rtp_header_t)){
                            free(pkt);
                            free(recv_pkt);
                            perror("[Receiver] Send failure");
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
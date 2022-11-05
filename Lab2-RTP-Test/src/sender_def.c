#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "rtp.h"
#include "sender_def.h"

rtp_sender_t* sender_control = NULL;
struct sockaddr_in servaddr;
int sendfd;

int initSender(const char* receiver_ip, uint16_t receiver_port, uint32_t window_size){
    // Create a socket.
    sendfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sendfd == -1){
        perror("[Sender] Socket failure");
        return -1;
    }

    // Initialize server sockaddr.
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, receiver_ip, &servaddr.sin_addr);
    servaddr.sin_port = htons(receiver_port);

    // Connect to server.
    socklen_t len = sizeof(servaddr);
    int conn = rtp_connect(sendfd, &servaddr, &len);
    if(conn == -1){
        perror("[Sender] Connection failure");
        close(sendfd);
        return -1;
    }

    // Initialize sender_control.
    sender_control = malloc(sizeof(rtp_sender_t));
    sender_control->window_size = window_size;
    sender_control->seq_base = 0;
    sender_control->seq_next = 0;
    sender_control->send_buf = malloc(window_size * sizeof(char*));
    sender_control->send_length = malloc(window_size * sizeof(size_t));
    sender_control->send_ack = malloc(window_size * sizeof(size_t));
    for(int i=0; i < window_size; ++i){
        sender_control->send_length[i] = 0;
        sender_control->send_ack[i] = 0;
        sender_control->send_buf[i] = malloc(PAYLOAD_SIZE);
        memset(sender_control->send_buf[i], 0, PAYLOAD_SIZE);
    }

    return 0;
}

int sendMessage(const char* message){
    // Open file whose name is message.
    FILE* send_file = fopen(message, "r");
    size_t message_num = 0;
    if(!send_file){
        perror("[Sender] Open file failure");
        return -1;
    }

    // Read file segments to window.
    for(int i=0; i < sender_control->window_size; i++){
        size_t read_byte = fread(sender_control->send_buf[i], 1, PAYLOAD_SIZE, send_file);
        if(read_byte == 0)
            break;
        sender_control->send_length[i] = read_byte;
        message_num++;
    }

    // Send message.
    for(int i=0; i < sender_control->window_size; i++){
        if(sender_control->send_length[i] == 0)
            break;
        //rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_next, sender_control->send_buf[i]);
        ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
        if(send_len != sizeof(rtp_header_t) + sender_control->send_length[i]){
            free(pkt);
            fclose(send_file);
            perror("[Sender] Send failure");
            return -1;
        }
        free(pkt);
        sender_control->seq_next++;
    }

    // Wait for ACK
    fd_set wait_fd;
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(sendfd, &wait_fd);
        struct timeval timeout = {0, 100000}; // 100ms
        int res = select(sendfd + 1, &wait_fd, NULL, NULL, &timeout);
        if(res == -1){
            fclose(send_file);
            return -1;
        }
        else if(res == 0){
            // No message to sent.
            if(sender_control->send_length[0] == 0)
                return 0;

            // Resend message.
            for(int i=0; i < sender_control->window_size; i++){
                if(sender_control->send_length[i] == 0)
                    break;
                rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
                ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
                if(send_len != sizeof(rtp_header_t) + sender_control->send_length[i]){
                    free(pkt);
                    fclose(send_file);
                    perror("[Sender] Resend failure");
                    return -1;
                }
                free(pkt);
            }
        }
        else if(FD_ISSET(sendfd, &wait_fd)){
            // Receive ACK and check its checksum.
            socklen_t addrlen = sizeof(servaddr);
            rtp_packet_t* recv_ack = rtp_recvfrom(sendfd, (struct sockaddr*)&servaddr, &addrlen);
            if(!recv_ack)
                continue;
            else{
                if(recv_ack->rtp.seq_num >= sender_control->seq_base + 1){
                    // Update sliding window
                    int sliding_num = recv_ack->rtp.seq_num - sender_control->seq_base;
                    for(int i = sliding_num; i < sender_control->window_size; i++){
                        memset(sender_control->send_buf[i - sliding_num], 0, PAYLOAD_SIZE);
                        if(sender_control->send_length[i] != 0)
                            memcpy(sender_control->send_buf[i - sliding_num], sender_control->send_buf[i], sender_control->send_length[i]);
                        sender_control->send_length[i - sliding_num] = sender_control->send_length[i];
                    }
                    for(int i = 0; i < sliding_num; i++){
                        int j = sender_control->window_size - 1 - i;
                        if(sender_control->send_length[j] != 0){
                            memset(sender_control->send_buf[j], 0, PAYLOAD_SIZE);
                            sender_control->send_length[j] = 0;
                        }
                    }
                    sender_control->seq_base = recv_ack->rtp.seq_num;
                    
                    // Send more message.
                    // Read from file first.
                    for(int i=0; i < sliding_num; i++){
                        int j = sender_control->window_size - sliding_num + i;
                        size_t read_byte = fread(sender_control->send_buf[j], 1, PAYLOAD_SIZE, send_file);
                        if(read_byte == 0)
                            break;
                        sender_control->send_length[j] = read_byte;
                        message_num++;
                    }

                    // No more message to send.
                    if(sender_control->seq_base >= message_num){
                        free(recv_ack);
                        break;
                    }

                    // Send new message.
                    for(int i = 0; i < sliding_num; i++){
                        int j = sender_control->window_size - sliding_num + i;
                        if(sender_control->send_length[j] == 0)
                            break;
                        // rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[j], sender_control->seq_base + j, sender_control->send_buf[j]);
                        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[j], sender_control->seq_next, sender_control->send_buf[j]);
                        ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[j], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
                        if(send_len != sizeof(rtp_header_t) + sender_control->send_length[j]){
                            free(pkt);
                            perror("[Sender] Send failure");
                            fclose(send_file);
                            return -1;
                        }
                        free(pkt);
                        sender_control->seq_next++;
                    }
                }
            }
            free(recv_ack);
        }
    }
    fclose(send_file);
    return 0;
}

void terminateSender(){
    socklen_t len = sizeof(servaddr);
    rtp_sendEND(sendfd, (struct sockaddr*)&servaddr, &len, sender_control);
    close(sendfd);
    rtp_freeSenderControl(sender_control);
    return;
}

int sendMessageOpt(const char* message){
    // Open file whose name is message.
    FILE* send_file = fopen(message, "r");
    size_t message_num = 0;
    if(!send_file){
        perror("[Sender] Open file failure");
        return -1;
    }

    // Read file segments.
    for(int i=0; i < sender_control->window_size; i++){
        size_t read_byte = fread(sender_control->send_buf[i], 1, PAYLOAD_SIZE, send_file);
        if(read_byte == 0)
            break;
        sender_control->send_length[i] = read_byte;
        message_num++;
    }

    // Send message.
    for(int i=0; i < sender_control->window_size; i++){
        if(sender_control->send_length[i] == 0)
            break;
        // rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_next, sender_control->send_buf[i]);
        ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
        if(send_len != sizeof(rtp_header_t) + sender_control->send_length[i]){
            free(pkt);
            fclose(send_file);
            perror("[Sender] Send failure");
            return -1;
        }
        free(pkt);
        sender_control->seq_next++;
    }

    // Wait for ACK
    fd_set wait_fd;
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(sendfd, &wait_fd);
        struct timeval timeout = {0, 100000}; // 100ms
        int res = select(sendfd + 1, &wait_fd, NULL, NULL, &timeout);
        if(res == -1){
            fclose(send_file);
            return -1;
        }
        else if(res == 0){
            // No more message to send.
            if(sender_control->send_length[0] == 0)
                return 0;

            // Resend message.
            for(int i=0; i < sender_control->window_size; i++){
                // Send message not acked.
                if(sender_control->send_ack[i] == 1)
                    continue;
                if(sender_control->send_length[i] == 0)
                    break;
                rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
                ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
                if(send_len != sizeof(rtp_header_t) + sender_control->send_length[i]){
                    free(pkt);
                    fclose(send_file);
                    perror("[Sender] Resend failure");
                    return -1;
                }
                free(pkt);
            }
        }
        else if(FD_ISSET(sendfd, &wait_fd)){
            // Receive ACK and check its checksum.
            socklen_t addrlen = sizeof(servaddr);
            rtp_packet_t* recv_ack = rtp_recvfrom(sendfd, (struct sockaddr*)&servaddr, &addrlen);
            if(!recv_ack)
                // If ACK pkt is broken
                continue;
            else{
                if(recv_ack->rtp.seq_num == sender_control->seq_base){
                    // Update sliding window
                    int sliding_num = 0;
                    sender_control->send_ack[0] = 1;
                    for(;sliding_num < sender_control->window_size; sliding_num++){
                        if(sender_control->send_ack[sliding_num] == 0)
                            break;
                    }
                    for(int i = sliding_num; i < sender_control->window_size; i++){
                        memset(sender_control->send_buf[i - sliding_num], 0, PAYLOAD_SIZE);
                        if(sender_control->send_length[i] != 0)
                            memcpy(sender_control->send_buf[i - sliding_num], sender_control->send_buf[i], sender_control->send_length[i]);
                        sender_control->send_length[i - sliding_num] = sender_control->send_length[i];
                        sender_control->send_ack[i - sliding_num] = sender_control->send_ack[i];
                    }
                    for(int i = 0; i < sliding_num; i++){
                        int j = sender_control->window_size - 1 - i;
                        if(sender_control->send_length[j] != 0){
                            memset(sender_control->send_buf[j], 0, PAYLOAD_SIZE);
                            sender_control->send_length[j] = 0;
                        }
                        sender_control->send_ack[j] = 0;
                    }
                    sender_control->seq_base += sliding_num;
                    
                    // Send more message.
                    // Read from file first.
                    for(int i=0; i < sliding_num; i++){
                        int j = sender_control->window_size - sliding_num + i;
                        size_t read_byte = fread(sender_control->send_buf[j], 1, PAYLOAD_SIZE, send_file);
                        if(read_byte == 0)
                            break;
                        sender_control->send_length[j] = read_byte;
                        message_num++;
                    }

                    // No more message to send.
                    if(sender_control->seq_base >= message_num){
                        free(recv_ack);
                        break;
                    }

                    // Send new message.
                    for(int i = 0; i < sliding_num; i++){
                        int j = sender_control->window_size - sliding_num + i;
                        if(sender_control->send_length[j] == 0)
                            break;
                        // rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[j], sender_control->seq_base + j, sender_control->send_buf[j]);
                        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[j], sender_control->seq_next, sender_control->send_buf[j]);                        
                        ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[j], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
                        if(send_len != sizeof(rtp_header_t) + sender_control->send_length[j]){
                            free(pkt);
                            perror("[Sender] Send failure");
                            fclose(send_file);
                            return -1;
                        }
                        free(pkt);
                        sender_control->seq_next++;
                    }
                }
                else if(recv_ack->rtp.seq_num > sender_control->seq_base){
                    int ack_num = recv_ack->rtp.seq_num - sender_control->seq_base;
                    sender_control->send_ack[ack_num] = 1;
                }
            }
            free(recv_ack);
        }
    }
    fclose(send_file);
    return 0;
}
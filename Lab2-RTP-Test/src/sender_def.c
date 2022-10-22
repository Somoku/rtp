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
    //printf("Sender: Creating a socket...\n");
    sendfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sendfd == -1){
        perror("Socket failure");
        return -1;
    }

    // Initialize server sockaddr.
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, receiver_ip, &servaddr.sin_addr);
    servaddr.sin_port = htons(receiver_port);

    // Connect to server.
    //printf("Sender: Connecting...\n");
    socklen_t len = sizeof(servaddr);
    int conn = rtp_connect(sendfd, &servaddr, &len);
    if(conn == -1){
        //printf("conn = -1\n");
        perror("Connection failure");
        close(sendfd);
        // rtp_freeSenderControl(sender_control);
        return -1;
    }
    //printf("Sender: Connected.\n");

    // Initialize sender_control.
    //printf("Sender: Initializing sender_control...\n");
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
    //printf("Sender: Opening a file.\n");
    //printf("filename = %s\n", message);
    FILE* send_file = fopen(message, "r");
    size_t message_num = 0;
    if(!send_file){
        perror("Open file failure");
        return -1;
    }

    // Read file segments.
    //printf("Sender: Reading file...\n");
    //printf("sender_control->window_size = %d\n", sender_control->window_size);
    for(int i=0; i < sender_control->window_size; i++){
        // sender_control->send_buf[i] = malloc(sizeof(PAYLOAD_SIZE));
        size_t read_byte = fread(sender_control->send_buf[i], 1, PAYLOAD_SIZE, send_file);
        //printf("read_byte = %d\n", read_byte);
        if(read_byte == 0){
            // free(sender_control->send_buf[i]);
            // sender_control->send_buf[i] = NULL;
            break;
        }
        sender_control->send_length[i] = read_byte;
        message_num++;
    }
    //printf("Sender: Read message_num = %d\n", message_num);

    // Send message.
    for(int i=0; i < sender_control->window_size; i++){
        if(sender_control->send_length[i] == 0)
            break;
        // printf("Sender: sending message %d.\n", i);
        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
        ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
        if(send_len != sizeof(rtp_header_t) + sender_control->send_length[i]){
            free(pkt);
            fclose(send_file);
            perror("Data send failure");
            return -1;
        }
        free(pkt);
        sender_control->seq_next++;
    }

    // Wait for ACK
    fd_set wait_fd;
    //printf("Sender: Waiting for ACK.\n");
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(sendfd, &wait_fd);
        struct timeval timeout = {0, 100000};
        // printf("Sender: Waiting...\n");
        int res = select(sendfd + 1, &wait_fd, NULL, NULL, &timeout);
        if(res == -1){
            perror("Select failure");
            fclose(send_file);
            return -1;
        }
        else if(res == 0){
            if(sender_control->send_length[0] == 0){
                printf("Returning...\n");
                return 0;
            }
            // printf("Sender: Time out. Resending...\n");
            // Resend message.
            for(int i=0; i < sender_control->window_size; i++){
                if(sender_control->send_length[i] == 0)
                    break;
                rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
                ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
                if(send_len != sizeof(rtp_header_t) + sender_control->send_length[i]){
                    free(pkt);
                    fclose(send_file);
                    perror("Data send failure");
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
                //printf("Sender: Receive ACK...\n");
                //printf("Sender: received seq_num = %d\n", recv_ack->rtp.seq_num);
                //printf("Sender: seq_base = %d\n", sender_control->seq_base);
                if(recv_ack->rtp.seq_num >= sender_control->seq_base + 1){
                    // Update sliding window
                    int sliding_num = recv_ack->rtp.seq_num - sender_control->seq_base;
                    //printf("Sender: sliding_num = %d\n", sliding_num);
                    for(int i = sliding_num; i < sender_control->window_size; i++){
                        memset(sender_control->send_buf[i - sliding_num], 0, PAYLOAD_SIZE);
                        if(sender_control->send_length[i] != 0)
                            memcpy(sender_control->send_buf[i - sliding_num], sender_control->send_buf[i], sender_control->send_length[i]);
                        //memset(sender_control->send_buf[i], 0, PAYLOAD_SIZE);
                        sender_control->send_length[i - sliding_num] = sender_control->send_length[i];
                        //sender_control->send_length[i] = 0;
                    }
                    for(int i = 0; i < sliding_num; i++){
                        int j = sender_control->window_size - 1 - i;
                        //printf("Sender: length = %d\n", sender_control->send_length[j]);
                        //printf("Sender: Fourth step.\n");
                        if(sender_control->send_length[j] != 0){
                            memset(sender_control->send_buf[j], 0, PAYLOAD_SIZE);
                            sender_control->send_length[j] = 0;
                        }
                        //printf("Sender: Step i = %d, j = %d\n", i , j);
                    }
                    sender_control->seq_base = recv_ack->rtp.seq_num;
                    //printf("Sender: Update finished.\n");
                    // printf("Sender: seq_base = %d\n", sender_control->seq_base);
                    
                    // Send more message.
                    // Read from file first.
                    for(int i=0; i < sliding_num; i++){
                        int j = sender_control->window_size - sliding_num + i;
                        // sender_control->send_buf[j] = malloc(sizeof(PAYLOAD_SIZE));
                        size_t read_byte = fread(sender_control->send_buf[j], 1, PAYLOAD_SIZE, send_file);
                        if(read_byte == 0){
                            // free(sender_control->send_buf[j]);
                            //sender_control->send_buf[j] = NULL;
                            //sender_control->send_length[j] = 0;
                            break;
                        }
                        sender_control->send_length[j] = read_byte;
                        message_num++;
                    }
                    //printf("Sender: New read message_num = %d\n", message_num);

                    // No more message to send.
                    if(sender_control->seq_base >= message_num){
                        printf("Sender: Quit...\n");
                        free(recv_ack);
                        break;
                    }

                    // Send message.
                    for(int i = 0; i < sliding_num; i++){
                        //if(sender_control->send_length[sender_control->window_size - sliding_num] == 0)
                        //    return 0;
                        int j = sender_control->window_size - sliding_num + i;
                        //printf("Sender: sending message. seq_base + j = %d\n", sender_control->seq_base + j);
                        if(sender_control->send_length[j] == 0)
                            break;
                        //printf("Sender: sending message. seq_next = %d\n", sender_control->seq_next);
                        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[j], sender_control->seq_base + j, sender_control->send_buf[j]);
                        ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[j], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
                        if(send_len != sizeof(rtp_header_t) + sender_control->send_length[j]){
                            free(pkt);
                            perror("Data send failure");
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
    //printf("Sender: terminate.\n");
    socklen_t len = sizeof(servaddr);
    rtp_sendEND(sendfd, (struct sockaddr*)&servaddr, &len, sender_control);
    close(sendfd);
    rtp_freeSenderControl(sender_control);
    return;
}


/**
 * @brief 用于发送数据 (优化版本的RTP)
 * @param message 要发送的文件名
 * @return -1表示发送失败，0表示发送成功
 **/
int sendMessageOpt(const char* message){
    // Open file whose name is message.
    //printf("Sender: Opening a file.\n");
    //printf("filename = %s\n", message);
    FILE* send_file = fopen(message, "r");
    size_t message_num = 0;
    if(!send_file){
        perror("Open file failure");
        return -1;
    }

    // Read file segments.
    //printf("Sender: Reading file...\n");
    //printf("sender_control->window_size = %d\n", sender_control->window_size);
    for(int i=0; i < sender_control->window_size; i++){
        // sender_control->send_buf[i] = malloc(sizeof(PAYLOAD_SIZE));
        size_t read_byte = fread(sender_control->send_buf[i], 1, PAYLOAD_SIZE, send_file);
        //printf("read_byte = %d\n", read_byte);
        if(read_byte == 0){
            // free(sender_control->send_buf[i]);
            // sender_control->send_buf[i] = NULL;
            break;
        }
        sender_control->send_length[i] = read_byte;
        message_num++;
    }
    //printf("Sender: Read message_num = %d\n", message_num);

    // Send message.
    for(int i=0; i < sender_control->window_size; i++){
        if(sender_control->send_length[i] == 0)
            break;
        // printf("Sender: sending message %d.\n", i);
        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
        ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
        if(send_len != sizeof(rtp_header_t) + sender_control->send_length[i]){
            free(pkt);
            fclose(send_file);
            perror("Data send failure");
            return -1;
        }
        free(pkt);
        sender_control->seq_next++;
    }

    // Wait for ACK
    fd_set wait_fd;
    //printf("Sender: Waiting for ACK.\n");
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(sendfd, &wait_fd);
        struct timeval timeout = {0, 100000}; // 100ms
        // printf("Sender: Waiting...\n");
        int res = select(sendfd + 1, &wait_fd, NULL, NULL, &timeout);
        if(res == -1){
            perror("Select failure");
            fclose(send_file);
            return -1;
        }
        else if(res == 0){
            if(sender_control->send_length[0] == 0){
                printf("Returning...\n");
                return 0;
            }
            // printf("Sender: Time out. Resending...\n");
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
                    perror("Data send failure");
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
                //printf("Sender: Receive ACK...\n");
                //printf("Sender: received seq_num = %d\n", recv_ack->rtp.seq_num);
                //printf("Sender: seq_base = %d\n", sender_control->seq_base);
                if(recv_ack->rtp.seq_num == sender_control->seq_base){
                    // Update sliding window
                    //int sliding_num = recv_ack->rtp.seq_num - sender_control->seq_base;
                    int sliding_num = 0;
                    sender_control->send_ack[0] = 1;
                    for(;sliding_num < sender_control->window_size; sliding_num++){
                        if(sender_control->send_ack[sliding_num] == 0)
                            break;
                    }
                    //printf("Sender: sliding_num = %d\n", sliding_num);
                    for(int i = sliding_num; i < sender_control->window_size; i++){
                        memset(sender_control->send_buf[i - sliding_num], 0, PAYLOAD_SIZE);
                        if(sender_control->send_length[i] != 0)
                            memcpy(sender_control->send_buf[i - sliding_num], sender_control->send_buf[i], sender_control->send_length[i]);
                        //memset(sender_control->send_buf[i], 0, PAYLOAD_SIZE);
                        sender_control->send_length[i - sliding_num] = sender_control->send_length[i];
                        sender_control->send_ack[i - sliding_num] = sender_control->send_ack[i];
                        //sender_control->send_length[i] = 0;
                    }
                    for(int i = 0; i < sliding_num; i++){
                        int j = sender_control->window_size - 1 - i;
                        //printf("Sender: length = %d\n", sender_control->send_length[j]);
                        //printf("Sender: Fourth step.\n");
                        if(sender_control->send_length[j] != 0){
                            memset(sender_control->send_buf[j], 0, PAYLOAD_SIZE);
                            sender_control->send_length[j] = 0;
                        }
                        sender_control->send_ack[j] = 0;
                        //printf("Sender: Step i = %d, j = %d\n", i , j);
                    }
                    //sender_control->seq_base = recv_ack->rtp.seq_num;
                    sender_control->seq_base += sliding_num;
                    //printf("Sender: Update finished.\n");
                    // printf("Sender: seq_base = %d\n", sender_control->seq_base);
                    
                    // Send more message.
                    // Read from file first.
                    for(int i=0; i < sliding_num; i++){
                        int j = sender_control->window_size - sliding_num + i;
                        // sender_control->send_buf[j] = malloc(sizeof(PAYLOAD_SIZE));
                        size_t read_byte = fread(sender_control->send_buf[j], 1, PAYLOAD_SIZE, send_file);
                        if(read_byte == 0){
                            // free(sender_control->send_buf[j]);
                            //sender_control->send_buf[j] = NULL;
                            //sender_control->send_length[j] = 0;
                            break;
                        }
                        sender_control->send_length[j] = read_byte;
                        message_num++;
                    }
                    //printf("Sender: New read message_num = %d\n", message_num);

                    // No more message to send.
                    if(sender_control->seq_base >= message_num){
                        free(recv_ack);
                        break;
                    }

                    // Send message.
                    for(int i = 0; i < sliding_num; i++){
                        //if(sender_control->send_length[sender_control->window_size - sliding_num] == 0)
                        //    return 0;
                        int j = sender_control->window_size - sliding_num + i;
                        //printf("Sender: sending message. seq_base + j = %d\n", sender_control->seq_base + j);
                        if(sender_control->send_length[j] == 0)
                            break;
                        //printf("Sender: sending message. seq_next = %d\n", sender_control->seq_next);
                        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[j], sender_control->seq_base + j, sender_control->send_buf[j]);
                        ssize_t send_len = sendto(sendfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[j], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
                        if(send_len != sizeof(rtp_header_t) + sender_control->send_length[j]){
                            free(pkt);
                            perror("Data send failure");
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
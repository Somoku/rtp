#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "rtp.h"
#include "sender_def.h"

//TODO: Remember to free sender_control
rtp_sender_t* sender_control = NULL;
struct sockaddr_in servaddr;
int sockfd;

int initSender(const char* receiver_ip, uint16_t receiver_port, uint32_t window_size){
    // Create a socket.
    printf("Sender: Creating a socket...\n");
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd == -1){
        perror("Socket failure");
        return -1;
    }

    // Initialize sender_control.
    printf("Sender: Initializing sender_control...\n");
    sender_control = malloc(sizeof(rtp_sender_t));
    sender_control->window_size = window_size;
    sender_control->seq_base = 0;
    sender_control->seq_next = 0;
    sender_control->send_buf = malloc(window_size * sizeof(char*));
    sender_control->send_length = malloc(window_size * sizeof(size_t));
    for(int i=0; i < window_size; ++i){
        sender_control->send_length[i] = 0;
        sender_control->send_buf[i] = malloc(PAYLOAD_SIZE);
    }

    // Initialize server sockaddr.
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, receiver_ip, &servaddr.sin_addr);
    servaddr.sin_port = htons(receiver_port);

    // Connect to server.
    printf("Sender: Connecting...\n");
    int conn = rtp_connect(sockfd, &servaddr, sizeof(servaddr), sender_control);
    if(conn == -1){
        perror("Connection failure");
        close(sockfd);
        rtp_freeSenderControl(sender_control);
        return -1;
    }
    printf("Sender: Connected.\n");

    return 0;
}

int sendMessage(const char* message){
    // Open file whose name is message.
    printf("Sender: Opening a file.\n");
    printf("filename = %s\n", message);
    FILE* send_file = fopen(message, "r");
    size_t message_num = 0;
    if(!send_file){
        perror("Open file failure");
        return -1;
    }

    // Read file segments.
    printf("Sender: Reading file...\n");
    printf("sender_control->window_size = %d\n", sender_control->window_size);
    for(int i=0; i < sender_control->window_size; i++){
        // sender_control->send_buf[i] = malloc(sizeof(PAYLOAD_SIZE));
        size_t read_byte = fread(sender_control->send_buf[i], 1, PAYLOAD_SIZE, send_file);
        printf("read_byte = %d\n", read_byte);
        if(read_byte == 0){
            // free(sender_control->send_buf[i]);
            // sender_control->send_buf[i] = NULL;
            break;
        }
        sender_control->send_length[i] = read_byte;
        message_num++;
    }
    printf("Sender: Read message_num = %d\n", message_num);

    // Send message.
    for(int i=0; i < sender_control->window_size; i++){
        if(sender_control->send_length[i] == 0)
            break;
        printf("Sender: sending message %d.\n", i);
        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
        ssize_t send_len = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
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
    struct timeval timeout = {0, 100000};
    printf("Sender: Waiting for ACK.\n");
    while(true){
        FD_ZERO(&wait_fd);
        FD_SET(sockfd, &wait_fd);
        // printf("Sender: Waiting...\n");
        int res = select(sockfd + 1, &wait_fd, NULL, NULL, &timeout);
        if(res == -1){
            perror("Select failure");
            fclose(send_file);
            return -1;
        }
        else if(res == 0){
            if(sender_control->send_length[0] == 0)
                return 0;
            // printf("Sender: Time out. Resending...\n");
            // Resend message.
            for(int i=0; i < sender_control->window_size; i++){
                if(sender_control->send_length[i] == 0)
                    break;
                rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[i], sender_control->seq_base + i, sender_control->send_buf[i]);
                ssize_t send_len = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[i], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
                if(send_len != sizeof(rtp_header_t) + sender_control->send_length[i]){
                    free(pkt);
                    fclose(send_file);
                    perror("Data send failure");
                    return -1;
                }
                free(pkt);
            }
        }
        else if(FD_ISSET(sockfd, &wait_fd)){
            // Receive ACK and check its checksum.
            socklen_t addrlen = sizeof(servaddr);
            rtp_packet_t* recv_ack = rtp_recvfrom(sockfd, (struct sockaddr*)&servaddr, &addrlen);
            if(!recv_ack)
                // If ACK pkt is broken
                continue;
            else{
                printf("Sender: Receive ACK...\n");
                printf("Sender: received seq_num = %d\n", recv_ack->rtp.seq_num);
                printf("Sender: seq_base = %d\n", sender_control->seq_base);
                if(recv_ack->rtp.seq_num >= sender_control->seq_base + 1){
                    // Update sliding window
                    int sliding_num = recv_ack->rtp.seq_num - sender_control->seq_base;
                    printf("Sender: sliding_num = %d\n", sliding_num);
                    for(int i = sliding_num; i < sender_control->window_size; i++){
                        if(sender_control->send_length[i] == 0)
                            break;
                        memset(sender_control->send_buf[i - sliding_num], 0, PAYLOAD_SIZE);
                        memcpy(sender_control->send_buf[i - sliding_num], sender_control->send_buf[i], sender_control->send_length[i]);
                        memset(sender_control->send_buf[i], 0, PAYLOAD_SIZE);
                        sender_control->send_length[i - sliding_num] = sender_control->send_length[i];
                        sender_control->send_length[i] = 0;
                    }
                    sender_control->seq_base = recv_ack->rtp.seq_num;
                    printf("Sender: seq_base = %d\n", sender_control->seq_base);
                    
                    // Send more message.
                    // Read from file first.
                    for(int i=0; i < sliding_num; i++){
                        int j = sender_control->window_size - sliding_num + i;
                        // sender_control->send_buf[j] = malloc(sizeof(PAYLOAD_SIZE));
                        size_t read_byte = fread(sender_control->send_buf[j], 1, PAYLOAD_SIZE, send_file);
                        if(read_byte == 0){
                            // free(sender_control->send_buf[j]);
                            sender_control->send_buf[j] = NULL;
                            sender_control->send_length[j] = 0;
                            break;
                        }
                        sender_control->send_length[j] = read_byte;
                        message_num++;
                    }
                    printf("Sender: Sent message_num = %d\n", message_num);

                    // No more message to send.
                    //if(sender_control->seq_base >= message_num){
                    //    printf("Sender: Quit...\n");
                    //    free(recv_ack);
                    //    break;
                    //}

                    // Send message.
                    for(int i = 0; i < sliding_num; i++){
                        if(sender_control->send_length[0] == 0)
                            return 0;
                        printf("Sender: sending message. seq_next = %d\n", sender_control->seq_next);
                        int j = sender_control->window_size - sliding_num + i;
                        printf("Sender: sending message. seq_base + j = %d\n", sender_control->seq_base + j);
                        if(sender_control->send_length[j] == 0)
                            break;
                        rtp_packet_t* pkt = rtp_packet(RTP_DATA, sender_control->send_length[j], sender_control->seq_base + j, sender_control->send_buf[j]);
                        ssize_t send_len = sendto(sockfd, (void*)pkt, sizeof(rtp_header_t) + sender_control->send_length[j], 0, (struct sockaddr*)&servaddr, sizeof(servaddr));
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
    printf("Sender: terminate.\n");
    rtp_sendEND(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr), sender_control);
    close(sockfd);
    rtp_freeSenderControl(sender_control);
    return;
}

int sendMessageOpt(const char* message){
    return 0;
}
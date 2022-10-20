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

/**
 * @brief 开启receiver并在所有IP的port端口监听等待连接
 * 
 * @param port receiver监听的port
 * @param window_size window大小
 * @return -1表示连接失败，0表示连接成功
 */
int initReceiver(uint16_t port, uint32_t window_size){
    // Create a socket.
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd == -1){
        perror("Socket failure");
        return -1;
    }

    // Initialize server control.
    receiver_control = malloc(sizeof(rtp_receiver_t));
    receiver_control->window_size = window_size;
    receiver_control->recv_buf = malloc(window_size * sizeof(char*));
    receiver_control->recv_length = malloc(window_size * sizeof(size_t));
    for(int i=0; i < window_size; ++i){
        receiver_control->recv_length[i] = 0;
        receiver_control->recv_buf[i] = NULL;
    } 

    // Initialize sockaddr.
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1){
        rtp_freeReceiverControl(receiver_control);
        perror("Bind failure");
        return -1;
    }

    // Wait for START
    fd_set wait_fd;
    FD_SET(sockfd, &wait_fd);
    struct timeval timeout = {10, 0}; // 10s
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
        size_t addrlen = sizeof(addr);
        rtp_packet_t* recv_ack = rtp_recvfrom(sockfd, &addr, &addrlen);
        if(!recv_ack){
            rtp_freeReceiverControl(receiver_control);
            close(sockfd);
            return -1;
        }
        else if(recv_ack->rtp.type == RTP_START){
            // Send ACK.
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
        else{
            free(recv_ack);
            rtp_freeReceiverControl(receiver_control);
            perror("Type failure");
            return -1;
        }
    }
    return -1;
}

/**
 * @brief 用于接收数据并在接收完后断开RTP连接
 * @param filename 用于接收数据的文件名
 * @return >0表示接收完成后到数据的字节数 -1表示出现其他错误
 */
int recvMessage(char* filename){
    // Open file whose name is filename.
    FILE* recv_file = fopen(filename, "w");
    if(!recv_file){
        perror("Open file failure");
        return -1;
    }

    // Wait for data.
    
}

/**
 * @brief 用于接收数据失败时断开RTP连接以及关闭UDP socket
 */
void terminateReceiver(){

}
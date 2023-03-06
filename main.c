#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <net/if.h>
#include <fcntl.h>

#define DEFAULT_PORT 18191
#define BUFFER_SIZE 1024

// typedef void *(* func)(void *);
typedef struct 
{
    int sockfd;
    int epfd;
    int (*callback)(int sockfd, int event, void *arg);
    char send_buffer[BUFFER_SIZE];
    char recv_buffer[BUFFER_SIZE];
    ssize_t str_len;
    
}event_item;

static int set_block(int fd, int block);                    //设置非阻塞socket
// 回调函数
int accept_callback(int sock_fd, int event, void *arg);     // 监听套接字对应回调函数
int receive_callback(int sock_fd, int event, void *arg);    // 可接收数据套接字回调函数
int send_callback(int sock_fd, int event, void *arg);       // 可发送数据套接字回调函数


int createTcpSocket(char* ip, int port)
{
    int sock = 0, flag = 0;
    struct sockaddr_in bindAddr;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) return -1;

    set_block(sock, 0);
    flag = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag)) < 0) {
        printf("can't set SO_RESUSEADDR option");
    }
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    if (port != 0)
        bindAddr.sin_port = htons(port);
    else
        bindAddr.sin_port = htons(DEFAULT_PORT);
    if (NULL != ip)
        bindAddr.sin_addr.s_addr = inet_addr(ip);
    else
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if ((bind(sock, (struct sockaddr *)&bindAddr, sizeof(struct sockaddr_in))) == -1) return -1;

    return sock;

}

void * echo_action(void* arg)
{
    int epfd = *(int*)arg;
    int i;
    event_item *item = NULL;

    while(1) {
        struct epoll_event events[128] = {0};

        int nready = epoll_wait(epfd, events, 128, -1);
        if (nready < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            } else {
                break;
            }
        } else if (nready == 0) {
            continue;
        }
        
        for (i = 0; i < nready; i++) {
            if (events[i].events & EPOLLIN) {
                item = events[i].data.ptr;
                item->callback(item->sockfd, events[i].events, (void *)item);
            }

            if (events[i].events & EPOLLOUT) {
                item = events[i].data.ptr;
                item->callback(item->sockfd, events[i].events, (void *)item);
            }
        }
    }
    return NULL;
}

int main() 
{
    int epfd;
    int lfd,ret;
    pthread_t thread_id;
    struct epoll_event ev;
    event_item *item = NULL;

    epfd = epoll_create(1);
    if (epfd < 0) return 0;
    lfd = createTcpSocket(NULL, DEFAULT_PORT);

    /*listen*/
    if((listen(lfd, 1024)) == -1) {
        perror("listen error");
        return -1;
    }
    /*set data to ptr*/
    item = (event_item *)malloc(sizeof(event_item));
    if(NULL == item) {
        perror("malloc listenfd data fail.");
        return -1;
    }
    item->callback = accept_callback;
    item->sockfd = lfd;
    item->epfd = epfd;

    ev.data.fd = lfd;
    ev.data.ptr = (void *)item;
    ev.events = EPOLLIN;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    ret = pthread_create(&thread_id, NULL, echo_action, (void *)&epfd);
    if (ret) return -1;

    pthread_join(thread_id, NULL);

    return 1;
}

int accept_callback(int sockfd, int event, void *arg)
{
    /*accept client conn*/
    int client_fd;
    char str[INET_ADDRSTRLEN]= {0};
    struct sockaddr_in clientAddr;
    socklen_t clientAddr_size = sizeof(clientAddr);
    struct epoll_event ev;
    event_item *ev_item = NULL;
    event_item *ptr = (event_item *)arg;

    client_fd = accept(sockfd, (struct sockaddr *)&clientAddr, &clientAddr_size);
    if (client_fd == -1) {
        return -1;
    } else {
        printf("connect client ip[%s], port[%d]\n",inet_ntop(AF_INET, &clientAddr.sin_addr.s_addr, str, INET_ADDRSTRLEN),
                                        ntohs(clientAddr.sin_port));
    }

    set_block(client_fd, 0);

    ev_item = (event_item *)malloc(sizeof(event_item));
    ev_item->callback = receive_callback;
    ev_item->sockfd = client_fd;
    ev_item->epfd = ptr->epfd;

    ev.events = EPOLLIN;
    ev.data.fd = client_fd;
    ev.data.ptr = ev_item;
    epoll_ctl(ptr->epfd, EPOLL_CTL_ADD, client_fd, &ev);
    return 0;
}

int receive_callback(int sock_fd, int event, void *arg)
{
    event_item *ptr = (event_item *)arg;
    char *pRecvdata = ptr->send_buffer;
    ssize_t recvTotal = 0;
    struct epoll_event ev;

    while(1) {
        ptr->str_len = recv(ptr->sockfd, ptr->recv_buffer, BUFFER_SIZE, 0);
        if (ptr->str_len == 0) {
            perror("client connect shutdown.");
            epoll_ctl(ptr->epfd, EPOLL_CTL_DEL, sock_fd, NULL);
            free(ptr);
            return 0;
        } else if (ptr->str_len > 0) {
            memcpy(pRecvdata, ptr->recv_buffer, ptr->str_len);
            printf("recvData: [%s]\n",pRecvdata);
            memset(ptr->recv_buffer, 0 ,BUFFER_SIZE);
            pRecvdata = pRecvdata + ptr->str_len; 
            recvTotal += ptr->str_len;      
        }else {
            if (errno = EAGAIN) {
                printf("clientFD[%d] receive data completed.\n", sock_fd);
                break;
            } else {
                perror("recv");
                exit(0);
            }
        }
    }

    ev.events = EPOLLOUT;
    ev.data.fd = sock_fd;
    ptr->callback = send_callback;
    ptr->str_len = recvTotal;
    ev.data.ptr = ptr;
    epoll_ctl(ptr->epfd, EPOLL_CTL_MOD, sock_fd, &ev);
    return 1;
}

int send_callback(int sock_fd, int event, void *arg)
{
    /* 发送完成  事件改为可读*/
    struct epoll_event ev;

    event_item *ptr = (event_item *)arg;
    send(sock_fd, ptr->send_buffer, ptr->str_len, 0);
    __bzero(ptr->send_buffer, BUFFER_SIZE);
    ev.events = EPOLLIN;
    ev.data.fd = sock_fd;
    ev.data.ptr = ptr;
    ptr->callback = receive_callback;
    epoll_ctl(ptr->epfd, EPOLL_CTL_MOD, sock_fd, &ev);
    return 1;
}

static int set_block(int fd, int block) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) return flags;

	if (block) {        
		flags &= ~O_NONBLOCK;    
	} else {        
		flags |= O_NONBLOCK;    
	}

	if (fcntl(fd, F_SETFL, flags) < 0) return -1;

	return 0;
}

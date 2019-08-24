#include <stdio.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

#include "wrap.h"

#define MAX_EVENTS  1024                                    //监听上限数
#define BUFLEN 4096
#define SERV_PORT   6666

/* 描述就绪文件描述符相关信息 */
struct myevent_s {
    int fd;                                                 //要监听的文件描述符
    int events;                                             //对应的监听事件
    void *arg;                                              //泛型参数
    void (*call_back)(int fd, int events, void *arg);       //回调函数
    int status;                                             //是否在监听:1->在红黑树上(监听), 0->不在(不监听)
    char buf[BUFLEN];
    int len;
    long last_active;                                       //记录每次加入红黑树 g_efd 的时间值
};

int g_efd;                                                  //全局变量, 保存epoll_create返回的文件描述符
struct myevent_s g_events[MAX_EVENTS+1];                    //自定义结构体类型数组. +1-->listen fd

/*函数声明*/
void initlistensocket(int efd, short port);
void eventset(struct myevent_s *ev, int fd, void (*call_back)(int, int, void *), void *arg);
void eventadd(int efd, int events, struct myevent_s *ev);
void eventdel(int efd, struct myevent_s *ev);
void acceptconn(int lfd, int events, void *arg);
void recvdata(int fd, int events, void *arg);
void senddata(int fd, int events, void *arg);

/*创建 socket, 初始化lfd，将lfd挂到红黑树上 */
void initlistensocket(int efd, short port)
{
    /*创建监听套接字*/
    int lfd = Socket(AF_INET, SOCK_STREAM, 0);

    /*修改lfd为非阻塞*/
    int flag = 0;
    if ((flag = fcntl(lfd, F_SETFL, O_NONBLOCK)) < 0) {
        printf("%s: fcntl nonblocking failed, %s\n", __func__, strerror(errno));
        return;
    }

    /*端口复用*/
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /*初始化lfd*/
    //void eventset(struct myevent_s *ev, int fd, void (*call_back)(int, int, void *), void *arg);
    //将lfd放置在将自定义结构体数组的最后
    //acceptconn()是lfd的回调函数
    //arg是回调函数acceptconn()的参数，指向lfd带的结构体中ptr指向的自定义结构体
    eventset(&g_events[MAX_EVENTS], lfd, acceptconn, &g_events[MAX_EVENTS]);

    /*将lfd挂在红黑树上，设定监听事件为读事件*/
    eventadd(efd, EPOLLIN, &g_events[MAX_EVENTS]);

    /*绑定*/
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);
    Bind(lfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    
    /*监听*/
    Listen(lfd, 128);

    return;
}

/*将自定义结构体 myevent_s 成员变量 初始化*/
void eventset(struct myevent_s *ev, int fd, void (*call_back)(int, int, void *), void *arg)
{
    ev->fd = fd;
    ev->call_back = call_back;
    ev->events = 0;
    ev->arg = arg;
    ev->status = 0;
    ev->last_active = time(NULL);

    return;
}

/* 向 epoll监听的红黑树 添加一个 文件描述符 */
void eventadd(int efd, int events, struct myevent_s *ev)
{
    struct epoll_event epv = {0, {0}};
    epv.data.ptr = ev;
    epv.events = events; //EPOLLIN 或 EPOLLOUT
    ev->events = events; //EPOLLIN 或 EPOLLOUT

    int op;
    if(ev->status == 1){//已经在红黑树 g_efd 里
        op = EPOLL_CTL_MOD;//修改其属性
    }
    else{//不在红黑树里
        op = EPOLL_CTL_ADD;//将其加入红黑树 g_efd
        ev->status = 1;//并将status置1
    }

    if(epoll_ctl(efd, op, ev->fd, &epv) < 0){
        printf("event add failed fd[%d], events[%d]\n", ev->fd, events);
    }
    else{
        printf("event add OK fd[%d], events[%d]\n", ev->fd, events);
    }

    return;
}

/* 从epoll 监听的 红黑树中删除一个 文件描述符*/
void eventdel(int efd, struct myevent_s *ev)
{
    struct epoll_event epv = {0, {0}};

    if(ev->status == 0){//不在红黑树上
        return;
    }

    epv.data.ptr = ev;
    ev->status = 0;
    epoll_ctl(efd, EPOLL_CTL_DEL, ev->fd, &epv);

    return;
}

/*lfd的回调函数：当有文件描述符就绪, epoll返回, 调用该函数 与客户端建立链接*/
void acceptconn(int lfd, int events, void *arg)
{
    struct sockaddr_in clieaddr;
    socklen_t clieaddrlen = sizeof(clieaddr);

    int cfd = Accept(lfd, (struct sockaddr*)&clieaddr, &clieaddrlen);

    int i;
    do{
        //从全局数组g_events中找一个空闲元素
        for(i=0; i<MAX_EVENTS; i++){
            if(g_events[i].status == 0){
                break;//得到空闲位置i后跳出for循环
            }
        }

        /*树上的节点数已满*/
        if(i == MAX_EVENTS){
            printf("%s:max connect limit[%d]\n", __func__, MAX_EVENTS);
            break;//跳出do{}while(0),相当于goto
        }

        /*修改cfd为非阻塞*/
        int flag = 0;
        if ((flag = fcntl(cfd, F_SETFL, O_NONBLOCK)) < 0) {
            printf("%s: fcntl nonblocking failed, %s\n", __func__, strerror(errno));
            break;
        }

        /* 给cfd设置一个 myevent_s 结构体, 回调函数 设置为 recvdata */
        eventset(&g_events[i], cfd, recvdata, &g_events[i]);
        eventadd(g_efd, EPOLLIN, &g_events[i]);

    }while(0);

    char str[INET_ADDRSTRLEN];
    printf("new connect [%s:%d][time:%ld], pos[%d]\n",
        inet_ntop(AF_INET, &clieaddr.sin_addr.s_addr, str, sizeof(str)),
        ntohs(clieaddr.sin_port),
        g_events[i].last_active, 
        i);

    return;
}

void recvdata(int fd, int events, void *arg)
{
    struct myevent_s *ev = (struct myevent_s *)arg;

    int len = recv(fd, ev->buf, sizeof(ev->buf), 0);
    
    /*将该节点从红黑树上摘除*/
    eventdel(g_efd, ev);

    if(len > 0){
        /*处理客户端数据*/
        ev->len = len;
        ev->buf[len] = '\0';
        printf("client[%d]:%s\n", fd, ev->buf);
        int i;
        for(i=0; i<len; i++){
            ev->buf[i] = toupper(ev->buf[i]);
        }

        /*重新设置cfd对应的回调函数和监听事件*/
        eventset(ev, fd, senddata, ev);
        eventadd(g_efd, EPOLLOUT, ev);
    }
    else if(len == 0){//对端关闭
        Close(ev->fd);

        /* ev-g_events 地址相减得到偏移元素位置 */
        printf("fd[%d] pos[%ld] ----> closed\n", fd, (ev - g_events));
    }
    else{
        Close(ev->fd);
        printf("recv from fd[%d] err[%d]:%s\n", fd, errno, strerror(errno));
    }

    return;
}

void senddata(int fd, int events, void *arg)
{
    struct myevent_s *ev = (struct myevent_s *)arg;

    int len = send(fd, ev->buf, ev->len, 0);

    if(len > 0){
        printf("send fd[%d] len[%d] buf:%s\n", fd, len, ev->buf);

        /*从红黑树g_efd中移除*/
        eventdel(g_efd, ev);
        /*将该fd的 回调函数改为 recvdata*/
        eventset(ev, fd, recvdata, ev);
        /*重新添加到红黑树上， 设为监听读事件*/
        eventadd(g_efd, EPOLLIN, ev);
    }
    else{
        printf("send fd[%d] err : %s\n", fd, strerror(errno));
        Close(ev->fd);
        eventdel(g_efd, ev);      
    }
    return;
}

int main(int argc, char *argv[])
{
    /*使用默认端口 或 命令行参数传入用户指定端口*/
    unsigned short port = SERV_PORT;
    if(argc == 2){
        port = atoi(argv[1]);
    }

    /*创建红黑树，返回给全局g_efd*/
    g_efd = epoll_create(MAX_EVENTS + 1);
    if(g_efd <= 0){
        printf("create efd in %s err : %s\n", __func__, strerror(errno));
        return 1;
    }

    /*初始化监听socket*/
    initlistensocket(g_efd, port);

    printf("server running : port[%d]\n", port);

    /*变量定义*/
    struct epoll_event events[MAX_EVENTS+1];//保存已经满足就绪事件的文件描述符数组
    int checkpos = 0, i;
    int nfd;

    /*死循环*/
    while(1){
        /*开始监听*/
        //将满足事件的文件描述符加入到events数组中，1秒钟没有事件满足，返回0       
        nfd = epoll_wait(g_efd, events, MAX_EVENTS+1, 1000);
        if(nfd < 0){
            printf("epoll_wait err, exit...\n");
            break;
        }

        /*轮询满足监听事件的文件描述符，循环nfd次*/
        for(i=0; i<nfd; i++){
            /*使用自定义结构体myevent_s类型指针, 接收 联合体data的void *ptr成员*/
            struct myevent_s *ev = (struct myevent_s *)events[i].data.ptr;

            if((events[i].events & EPOLLIN) && (ev->events & EPOLLIN)){
                ev->call_back(ev->fd, events[i].events, ev->arg);
            }
            if((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)){
                ev->call_back(ev->fd, events[i].events, ev->arg);
            }
        }
    }
    /* 退出前释放所有资源 */
    return 0;
}


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

#include "wrap.h"

#define MAX_EVENTS 1024
#define BUFLEN 4096
#define SERV_PORT 8888

void recvdata(int fd, int events, void *arg);
void senddata(int fd, int events, void *arg);
void initlistensocket(int efd, short port);

/* 描述就绪文件描述符相关信息 */
struct myevent_s {
    int fd; //要监听的文件描述符
    int events; //对应的监听事件
    
    void *arg;  //泛型参数
    void (*call_back)(int fd, int events, void *arg);   //回调函数
    
    int status; //是否在监听:1->在红黑树上(监听), 0->不在(不监听)
    char buf[BUFLEN];
    int len;    
    long last_active;   //记录每次加入红黑树 g_efd 的时间值
};

int g_efd;  //全局变量, 保存epoll_create返回的文件描述符
struct myevent_s g_events[MAX_EVENTS+1];    //自定义结构体类型数组. +1-->listen fd

/*将结构体 myevent_s 成员变量 初始化*/
void eventset(struct myevent_s *ev, int fd, void (*call_back)(int, int, void *), void *arg)
{
    ev->fd = fd;
    ev->call_back = call_back;
    ev->events = 0;
    ev->arg = arg;
    ev->status = 0;
    memset(ev->buf, 0, sizeof(ev->buf));
    ev->len = 0;
    ev->last_active = time(NULL);//调用eventset函数的时间
}

/* 向 epoll监听的红黑树 添加一个 文件描述符 */
void eventadd(int efd, int events, struct myevent_s *ev)
{
    struct epoll_event epv = {0, {0}};
    int op;
    epv.data.ptr = ev;
    epv.events = ev->events = events;

    if(ev->status == 1){//已经在红黑树 g_efd 里
        op = EPOLL_CTL_MOD;//修改其属性
    }
    else {//不在红黑树里
        op = EPOLL_CTL_ADD;//将其加入红黑树 g_efd, 并将status置1
        ev->status = 1;
    }

    if(epoll_ctl(efd, op, ev->fd, &epv) < 0){
        printf("event add failed [fd=%d], event[%d]\n", ev->fd, events);
    }
    else{
        printf("event add succeed [fd=%d], op=%d, event[%d]\n", ev->fd, op, events);
    }
    return;
}

int main(int argc, char *argv[])
{
    /* 使用用户指定端口.如未指定,用默认端口 */
    unsigned short port = SERV_PORT;
    if(argc == 2){
        port = atoi(argv[1]);                               
    }

    /* 创建红黑树,返回给全局 g_efd */
    g_efd = epoll_create(MAX_EVENTS+1);
    if(g_efd < 0){
        printf("create efd in %s err %s\n", __func__, strerror(errno));
    }

    /* 初始化监听socket */
    initlistensocket(g_efd, port);

    struct epoll_event events[MAX_EVENTS+1];
    int nfd;

    int i;
    while(1){

        /*监听红黑树g_efd, 
        将满足的事件的文件描述符加至events数组中, 
        1秒没有事件满足, 返回 0*/
        nfd = epoll_wait(g_efd, events, MAX_EVENTS+1, 1000);
        if(nfd < 0){
            printf("epoll_wait err -- exit...\n");
            break;
        }

        for(i=0; i<nfd; i++){
            struct myevent_s *ev = (struct myevent_s)events[i].data.ptr;

            if( (events[i].events & EPOLLIN) && (ev->events & EPOLLIN)){
                ev->call_back(ev->fd, events[i].events, ev->arg);
            }

            if( (events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)){
                ev->call_back(ev->fd, events[i].events, ev->arg);
            }
        }
    }
    return 0;
}


#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include "timerlib.h"

int count = 0;
void func(void *arg)
{
    printf("timerCallback=[%s]  cout=[%d]\n",(char *)arg, ++count);
    return;
}


int main()
{   
    // int timerid;
    int timerArray[5] = {0};

    /*当前线程屏蔽alarm信号*/
    sigset_t mask;
    sigemptyset(&mask);
    __sigaddset(&mask, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    /*初始化定时器*/
    if(0 != TimerInit()) return -1;
 
    /*增加定时器*/
    TimerAdd(3, 0, func, "timer 3s", &timerArray[0]);

    TimerAdd(10, 0, func, "timer 10s", &timerArray[1]);

    TimerAdd(5, 0, func, "timer 5s", &timerArray[2]);

    TimerPrint();
    while(1) {
        if (count == 3) {
            break;
        }
        usleep(10000);
    }
    TimerDestroy();

    return 0;
}
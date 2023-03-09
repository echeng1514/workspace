#ifndef __TIMERLIB_H__
#define __TIMERLIB_H__
/*
 * Initializes the timer library. It is important that the calling thread and
 * any other thread in the process block the signal SIGALRM.
 *
 * Return: 0 in case of success; -1 otherwise.
 */
int  TimerInit();

/*
*
*Timer Destory, free any data allocated for the library.
*/
void TimerDestroy();

/*
*add a Timer
*
*/
int  TimerAdd(long sec, 
            long usec, 
            void (*hndlr)(void*), 
            void* hndlr_arg, 
            int *id);

/*
*Remove from the queue the timer of identifier "id"
*/
void TimerRemove(int id, void (*free_arg)(void*));

/*
*Prints the content of the timer queue (to debug)
*/
void TimerPrint();
#endif /*__TIMERLIB_H__*/
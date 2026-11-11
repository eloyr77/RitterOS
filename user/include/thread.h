/*
 * Threads: several flows of one program that share its memory and files,
 * each with its own stack (8 MiB, its pages made when first touched: 54d)
 * and registers, the floating point ones included. Up to 256 per program;
 * number 0 is the one that runs main.
 * exit() from any thread ends the whole program; when the last thread ends
 * with thread_exit, the program ends with exit code 0.
 */
#ifndef RITTEROS_THREAD_H
#define RITTEROS_THREAD_H

/* Start fn(arg) in a new thread; its number, or -1 (errno: EAGAIN when all
 * 256 numbers are taken). */
int thread_create(int (*fn)(void *arg), void *arg);

/* Wait until thread `number` has ended; its result (the value fn returned
 * or thread_exit was given) goes to *result. -1 (errno ESRCH) if there is
 * no such thread or it was joined already. */
int thread_join(int number, int *result);

/* End the calling thread with a result. */
__attribute__((noreturn)) void thread_exit(int result);

/* The calling thread's number. */
int thread_self(void);

#endif

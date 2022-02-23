/*
 * This file provides definitions for users of librmt. Private library
 * definitions are in rmtlib.h
 */

struct stat;

/* Remote tape protocol declarations.
 */
int rmtopen(const char* path, int oflag, ...);
int rmtclose(int fd);
int rmtfstat(int, struct stat *buf);
int rmtioctl(int fd, unsigned int request, void *arg);
int rmtread(int fd, char* buf, unsigned int nbyte);
int rmtwrite(int fd, char *buf, unsigned int nbyte);


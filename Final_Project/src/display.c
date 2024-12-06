#include "../include/display.h"
#include <stdio.h>

void display(const char *msg)
{
    printf("%s\n", msg);
}

void *send_position(void *arg);
void *receive_position(void *arg);

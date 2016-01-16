#include "user.h"

//
// Created by amirahmad on 1/16/16.
//
int main()
{
    printf(1,"Starting the child\n");
    int i;
    for (i = 0; i < 50; i++)
        printf(2, "Counter: %d\n", i);
    exit();
}
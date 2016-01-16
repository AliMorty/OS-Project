//
// Created by amirahmad on 1/13/16.
//

#include "user.h"

int main()
{
    int i;
    for (i = 0; i < 100; i++)
    {
        printf(2, "Counter: %d\n", i);
        if (i == 10) isvpcb();
    }
    exit();
}

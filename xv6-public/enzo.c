//
// Created by amirahmad on 1/13/16.
//

#include "user.h"
#include "fcntl.h"

int main()
{
    isvpcb("tmp",O_CREATE|O_RDWR);
    ildpcb("tmp",O_RDONLY);
    return 0;
}

//
// Created by amirahmad on 1/13/16.
//

#include "user.h"

int main()
{
    static char *child = "the_sample_process";
    char input[1][1] = {{""}};

    int pid = fork();
    if (pid < 0)
    {
        printf(1, "Error: Faild to fork.\n");
        return -1;
    }
    else if (pid == 0)//child
    {
        exec(child, (char **) input);
        printf(1, "Child process started.\n");
    }
    else//parent
    {
        isvpcb();
        wait();
    }
    exit();
}

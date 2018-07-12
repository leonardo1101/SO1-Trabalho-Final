#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{

    uint *p = (uint*)0;
    *p = 15;
    printf(1,"%x %x\n", p, *p);
    exit();
} 

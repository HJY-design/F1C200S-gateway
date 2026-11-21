#include <stdio.h>
#include <unistd.h>

int main(void)
{
    volatile float a = 1.25f;
    volatile float b = 2.5f;
    float result = a * b;
    printf("ARM_ARCH=%d\n", __ARM_ARCH);
#ifdef __SOFTFP__
    puts("SOFT_FLOAT=yes");
#else
    puts("SOFT_FLOAT=no");
    return 1;
#endif
    printf("FLOAT_RESULT=%.3f\n", (double)result);
    printf("POINTER_BITS=%zu\n", sizeof(void *) * 8);
    printf("UID=%ld\n", (long)getuid());
    return (__ARM_ARCH == 5 && result == 3.125f && sizeof(void *) == 4) ? 0 : 1;
}

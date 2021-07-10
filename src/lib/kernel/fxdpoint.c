#include "fxdpoint.h"

static const fxdpoint_t f = 1<<FRAC_BITS;

/* Converts n to a fixed point number with the same value */
fxdpoint_t
to_fxdpoint(int n)
{
    int64_t ans = n*f;
    CHECK_OVERFLOW(ans);
    return ans;
}

/* Rounds x to the nearest integer*/
int
to_int_round(fxdpoint_t x)
{
    int64_t ans;
    if(x > 0){
        ans = (((int64_t)x) + f/2)/f;
    }else{
        ans = (((int64_t)x) - f/2)/f;
    }
    CHECK_OVERFLOW(ans);
    return ans;
}

/* Truncate fractional part */
int
to_int_trunc(fxdpoint_t x)
{
    return x/f; //always towards zero, no overflow can occur
}

/* Addition x+y */
fxdpoint_t
add(fxdpoint_t x, fxdpoint_t y)
{
    int64_t ans = ((int64_t)x)+y;
    CHECK_OVERFLOW(ans);
    return ans;
}

/* Subtraction x-y */ 
fxdpoint_t
subtract(fxdpoint_t x, fxdpoint_t y)
{
    int64_t ans = ((int64_t)x)-y;
    CHECK_OVERFLOW(ans);
    return ans;
}

/* Multiplication x*y*/
fxdpoint_t
multiply(fxdpoint_t x, fxdpoint_t y)
{
    int64_t ans = ((int64_t)x)*y/f;
    CHECK_OVERFLOW(ans);
    return ans;
}

/* Division x/y */
fxdpoint_t
divide(fxdpoint_t x, fxdpoint_t y)
{
    int64_t ans = ((int64_t)x)*f/y;
    CHECK_OVERFLOW(ans);
    return ans;
}
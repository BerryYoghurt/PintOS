#ifndef __LIB_KERNEL_FXDPOINT_H
#define __LIB_KERNEL_FSDPOINT_H

/* Types and functions for fixed point arithmetic.
For now, implements 17.14 format, but could change if needed

Using 17.14: maximum positive is 131,071.9999
             maximum negative is -131,072
             minimum is about +-6.1e-5
I think I don't need to check overflows so frequently,
because load_avg is unlikely to be that big (130,000*60 = 7,800,000 threads!)
as for recent_cpu.. a VERY nice thread (nice=20) will need to stay 
in the ready queue for about 120 seconds (with a high load_avg) to overflow.. 

Using 18.13 will be slightly better (need about 500 seconds to overflow) but
reduce resolution. Might use if needed */

#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../debug.h"

#define FRAC_BITS 14 /* Must be less than 32 */

#define CHECK_OVERFLOW(ANSWER) ASSERT(ANSWER <= (int64_t)INT32_MAX \
&& ANSWER >= (int64_t)INT32_MIN)

typedef signed int fxdpoint_t;

fxdpoint_t to_fxdpoint(int);
int to_int_trunc(fxdpoint_t);
int to_int_round(fxdpoint_t);

fxdpoint_t add(fxdpoint_t, fxdpoint_t);
fxdpoint_t subtract(fxdpoint_t,fxdpoint_t);

fxdpoint_t multiply(fxdpoint_t, fxdpoint_t);
fxdpoint_t divide(fxdpoint_t,fxdpoint_t);

//void to_string(char*, fxdpoint_t);


#endif
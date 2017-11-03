#include <stdio.h>

#include "lib.h"

int fn_ij (int a, int b)
{
  printf ("fn_ij (%d, %d)\n", a, b);
  return a + b;
}

int fn (int a, int b, int c)
{
  return C (fn_ij, a, b) + c;
}

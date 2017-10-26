#include "lib.h"

int main ()
{
  struct s x = { 1 };
  return fn (A, B, x.a) + D (i, j, x.b, E);
}

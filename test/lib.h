#define A 0
#undef A
#define A 1

#ifdef X
#define B (A + A)
#else
#define B 0
#endif

#define C(f, a, b) f (a, b)
#define D(a, b, c, d) C (fn_ ## a ## b, c, d)

enum e
{
  E = 2
};

struct s
{
  int a;
  enum e b;
};

int fn (int, int, int);
int fn_ij (int, int);

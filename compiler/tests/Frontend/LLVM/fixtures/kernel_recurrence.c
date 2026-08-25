int kernel(int seed, unsigned n) {
  int sum = seed;
  unsigned i = 0;
  do {
    sum = sum + 1;
    ++i;
  } while (i < n);
  return sum;
}

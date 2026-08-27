int linear_recurrence_canonical(int seed, unsigned n) {
  int value = seed;
  for (unsigned i = 0; i < n; ++i)
    value += 1;
  return value;
}

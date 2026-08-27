void linear_recurrence(int seed, int* out, unsigned n) {
  int value = seed;
  for (unsigned i = 0; i < n; ++i) {
    value += 1;
    value += 1;
    value -= 1;
    *out = value;
  }
}

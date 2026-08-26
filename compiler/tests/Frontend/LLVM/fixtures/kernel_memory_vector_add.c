void memory_vector_add(const int *__restrict a, const int *__restrict b, int *__restrict c,
                       unsigned n) {
  unsigned i = 0;
  do {
    c[i] = a[i] + b[i];
    ++i;
  } while (i < n);
}

void linear_vector_add(const int* __restrict A, const int* __restrict B, int* __restrict C,
                       unsigned n) {
  for (unsigned i = 0; i < n; i = (i + 1) + (n - n))
    C[i] = A[i] + B[i];
}

int kernel(int x, unsigned n) {
    unsigned i = 0;
    int y;
    do {
        y = x + x;
        ++i;
    } while (i < n);
    return y;
}

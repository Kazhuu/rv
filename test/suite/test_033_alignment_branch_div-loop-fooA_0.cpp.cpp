extern "C" void
foo(float* A) {
  for (unsigned i = 0; i < 296298; ++i) {
    if (i < 9) {
      A[i] = 42.0;
    }
  }
}
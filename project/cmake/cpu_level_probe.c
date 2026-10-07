/* Build-time probe (CMakeLists.txt, FNC_CPU_LEVEL=auto): prints "v3" when the
   CPU doing the build has everything x86-64-v3 needs (AVX2, BMI1/2, FMA, F16C,
   LZCNT, MOVBE, OS-saved YMM state), else "v2". */
#include <stdio.h>
#include <intrin.h>
int main(void) {
  int r[4];
  __cpuid(r, 0);
  int max_leaf = r[0];
  __cpuid(r, 1);
  int ecx1 = r[2];
  int ok = ((ecx1 >> 27) & 1) && ((ecx1 >> 28) & 1) && ((ecx1 >> 12) & 1) && ((ecx1 >> 29) & 1) &&
           ((ecx1 >> 22) & 1);
  if (ok && ((_xgetbv(0) & 6) != 6)) ok = 0;
  int ebx7 = 0;
  if (max_leaf >= 7) { __cpuidex(r, 7, 0); ebx7 = r[1]; }
  ok = ok && ((ebx7 >> 5) & 1) && ((ebx7 >> 3) & 1) && ((ebx7 >> 8) & 1);
  __cpuid(r, 0x80000000);
  int ecx_ext = 0;
  if ((unsigned)r[0] >= 0x80000001u) { __cpuid(r, 0x80000001); ecx_ext = r[2]; }
  ok = ok && ((ecx_ext >> 5) & 1);
  printf("%s", ok ? "v3" : "v2");
  return 0;
}

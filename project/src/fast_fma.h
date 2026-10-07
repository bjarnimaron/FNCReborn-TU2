// Fast std::fma for the x86-64-v2 build (CPUs without FMA3: Sandy / Ivy Bridge
// and older). The recompiled code turns every PowerPC fmadds/fmsubs/fnmadds/
// fnmsubs/fmadd... into std::fma(double, double, double). Without FMA3 that is a
// call into the C runtime's software fma, ~30-50 ns each.
//
// Same result, bit for bit: when a and b are both exactly representable as
// floats (always the case for single-precision PowerPC operands), a * b is exact
// in double (24 + 24 bits <= 53, exponents far inside double range), so
// a * b + c has one rounding - exactly what fma does. Anything else (doubles,
// NaN) still goes to the runtime's fma. -ffp-contract=off keeps the multiply and
// add separate.
//
// Force-included into the generated sources of the v2 build only (see
// CMakeLists.txt).
#pragma once
#include <cmath>

inline double fnc_fast_fma(double a, double b, double c) {
  if (double(float(a)) == a && double(float(b)) == b) {
    return a * b + c;
  }
  return ::fma(a, b, c);
}
namespace std {
using ::fnc_fast_fma;
}
#define fma(a, b, c) fnc_fast_fma(a, b, c)

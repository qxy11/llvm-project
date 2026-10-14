#include <hip/hip_runtime.h>

#include <cstdlib>
#include <iostream>

#define HIP_CHECK(condition)                                                   \
  {                                                                            \
    const hipError_t error = condition;                                        \
    if (error != hipSuccess) {                                                 \
      std::cerr << "HIP error: " << hipGetErrorString(error) << std::endl;     \
      std::exit(EXIT_FAILURE);                                                 \
    }                                                                          \
  }

__global__ void register_flags_kernel(float *out) {
  // MODE bits 0-3: FP_ROUND.32 = PLUS_INF (1), FP_ROUND.64_16 = MINUS_INF (2).
  asm volatile("s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 4), 9");
  // TRAPSTS is written directly: on MI300X, v_rcp_f32(0) does not set the
  // sticky EXCP.DIV0 bit while its trap is disabled.
  // EXCP.INVALID (bit 0) and EXCP.DIV0 (bit 2), then EXCP_CYCLE = 5.
  asm volatile("s_setreg_imm32_b32 hwreg(HW_REG_TRAPSTS, 0, 3), 5");
  asm volatile("s_setreg_imm32_b32 hwreg(HW_REG_TRAPSTS, 16, 6), 5");
  out[threadIdx.x] = 1.0f; // GPU BREAKPOINT
}

int main() {
  float *out;
  HIP_CHECK(hipMalloc(&out, 64 * sizeof(float)));
  register_flags_kernel<<<1, 64>>>(out);
  HIP_CHECK(hipDeviceSynchronize());
  HIP_CHECK(hipFree(out));
  return 0;
}

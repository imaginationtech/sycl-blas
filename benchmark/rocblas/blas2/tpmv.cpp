/**************************************************************************
 *
 *  @license
 *  Copyright (C) Codeplay Software Limited
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  For your convenience, a copy of the License has been included in this
 *  repository.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  SYCL-BLAS: BLAS implementation using SYCL
 *
 *  @filename tpmv.cpp
 *
 **************************************************************************/

#include "../utils.hpp"

template <typename scalar_t>
std::string get_name(std::string uplo, std::string t, std::string diag, int n) {
  std::ostringstream str{};
  str << "BM_Tpmv<" << blas_benchmark::utils::get_type_name<scalar_t>() << ">/"
      << uplo << "/" << t << "/" << diag << "/" << n;
  return str.str();
}

template <typename scalar_t, typename... args_t>
static inline void rocblas_tpmv_f(args_t&&... args) {
  if constexpr (std::is_same_v<scalar_t, float>) {
    CHECK_ROCBLAS_STATUS(rocblas_stpmv(std::forward<args_t>(args)...));
  } else if constexpr (std::is_same_v<scalar_t, double>) {
    CHECK_ROCBLAS_STATUS(rocblas_dtpmv(std::forward<args_t>(args)...));
  }
  return;
}

template <typename scalar_t>
void run(benchmark::State& state, rocblas_handle& rb_handle, std::string uplo,
         std::string t, std::string diag, index_t n, bool* success) {
  // Standard test setup.
  const char* uplo_str = uplo.c_str();
  const char* t_str = t.c_str();
  const char* diag_str = diag.c_str();

  index_t xlen = n;
  index_t incX = 1;

  blas_benchmark::utils::init_level_2_counters<
      blas_benchmark::utils::Level2Op::tpmv, scalar_t>(state, "n", 0, 0, n);

  // Matrix options (rocBLAS)
  const rocblas_fill uplo_rb =
      uplo_str[0] == 'u' ? rocblas_fill_upper : rocblas_fill_lower;
  const rocblas_diagonal diag_rb =
      diag_str[0] == 'u' ? rocblas_diagonal_unit : rocblas_diagonal_non_unit;
  const rocblas_operation trans_rb =
      t_str[0] == 'n' ? rocblas_operation_none : rocblas_operation_transpose;

  // Data sizes
  const int m_size = n * n;  // Minimum required size
  const int v_size = 1 + (xlen - 1) * incX;

  // Input matrix/vector, output vector.
  std::vector<scalar_t> m_a =
      blas_benchmark::utils::random_data<scalar_t>(m_size);
  std::vector<scalar_t> v_x =
      blas_benchmark::utils::random_data<scalar_t>(v_size);

  {
    // Device memory allocation & H2D copy
    blas_benchmark::utils::HIPVector<scalar_t> m_a_gpu(m_size, m_a.data());
    blas_benchmark::utils::HIPVector<scalar_t> v_x_gpu(v_size, v_x.data());

#ifdef BLAS_VERIFY_BENCHMARK
    // Reference tbmv
    std::vector<scalar_t> v_x_ref = v_x;
    reference_blas::tpmv(uplo_str, t_str, diag_str, n, m_a.data(),
                         v_x_ref.data(), incX);

    // Rocblas verification tbmv
    std::vector<scalar_t> v_x_temp = v_x;
    {
      // Temp result on device (copied back to Host upon destruction)
      blas_benchmark::utils::HIPVector<scalar_t, true> v_x_temp_gpu(
          xlen * incX, v_x_temp.data());
      // rocBLAS function call
      rocblas_tpmv_f<scalar_t>(rb_handle, uplo_rb, trans_rb, diag_rb, n,
                               m_a_gpu, v_x_temp_gpu, incX);
    }

    std::ostringstream err_stream;
    if (!utils::compare_vectors(v_x_temp, v_x_ref, err_stream, "")) {
      const std::string& err_str = err_stream.str();
      state.SkipWithError(err_str.c_str());
      *success = false;
    };
#endif

    auto blas_warmup = [&]() -> void {
      rocblas_tpmv_f<scalar_t>(rb_handle, uplo_rb, trans_rb, diag_rb, n,
                               m_a_gpu, v_x_gpu, incX);
      return;
    };

    hipEvent_t start, stop;
    CHECK_HIP_ERROR(hipEventCreate(&start));
    CHECK_HIP_ERROR(hipEventCreate(&stop));

    auto blas_method_def = [&]() -> std::vector<hipEvent_t> {
      CHECK_HIP_ERROR(hipEventRecord(start, NULL));
      rocblas_tpmv_f<scalar_t>(rb_handle, uplo_rb, trans_rb, diag_rb, n,
                               m_a_gpu, v_x_gpu, incX);
      CHECK_HIP_ERROR(hipEventRecord(stop, NULL));
      CHECK_HIP_ERROR(hipEventSynchronize(stop));
      return std::vector{start, stop};
    };

    // Warmup
    blas_benchmark::utils::warmup(blas_warmup);
    CHECK_HIP_ERROR(hipStreamSynchronize(NULL));

    blas_benchmark::utils::init_counters(state);

    // Measure
    for (auto _ : state) {
      // Run
      std::tuple<double, double> times =
          blas_benchmark::utils::timef_hip(blas_method_def);

      // Report
      blas_benchmark::utils::update_counters(state, times);
    }

    state.SetItemsProcessed(state.iterations() * state.counters["n_fl_ops"]);
    state.SetBytesProcessed(state.iterations() *
                            state.counters["bytes_processed"]);

    blas_benchmark::utils::calc_avg_counters(state);

    CHECK_HIP_ERROR(hipEventDestroy(start));
    CHECK_HIP_ERROR(hipEventDestroy(stop));
  }  // release device memory via utils::DeviceVector destructors
}

template <typename scalar_t>
void register_benchmark(blas_benchmark::Args& args, rocblas_handle& rb_handle,
                        bool* success) {
  auto tpmv_params = blas_benchmark::utils::get_trsv_params(args);

  for (auto p : tpmv_params) {
    std::string uplo;
    std::string ts;
    std::string diags;
    index_t n;
    std::tie(uplo, ts, diags, n) = p;

    auto BM_lambda = [&](benchmark::State& st, rocblas_handle rb_handle,
                         std::string uplo, std::string ts, std::string diags,
                         index_t n, bool* success) {
      run<scalar_t>(st, rb_handle, uplo, ts, diags, n, success);
    };
    benchmark::RegisterBenchmark(get_name<scalar_t>(uplo, ts, diags, n).c_str(),
                                 BM_lambda, rb_handle, uplo, ts, diags, n,
                                 success)
        ->UseRealTime();
  }
}

namespace blas_benchmark {
void create_benchmark(blas_benchmark::Args& args, rocblas_handle& rb_handle,
                      bool* success) {
  BLAS_REGISTER_BENCHMARK(args, rb_handle, success);
}
}  // namespace blas_benchmark

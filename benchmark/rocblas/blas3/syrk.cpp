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
 *  @filename syrk.cpp
 *
 **************************************************************************/

#include "../utils.hpp"

template <typename scalar_t>
std::string get_name(char uplo, char trans, int n, int k, scalar_t alpha,
                     scalar_t beta) {
  std::ostringstream str{};
  str << "BM_Syrk<" << blas_benchmark::utils::get_type_name<scalar_t>() << ">/"
      << uplo << "/" << trans << "/" << n << "/" << k << "/" << alpha << "/"
      << beta;
  return str.str();
}

template <typename scalar_t, typename... args_t>
static inline void rocblas_syrk_f(args_t&&... args) {
  if constexpr (std::is_same_v<scalar_t, float>) {
    CHECK_ROCBLAS_STATUS(rocblas_ssyrk(std::forward<args_t>(args)...));
  } else if constexpr (std::is_same_v<scalar_t, double>) {
    CHECK_ROCBLAS_STATUS(rocblas_dsyrk(std::forward<args_t>(args)...));
  }
  return;
}

template <typename scalar_t>
void run(benchmark::State& state, rocblas_handle& rb_handle, char uplo,
         char trans, index_t n, index_t k, scalar_t alpha, scalar_t beta,
         bool* success) {
  // Standard test setup.
  index_t lda = (trans == 'n') ? n : k;
  index_t ldc = n;

  blas_benchmark::utils::init_level_3_counters<
      blas_benchmark::utils::Level3Op::syrk, scalar_t>(state, beta, 0, n, k);

  // Matrix options (rocBLAS)
  const rocblas_fill uplo_rb =
      (uplo == 'u') ? rocblas_fill_upper : rocblas_fill_lower;
  const rocblas_operation trans_rb =
      (trans == 'n') ? rocblas_operation_none : rocblas_operation_transpose;

  // Data sizes
  const index_t a_size = (trans == 'n') ? (lda * k) : (lda * n);
  const index_t c_size = ldc * n;

  // Matrices
  std::vector<scalar_t> a =
      blas_benchmark::utils::random_data<scalar_t>(a_size);
  std::vector<scalar_t> c =
      blas_benchmark::utils::random_data<scalar_t>(c_size);

  {
    // Device memory allocation & H2D copy
    blas_benchmark::utils::HIPVector<scalar_t> a_gpu(a_size, a.data());
    blas_benchmark::utils::HIPVector<scalar_t> c_gpu(c_size, c.data());

#ifdef BLAS_VERIFY_BENCHMARK
    // Reference syrk
    std::vector<scalar_t> c_ref = c;
    reference_blas::syrk(&uplo, &trans, n, k, alpha, a.data(), lda, beta,
                         c_ref.data(), ldc);

    // Rocblas verification syrk
    std::vector<scalar_t> c_temp = c;
    {
      blas_benchmark::utils::HIPVector<scalar_t, true> c_temp_gpu(
          c_size, c_temp.data());
      rocblas_syrk_f<scalar_t>(rb_handle, uplo_rb, trans_rb, n, k, &alpha,
                               a_gpu, lda, &beta, c_temp_gpu, ldc);
    }

    std::ostringstream err_stream;
    if (!utils::compare_vectors(c_temp, c_ref, err_stream, "")) {
      const std::string& err_str = err_stream.str();
      state.SkipWithError(err_str.c_str());
      *success = false;
    };
#endif

    auto blas_warmup = [&]() -> void {
      rocblas_syrk_f<scalar_t>(rb_handle, uplo_rb, trans_rb, n, k, &alpha,
                               a_gpu, lda, &beta, c_gpu, ldc);
      return;
    };

    hipEvent_t start, stop;
    CHECK_HIP_ERROR(hipEventCreate(&start));
    CHECK_HIP_ERROR(hipEventCreate(&stop));

    auto blas_method_def = [&]() -> std::vector<hipEvent_t> {
      CHECK_HIP_ERROR(hipEventRecord(start, NULL));
      rocblas_syrk_f<scalar_t>(rb_handle, uplo_rb, trans_rb, n, k, &alpha,
                               a_gpu, lda, &beta, c_gpu, ldc);
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

    state.SetBytesProcessed(state.iterations() *
                            state.counters["bytes_processed"]);
    state.SetItemsProcessed(state.iterations() * state.counters["n_fl_ops"]);

    blas_benchmark::utils::calc_avg_counters(state);

    CHECK_HIP_ERROR(hipEventDestroy(start));
    CHECK_HIP_ERROR(hipEventDestroy(stop));
  }  // release device memory via utils::DeviceVector destructors
};

template <typename scalar_t>
void register_benchmark(blas_benchmark::Args& args, rocblas_handle& rb_handle,
                        bool* success) {
  auto syrk_params = blas_benchmark::utils::get_syrk_params<scalar_t>(args);

  for (auto p : syrk_params) {
    char uplo, trans;
    index_t n, k;
    scalar_t alpha, beta;
    std::tie(uplo, trans, n, k, alpha, beta) = p;

    auto BM_lambda = [&](benchmark::State& st, rocblas_handle rb_handle,
                         char uplo, char trans, index_t n, index_t k,
                         scalar_t alpha, scalar_t beta, bool* success) {
      run<scalar_t>(st, rb_handle, uplo, trans, n, k, alpha, beta, success);
    };
    benchmark::RegisterBenchmark(
        get_name<scalar_t>(uplo, trans, n, k, alpha, beta).c_str(), BM_lambda,
        rb_handle, uplo, trans, n, k, alpha, beta, success)
        ->UseRealTime();
  }
}

namespace blas_benchmark {
void create_benchmark(blas_benchmark::Args& args, rocblas_handle& rb_handle,
                      bool* success) {
  BLAS_REGISTER_BENCHMARK(args, rb_handle, success);
}
}  // namespace blas_benchmark

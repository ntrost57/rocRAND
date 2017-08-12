// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <numeric>
#include <utility>
#include <type_traits>
#include <algorithm>

#include <boost/program_options.hpp>

#include <hip/hip_runtime.h>
#include <rocrand.h>
#include <rocrand_kernel.h>
#include <rocrand_mtgp32_11213.h>
#include <rocrand_sobol_precomputed.h>

#include "stat_test_common.hpp"

extern "C" {
#include "gofs.h"
#include "fdist.h"
#include "fbar.h"
#include "finv.h"
}

#define HIP_CHECK(condition)         \
  {                                  \
    hipError_t error = condition;    \
    if(error != hipSuccess){         \
        std::cout << "HIP error: " << error << " line: " << __LINE__ << std::endl; \
        exit(error); \
    } \
  }

#define ROCRAND_CHECK(condition)                 \
  {                                              \
    rocrand_status status = condition;           \
    if(status != ROCRAND_STATUS_SUCCESS) {       \
        std::cout << "ROCRAND error: " << status << " line: " << __LINE__ << std::endl; \
        exit(status); \
    } \
  }

template<typename GeneratorState>
__global__
void init_kernel(GeneratorState * states,
                 const unsigned long long seed,
                 const unsigned long long offset)
{
    const unsigned int state_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
    GeneratorState state;
    rocrand_init(seed, state_id, offset, &state);
    states[state_id] = state;
}

template<typename GeneratorState>
void initialize(const size_t dimensions,
                const size_t blocks,
                const size_t threads,
                GeneratorState * states,
                const unsigned long long seed,
                const unsigned long long offset)
{
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(init_kernel),
        dim3(blocks), dim3(threads), 0, 0,
        states, seed, offset
    );
}

template<typename GeneratorState, typename Directions>
__global__
void init_kernel_sobol(GeneratorState * states,
                       const Directions directions,
                       const unsigned long long offset)
{
    const unsigned int dimension = hipBlockIdx_y;
    const unsigned int state_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
    GeneratorState state;
    rocrand_init(&directions[dimension * 32], offset + state_id, &state);
    states[hipGridDim_x * hipBlockDim_x * dimension + state_id] = state;
}

size_t next_power2(size_t x)
{
    size_t power = 1;
    while (power < x)
    {
        power *= 2;
    }
    return power;
}

template<>
void initialize(const size_t dimensions,
                const size_t blocks,
                const size_t threads,
                rocrand_state_sobol32 * states,
                const unsigned long long seed,
                const unsigned long long offset)
{
    unsigned int * directions;
    const size_t size = dimensions * 32 * sizeof(unsigned int);
    HIP_CHECK(hipMalloc((void **)&directions, size));
    HIP_CHECK(hipMemcpy(directions, h_sobol32_direction_vectors, size, hipMemcpyHostToDevice));

    const size_t blocks_x = next_power2((blocks + dimensions - 1) / dimensions);
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(init_kernel_sobol),
        dim3(blocks_x, dimensions), dim3(threads), 0, 0,
        states, directions, offset
    );

    HIP_CHECK(hipPeekAtLastError());
    HIP_CHECK(hipDeviceSynchronize());

    HIP_CHECK(hipFree(directions));
}

template<>
void initialize(const size_t dimensions,
                const size_t blocks,
                const size_t threads,
                rocrand_state_mtgp32 * states,
                const unsigned long long seed,
                const unsigned long long offset)
{
    ROCRAND_CHECK(rocrand_make_state_mtgp32(states, mtgp32dc_params_fast_11213, blocks, seed));
}

template<typename T, typename GeneratorState, typename GenerateFunc, typename Extra>
__global__
void generate_kernel(GeneratorState * states,
                     T * data,
                     const size_t size,
                     const GenerateFunc& generate_func,
                     const Extra extra)
{
    const unsigned int state_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
    const unsigned int stride = hipGridDim_x * hipBlockDim_x;

    GeneratorState state = states[state_id];
    unsigned int index = state_id;
    while(index < size)
    {
        data[index] = generate_func(&state, extra);
        index += stride;
    }
    states[state_id] = state;
}

template<typename T, typename GeneratorState, typename GenerateFunc, typename Extra>
void generate(const size_t dimensions,
              const size_t blocks,
              const size_t threads,
              GeneratorState * states,
              T * data,
              const size_t size,
              const GenerateFunc& generate_func,
              const Extra extra)
{
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(generate_kernel),
        dim3(blocks), dim3(threads), 0, 0,
        states, data, size, generate_func, extra
    );
}

template<typename T, typename GenerateFunc, typename Extra>
__global__
void generate_kernel(rocrand_state_sobol32 * states,
                     T * data,
                     const size_t size,
                     const GenerateFunc& generate_func,
                     const Extra extra)
{
    const unsigned int dimension = hipBlockIdx_y;
    const unsigned int state_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
    const unsigned int stride = hipGridDim_x * hipBlockDim_x;

    rocrand_state_sobol32 state = states[hipGridDim_x * hipBlockDim_x * dimension + state_id];
    const unsigned int offset = dimension * size;
    unsigned int index = state_id;
    while(index < size)
    {
        data[offset + index] = generate_func(&state, extra);
        skipahead(stride - 1, &state);
        index += stride;
    }
    state = states[hipGridDim_x * hipBlockDim_x * dimension + state_id];
    skipahead(static_cast<unsigned int>(size), &state);
    states[hipGridDim_x * hipBlockDim_x * dimension + state_id] = state;
}

template<typename T, typename GenerateFunc, typename Extra>
void generate(const size_t dimensions,
              const size_t blocks,
              const size_t threads,
              rocrand_state_sobol32 * states,
              T * data,
              const size_t size,
              const GenerateFunc& generate_func,
              const Extra extra)
{
    const size_t blocks_x = next_power2((blocks + dimensions - 1) / dimensions);
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(generate_kernel),
        dim3(blocks_x, dimensions), dim3(threads), 0, 0,
        states, data, size / dimensions, generate_func, extra
    );
}

template<typename T, typename GenerateFunc, typename Extra>
__global__
void generate_kernel(rocrand_state_mtgp32 * states,
                     T * data,
                     const size_t size,
                     const GenerateFunc& generate_func,
                     const Extra extra)
{
    const unsigned int state_id = hipBlockIdx_x;
    const unsigned int thread_id = hipThreadIdx_x;
    unsigned int index = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
    unsigned int stride = hipGridDim_x * hipBlockDim_x;
        
    __shared__ rocrand_state_mtgp32 state;
       
    if (thread_id == 0)
        state = states[state_id];
    __syncthreads();
        
    while(index < size)
    {
        data[index] = generate_func(&state, extra);
        index += stride;
    }
    __syncthreads();
        
    if (thread_id == 0)
        states[state_id] = state;
}

template<typename T, typename GenerateFunc, typename Extra>
void generate(const size_t dimensions,
              const size_t blocks,
              const size_t threads,
              rocrand_state_mtgp32 * states,
              T * data,
              const size_t size,
              const GenerateFunc& generate_func,
              const Extra extra)
{
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(generate_kernel),
        dim3(blocks), dim3(256), 0, 0,
        states, data, size, generate_func, extra
    );
}

template<typename T, typename GeneratorState, typename GenerateFunc, typename Extra>
void run_test(const boost::program_options::variables_map& vm,
              const std::string plot_name,
              const GenerateFunc& generate_func,
              const Extra extra,
              const double mean, const double stddev,
              const distribution_func_type& distribution_func)
{
    const size_t size = vm["size"].as<size_t>();
    const size_t level1_tests = vm["level1-tests"].as<size_t>();
    const size_t level2_tests = vm["level2-tests"].as<size_t>();
    const bool save_plots = vm.count("plots");

    const size_t blocks = vm["blocks"].as<size_t>();
    const size_t threads = vm["threads"].as<size_t>();

    const size_t dimensions = level1_tests;

    T * data;
    HIP_CHECK(hipMalloc((void **)&data, size * level1_tests * sizeof(T)));

    const size_t states_size = blocks * threads * dimensions;
    GeneratorState * states;
    HIP_CHECK(hipMalloc((void **)&states, states_size * sizeof(GeneratorState)));

    initialize(dimensions, blocks, threads, states, 0, 0);
    HIP_CHECK(hipPeekAtLastError());
    HIP_CHECK(hipDeviceSynchronize());

    for (size_t level2_test = 0; level2_test < level2_tests; level2_test++)
    {
        generate(dimensions, blocks, threads, states, data, size * level1_tests, generate_func, extra);
        HIP_CHECK(hipPeekAtLastError());
        HIP_CHECK(hipDeviceSynchronize());

        std::vector<T> h_data(size * level1_tests);
        HIP_CHECK(hipMemcpy(h_data.data(), data, size * level1_tests * sizeof(T), hipMemcpyDeviceToHost));

        analyze(size, level1_tests, h_data.data(),
                save_plots, plot_name + "-" + std::to_string(level2_test),
                mean, stddev, distribution_func);
    }

    HIP_CHECK(hipFree(states));
    HIP_CHECK(hipFree(data));
}

template<typename GeneratorState>
void run_tests(const boost::program_options::variables_map& vm,
               const std::string& distribution,
               const std::string plot_name)
{
    if (distribution == "uniform-float")
    {
        run_test<float, GeneratorState>(vm, plot_name,
            [] __device__ (GeneratorState * state, int) {
                return rocrand_uniform(state);
            }, 0,
            0.5, std::sqrt(1.0 / 12.0),
            [](double x) { return fdist_Unif(x); }
        );
    }
    if (distribution == "uniform-double")
    {
        run_test<double, GeneratorState>(vm, plot_name,
            [] __device__ (GeneratorState * state, int) {
                return rocrand_uniform_double(state);
            }, 0,
            0.5, std::sqrt(1.0 / 12.0),
            [](double x) { return fdist_Unif(x); }
        );
    }
    if (distribution == "normal-float")
    {
        run_test<float, GeneratorState>(vm, plot_name,
            [] __device__ (GeneratorState * state, int) {
                return rocrand_normal(state);
            }, 0,
            0.0, 1.0,
            [](double x) { return fdist_Normal2(x); }
        );
    }
    if (distribution == "normal-double")
    {
        run_test<double, GeneratorState>(vm, plot_name,
            [] __device__ (GeneratorState * state, int) {
                return rocrand_normal_double(state);
            }, 0,
            0.0, 1.0,
            [](double x) { return fdist_Normal2(x); }
        );
    }
    if (distribution == "log-normal-float")
    {
        run_test<float, GeneratorState>(vm, plot_name,
            [] __device__ (GeneratorState * state, int) {
                return rocrand_log_normal(state, 0.0f, 1.0f);
            }, 0,
            std::exp(0.5), std::sqrt((std::exp(1.0) - 1.0) * std::exp(1.0)),
            [](double x) { return fdist_LogNormal(0.0, 1.0, x); }
        );
    }
    if (distribution == "log-normal-double")
    {
        run_test<double, GeneratorState>(vm, plot_name,
            [] __device__ (GeneratorState * state, int) {
                return rocrand_log_normal_double(state, 0.0, 1.0);
            }, 0,
            std::exp(0.5), std::sqrt((std::exp(1.0) - 1.0) * std::exp(1.0)),
            [](double x) { return fdist_LogNormal(0.0, 1.0, x); }
        );
    }
    if (distribution == "poisson")
    {
        const auto lambdas = vm["lambda"].as<std::vector<double>>();
        for (double lambda : lambdas)
        {
            std::cout << "    " << "lambda "
                 << std::fixed << std::setprecision(1) << lambda << std::endl;
            run_test<unsigned int, GeneratorState>(vm, plot_name + "-" + std::to_string(lambda),
                [] __device__ (GeneratorState * state, double lambda) {
                    return rocrand_poisson(state, lambda);
                }, lambda,
                lambda, std::sqrt(lambda),
                [lambda](double x) { return fdist_Poisson1(lambda, static_cast<long>(std::round(x)) - 1); }
            );
        }
    }
    if (distribution == "discrete-poisson")
    {
        const auto lambdas = vm["lambda"].as<std::vector<double>>();
        for (double lambda : lambdas)
        {
            std::cout << "    " << "lambda "
                 << std::fixed << std::setprecision(1) << lambda << std::endl;
            rocrand_discrete_distribution discrete_distribution;
            ROCRAND_CHECK(rocrand_create_poisson_distribution(lambda, &discrete_distribution));
            run_test<unsigned int, GeneratorState>(vm, plot_name + "-" + std::to_string(lambda),
                [] __device__ (GeneratorState * state, rocrand_discrete_distribution discrete_distribution) {
                    return rocrand_discrete(state, discrete_distribution);
                }, discrete_distribution,
                lambda, std::sqrt(lambda),
                [lambda](double x) { return fdist_Poisson1(lambda, static_cast<long>(std::round(x)) - 1); }
            );
            ROCRAND_CHECK(rocrand_destroy_discrete_distribution(discrete_distribution));
        }
    }
    if (distribution == "discrete-custom")
    {
        const unsigned int offset = 123;
        std::vector<double> probabilities = { 10, 10, 1, 120, 8, 6, 140, 2, 150, 150, 10, 80 };
        const int size = probabilities.size();
        double sum = 0.0;
        for (int i = 0; i < size; i++)
        {
            sum += probabilities[i];
        }
        for (int i = 0; i < size; i++)
        {
            probabilities[i] /= sum;
        }
        std::vector<double> cdf(size);
        for (int i = 0; i < size; i++)
        {
            cdf[i] = (i == 0 ? 0.0 : cdf[i - 1]) + probabilities[i];
        }

        double mean = 0.0;
        for (int i = 0; i < size; i++)
        {
            mean += probabilities[i] * i;
        }

        double stddev = 0.0;
        for (int i = 0; i < size; i++)
        {
            const double d = i - mean;
            stddev += d * d * probabilities[i];
        }
        stddev = std::sqrt(stddev);

        rocrand_discrete_distribution discrete_distribution;
        ROCRAND_CHECK(rocrand_create_discrete_distribution(probabilities.data(), probabilities.size(), offset, &discrete_distribution));
        run_test<unsigned int, GeneratorState>(vm, plot_name,
            [] __device__ (GeneratorState * state, rocrand_discrete_distribution discrete_distribution) {
                return rocrand_discrete(state, discrete_distribution);
            }, discrete_distribution,
            offset + mean, stddev,
            [size, &cdf](double x) {
                const int i = static_cast<int>(std::round(x)) - offset - 1;
                if (i < 0)
                    return 0.0;
                else
                    return cdf[std::min(size - 1, i)];
            }
        );
        ROCRAND_CHECK(rocrand_destroy_discrete_distribution(discrete_distribution));
    }
}

const std::vector<std::string> all_engines = {
    "xorwow",
    "mrg32k3a",
    "mtgp32",
    // "mt19937",
    "philox",
    "sobol32",
    // "scrambled_sobol32",
    // "sobol64",
    // "scrambled_sobol64",
};

const std::vector<std::string> all_distributions = {
    "uniform-float",
    "uniform-double",
    "normal-float",
    "normal-double",
    "log-normal-float",
    "log-normal-double",
    "poisson",
    "discrete-poisson",
    "discrete-custom",
};

int main(int argc, char *argv[])
{
    namespace po = boost::program_options;
    po::options_description options("options");

    const std::string distribution_desc =
        "space-separated list of distributions:" +
        std::accumulate(all_distributions.begin(), all_distributions.end(), std::string(),
            [](std::string a, std::string b) {
                return a + "\n   " + b;
            }
        ) +
        "\nor all";
    const std::string engine_desc =
        "space-separated list of random number engines:" +
        std::accumulate(all_engines.begin(), all_engines.end(), std::string(),
            [](std::string a, std::string b) {
                return a + "\n   " + b;
            }
        ) +
        "\nor all";
    options.add_options()
        ("help", "show usage instructions")
        ("size", po::value<size_t>()->default_value(10000), "number of samples in every first level test")
        ("level1-tests", po::value<size_t>()->default_value(10), "number of first level tests")
        ("level2-tests", po::value<size_t>()->default_value(10), "number of second level tests")
        ("blocks", po::value<size_t>()->default_value(64), "number of blocks")
        ("threads", po::value<size_t>()->default_value(256), "number of threads in each block")
        ("dis", po::value<std::vector<std::string>>()->multitoken()->default_value({ "all" }, "all"),
            distribution_desc.c_str())
        ("engine", po::value<std::vector<std::string>>()->multitoken()->default_value({ "philox" }, "philox"),
            engine_desc.c_str())
        ("lambda", po::value<std::vector<double>>()->multitoken()->default_value({ 100.0 }, "100.0"),
            "space-separated list of lambdas of Poisson distribution")
        ("plots", "save plots for GnuPlot")
    ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, options), vm);
    po::notify(vm);

    if(vm.count("help"))
    {
        std::cout << options << std::endl;
        return 0;
    }

    std::vector<std::string> engines;
    {
        auto es = vm["engine"].as<std::vector<std::string>>();
        if (std::find(es.begin(), es.end(), "all") != es.end())
        {
            engines = all_engines;
        }
        else
        {
            for (auto e : all_engines)
            {
                if (std::find(es.begin(), es.end(), e) != es.end())
                    engines.push_back(e);
            }
        }
    }

    std::vector<std::string> distributions;
    {
        auto ds = vm["dis"].as<std::vector<std::string>>();
        if (std::find(ds.begin(), ds.end(), "all") != ds.end())
        {
            distributions = all_distributions;
        }
        else
        {
            for (auto d : all_distributions)
            {
                if (std::find(ds.begin(), ds.end(), d) != ds.end())
                    distributions.push_back(d);
            }
        }
    }

    std::cout << "rocRAND:" << std::endl << std::endl;
    for (auto engine : engines)
    {
        std::cout << engine << ":" << std::endl;
        for (auto distribution : distributions)
        {
            std::cout << "  " << distribution << ":" << std::endl;
            const std::string plot_name = engine + "-" + distribution;
            if (engine == "xorwow")
            {
                run_tests<rocrand_state_xorwow>(vm, distribution, plot_name);
            }
            else if (engine == "mrg32k3a")
            {
                run_tests<rocrand_state_mrg32k3a>(vm, distribution, plot_name);
            }
            else if (engine == "philox")
            {
                run_tests<rocrand_state_philox4x32_10>(vm, distribution, plot_name);
            }
            else if (engine == "sobol32")
            {
                run_tests<rocrand_state_sobol32>(vm, distribution, plot_name);
            }
            else if (engine == "mtgp32")
            {
                run_tests<rocrand_state_mtgp32>(vm, distribution, plot_name);
            }
        }
    }

    return 0;
}
/*
 * gemm_opt.cpp
 *
 *  Created on: 2017/10/19
 *      Author: ZhangHua
 */

#include <iostream>
#include <csignal>

#ifdef CUBLAS_ENABLE
#undef __cdecl
#include <cuda_runtime.h>
#include <cublas_v2.h>
#endif
#include <clblast.h>
#include <clBLAS.h>
#ifdef MIOPENGEMM_ENABLE
#include <miopengemm/gemm.hpp>
#endif
#ifdef MKL_ENABLE
#include <mkl.h>
#endif

#include <tensor.hpp>
#include <device_instance.hpp>

using namespace std;
using namespace clnet;
using namespace clblast;

float* bufA = 0;
float* bufB = 0;
float* bufC = 0;
float alpha = 0;
float beta = 0;
#ifdef CUBLAS_ENABLE
cublasHandle_t handle;
#endif

T gemm_opt()
{
	T initializer = XavierNormalDistributionInitializer({}, 0, 2.34f);
	int M = optional<int>("M", 2048); //dim_hidden
	int N = optional<int>("N", 512); //batch_size
	int K = optional<int>("K", 2048); //dim_in
	int STEP = optional<int>("step", 4);

	double REPEAT = optional<double>("repeat", 1.0);
	alpha = optional<double>("alpha", 1.0);
	beta = optional<double>("beta", 0);

	bool parallel = optional<int>("parallel", false);
	bool cublas = optional<int>("cublas", false);
	bool clblast = optional<int>("clblast", false);
	bool clBLAS = optional<int>("clblas", false);
	bool MIOpen = optional<int>("miopen", false);
	bool MKL = optional<int>("mkl", false);
	bool verify = optional<int>("verify", false);
	logger << "Compared with ";
	if (clblast)
		logger << "clblast";
	else if (cublas)
		logger << "cublas";
	else if (clBLAS)
		logger << "clBLAS";
	else if (MIOpen)
		logger << "MIOpenGemm";
	else if (MKL)
		logger << "Intel MKL";
	else
		logger << "revised clNET kernel";
	logger << ", Verification " << (verify? "enabled" : "disabled");
	logger << ", Parallel " << (parallel? "enabled" : "disabled");
	logger << endl;

	T w = Weight({M, K}, "w", &initializer);
	T x = Data({N, K}, &initializer, "x");
	T result = Data({M, N}, nullptr, "gemm");

	T graph = *new InstantTensor("gemm_opt", {&x, &w},
		[M, N, K, STEP, REPEAT, parallel, &result, &initializer, cublas, clBLAS, MIOpen, MKL, verify](InstantTensor* self, DeviceInstance& I) {
		auto& kernel = prepare_for_running_kernel(self, I);
		T x = *self->inputs[0];
		T w = *self->inputs[1];
		kernel.setArg(0, I.buffers[&result]);
		kernel.setArg(1, I.buffers[&x]);
		kernel.setArg(2, I.buffers[&w]);
		kernel.setArg(3, nullptr);

#ifdef CUBLAS_ENABLE
		extern void prepare_cublas(float* A, float* B, float* C, int K, int M, int N);
		if (cublas)
			prepare_cublas(w.pointer, x.pointer, result.pointer, K, M, N);
#endif
		if (clBLAS) {
			auto err = clblasSetup();
			if (err != CL_SUCCESS)
				throw runtime_error("clBLAS error: " + to_string((int) err));
		}

		int i = 0;
		for (int m = M; m >= 32; m /= STEP)
			for (int n = N; n >= 32; n /= STEP)
				for (int  k = K; k >= 32; k /= STEP) {
					int64 total = (sqrt(REPEAT * M * N * K / m / n / k) - 0.8) * 50;
					logger << "M=" << m << " \tN=" << n << " \tK=" << k << " \ttimes=" << total << flush;
					//warm up
					kernel.setArg(4, m);
					kernel.setArg(5, k);
					cl::NDRange global(m * n);
					I.queue.enqueueNDRangeKernel(kernel, cl::NullRange, global, cl::NullRange, &I.precondition_events, &I.events[&result]);
					wait_for_all_kernels_finished(I);

					//timing the standard version now......
					size_t time = MICROS(0);
					for (int j = 0; j < total; j++) {
						I.queue.enqueueNDRangeKernel(kernel, cl::NullRange, global, cl::NullRange, &I.precondition_events, &I.events[&result]);
						if (!parallel)
							wait_for_all_kernels_finished(I);
					}
					if (parallel)
						wait_for_all_kernels_finished(I);
					time = MICROS(time);
					//end timing-----------------------------

					float baseline = time / total / 1000.0f;
					if (verify) {
						result.upload(I);
						memcpy(result.pointer, I.pointers[&result], m * n * sizeof(float));
					}
					logger << " \tTime=" << time / 1000.0f << "/" << baseline << flush;
//					operate_tensor_data<float>(&result, I, {0, 0}, {1, 9}, result.dimensions, "2");

					//warm up
					self->peers[i]->run(I);

					//timing the second version now......
					size_t time2 = MICROS(0);
					for (int j = 0; j < total; j++)
						self->peers[i]->run(I);
					if (parallel) {
#ifdef CUBLAS_ENABLE
						if (cublas)
							cudaDeviceSynchronize();
						else
#endif
							wait_for_all_kernels_finished(I);
					}
					time2 = MICROS(time2);
					//end timing-----------------------------

					float compared = time2 / total / 1000.0f;
					float delta = 0;
					if (verify) {
#ifdef CUBLAS_ENABLE
						if (cublas)
							cudaMemcpy((void*)I.pointers[&result], (void*)bufC, result.size, cudaMemcpyDeviceToHost);
						else
#endif
						if (!MKL) //For MKL, data has stored in I.pointers[&result]
							result.upload(I);
						auto p1 = result.pointer, p2 = I.pointers[&result];
						for (int j = 0; j < m * n; j ++, p1++, p2++) { //validiate the correction
							float diff = *p1 - *p2;
							delta += diff * diff;
						}
					}
//					operate_tensor_data<float>(&result, I, {0, 0}, {1, 9}, result.dimensions, "1");

					logger << ", " << time2 / 1000.0f << "/" << compared << "ms \tSpeedUp=" << baseline / compared;
					if (verify)
						logger << " \tdelta=" << delta;
					logger << endl;
					i++;
				}

#ifdef CUBLAS_ENABLE
		extern void shutdown_cublas();
		if (cublas)
			shutdown_cublas();
#endif
		if (clBLAS)
			clblasTeardown();
	}, {},
	[](InstantTensor* self) -> Tensor* { return nullptr; },
	[](InstantTensor* self) -> std::vector<Tensor*> { return self->peers; },
	[](InstantTensor* self, DeviceInstance& I) -> string { //This example used as a trial to test OpenCL kernel code
		string content;
		read_file_content<char>(OpenCL.location + "src/base.cl", content);
		return content;
	});

	for (int m = M; m >= 32; m /= STEP)
		for (int n = N; n >= 32; n /= STEP)
			for (int  k = K; k >= 32; k /= STEP)
				graph.peers.push_back(new InstantTensor("tiling_" + to_string(m)  + "_" + to_string(n) + "_" + to_string(k), {&x, &w}, {}, [m, n, k, parallel, &result, clblast, cublas, clBLAS, MIOpen, MKL](InstantTensor* self, DeviceInstance& I) {
					T x = *self->inputs[0];
					T w = *self->inputs[1];

					if (clblast) {
						auto status = Gemm<float>(Layout::kColMajor, Transpose::kNo, Transpose::kNo, m, n, k, alpha, I.buffers[&w](), 0, m, I.buffers[&x](), 0, k, beta, I.buffers[&result](), 0, m, &I.queue(), &I.events[&result]());
						if (status != clblast::StatusCode::kSuccess)
							throw runtime_error("clblast error: " + to_string((int) status));
						if (!parallel)
							wait_for_all_kernels_finished(I);
					}
#ifdef CUBLAS_ENABLE
					else if (cublas) {
						auto status = cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N, m, n, k, &alpha, bufA, m, bufB, k, &beta, bufC, m);
						if (status != CUBLAS_STATUS_SUCCESS)
							exit(1);
						if (!parallel)
							cudaDeviceSynchronize();
					}
#endif
					else if (clBLAS) {
						auto err = clblasSgemm(clblasColumnMajor, clblasNoTrans, clblasNoTrans, m, n, k, alpha, I.buffers[&w](), 0, m,
								I.buffers[&x](), 0, k, beta, I.buffers[&result](), 0, m, 1, &I.queue(), 0, NULL, &I.events[&result]());
						if (err != CL_SUCCESS)
							throw runtime_error("clBLAS error: " + to_string((int) err));
					}
#ifdef MIOPENGEMM_ENABLE
					else if (MIOpen) {
						auto stat = MIOpenGEMM::gemm0<float>(true, false, false, m, n, k, alpha, I.buffers[&w](), 0, m,
								I.buffers[&x](), 0, k, beta, I.buffers[&result](), 0, m, &I.queue(), 0, NULL, &I.events[&result]());
						if (!stat.success)
							throw runtime_error("MIOpenGemm error: " + to_string(stat.ID));
					}
#endif
#ifdef MKL_ENABLE
					else if (MKL) {
						cblas_sgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, m, n, k, alpha, I.pointers[&w], m, I.pointers[&x], k, beta, I.pointers[&result], m);
					}
#endif
					else {
						auto& kernel = prepare_for_running_kernel(self, I);
						kernel.setArg(0, I.buffers[&result]);
						kernel.setArg(1, I.buffers[&x]);
						kernel.setArg(2, I.buffers[&w]);
						kernel.setArg(3, nullptr);
						kernel.setArg(4, m);
						kernel.setArg(5, k);
						int local_size = find_proper_local_size(k, I.work_group_size);
						if (local_size > m * n)
							local_size = m * n;
						const auto& local = I.work_group_size <= 256? cl::NDRange(16, 16) : cl::NDRange(32, 32);
						cl::NDRange global(m, n);
						I.queue.enqueueNDRangeKernel(kernel, cl::NullRange, global, local/*cl::NullRange*/, &I.precondition_events, &I.events[&result]);
						if (!parallel)
							wait_for_all_kernels_finished(I);
					}
//					operate_tensor_data<float>(&result, I, {0, 0}, {2, 4}, result.dimensions);
				}, [m, n, k](InstantTensor* self, DeviceInstance& I) -> string {
					if (cl_build_options.find("-DTS=") == string::npos)
						cl_build_options += "-DTS=" + to_string(I.work_group_size <= 256? 16 : 32);
					string content;
					read_file_content<char>(OpenCL.location + "src/revised.cl", content);
					return content;
				}));

	return graph;
}

#ifdef CUBLAS_ENABLE
void prepare_cublas(float* A, float* B, float* C, int K, int M, int N)
{
	// cuBLAS configuration
	cublasStatus_t status;
	status = cublasCreate(&handle);
	if (status != CUBLAS_STATUS_SUCCESS)
		exit(1);
	// Prepare CUDA memory objects
	cudaMalloc((void**)&bufA, M*K*sizeof(*A));
	cudaMalloc((void**)&bufB, K*N*sizeof(*B));
	cudaMalloc((void**)&bufC, M*N*sizeof(*C));
	// Copy matrices to the GPU (also C to erase the results of the previous run)
	cudaMemcpy((void*)bufA, (void*)A, M*K*sizeof(*A), cudaMemcpyHostToDevice);
	cudaMemcpy((void*)bufB, (void*)B, K*N*sizeof(*B), cudaMemcpyHostToDevice);
	cudaMemcpy((void*)bufC, (void*)C, M*N*sizeof(*C), cudaMemcpyHostToDevice);
}

void shutdown_cublas()
{
	cublasStatus_t status;
	// Free the GPU memory objects
	cudaFree(bufA);
	cudaFree(bufB);
	cudaFree(bufC);
	// Clean-up cuBLAS
	status = cublasDestroy(handle);
	if (status != CUBLAS_STATUS_SUCCESS)
		exit(1);
}
#endif

namespace clnet {
extern unordered_map<string, string> key_values;
extern Tensor* _breakpoint;
}

int main(int argc, char** argv)
{
	signal(SIGINT, [](int signal) {
		logger << "User breaks by Ctrl+C." << endl;
		CLNET_TENSOR_GLOBALS |= CLNET_STEP_INTO_MODE;
		for (auto& iter : DeviceInstance::ALL)
			wait_for_all_kernels_finished(iter.second);
		exit(1);
	});

	OpenCL.location = "/GIT/gemm_optimization/";
	bool use_debugger = false, stop_on_startup = false, list_devices = false, display_structure = false, console_output = true, log_to_file = false;
	vector<int> devices;
	for (int i = 1; i < argc; i++) {
		string param(argv[i]);
		if (param.empty())
			return 1;
		else if (param[0] == ':' && i + 1 < argc)
			key_values[param.substr(1)] = argv[++i];
		else if (param == "/p")
			CLNET_TENSOR_GLOBALS |= CLNET_PREDICT_ONLY;
		else if (param == "/d")
			use_debugger = true;
		else if (param == "/ss")
			stop_on_startup = true;
		else if (param == "/ld")
			list_devices = true;
		else if (param == "/ds")
			display_structure = true;
		else if (param == "/nf")
			CLNET_TENSOR_GLOBALS ^= CLNET_FEED_FORWARD_FUSION | CLNET_BACK_PROPAGATE_FUSION;
		else if (param == "/os")
			CLNET_TENSOR_GLOBALS |= CLNET_OPENCL_SHOW_SOURCE;
		else if (param == "/all")
			OpenCL.device_type = CL_DEVICE_TYPE_ALL;
		else if (param == "/nlogc")
			console_output = false;
		else if (param == "/logf")
			log_to_file = true;
		else if (param[0] == '/') {
			if ((param[1] == '[' && param[param.length() - 1] == ']') || (param[1] >= '0' && param[1] <= '9')) //Linux shell strips '[' and ']' in "/[1,2]"
				parse_dimensions<int>(param.substr(1), &devices);
			else
				cout << "Unknown option " << param << " ignored." << endl;
		}
		else
			key_values["model"] = param;
	}

	if (log_to_file) {
		logger += optional<string>("log_file", OpenCL.location + "clnet.log");
		for (auto p = argv, end = argv + argc; p < end; p++) {
			string param(*p);
			if (param.find(' ') != string::npos)
				param = "\"" + param + "\"";
			logger << param << " ";
		}
		logger << endl;
	}
	if (console_output)
		logger += cout;
	if (devices.empty())
		devices = {0};
	int device_master = optional<int>("master", devices[0]);
	int device_debugger = optional<int>("debugger", use_debugger? devices[0] : -1);

	T graph = gemm_opt();
	if (list_devices)
		OpenCL.print_device_info(cout);
	if (display_structure)
		OpenCL.print_tensor_structure(graph);
	if (stop_on_startup)
		_breakpoint = &graph;
	OpenCL.run(graph, devices, device_debugger, device_master);
	return 0;
}




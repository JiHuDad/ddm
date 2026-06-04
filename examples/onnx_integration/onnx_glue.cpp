/*
 * onnx_glue.cpp — 5-line pattern for wiring ONNX Runtime inference into driftmon.
 *
 * This file is a PATTERN, not a runnable binary.  It shows the minimal glue
 * between an inference engine and the driftmon C ABI.  The pattern is
 * deliberately simple: driftmon only sees raw feature doubles, never the
 * inference-engine types.  Compile with ONNXRUNTIME_AVAILABLE to include the
 * real ONNX Runtime path; without it the file still compiles as a reference.
 *
 * Build (ONNX Runtime installed at /opt/onnxruntime):
 *
 *   g++ -std=c++17 -DONNXRUNTIME_AVAILABLE \
 *       -I/opt/onnxruntime/include -I../../include \
 *       onnx_glue.cpp -L/opt/onnxruntime/lib -lonnxruntime \
 *       -L../../build -ldriftmon -o onnx_glue_demo
 */

#include "driftmon.h"

#include <cstdio>
#include <vector>

#ifdef ONNXRUNTIME_AVAILABLE
#include <onnxruntime_cxx_api.h>

// ---------------------------------------------------------------------------
// ONNX Runtime + driftmon integration — 5-line glue pattern
// ---------------------------------------------------------------------------
void run_with_ort(const char* model_path, const char* reference_json,
                  const std::vector<std::vector<float>>& batches)
{
    // --- driftmon setup (one-time) ------------------------------------------
    driftmon_t* dm = driftmon_create(reference_json);         // line 1
    int nf = driftmon_num_features(dm);                       // line 2

    // --- ONNX Runtime setup (one-time) --------------------------------------
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "onnx_glue");
    Ort::Session session(env, model_path, Ort::SessionOptions{});

    std::vector<const char*> input_names  = {"features"};
    std::vector<const char*> output_names = {"output"};
    Ort::AllocatorWithDefaultOptions alloc;

    // --- inference + drift monitoring loop ----------------------------------
    std::vector<double> feats_d(nf);

    for (const auto& sample : batches) {
        // Run inference.
        auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> shape = {1, static_cast<int64_t>(sample.size())};
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            mem_info, const_cast<float*>(sample.data()), sample.size(),
            shape.data(), shape.size());
        session.Run({}, input_names.data(), &input_tensor, 1,
                    output_names.data(), 1);

        // Extract features of interest and feed driftmon.
        // (In practice: map model output / pre-processing features → feats_d.)
        for (int i = 0; i < nf; ++i) feats_d[i] = static_cast<double>(sample[i]);

        driftmon_observe(dm, feats_d.data(), nf);             // line 3

        if (driftmon_ready(dm)) {                             // line 4
            double max_psi = 0.0;
            std::vector<double> psi(nf);
            driftmon_compute(dm, psi.data(), &max_psi);
            auto sev = driftmon_classify(max_psi);
            std::printf("drift: max_psi=%.4f  severity=%d\n", max_psi, (int)sev);
            driftmon_reset(dm);                               // line 5
        }
    }

    driftmon_destroy(dm);
}

#else // !ONNXRUNTIME_AVAILABLE

// ---------------------------------------------------------------------------
// Stub that compiles without ONNX Runtime — shows the pure driftmon side.
// Replace inference_output_to_features() with your engine's extraction logic.
// ---------------------------------------------------------------------------
static std::vector<double> inference_output_to_features(const void* /* output */, int nf) {
    return std::vector<double>(nf, 0.0); // placeholder
}

void run_without_ort(const char* reference_json, int n_samples) {
    driftmon_t* dm = driftmon_create(reference_json);
    if (!dm) { std::fprintf(stderr, "driftmon_create failed\n"); return; }
    int nf = driftmon_num_features(dm);

    for (int i = 0; i < n_samples; ++i) {
        // Call your inference engine here and convert to doubles.
        auto feats = inference_output_to_features(nullptr, nf);
        driftmon_observe(dm, feats.data(), nf);
        if (driftmon_ready(dm)) {
            double max_psi = 0.0;
            std::vector<double> psi(nf);
            driftmon_compute(dm, psi.data(), &max_psi);
            std::printf("drift: max_psi=%.4f  severity=%d\n",
                        max_psi, (int)driftmon_classify(max_psi));
            driftmon_reset(dm);
        }
    }
    driftmon_destroy(dm);
}

int main() {
    std::printf("onnx_glue stub compiled (ONNXRUNTIME_AVAILABLE not set).\n");
    return 0;
}

#endif // ONNXRUNTIME_AVAILABLE

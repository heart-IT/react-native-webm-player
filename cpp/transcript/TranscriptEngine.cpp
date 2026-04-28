// whisper.cpp integration -- links against the whisper library.
// Isolated in a .cpp file so TranscriptEngine.h doesn't expose whisper.h.
// Entire file is gated on HAVE_WHISPER — without it, TranscriptEngine is never instantiated.

#if HAVE_WHISPER

#include "transcript/TranscriptEngine.h"
#include <whisper.h>

namespace media::transcript {

whisper_context* TranscriptEngine::loadModel(const std::string& path) {
    whisper_context_params cparams = whisper_context_default_params();
    return whisper_init_from_file_with_params(path.c_str(), cparams);
}

void TranscriptEngine::freeModel(whisper_context* ctx) {
    whisper_free(ctx);
}

int TranscriptEngine::runInference(whisper_context* ctx, const float* samples, size_t count, bool translate) {
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = config::kWhisperThreads;
    params.no_timestamps = false;
    params.single_segment = false;
    params.no_context = true;
    params.suppress_blank = true;
    params.suppress_nst = true;
    params.translate = translate;
    params.language = "auto";

    int result = whisper_full(ctx, params, samples, static_cast<int>(count));
    if (result != 0) {
        MEDIA_LOG_W("TranscriptEngine: whisper_full returned %d", result);
        return 0;
    }

    return whisper_full_n_segments(ctx);
}

const char* TranscriptEngine::getSegmentText(whisper_context* ctx, int index) {
    return whisper_full_get_segment_text(ctx, index);
}

int64_t TranscriptEngine::getSegmentStartMs(whisper_context* ctx, int index) {
    return whisper_full_get_segment_t0(ctx, index) * 10;
}

int64_t TranscriptEngine::getSegmentEndMs(whisper_context* ctx, int index) {
    return whisper_full_get_segment_t1(ctx, index) * 10;
}

}  // namespace media::transcript

#endif  // HAVE_WHISPER

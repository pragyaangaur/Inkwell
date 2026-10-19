// inkllm: a small chat process around llama.cpp for Quill, Inkwell's chatbot.
// It loads a GGUF model, primes it with a system prompt (cached to disk after
// the first run, because reading a prompt is slow on the Kindle), then talks
// over stdin and stdout:
//   in:  U <message>   ask; S   stop the current answer; R   start a new chat
//   out: READY once, then T <piece> per token (newlines as \n), then END.
// Messages escape newlines and backslashes the same way, so a whole file
// can be sent. INKLLM_CTX (1024), INKLLM_MAX (240 tokens per answer) and
// INKLLM_TEMP (0.55) change the defaults; quill-code uses longer answers.
// Usage: inkllm model.gguf system.txt cache.bin
#include "llama.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <poll.h>
#include <string>
#include <vector>

static llama_context *ctx;
static const llama_vocab *vocab;
static std::vector<llama_token> prefix; // the system prompt and primer
static int n_past;

static std::vector<llama_token> tokenize(const std::string &s, bool special) {
    std::vector<llama_token> t(s.size() + 16);
    int n = llama_tokenize(vocab, s.c_str(), (int)s.size(), t.data(), (int)t.size(), false, special);
    if (n < 0) { t.resize(-n); n = llama_tokenize(vocab, s.c_str(), (int)s.size(), t.data(), (int)t.size(), false, special); }
    t.resize(n > 0 ? n : 0);
    return t;
}

static bool eval(std::vector<llama_token> &toks) {
    const int batch = 64;
    for (size_t i = 0; i < toks.size(); i += batch) {
        int n = (int)std::min(toks.size() - i, (size_t)batch);
        if (llama_decode(ctx, llama_batch_get_one(toks.data() + i, n))) return false;
        n_past += n;
    }
    return true;
}

static void say(const char *tag, const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    printf("%s %s\n", tag, out.c_str());
    fflush(stdout);
}

// Back to just the primed prompt, from the cache file when it matches.
static bool reset(const char *cache) {
    llama_memory_clear(llama_get_memory(ctx), true);
    n_past = 0;
    std::vector<llama_token> saved(prefix.size() + 8);
    size_t got = 0;
    if (llama_state_load_file(ctx, cache, saved.data(), saved.size(), &got) && got == prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), saved.begin())) {
        n_past = (int)got;
        return true;
    }
    llama_memory_clear(llama_get_memory(ctx), true);
    n_past = 0;
    if (!eval(prefix)) return false;
    llama_state_save_file(ctx, cache, prefix.data(), prefix.size());
    return true;
}

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return v && atoi(v) > 0 ? atoi(v) : def;
}

// Undoes the \n and \\ escapes of a message.
static std::string unescape(const char *s) {
    std::string out;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; out += *s == 'n' ? '\n' : *s; }
        else out += *s;
    }
    return out;
}

static double now_s() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// inkllm --bench model.gguf: times reading a 64-token prompt and making 16 tokens.
static int bench(const char *path) {
    llama_backend_init();
    llama_model *model = llama_model_load_from_file(path, llama_model_default_params());
    if (!model) return 1;
    vocab = llama_model_get_vocab(model);
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512; cp.n_batch = 64; cp.n_threads = cp.n_threads_batch = 1;
    ctx = llama_init_from_model(model, cp);
    std::vector<llama_token> t(64, 100);
    double t0 = now_s();
    eval(t);
    double t1 = now_s();
    for (int i = 0; i < 16; i++) { std::vector<llama_token> one{(llama_token)(200 + i)}; eval(one); }
    double t2 = now_s();
    printf("prompt %.2f tok/s, generation %.2f tok/s\n", 64 / (t1 - t0), 16 / (t2 - t1));
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--bench")) return bench(argv[2]);
    if (argc < 4) { fprintf(stderr, "usage: inkllm model.gguf system.txt cache.bin\n"); return 2; }
    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model) { say("ERR", "model would not load"); return 1; }
    vocab = llama_model_get_vocab(model);
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = env_int("INKLLM_CTX", 1024);
    const int max_answer = env_int("INKLLM_MAX", 240);
    const char *temp_s = getenv("INKLLM_TEMP");
    const float temp = temp_s && atof(temp_s) > 0 ? (float)atof(temp_s) : 0.55f;
    cp.n_batch = 64;
    cp.n_threads = cp.n_threads_batch = 1; // one core
    // An f32 cache: the Cortex-A9 has no fp16 hardware, so f16 attention is scalar.
    cp.type_k = GGML_TYPE_F32;
    cp.type_v = GGML_TYPE_F32;
    ctx = llama_init_from_model(model, cp);
    if (!ctx) { say("ERR", "no context"); return 1; }

    // The system file holds the whole ChatML prefix, so the persona lives in
    // one editable text file.
    std::string sys;
    if (FILE *f = fopen(argv[2], "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) sys.append(buf, n);
        fclose(f);
    }
    prefix = tokenize(sys, true);
    if (!reset(argv[3])) { say("ERR", "could not read the prompt"); return 1; }

    llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 64, 1.1f, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(30));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.85f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist((uint32_t)time(nullptr)));
    say("READY", "");

    // Unbuffered, so poll() on stdin sees an S that arrives mid-answer.
    setvbuf(stdin, nullptr, _IONBF, 0);
    static char line[65536];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strcmp(line, "R")) { reset(argv[3]); say("END", ""); continue; }
        if (strncmp(line, "U ", 2)) continue;
        std::string turn = std::string("<|im_start|>user\n") + unescape(line + 2) + "<|im_end|>\n<|im_start|>assistant\n";
        std::vector<llama_token> t = tokenize(turn, true);
        if (n_past + (int)t.size() + max_answer + 20 > (int)llama_n_ctx(ctx)) reset(argv[3]);
        if (n_past + (int)t.size() + max_answer + 20 > (int)llama_n_ctx(ctx)) {
            say("ERR", "that message is too long for the model's memory");
            say("END", "");
            continue;
        }
        if (!eval(t)) { say("ERR", "decode failed"); say("END", ""); continue; }
        for (int i = 0; i < max_answer; i++) {
            struct pollfd pf = {0, POLLIN, 0};
            if (poll(&pf, 1, 0) > 0) {
                char cmd[64];
                if (fgets(cmd, sizeof cmd, stdin) && cmd[0] == 'S') break;
            }
            llama_token tok = llama_sampler_sample(smpl, ctx, -1);
            if (llama_vocab_is_eog(vocab, tok)) break;
            char piece[64];
            int n = llama_token_to_piece(vocab, tok, piece, sizeof piece, 0, false);
            say("T", std::string(piece, n > 0 ? n : 0));
            std::vector<llama_token> one{tok};
            if (!eval(one)) break;
        }
        std::vector<llama_token> end = tokenize("<|im_end|>\n", true);
        eval(end);
        say("END", "");
    }
    return 0;
}

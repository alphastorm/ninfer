#include "models/qwen3_5/program/internal.h"

#include <iostream>
#include <stdexcept>

int main() {
    using ninfer::models::qwen3_5::resume_without_replay;
    try {
        if (resume_without_replay(false, 0, 0) ||
            resume_without_replay(false, 128, 128))
            throw std::runtime_error("fresh binding was classified as a resume");
        if (!resume_without_replay(true, 0, 0))
            throw std::runtime_error("empty-root resume requires nonexistent history");
        if (!resume_without_replay(true, 128, 128))
            throw std::runtime_error("exact-frontier resume requires nonexistent history");
        if (resume_without_replay(true, 0, 128) ||
            resume_without_replay(true, 64, 128) ||
            resume_without_replay(true, 192, 128))
            throw std::runtime_error("non-exact source was classified as replay-free");
        std::cout << "OK no-replay resume predicate\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

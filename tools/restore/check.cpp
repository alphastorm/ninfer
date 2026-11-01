#include "core/restore_trace.h"
#include <cassert>
#include <fstream>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    assert(argc == 2);
    {
        ninfer::restore_trace::Recorder trace("RESTORE_TRACE_FILE");
        trace.emit("request.begin", "http-17", 9);
        trace.emit("request.response", "resp-23", 9);
        trace.emit("import.begin", {}, 9);
        trace.emit("import.end", {}, 9);
        trace.emit("binding.begin", {}, 9);
        trace.emit("binding.end", {}, 9);
        trace.emit("first.token", {}, 9);
    }
    std::ifstream input(argv[1], std::ios::binary);
    ninfer::restore_trace::Record row{};
    std::vector<ninfer::restore_trace::Record> records;
    while (input.read(reinterpret_cast<char*>(&row), sizeof row)) records.push_back(row);
    assert(records.size() == 10);
    assert(std::string(records.front().name) == "clock.start");
    for (std::size_t i = 1; i < records.size(); ++i) assert(records[i].monotonic >= records[i-1].monotonic);
    assert(std::string(records[1].text) == "http-17");
    assert(std::string(records[2].text) == "resp-23");
    for (std::size_t i=1; i!=8; ++i) assert(records[i].values[0] == 9);
    assert(std::string(records.back().name) == "trace.dropped");
    assert(records.back().values[0] == 0);
}

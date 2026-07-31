#include "pin.H"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr UINT64 kLineSize = 64;

KNOB<std::string> output_path(KNOB_MODE_WRITEONCE, "pintool", "o",
                              "cachelines.bin", "raw cache-line trace output");
KNOB<UINT32> cache_megabytes(KNOB_MODE_WRITEONCE, "pintool", "cache-mb", "16",
                             "simulated LLC capacity in MiB");
KNOB<UINT32> cache_ways(KNOB_MODE_WRITEONCE, "pintool", "ways", "16",
                       "simulated LLC associativity");
KNOB<UINT64> warmup_misses(KNOB_MODE_WRITEONCE, "pintool", "warmup-misses", "0",
                          "LLC misses ignored before recording");
KNOB<UINT64> maximum_blocks(KNOB_MODE_WRITEONCE, "pintool", "max-blocks",
                           "1000000", "maximum cache lines to record");
KNOB<std::string> roi_routine(KNOB_MODE_WRITEONCE, "pintool", "roi", "",
                              "record only inside routines containing this text");

struct Entry {
    UINT64 tag = 0;
    UINT64 stamp = 0;
    bool valid = false;
};

PIN_LOCK lock;
std::ofstream output;
std::vector<Entry> entries;
UINT64 set_count = 0;
UINT64 clock_value = 0;
UINT64 accesses = 0;
UINT64 misses = 0;
UINT64 recorded = 0;
UINT64 copy_failures = 0;
UINT64 roi_depth = 0;

VOID enter_roi(THREADID thread_id) {
    PIN_GetLock(&lock, thread_id + 1);
    ++roi_depth;
    PIN_ReleaseLock(&lock);
}

VOID leave_roi(THREADID thread_id) {
    PIN_GetLock(&lock, thread_id + 1);
    if (roi_depth != 0) {
        --roi_depth;
    }
    PIN_ReleaseLock(&lock);
}

bool cache_miss(UINT64 line_number) {
    const UINT64 set = line_number % set_count;
    const UINT64 tag = line_number / set_count;
    const UINT64 begin = set * cache_ways.Value();
    const UINT64 end = begin + cache_ways.Value();
    ++clock_value;
    for (UINT64 i = begin; i < end; ++i) {
        if (entries[i].valid && entries[i].tag == tag) {
            entries[i].stamp = clock_value;
            return false;
        }
    }
    UINT64 victim = begin;
    for (UINT64 i = begin; i < end; ++i) {
        if (!entries[i].valid) {
            victim = i;
            break;
        }
        if (entries[i].stamp < entries[victim].stamp) {
            victim = i;
        }
    }
    entries[victim].valid = true;
    entries[victim].tag = tag;
    entries[victim].stamp = clock_value;
    return true;
}

VOID record_access(THREADID thread_id, ADDRINT address, UINT32 size) {
    if (size == 0) {
        return;
    }
    const UINT64 first_line = static_cast<UINT64>(address) / kLineSize;
    const UINT64 last_line =
        (static_cast<UINT64>(address) + static_cast<UINT64>(size) - 1) / kLineSize;

    PIN_GetLock(&lock, thread_id + 1);
    if (!roi_routine.Value().empty() && roi_depth == 0) {
        PIN_ReleaseLock(&lock);
        return;
    }
    for (UINT64 line = first_line; line <= last_line; ++line) {
        ++accesses;
        if (!cache_miss(line)) {
            continue;
        }
        ++misses;
        if (misses <= warmup_misses.Value() || recorded >= maximum_blocks.Value()) {
            continue;
        }
        UINT8 bytes[kLineSize];
        const ADDRINT line_address = static_cast<ADDRINT>(line * kLineSize);
        if (PIN_SafeCopy(bytes, reinterpret_cast<const VOID*>(line_address),
                         kLineSize) != kLineSize) {
            ++copy_failures;
            continue;
        }
        output.write(reinterpret_cast<const char*>(bytes), kLineSize);
        ++recorded;
    }
    PIN_ReleaseLock(&lock);
}

VOID instrument_routine(RTN routine, VOID*) {
    if (roi_routine.Value().empty() ||
        RTN_Name(routine).find(roi_routine.Value()) == std::string::npos) {
        return;
    }
    RTN_Open(routine);
    RTN_InsertCall(routine, IPOINT_BEFORE, AFUNPTR(enter_roi), IARG_THREAD_ID,
                   IARG_END);
    RTN_InsertCall(routine, IPOINT_AFTER, AFUNPTR(leave_roi), IARG_THREAD_ID,
                   IARG_END);
    RTN_Close(routine);
}

VOID instrument_instruction(INS instruction, VOID*) {
    const UINT32 operands = INS_MemoryOperandCount(instruction);
    for (UINT32 operand = 0; operand < operands; ++operand) {
        INS_InsertPredicatedCall(
            instruction, IPOINT_BEFORE, AFUNPTR(record_access),
            IARG_THREAD_ID, IARG_MEMORYOP_EA, operand, IARG_UINT32,
            static_cast<UINT32>(INS_MemoryOperandSize(instruction, operand)),
            IARG_END);
    }
}

VOID finish(INT32, VOID*) {
    output.close();
    std::cerr << "cacheline_trace accesses=" << accesses << " misses=" << misses
              << " recorded=" << recorded
              << " copy_failures=" << copy_failures << '\n';
}

INT32 usage() {
    std::cerr << "Records 64-byte memory contents on simulated LLC misses.\n"
              << KNOB_BASE::StringKnobSummary() << '\n';
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) {
        return usage();
    }
    if (cache_ways.Value() == 0 || cache_megabytes.Value() == 0) {
        std::cerr << "cache size and associativity must be non-zero\n";
        return 1;
    }
    const UINT64 line_count =
        static_cast<UINT64>(cache_megabytes.Value()) * 1024U * 1024U / kLineSize;
    set_count = line_count / cache_ways.Value();
    if (set_count == 0) {
        std::cerr << "cache configuration has no sets\n";
        return 1;
    }
    entries.resize(set_count * cache_ways.Value());
    output.open(output_path.Value().c_str(), std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot open trace output: " << output_path.Value() << '\n';
        return 1;
    }
    PIN_InitLock(&lock);
    INS_AddInstrumentFunction(instrument_instruction, nullptr);
    RTN_AddInstrumentFunction(instrument_routine, nullptr);
    PIN_AddFiniFunction(finish, nullptr);
    PIN_StartProgram();
    return 0;
}

// Copyright (c) 2026, Byte Select reproduction.
// Dumps a GAP Benchmark Suite graph's CSR arrays as a raw byte stream that
// can be used as a representative memory trace (each 64-byte chunk is one
// cache line of graph-workload data). The output is the vertex-offset array
// (int64, little-endian) followed by every vertex's outgoing neighbors
// (int32, little-endian) in vertex order, matching how the arrays are laid
// out in memory for real graph kernels. Use -O / -N to emit only the
// neighbor array or only the offset array, which produces homogeneous
// 64-byte lines (16 int32 or 8 int64 values).
//
// Usage (same graph options as the GAPBS kernels, plus custom options):
//   gapbs_dump_trace -g 18 -o kron18.trace
//   gapbs_dump_trace -g 18 -N -o kron18-offsets.trace
//   gapbs_dump_trace -g 18 -O -o kron18-neighbors.trace
//   gapbs_dump_trace -f graph.el -sf -o graph.trace

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "timer.h"

using namespace std;

void WriteLittleEndian(FILE* file, std::uint64_t value, int width) {
  for (int i = 0; i < width; ++i) {
    std::fputc(static_cast<int>((value >> (8 * i)) & 0xff), file);
  }
}

int main(int argc, char* argv[]) {
  std::string output_path = "graph.trace";
  bool dump_offsets = true;
  bool dump_neighbors = true;

  // Consume the dumper's own options before CLApp's getopt-based parser sees
  // them, so their order relative to the graph options does not matter.
  std::vector<char*> cli_args;
  cli_args.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "-o" && i + 1 < argc) {
      output_path = argv[++i];
    } else if (option == "-O") {
      dump_offsets = false;
    } else if (option == "-N") {
      dump_neighbors = false;
    } else {
      cli_args.push_back(argv[i]);
    }
  }

  CLApp cli(static_cast<int>(cli_args.size()), cli_args.data(),
            "dump GAPBS graph as a raw trace");
  if (!cli.ParseArgs())
    return -1;
  if (!dump_offsets && !dump_neighbors) {
    std::fprintf(stderr, "at least one of the offset or neighbor arrays must be dumped\n");
    return -1;
  }

  Builder b(cli);
  Graph g = b.MakeGraph();

  FILE* output = std::fopen(output_path.c_str(), "wb");
  if (output == nullptr) {
    std::fprintf(stderr, "cannot open output file: %s\n", output_path.c_str());
    return -1;
  }

  std::uint64_t bytes = 0;
  if (dump_offsets) {
    const auto offsets = g.VertexOffsets(false);
    for (const auto offset : offsets) {
      WriteLittleEndian(output, static_cast<std::uint64_t>(offset), 8);
    }
    bytes += offsets.size() * 8;
  }
  if (dump_neighbors) {
    for (NodeID u = 0; u < g.num_nodes(); ++u) {
      for (NodeID v : g.out_neigh(u)) {
        WriteLittleEndian(output, static_cast<std::uint64_t>(v), 4);
        bytes += 4;
      }
    }
  }
  std::fclose(output);

  std::cout << "dumped " << g.num_nodes() << " nodes, " << g.num_edges()
            << " (undirected) edges to " << output_path << " ("
            << bytes << " bytes)\n";
  return 0;
}

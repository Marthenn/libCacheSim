/*
 * File: libCacheSim/bin/traceUtils/traceLBNConv.cpp
 *
 * Converts a data trace LBN to metadata block IDs by dividing by the
 * provided fanout (supports floating point). Writes output in oracleGeneral binary format:
 *   clock_time, obj_id (mapped), obj_size, next_access_vtime = -2
 *
 * Usage: traceLBNConv <trace_path> <trace_type> <fanout> [output_dir] [filename_template]
 * Example: traceLBNConv /path/trace.oracleGeneral.zst oracleGeneral 78.17 ./out "%s_meta_%s.oracleGeneral"
 */

#include <cstring>
#include <libgen.h>
#include <fstream>
#include <iostream>
#include <string>
#include <stdio.h>
#include <errno.h>

#include "../cli_reader_utils.h"
#include "internal.hpp"
#include "libCacheSim/reader.h"
#include "libCacheSim/const.h"
#include "utils/include/mysys.h"
#include "utils/include/mymath.h"

struct output_format {
  uint32_t clock_time;
  uint64_t obj_id;
  uint32_t obj_size;
  int64_t next_access_vtime;
} __attribute__((packed));

int main(int argc, char *argv[]) {
  if (argc < 4 || argc > 7) {
    fprintf(stderr, "Usage: %s <trace_path> <trace_type> <fanout> [output_dir] [filename_template] [output_format]\n", argv[0]);
    fprintf(stderr, "Example: %s /path/trace.oracleGeneral.zst oracleGeneral 78.17 ./out \"%%s_meta_%%s.oracleGeneral\" oracleGeneral\n", argv[0]);
    return 1;
  }

  char* trace_path = argv[1];
  char* trace_type_str = argv[2];
  char* fanout_str = argv[3];

  errno = 0;
  char *endptr = nullptr;
  double fanout = strtod(fanout_str, &endptr);
  if (errno != 0 || endptr == fanout_str || fanout <= 0.0) {
    fprintf(stderr, "Invalid fanout: %s\n", fanout_str);
    return 1;
  }

  // Optional output directory, filename template, and output format
  // Usage now: [output_dir] [filename_template] [output_format]
  char* output_dir = (argc >= 5) ? argv[4] : (char*)".";
  const char* default_template = "%s_meta_%s.oracleGeneral";
  char* filename_template = (argc >= 6) ? argv[5] : (char*)default_template;
  char* output_format = (argc >= 7) ? argv[6] : (char*)"oracleGeneral";
  bool is_csv = (strcmp(output_format, "csv") == 0);

  // Derive output prefix from trace filename (same logic as before)
  char output_prefix[OFILEPATH_LEN];
  char *trace_filename_ptr = strrchr(trace_path, '/');
  if (trace_filename_ptr == NULL) {
    trace_filename_ptr = trace_path;
  } else {
    trace_filename_ptr = trace_filename_ptr + 1;
  }
  char *first_dot_ptr = strchr(trace_filename_ptr, '.');
  size_t prefix_len;
  if (first_dot_ptr == NULL) {
    prefix_len = strlen(trace_filename_ptr);
  } else {
    prefix_len = first_dot_ptr - trace_filename_ptr;
  }
  if (prefix_len >= OFILEPATH_LEN) prefix_len = OFILEPATH_LEN - 1;
  strncpy(output_prefix, trace_filename_ptr, prefix_len);
  output_prefix[prefix_len] = '\0';

  // Build base filename using the template: e.g. <prefix>_meta_<fanout>.oracleGeneral
  char base_filename[OFILEPATH_LEN];
  // Use fanout_str to preserve user's textual representation (e.g. "78.17")
  snprintf(base_filename, OFILEPATH_LEN, filename_template, output_prefix, fanout_str);

  if (is_csv) {
    // ensure base_filename ends with .csv
    const char *dot = strrchr(base_filename, '.');
    if (dot == NULL || strcmp(dot, ".csv") != 0) {
      char tmp_fname[OFILEPATH_LEN];
      snprintf(tmp_fname, OFILEPATH_LEN, "%s.csv", base_filename);
      strncpy(base_filename, tmp_fname, OFILEPATH_LEN - 1);
      base_filename[OFILEPATH_LEN - 1] = '\0';
    }
  }

  // Build full output path: join output_dir and base_filename
  char output_filename[OFILEPATH_LEN];
  size_t dir_len = strlen(output_dir);
  if (dir_len > 0 && output_dir[dir_len - 1] == '/') {
    snprintf(output_filename, OFILEPATH_LEN, "%s%s", output_dir, base_filename);
  } else {
    snprintf(output_filename, OFILEPATH_LEN, "%s/%s", output_dir, base_filename);
  }

  // Prepare reader
  struct arguments args;
  cli::init_arg(&args);
  args.trace_path = trace_path;
  args.trace_type_str = trace_type_str;
  args.trace_type = trace_type_str_to_enum(args.trace_type_str, args.trace_path);

  args.reader = create_reader(args.trace_type_str, args.trace_path,
                              args.trace_type_params, args.n_req,
                              args.ignore_obj_size, 0);
  if (args.reader == NULL) {
    ERROR("Failed to create reader for trace %s\n", trace_path);
    return 1;
  }

  INFO("Converting trace %s to metadata with fanout=%s. Output: %s\n",
       trace_path, fanout_str, output_filename);

  request_t *req = new_request();

  // std::ofstream output_file(output_filename, std::ios::binary);
  // if (!output_file.is_open()) {
  //   ERROR("Failed to open output file %s: %s\n", output_filename, strerror(errno));
  //   cli::free_arg(&args);
  //   free_request(req);
  //   return 1;
  // }
  //
  // struct output_format out_req;
  // out_req.next_access_vtime = -2;
  //
  // long total = 0;
  // // Read requests and convert
  // read_one_req(args.reader, req);
  // while (req->valid) {
  //   total++;
  //
  //   uint64_t meta_id = (uint64_t)((double)req->obj_id / fanout);
  //
  //   out_req.clock_time = req->clock_time;
  //   out_req.obj_id = meta_id;
  //   out_req.obj_size = req->obj_size; // preserve size (optional)
  //   output_file.write(reinterpret_cast<char *>(&out_req), sizeof(out_req));
  //
  //   read_one_req(args.reader, req);
  // }
  //
  // INFO("Finished conversion. Total requests processed: %ld\n", total);
  //
  // if (output_file.is_open()) output_file.close();

  std::ofstream output_file;
  if (is_csv) {
    output_file.open(output_filename); // text
  } else {
    output_file.open(output_filename, std::ios::binary);
  }
  if (!output_file.is_open()) {
    ERROR("Failed to open output file %s: %s\n", output_filename, strerror(errno));
    cli::free_arg(&args);
    free_request(req);
    return 1;
  }

  struct output_format out_req;
  out_req.next_access_vtime = -1;

  long total = 0;
  // Read requests and convert
  read_one_req(args.reader, req);
  while (req->valid) {
    total++;

    uint64_t meta_id = (uint64_t)((double)req->obj_id / fanout);

    out_req.clock_time = req->clock_time;
    out_req.obj_id = meta_id;
    out_req.obj_size = req->obj_size; // preserve size (optional)

    if (is_csv) {
      // csv: clock_time,meta_id,obj_size,next_access_vtime
      output_file << out_req.clock_time << "," << out_req.obj_id << "," << out_req.obj_size << "," << out_req.next_access_vtime << "\n";
    } else {
      // binary oracleGeneral (unchanged)
      output_file.write(reinterpret_cast<char *>(&out_req), sizeof(out_req));
    }

    read_one_req(args.reader, req);
  }

  if (output_file.is_open()) output_file.close();

  free_request(req);
  cli::free_arg(&args);
  return 0;
}
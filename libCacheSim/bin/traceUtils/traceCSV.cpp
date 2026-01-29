//
// Created by marthen on 1/29/26.
//

#include <fstream>
#include <ios>

#include "internal.hpp"
#include "libCacheSim/reader.h"

int main(int argc, char *argv[]) {
  struct arguments args;

  cli::parse_cmd(argc, argv, &args);
  if (strlen(args.ofilepath) == 0) {
    snprintf(args.ofilepath, OFILEPATH_LEN, "%s.%s", args.trace_path,
             args.output_format);
  }

  reader_t *reader = args.reader;
  std::string ofilepath = args.ofilepath;
  request_t *req = new_request();

  std::ofstream ofile(ofilepath,
                      std::ios::out | std::ios::trunc);
  int64_t n_req = 0;

  ofile << "clock_time,obj_id,obj_size,next_access_vtime\n";

  while (read_one_req(reader, req) == 0) {
    ofile << req->clock_time << ","
          << req->obj_id << ","
          << req->obj_size << ","
          << req->next_access_vtime << "\n";

    if ((++n_req) % 10000000 == 0) {
      printf("Processed %ld M requests...\n", (long)(n_req / 1e6));
    }
  }

  ofile.close();
  free_request(req);
  close_reader(reader);

  return 0;
}
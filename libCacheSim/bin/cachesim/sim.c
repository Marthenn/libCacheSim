#include <unistd.h>
#include <sys/wait.h>

#include "libCacheSim/cache.h"
#include "libCacheSim/reader.h"
#include "utils/include/mymath.h"
#include "utils/include/mystr.h"
#include "utils/include/mysys.h"

#ifdef __cplusplus
extern "C" {
#endif

char* csv_dir = "/mnt/mfs/results.csv";

double run_oracle_on_buffer(request_t *req_buffer, int count, cache_t *cache) {
  double candidates[] = {
    0.01, 0.05, 0.10, 0.15, 0.20,
    0.25, 0.30, 0.35, 0.40, 0.45,
    0.50, 0.55, 0.60, 0.65, 0.70,
    0.75, 0.80, 0.85, 0.90, 0.95,
    0.99
  };
  const int num_candidates = 21;

  int pipes[num_candidates][2];

  for (int i = 0; i < num_candidates; i++) {
    if (pipe(pipes[i]) == -1) {
      ERROR("Pipe creation failed: %s\n", strerror(errno));
      exit(1);
    }

    if (fork() == 0) { // CHILD PROCESS
      close(pipes[i][0]);
      extern void S3FIFO_resize(cache_t *cache, double new_small_ratio);
      S3FIFO_resize(cache, candidates[i]);

      int hits = 0;
      for (int k = 0; k < count; k++) {
        if (cache->get(cache, &req_buffer[k])) {
          hits++;
        }
      }

      double miss_ratio = 1.0 - ((double)hits / count);
      if (write(pipes[i][1], &miss_ratio, sizeof(double)) == -1) {
        ERROR("Write to pipe failed: %s\n", strerror(errno));
        exit(1);
      }
      exit(0);
    }
  }

  double best_miss = 2.0;
  double best_param = -1.0;

  for (int i = 0; i < num_candidates; i++) {
    close(pipes[i][1]);

    double child_miss;
    if (read(pipes[i][0], &child_miss, sizeof(double)) > 0) {
      if (child_miss < best_miss) {
        best_miss = child_miss;
        best_param = candidates[i];
      }
    }
    wait(NULL); // Wait for child process to finish
  }

  return (best_param == -1.0) ? 0.1 : best_param;
}

void print_head_requests(request_t *req, uint64_t req_cnt) {
  if (req_cnt < 2) {
    print_request(req);
  }
}

void simulate(reader_t *reader, cache_t *cache, int report_interval,
              int warmup_sec, char *ofilepath, bool ignore_obj_size,
              bool print_head_req, int epoch_size) {
  /* random seed */
  srand(time(NULL));
  set_rand_seed(rand());

  request_t *req = new_request();
  uint64_t req_cnt = 0, miss_cnt = 0;
  uint64_t last_req_cnt = 0, last_miss_cnt = 0;
  uint64_t req_byte = 0, miss_byte = 0;

  read_one_req(reader, req);
  uint64_t start_ts = (uint64_t)req->clock_time;
  uint64_t last_report_ts = warmup_sec;

  char detailed_cache_name[256];
  generate_cache_name(cache, detailed_cache_name, 256);

  double start_time = -1;

  // Initialize Buffer
  request_t *req_buffers = my_malloc_n(request_t, epoch_size);
  for (int i = 0 ; i < epoch_size; i++) {
    memset(&req_buffers[i], 0, sizeof(request_t));
  }

  while (req->valid) {
    int reqs_read = 0;
    for (int i = 0; i < epoch_size; i++) {
      if (read_one_req(reader, &req_buffers[i]) != 0) {
        req_buffers[i].valid = false;
        break;
      }
      reqs_read++;
    }

    if (reqs_read == 0) { //End of Trace
      break;
    }

    double optimal_param = run_oracle_on_buffer(req_buffers, reqs_read, cache);
    extern void S3FIFO_print_training_row(cache_t *cache, double optimal_param);
    S3FIFO_print_training_row(cache, optimal_param);

    extern void S3FIFO_resize(cache_t *cache, double new_small_ratio);
    S3FIFO_resize(cache, optimal_param);

    for (int i = 0; i < reqs_read; i++) {
      request_t *req = &req_buffers[i];
      req_cnt++;
      if (cache->get(cache,req) == false) {
        miss_cnt++;
      }
    }

    // if (print_head_req) {
    //   print_head_requests(req, req_cnt);
    // }
    //
    // req->clock_time -= start_ts;
    // if (req->clock_time <= warmup_sec) {
    //   cache->get(cache, req);
    //   read_one_req(reader, req);
    //   continue;
    // } else {
    //   if (start_time < 0) {
    //     start_time = gettime();
    //   }
    // }
    //
    // req_cnt++;
    // req_byte += req->obj_size;
    // if (cache->get(cache, req) == false) {
    //   miss_cnt++;
    //   miss_byte += req->obj_size;
    // }
    // if (req->clock_time - last_report_ts >= (uint64_t)report_interval &&
    //     req->clock_time != 0) {
    //   INFO(
    //       "%s %s %.2lf hour: %lu requests, miss ratio %.4lf, interval miss "
    //       "ratio "
    //       "%.4lf\n",
    //       mybasename(reader->trace_path), detailed_cache_name,
    //       (double)req->clock_time / 3600, (unsigned long)req_cnt,
    //       (double)miss_cnt / req_cnt,
    //       (double)(miss_cnt - last_miss_cnt) / (req_cnt - last_req_cnt));
    //   last_miss_cnt = miss_cnt;
    //   last_req_cnt = req_cnt;
    //   last_report_ts = (int64_t)req->clock_time;
    //     }
    //
    // read_one_req(reader, req);
  }

  double runtime = gettime() - start_time;

  char output_str[1024];
  char size_str[64];

  if (!ignore_obj_size) convert_size_to_str(cache->cache_size, size_str, 64);
#pragma GCC diagnostic push
  // Removed unknown pragma warning
  if (!ignore_obj_size) {
    snprintf(output_str, 1024,
             "%s %s cache size %8s, %16lu req, miss ratio %.4lf, throughput "
             "%lf QPS\n",
             reader->trace_path, detailed_cache_name, size_str,
             (unsigned long)req_cnt, (double)miss_cnt / (double)req_cnt,
             (double)req_cnt / runtime);
  } else {
    snprintf(output_str, 1024,
             "%s %s cache size %8lld, %16lu req, miss ratio %.4lf, throughput "
             "%lf QPS\n",
             reader->trace_path, detailed_cache_name,
             (long long)cache->cache_size, (unsigned long)req_cnt,
             (double)miss_cnt / (double)req_cnt,
             (double)req_cnt / runtime);
  }

#pragma GCC diagnostic pop
  printf("%s", output_str);
  char *output_dir = rindex(ofilepath, '/');
  if (output_dir != NULL) {
    size_t dir_length = output_dir - ofilepath;
    char dir_path[1024];
    snprintf(dir_path, dir_length + 1, "%s", ofilepath);
    create_dir(dir_path);
  }
  FILE *output_file = fopen(ofilepath, "a");
  if (output_file == NULL) {
    ERROR("cannot open file %s %s\n", ofilepath, strerror(errno));
    exit(1);
  }
  fprintf(output_file, "%s", output_str);
  fclose(output_file);

  FILE *csv_file = fopen(csv_dir, "a");
  if (csv_file == NULL) {
    ERROR("cannot open file %s %s\n", csv_dir, strerror(errno));
  }
  // Trace, Ignore Obj Size, Cache, Size,  Miss Ratio, Byte Miss Ratio, Throughput(QPS)
  if (!ignore_obj_size) {
    fprintf(csv_file,
            "%s,%s,%s,%lld,%.6lf,%.6lf,%.2lf\n",
            mybasename(reader->trace_path),
            "N",
            detailed_cache_name,
            (long long)cache->cache_size,
            (double)miss_cnt / (double)req_cnt,
            (double)miss_byte / (double)req_byte,
            (double)req_cnt / runtime);
  } else {
    fprintf(csv_file,
            "%s,%s,%s,%lld,%.6lf,%.6lf,%.2lf\n",
            mybasename(reader->trace_path),
            "Y",
            detailed_cache_name,
            (long long)cache->cache_size,
            (double)miss_cnt / (double)req_cnt,
            (double)miss_byte / (double)req_byte,
            (double)req_cnt / runtime);
  }
  fclose(csv_file);

#if defined(TRACK_EVICTION_V_AGE)
  while (cache->get_occupied_byte(cache) > 0) {
    cache->evict(cache, req);
  }

#endif
  free_request(req);
  cache->cache_free(cache);
}

#ifdef __cplusplus
}
#endif

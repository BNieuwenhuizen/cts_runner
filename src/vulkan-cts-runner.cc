/* Copyright 2016 Bas Nieuwenhuizen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <unordered_map>

#include <boost/filesystem.hpp>

std::vector<std::string> parse_testcase_file(std::string const &filename) {
  std::vector<std::string> cases;
  std::ifstream in(filename);
  if (!in.is_open()) {
    std::cerr << "could not find testcase file \"" << filename << "\""
              << std::endl;
    std::exit(1);
  }

  std::string line;
  while (std::getline(in, line))
    cases.push_back(line);
  return cases;
}

std::vector<std::string> get_testcases(std::string const &deqp) {
  std::string dir = boost::filesystem::path{deqp}.parent_path().native();
  FILE *f;
  char buf[4096];
  std::vector<std::string> cases;
  int fd[2];

  if (pipe(fd))
    throw - 1;

  if (!fork()) {
    close(fd[0]);
    dup2(fd[1], 1);
    dup2(fd[1], 2);
    if (chdir(dir.c_str()))
      throw - 1;
    execl(deqp.c_str(), deqp.c_str(), "--deqp-runmode=stdout-caselist",
          (char *)NULL);
  }
  close(fd[1]);
  f = fdopen(fd[0], "r");

  while (fgets(buf, 4096, f)) {
    if (strncmp(buf, "TEST: ", 6) == 0) {
      auto len = strlen(buf);
      cases.push_back(std::string(buf + 6, buf + len - 1));
    }
  }
  fclose(f);
  return cases;
}

struct Context {
  std::string deqp;
  std::vector<std::string> test_cases;
  std::chrono::time_point<std::chrono::steady_clock> start_time;
  std::vector<char const *> results;
  std::atomic<std::size_t> taken_cases, finished_cases;
  std::atomic<std::size_t> pass_count, fail_count, skip_count, crash_count,
      undetermined_count;

  Context() {
    pass_count = 0;
    fail_count = 0;
    skip_count = 0;
    crash_count = 0;
    undetermined_count = 0;
  }
};

bool process_block(Context &ctx, unsigned thread_id) {
  std::size_t base_idx, count;
  do {
    base_idx = ctx.taken_cases.load();
    if (base_idx >= ctx.test_cases.size())
      return false;
    count = std::min<std::size_t>(std::max<std::size_t>((ctx.test_cases.size() - base_idx) / 32, 1), 128);

  } while (
      !ctx.taken_cases.compare_exchange_strong(base_idx, base_idx + count));

  std::string filename = "/tmp/cts_runner." + std::to_string(getpid()) + "." +
                         std::to_string(thread_id);
  std::string dir = boost::filesystem::path{ctx.deqp}.parent_path().native();
  std::size_t idx = 0, test_idx = 0;
  FILE *f = NULL;
  int fd[2];
  char buf[4096];
  std::unordered_map<std::string, unsigned> indices;
  for (std::size_t i = 0; i < count; ++i)
    indices.insert({ctx.test_cases[base_idx + i], base_idx + i});

  while (idx < count) {
    if (!f) {
      std::string arg = "--deqp-caselist-file=" + filename;
      std::ofstream out(filename);
      for (auto &&e : indices)
        out << e.first << "\n";
      out.close();

      if (pipe2(fd, O_CLOEXEC))
        throw - 1;

      if (!fork()) {
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        if (chdir(dir.c_str()))
          throw - 1;
        execl(ctx.deqp.c_str(), ctx.deqp.c_str(), arg.c_str(),
              "--deqp-log-filename=/dev/null", (char *)NULL);
      }
      close(fd[1]);
      f = fdopen(fd[0], "r");
      if (!f)
        std::abort();
    }
    auto r = fgets(buf, 4096, f);
    if (!r || strncmp(buf, "DONE!", 5) == 0) {
      fclose(f);
      f = NULL;
      ctx.results[test_idx] = "Crash";
      //std::cerr << "\'" << ctx.test_cases[test_idx] << "\' crashed" << std::endl; 
      ++ctx.crash_count;
      ++idx;
    } else if (strncmp(buf, "  NotSupported", 14) == 0) {
      ctx.results[test_idx] = "Skip";
      ++idx;
      ++ctx.skip_count;
    } else if (strncmp(buf, "  Fail", 6) == 0) {
      ctx.results[test_idx] = "Fail";
      ++idx;
      ++ctx.fail_count;
    } else if (strncmp(buf, "  Pass", 6) == 0 ||
               strncmp(buf, "  CompatibilityWarning", 22) == 0) {
      ctx.results[test_idx] = "Pass";
      ++idx;
      ++ctx.pass_count;
    } else if (strncmp(buf, "Test case '", 11) == 0) {
      if (indices.size() + idx != count) {
        ctx.results[test_idx] = "Undetermined";
        ++idx;
        ++ctx.undetermined_count;
      }
      auto len = strlen(buf) - 4;
      auto name = std::string(buf + 11, buf + len);
      auto it = indices.find(name);
      if (it == indices.end())
        abort();
      test_idx = it->second;
      indices.erase(it);
    }
  }
  if (f)
    fclose(f);
  if (!indices.empty())
    abort();
  unlink(filename.c_str());
  ctx.finished_cases += count;
  return true;
}

void thread_runner(Context &ctx, unsigned thread_id) {
  while (process_block(ctx, thread_id))
    ;
}

std::string format_duration(std::int64_t seconds) {
  std::string ret;
  if (seconds >= 3600) {
    ret += std::to_string(seconds / 3600);
    ret += ':';
    seconds %= 3600;
  }

  if (seconds >= 60 || !ret.empty()) {
    if (!ret.empty() && seconds < 600)
      ret += '0';
    ret += std::to_string(seconds / 60);
    ret += ':';
    seconds %= 60;
  }
  if (!ret.empty() && seconds < 10)
    ret += '0';
  ret += std::to_string(seconds);
  return ret;
}

void update(Context &ctx) {
  std::size_t pass_count = ctx.pass_count;
  std::size_t fail_count = ctx.fail_count;
  std::size_t skip_count = ctx.skip_count;
  std::size_t crash_count = ctx.crash_count;
  std::size_t undetermined_count = ctx.undetermined_count;
  std::size_t total =
      pass_count + fail_count + skip_count + crash_count + undetermined_count;

  std::chrono::time_point<std::chrono::steady_clock> current_time =
      std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::duration<double>>(
      current_time - ctx.start_time);

  std::cout << "[" << total << "/" << ctx.test_cases.size() << "]";
  std::cout << " Pass: " << pass_count;
  std::cout << " Fail: " << fail_count;
  std::cout << " Skip: " << skip_count;
  std::cout << " Crash: " << crash_count;
  std::cout << " Undetermined: " << undetermined_count;
  std::cout << " Duration: " << format_duration(duration.count());
  if (total) {
    std::cout << " Remaining: "
              << format_duration(duration.count() / total *
                                 (ctx.test_cases.size() - total));
  }
  std::cout << "                  \r";
  std::cout.flush();
}

std::map<std::string, std::string> parse_args(int argc, char *argv[]) {
  std::map<std::string, std::string> args;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-' || argv[i][1] != '-') {
      std::cerr << "arguments have to start with '--'\n";
      std::exit(1);
    }
    auto eq = strchr(argv[i], '=');
    if (eq) {
      std::string arg_name{argv[i] + 2, eq};
      std::string value = eq + 1;
      if (args.find(arg_name) != args.end()) {
        std::cerr << "specified '" << arg_name << "' twice.\n";
        std::exit(1);
      }
      args[arg_name] = value;
    } else {
      std::string arg_name = argv[i] + 2;
      ++i;
      if (i == argc) {
        std::cerr << "missing value for argument '" << arg_name << "'\n";
        std::exit(1);
      }
      if (args.find(arg_name) != args.end()) {
        std::cerr << "specified '" << arg_name << "' twice.\n";
        std::exit(1);
      }
      args[arg_name] = argv[i];
    }
  }
  return args;
}

int main(int argc, char *argv[]) {
  signal(SIGCHLD, SIG_IGN);

  auto args = parse_args(argc, argv);
  if (args.find("deqp") == args.end()) {
    std::cerr << "--deqp missing\n";
    return 1;
  }
  if (args.find("output") == args.end()) {
    std::cerr << "--output missing\n";
    return 1;
  }

  Context ctx;
  ctx.deqp = args.find("deqp")->second;
  if (args.find("caselist") == args.end())
    ctx.test_cases = get_testcases(ctx.deqp);
  else
    ctx.test_cases = parse_testcase_file(args.find("caselist")->second);

  std::mt19937 rng;
  std::shuffle(ctx.test_cases.begin(), ctx.test_cases.end(), rng);
  ctx.results.resize(ctx.test_cases.size());
  auto thread_count = std::thread::hardware_concurrency();
  ctx.taken_cases = 0;
  ctx.finished_cases = 0;
  ctx.pass_count = 0;
  ctx.fail_count = 0;
  ctx.skip_count = 0;
  ctx.crash_count = 0;
  ctx.start_time = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < thread_count; ++i) {
    threads.emplace_back([&ctx, i]() { thread_runner(ctx, i); });
  }

  while (ctx.finished_cases.load() < ctx.test_cases.size()) {
    update(ctx);
    sleep(1);
  }
  update(ctx);

  std::cout << "\n";

  for (auto &t : threads)
    t.join();

  std::ofstream out(args.find("output")->second);
  std::vector<std::pair<std::string, std::string>> sorted_results;
  for (std::size_t i = 0; i < ctx.test_cases.size(); ++i)
    sorted_results.push_back({ctx.test_cases[i], ctx.results[i]});
  std::sort(sorted_results.begin(), sorted_results.end());
  for (auto &&entry : sorted_results)
    out << entry.first << "," << entry.second << "\n";
  update(ctx);
  std::cout << "\n";
  return 0;
}

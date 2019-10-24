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
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <unordered_map>
#include <regex>

bool string_matches(const char *str1, const char *str2)
{
  return strncmp(str1, str2, strlen(str2)) == 0;
}

bool is_excluded_test(std::string const &testname,
                      const std::vector<std::regex> &excluded_tests)

{
  std::vector<std::regex>::const_iterator it;
  for (it = excluded_tests.begin(); it != excluded_tests.end(); ++it) {
    if (regex_match(testname, *it))
      return true;
  }
  return false;
}

std::vector<std::string> parse_testcase_file(std::string const &filename,
                                             const std::vector<std::regex> &excluded_tests) {
  std::vector<std::string> cases;
  std::ifstream in(filename);
  if (!in.is_open()) {
    std::cerr << "could not find testcase file \"" << filename << "\""
              << std::endl;
    std::exit(1);
  }

  std::string line;
  while (std::getline(in, line)) {
    if (!is_excluded_test(line, excluded_tests))
      cases.push_back(line);
  }
  return cases;
}

std::vector<std::string> get_testcases(std::string const &deqp,
                                       const std::vector<std::regex> &excluded_tests,
                                       const std::vector<std::string> &deqp_args) {
  std::string dir = std::filesystem::path(deqp).parent_path().native();
  FILE *f;
  char buf[4096];
  std::vector<std::string> cases;
  int fd[2];

  std::vector<std::string> args;
  args.push_back(deqp.c_str());
  args.push_back("--deqp-runmode=stdout-caselist");
  args.insert(args.end(), deqp_args.begin(), deqp_args.end());

  const char *c_args[args.size() + 1];
  for (unsigned i = 0; i < args.size(); i++) {
    c_args[i] = args[i].c_str();
  }
  c_args[args.size()] = NULL;

  if (pipe(fd))
    throw - 1;

  if (!fork()) {
    close(fd[0]);
    dup2(fd[1], 1);
    dup2(fd[1], 2);
    if (chdir(dir.c_str()))
      throw - 1;
    execv(deqp.c_str(), (char *const *)c_args);
  }
  close(fd[1]);
  f = fdopen(fd[0], "r");

  while (fgets(buf, 4096, f)) {
    if (string_matches(buf, "TEST: ")) {
      auto len = strlen(buf);
      std::string testname = std::string(buf + 6, buf + len - 1);

      if (!is_excluded_test(testname, excluded_tests))
        cases.push_back(testname);
    } else if (string_matches(buf, "FATAL ERROR: ")) {
      std::cerr << "Error getting caselist:\n";
      std::cerr << buf;
    }
  }
  fclose(f);
  return cases;
}

enum status {
  PASS,
  FAIL,
  SKIP,
  CRASH,
  UNDETERMINED,
  TIMEOUT,
  MISSING,
};
#define STATUS_COUNT ((int)MISSING + 1)

std::string get_status_name(enum status status)
{
  switch (status) {
  case PASS:
    return "Pass";
  case FAIL:
    return "Fail";
  case SKIP:
    return "Skip";
  case CRASH:
    return "Crash";
  case UNDETERMINED:
    return "Undetermined";
  case TIMEOUT:
    return "Timeout";
  case MISSING:
    return "Missing";
  default:
    abort();
  }
}

struct Context {
  unsigned device_id;
  std::string deqp;
  std::vector<std::string> test_cases;
  std::chrono::time_point<std::chrono::steady_clock> start_time;
  std::vector<enum status> results;
  std::atomic<std::size_t> taken_cases, finished_cases;
  double timeout = 60.0;
  std::vector<std::string> deqp_args;

  std::atomic<std::size_t> status_counts[STATUS_COUNT];

  Context() {
    for (int i = 0; i < STATUS_COUNT; i++)
      status_counts[i] = 0;
  }
};

class Line_reader {
public:
  ~Line_reader();

  void set_fd(int fd);

  enum read_status {
    SUCCESS,
    PARTIAL,
    END,
    TIMEOUT,
    FAIL

  };

  read_status read(char **text, double timeout);

private:
  int fd_ = -1;
  std::array<char, 4096> buf_;
  int valid_sz_ = 0;
  int skip_sz_ = 0;
};

Line_reader::~Line_reader() {
  if (fd_ >= 0)
    close(fd_);
}

void Line_reader::set_fd(int fd) {
  if (fd_ >= 0)
    close(fd_);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  fd_ = fd;
  valid_sz_ = 0;
  skip_sz_ = 0;
}

Line_reader::read_status Line_reader::read(char **text, double timeout) {
  assert(fd_ >= 0);

  if (skip_sz_) {
    valid_sz_ -= skip_sz_;
    std::memmove(buf_.data(), buf_.data() + skip_sz_, valid_sz_);
    skip_sz_ = 0;
  }

  char *newline;
  bool end = false;
  while (!(newline = (char *)std::memchr(buf_.data(), '\n', valid_sz_)) &&
         !end && valid_sz_ + 1 < (int)buf_.size()) {
    for (;;) {
      struct pollfd p = {};
      p.fd = fd_;
      p.events = POLLIN | POLLHUP;

      int r = poll(&p, 1, (int)(timeout * 1000.0));
      if (r > 0)
        break;
      else if (r == 0)
        return TIMEOUT;
      else if (r < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      else
        return FAIL;
    }
    for (;;) {
      ssize_t r =
          ::read(fd_, buf_.data() + valid_sz_, buf_.size() - 1 - valid_sz_);
      if (r >= 0) {
        valid_sz_ += r;
        end = r == 0;
        break;
      } else if (r < 0 && errno == EAGAIN)
        break;
      else if (r < 0 && errno != EINTR)
        return FAIL;
    }
  }

  if (newline) {
    skip_sz_ = newline - buf_.data() + 1;
    *newline = 0;
    *text = buf_.data();
    return SUCCESS;
  } else {
    buf_[valid_sz_] = 0;
    skip_sz_ = valid_sz_;
    *text = buf_.data();
    return end ? END : PARTIAL;
  }
}

class ProcessBlockState {
public:
  Context &ctx;
  std::map<std::string, unsigned> indices;

  bool start; /* Set if we are due to spawn a new CTS instance. */
  bool test_active; /* Set if we're between test start and test status */

  std::size_t test_idx; /* Index in the main test list of the active test. */

  void record_result(enum status status) {
    assert(test_active);
    ctx.results[test_idx] = status;
    ctx.status_counts[status]++;
    test_active = false;
  }

  ProcessBlockState(Context &ctx, size_t base_idx, size_t count)
    : ctx(ctx)
  {
    /* Make a list of all the tests to run and their indices in the main
     * test list.  As we see the CTS start running a test, we'll erase
     * that entry.
     */
    for (std::size_t i = 0; i < count; ++i)
      indices.insert({ctx.test_cases[base_idx + i], base_idx + i});

    start = true;
    test_active = false;
    test_idx = 0;
  }
};

bool process_block(Context &ctx, unsigned thread_id) {
  /* Use cmpxchg to take a block of tests from the whole list */
  std::size_t base_idx, count;
  do {
    base_idx = ctx.taken_cases.load();
    if (base_idx >= ctx.test_cases.size())
      return false;
    count = std::min<std::size_t>(
        std::max<std::size_t>((ctx.test_cases.size() - base_idx) / 32, 1), 128);

  } while (
      !ctx.taken_cases.compare_exchange_strong(base_idx, base_idx + count));

  std::string filename = "/tmp/cts_runner." + std::to_string(getpid()) + "." +
                         std::to_string(thread_id);
  std::string dir = std::filesystem::path(ctx.deqp).parent_path().native();
  Line_reader reader;
  bool before_first_test = true;

  ProcessBlockState state(ctx, base_idx, count);

  /* Loop running the test suite on the remaining tests to run until
   * we have processed them all.  This may take more than one run in
   * the case of crashes.
   */
  while (state.test_active || !state.indices.empty()) {
    if (state.start) {
      int fd[2];
      std::ofstream out(filename);
      for (auto &&e : state.indices)
        out << e.first << "\n";
      out.close();

      std::vector<std::string> args;
      args.push_back(ctx.deqp.c_str());
      args.push_back("--deqp-log-images=disable");
      args.push_back("--deqp-log-shader-sources=disable");
      args.push_back("--deqp-log-flush=disable");
      args.push_back("--deqp-shadercache=disable");
      args.push_back("--deqp-log-filename=/dev/null");
      args.push_back("--deqp-caselist-file=" + filename);
      args.push_back("--deqp-vk-device-id=" + std::to_string(ctx.device_id));
      args.insert(args.end(), ctx.deqp_args.begin(), ctx.deqp_args.end());

      const char *c_args[args.size() + 1];
      for (unsigned i = 0; i < args.size(); i++) {
        c_args[i] = args[i].c_str();
      }
      c_args[args.size()] = NULL;

      if (pipe2(fd, O_CLOEXEC))
        throw - 1;

      if (!fork()) {
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        if (chdir(dir.c_str()))
          throw - 1;

        execv(ctx.deqp.c_str(), (char *const *)c_args);
      }
      close(fd[1]);
      reader.set_fd(fd[0]);
      state.start = false;
      state.test_active = false;
      before_first_test = true;
    }
    char *line = NULL;
    auto r = reader.read(&line, ctx.timeout);

    if (r == Line_reader::FAIL) {
      abort();
    } else if (r == Line_reader::TIMEOUT) {
      state.start = true;
      if (state.test_active)
        state.record_result(TIMEOUT);
    } else if (r == Line_reader::END || string_matches(line, "DONE!")) {
      state.start = true;
      if (state.test_active) {
        state.record_result(CRASH);
      } else if(before_first_test) {
        while(!state.indices.empty()) {
          auto it = state.indices.begin();
          state.test_idx = it->second;
          state.test_active = true;
          state.indices.erase(it);

          state.record_result(MISSING);
        }
      }
    } else if (string_matches(line, "  NotSupported")) {
      state.record_result(SKIP);
    } else if (string_matches(line, "  Fail")) {
      state.record_result(FAIL);
    } else if (string_matches(line, "  Pass") ||
               string_matches(line, "  CompatibilityWarning") ||
               string_matches(line, "  QualityWarning")) {
      state.record_result(PASS);
    } else if (string_matches(line, "Test case '")) {
      auto len = strlen(line) - 3;
      auto name = std::string(line + 11, line + len);
      auto it = state.indices.find(name);
      if (it == state.indices.end()) {
        abort();
      }
      state.test_idx = it->second;
      state.indices.erase(it);
      state.test_active = true;
      before_first_test = false;
    } else if (string_matches(line, "FATAL ERROR: ")) {
      if (state.test_active) {
        state.record_result(FAIL);
      } else {
        std::cerr << line;
        std::cerr << "\n";
        abort();
      }
    }
  }
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
  std::size_t status_counts[STATUS_COUNT];
  /* Snapshot the status counts from the context, so our numbers in
   * the report add up even if the atomics are being updated.
   */
  std::size_t total = 0;
  for (int i = 0; i < STATUS_COUNT; i++) {
    status_counts[i] = ctx.status_counts[i];
    total += status_counts[i];
  }

  std::chrono::time_point<std::chrono::steady_clock> current_time =
      std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::duration<double>>(
      current_time - ctx.start_time);

  std::cout << "[" << total << "/" << ctx.test_cases.size() << "]";
  for (int i = 0; i < STATUS_COUNT; i++) {
    std::cout << " "
              << get_status_name((enum status)i)
              << ": "
              << status_counts[i];
  }
  std::cout << " Duration: " << format_duration(duration.count());
  if (total) {
    std::cout << " Remaining: "
              << format_duration(duration.count() / total *
                                 (ctx.test_cases.size() - total));
  }
  std::cout << "                  \r";
  std::cout.flush();
}

void
usage(char *progname)
{
  std::cerr << "Usage: " << progname << "\n";
  std::cerr << "    --deqp <deqp-vk or deqp-gles* binary>\n";
  std::cerr << "    --output <log filename>\n";
  std::cerr << "    [--caselist <mustpass.txt>]\n";
  std::cerr << "    [--timeout <seconds>]\n";
  std::cerr << "    [--exclude-tests <regex,regex,...>]\n";
  std::cerr << "    [--job <threadcount>]\n";
  std::cerr << "    [--device <vk device id>]\n";
  std::cerr << "    [-- --deqp-binary-arguments]\n";
  std::cerr << "\n";

  std::cerr << "--timeout defaults to 60 seconds of no new dEQP output\n";
  std::cerr << "--job defaults to CPU thread count\n";
  exit(1);
}

std::map<std::string, std::string> parse_args(int argc, char *argv[],
					      std::vector<std::string> *deqp_args) {
  std::map<std::string, std::string> args;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-' || argv[i][1] != '-') {
      std::cerr << "arguments have to start with '--'\n";
      usage(argv[0]);
    }
    auto eq = strchr(argv[i], '=');
    if (eq) {
      std::string arg_name{argv[i] + 2, eq};
      std::string value = eq + 1;
      if (args.find(arg_name) != args.end()) {
        std::cerr << "specified '" << arg_name << "' twice.\n";
	usage(argv[0]);
      }
      args[arg_name] = value;
    } else if (strcmp(argv[i], "--") != 0) {
      std::string arg_name = argv[i] + 2;
      ++i;
      if (i == argc) {
        std::cerr << "missing value for argument '" << arg_name << "'\n";
	usage(argv[0]);
      }
      if (args.find(arg_name) != args.end()) {
        std::cerr << "specified '" << arg_name << "' twice.\n";
	usage(argv[0]);
      }
      args[arg_name] = argv[i];
    } else {
      ++i;
      for (; i < argc; ++i) {
	deqp_args->push_back(argv[i]);
      }
    }
  }
  return args;
}

int main(int argc, char *argv[]) {
  signal(SIGCHLD, SIG_IGN);

  std::vector<std::string> deqp_args;
  auto args = parse_args(argc, argv, &deqp_args);
  if (args.find("deqp") == args.end()) {
    std::cerr << "--deqp missing\n";
    usage(argv[0]);
  }
  if (args.find("output") == args.end()) {
    std::cerr << "--output missing\n";
    usage(argv[0]);
  }

  std::vector<std::regex> excluded_tests;
  if (args.find("exclude-tests") != args.end()) {
    std::istringstream excluded(args.find("exclude-tests")->second);

    while (excluded.good()) {
      std::string test;
      getline(excluded, test, ',');
      excluded_tests.push_back(std::regex(test));
    }
  }

  Context ctx;
  ctx.deqp = args.find("deqp")->second;
  ctx.deqp_args = deqp_args;
  if (args.find("caselist") == args.end()) {
    ctx.test_cases = get_testcases(ctx.deqp, excluded_tests, deqp_args);
  } else {
    ctx.test_cases = parse_testcase_file(args.find("caselist")->second,
                                         excluded_tests);
  }

  if (args.find("timeout") != args.end())
    ctx.timeout = strtof(args.find("timeout")->second.c_str(), NULL);

  int job = 0;
  if (args.find("job") != args.end())
    job = strtol(args.find("job")->second.c_str(), NULL, 10);

  ctx.device_id = 1;
  if (args.find("device") != args.end())
    ctx.device_id = strtol(args.find("device")->second.c_str(), NULL, 10);

  /* Shuffle the tests so that the groups that our threads grab are
   * more or less evenly distributed in test namespace (since test
   * runtime is closely related to test name).
   */
  std::mt19937 rng;
  std::shuffle(ctx.test_cases.begin(), ctx.test_cases.end(), rng);
  ctx.results.resize(ctx.test_cases.size());
  for (unsigned i = 0; i < ctx.test_cases.size(); i++)
    ctx.results[i] = UNDETERMINED;
  auto thread_count = job > 0 ? job : std::thread::hardware_concurrency();
  ctx.taken_cases = 0;
  ctx.finished_cases = 0;
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
  std::vector<std::pair<std::string, enum status>> sorted_results;
  for (std::size_t i = 0; i < ctx.test_cases.size(); ++i)
    sorted_results.push_back({ctx.test_cases[i], ctx.results[i]});
  std::sort(sorted_results.begin(), sorted_results.end());
  for (auto &&entry : sorted_results)
    out << entry.first << "," << get_status_name(entry.second) << "\n";
  update(ctx);
  std::cout << "\n";

  for (int i = 0; i < STATUS_COUNT; i++) {
    switch ((enum status)i) {
    case PASS:
    case SKIP:
      break;
    default:
      if (ctx.status_counts[i] != 0)
        return 1;
    }
  }

  return 0;
}

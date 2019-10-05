/*
 * Copyright © 2019 Google LLC
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

#include <cassert>
#include <cstring>
#include <string>
#include <fstream>
#include <iostream>

enum status {
  STATUS_PASS,
  STATUS_FAIL,
};

bool string_matches(const char *str1, const char *str2)
{
  return strncmp(str1, str2, strlen(str2)) == 0;
}

enum status mock_pass(std::string testcase)
{
  return STATUS_PASS;
}

enum status mock_fail(std::string testcase)
{
  if (testcase == "dEQP-GLES2.functional.test.3")
    return STATUS_FAIL;

  return STATUS_PASS;
}

enum status mock_crash(std::string testcase)
{
  if (testcase == "dEQP-GLES2.functional.test.2") {
    std::cout.flush();
    abort();
  }

  return STATUS_PASS;
}

enum status mock_missingtest(std::string testcase)
{
  return STATUS_PASS;
}

typedef enum status (*test_fn)(std::string testcase);

test_fn parse_mock_mode(std::string mode)
{
  if (mode == "pass")
    return mock_pass;
  else if (mode == "fail")
    return mock_fail;
  else if (mode == "crash")
    return mock_crash;
  else if (mode == "missingtest")
    return mock_missingtest;
  else
    abort(); /* early-crash */
}

int main(int argc, char *argv[]) {
  std::cout << "dEQP Mock starting..\n";

  enum status (*mock_test)(std::string testcase) = NULL;
  std::string caselist_path;
  for (int i = 1; i < argc; ++i) {
    const char *caselist_file = "--deqp-caselist-file=";
    const char *deqp_mock_mode = "--deqp-mock-mode=";

    if (string_matches(argv[i], caselist_file)) {
      caselist_path = argv[i] + strlen(caselist_file);
    } else if (string_matches(argv[i], deqp_mock_mode)) {
      mock_test = parse_mock_mode(argv[i] + strlen(deqp_mock_mode));
    }
  }

  assert(caselist_path.size() != 0);
  assert(mock_test != NULL);

  std::ifstream caselist(caselist_path);
  std::string testname;
  while (std::getline(caselist, testname)) {
    /* For the missing test mock, we need to not even print out the
     * test case name.
     */
    if (mock_test == mock_missingtest &&
        testname == "dEQP-GLES2.functional.test.1") {
      continue;
    }

    std::cout << "Test case '" << testname << "'..\n";

    switch (mock_test(testname)) {
    case STATUS_PASS:
      std::cout << "  Pass (Pass)\n";
      break;
    case STATUS_FAIL:
      std::cout << "  Fail (Fail)\n";
      break;
    }
  }

  std::cout << "DONE!";

  return 0;
}

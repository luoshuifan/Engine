// Copyright 2017 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include "common/path_helper.h"

#include <assert.h>
#include <stdlib.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <libgen.h>
#endif

#include <stdlib.h>
#include <string.h>

namespace google_breakpad {

string BaseName(const string& path) {
#if defined(_WIN32) || defined(_WIN64)
  char fname[256] = { 0 };
  _splitpath_s(path.c_str(), NULL, 0, NULL, 0, fname, sizeof(fname), NULL, 0);
  string result(fname);
#else
  char* path_tmp = strdup(path.c_str());
  assert(path_tmp);
  string result(basename(path_tmp));
  free(path_tmp);
#endif
  return result;
}

string DirName(const string& path) {
#if defined(_WIN32) || defined(_WIN64)
  char dir[256] = { 0 };
  _splitpath_s(path.c_str(), NULL, 0, dir, sizeof(dir), NULL, 0, NULL, 0);
  string result(dir);
#else
  char* path_tmp = strdup(path.c_str());
  assert(path_tmp);
  string result(dirname(path_tmp));
  free(path_tmp);
#endif
  return result;
}

}  // namespace google_breakpad

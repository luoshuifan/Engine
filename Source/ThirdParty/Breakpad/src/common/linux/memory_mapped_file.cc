// Copyright 2011 Google LLC
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

// memory_mapped_file.cc: Implement google_breakpad::MemoryMappedFile.
// See memory_mapped_file.h for details.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include "common/linux/memory_mapped_file.h"

#include <fcntl.h>
#if defined(__ANDROID__)
#include <sys/stat.h>
#endif

#include "common/os_handle.h"
#include "common/memory_range.h"

#if defined(__linux__)
#include "third_party/lss/linux_syscall_support.h"
#endif

namespace google_breakpad {

MemoryMappedFile::MemoryMappedFile() {}

MemoryMappedFile::MemoryMappedFile(const char* path, size_t offset) {
  Map(path, offset);
}

MemoryMappedFile::~MemoryMappedFile() {
  Unmap();
}

bool MemoryMappedFile::Map(const char* path, size_t offset) {
  Unmap();

#if defined(__linux__)
  // Based on https://pubs.opengroup.org/onlinepubs/7908799/xsh/open.html
  // If O_NONBLOCK is set: The open() function will return without blocking
  // for the device to be ready or available. Setting this value will provent
  // hanging if file is not avilable.
  OSHandle fd = sys_open(path, O_RDONLY | O_NONBLOCK, 0);
#else
  OSHandle fd = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
#endif

  if (fd == OSHandleInvalid) {
    return false;
  }

#if defined(__linux__)
#if defined(__x86_64__) || defined(__aarch64__) || \
   (defined(__mips__) && _MIPS_SIM == _ABI64) || \
   (defined(__riscv) && __riscv_xlen == 64)

  struct kernel_stat st;
  if (sys_fstat(fd, &st) == -1 || st.st_size < 0) {
#else
  struct kernel_stat64 st;
  if (sys_fstat64(fd, &st) == -1 || st.st_size < 0) {
#endif
    sys_close(fd);
    return false;
  }
  // Strangely file size can be negative, but we check above that it is not.
  size_t file_len = static_cast<size_t>(st.st_size);
  // If the file does not extend beyond the offset, simply use an empty
  // MemoryRange and return true. Don't bother to call mmap()
  // even though mmap() can handle an empty file on some platforms.
  if (offset >= file_len) {
    sys_close(fd);
    return true;
  }

  size_t content_len = file_len - offset;
  void* data = sys_mmap(NULL, content_len, PROT_READ, MAP_PRIVATE, fd, offset);
  sys_close(fd);
  if (data == MAP_FAILED) {
    return false;
  }
#else
  int64_t file_len = GetOSHandleSize(fd);
  if (file_len <= 0) {
    return false;
  }

  size_t content_len = file_len;

  // Not sure the *proper* way to do this on windows
  void* data = CreateOSMapping(NULL, file_len, PAGE_EXECUTE_READWRITE, FILE_MAP_READ, fd, 0);

#endif

  if (data == nullptr) {
    return false;
  }

  content_.Set(data, content_len);
  return true;
}

void MemoryMappedFile::Unmap() {
  if (content_.data()) {
#if defined(__linux__)
    sys_munmap(const_cast<uint8_t*>(content_.data()), content_.length());
#else
    CloseOSMapping(const_cast<uint8_t*>(content_.data()));
#endif
    content_.Set(NULL, 0);
  }
}

}  // namespace google_breakpad

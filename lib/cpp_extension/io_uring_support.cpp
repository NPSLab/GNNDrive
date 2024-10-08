#include <torch/extension.h>
#include <errno.h>
#include <linux/io_uring.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>

// From https://unix.stackexchange.com/a/596284
bool io_uring_support() {
  if (syscall(__NR_io_uring_register, 0, IORING_UNREGISTER_BUFFERS, NULL, 0) && errno == ENOSYS) {
    return false;
  } else {
    return true;
  }
}

PYBIND11_MODULE(io_uring_support, m)
{
    m.def("io_uring_support", []() {
        return io_uring_support();
    });
}
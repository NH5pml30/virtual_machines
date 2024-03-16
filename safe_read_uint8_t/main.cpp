#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>

#include <cstdint>
#include <iostream>
#include <optional>
#include <random>

sigjmp_buf jump_buffer;
bool safe_handler_expected = false;

void safe_handler(int) {
  // fire only once
  if (safe_handler_expected) {
    safe_handler_expected = false;
    siglongjmp(jump_buffer, 1);
  }
}

struct sigaction_guard {
  int signum;
  struct sigaction *act, old_act{};
  sigaction_guard(int signum, struct sigaction *act)
      : signum(signum), act(act) {
    sigaction(signum, act, &old_act);
  }

  ~sigaction_guard() { sigaction(signum, &old_act, NULL); }
};

std::optional<uint8_t> safe_read_uint8_t(const uint8_t *p) {
  struct sigaction segv_act{}, bus_act{};
  segv_act.sa_handler = &safe_handler;
  bus_act.sa_handler = &safe_handler;
  sigemptyset(&segv_act.sa_mask);
  sigemptyset(&bus_act.sa_mask);
  sigaddset(&(segv_act.sa_mask), SIGBUS);
  sigaddset(&(bus_act.sa_mask), SIGSEGV);
  sigaction_guard segv_guard(SIGSEGV, &segv_act), bus_guard(SIGBUS, &bus_act);

  if (sigsetjmp(jump_buffer, 1) == 0) {
    safe_handler_expected = true;
    uint8_t val = *p;
    safe_handler_expected = false;
    return val;
  }
  return {};
}

uint8_t global_var;

uint8_t custom_handler_flag = 0;
void custom_handler(int) {
  custom_handler_flag = 1;
}

int main() {
  auto print_opt = [](std::ostream &o,
                      std::optional<uint8_t> opt) -> std::ostream & {
    if (opt.has_value())
      return o << "{" << std::hex << (int)*opt << "}";
    else
      return o << "{}";
  };

  auto print_addr_opt = [&](std::ostream &o, const uint8_t *addr,
                            std::optional<uint8_t> opt) -> std::ostream & {
    return print_opt(o << "*" << (void *)addr << " == ", opt);
  };

  auto check_opt = [&](const uint8_t *addr, std::optional<uint8_t> expected) {
    auto res = safe_read_uint8_t(addr);
    if (res != expected) {
      print_addr_opt(std::cerr << "Error: expected ", addr, expected)
          << ", got ";
      print_opt(std::cerr, res) << std::endl;
      abort();
    } else {
      print_addr_opt(std::cerr, addr, res) << std::endl;
    }
  };

  auto test_opt = [&](const uint8_t *addr) {
    auto res = safe_read_uint8_t(addr);
    print_addr_opt(std::cerr, addr, res) << std::endl;
  };

  // Set up custom signal handlers
  struct sigaction segv_act{}, bus_act{};
  segv_act.sa_handler = &custom_handler;
  bus_act.sa_handler = &custom_handler;
  sigemptyset(&segv_act.sa_mask);
  sigemptyset(&bus_act.sa_mask);
  sigaction_guard segv_guard(SIGSEGV, &segv_act), bus_guard(SIGBUS, &bus_act);

  // Random accesses
  {
    std::default_random_engine eng;
    std::uniform_int_distribution<uintptr_t> dist;
    for (int i = 0; i < 10; i++)
      test_opt((uint8_t *)dist(eng));
  }

  // Nullptr access
  check_opt(nullptr, {});

  // Stack access
  {
    uint8_t var = 30;
    check_opt(&var, 30);
  }

  // Code access
  test_opt((uint8_t *)&main);

  // Global variable access
  global_var = 90;
  check_opt(&global_var, 90);

  // String literal access
  check_opt((uint8_t *)"string literal", 's');

  // No read access
  {
    uint8_t *mem = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *mem = 'a';
    check_opt(mem, 'a');
    mprotect(mem, 4096, PROT_NONE);
    check_opt(mem, {});
  }

  // SIGBUS past the end of an object
  {
    const char *name = "/a";
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT, (mode_t)0600);
    // ftruncate(fd, 1);
    uint8_t *mem =
        (uint8_t *)mmap(NULL, 1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check_opt(mem, {});
  }

  // Check custom signal handlers
  custom_handler_flag = 0;
  raise(SIGSEGV);
  check_opt(&custom_handler_flag, 1);
  custom_handler_flag = 0;
  raise(SIGBUS);
  check_opt(&custom_handler_flag, 1);

  return 0;
}

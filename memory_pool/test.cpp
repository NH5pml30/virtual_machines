#include <sys/mman.h>
#include <sys/resource.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <stdint.h>
#include <vector>

constexpr size_t PAGE_SIZE = 4096;
struct pool_allocator_base {
  pool_allocator_base(size_t sz) {
    // Set rlimit, otherwise only 2048 pages can be allocated for the "stack"
    rlimit rl{};
    int result = getrlimit(RLIMIT_STACK, &rl);
    assert(result == 0);
    if (rl.rlim_cur < sz) {
      rl.rlim_cur = sz;
      result = setrlimit(RLIMIT_STACK, &rl);
      assert(result == 0);
    }

    // Find free virtual memory range
    char *mem = (char *)mmap(NULL, sz, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem);
    munmap(mem, sz);
    // mmap at the last page of the range, set first_free above
    bottom = mem;
    mem += sz -= PAGE_SIZE;
    first_free =
        (char *)mmap(mem, PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE | MAP_GROWSDOWN,
                     -1, 0) +
        PAGE_SIZE;
  }

  char *first_free{};
  char *bottom{};

  static pool_allocator_base instance;
};

constexpr size_t RESERVED_PAGES = 1024 * 1024;
pool_allocator_base pool_allocator_base::instance(PAGE_SIZE * RESERVED_PAGES);

template <typename T> struct pool_allocator {
  using value_type = T;

  pool_allocator() = default;
  template <class U> pool_allocator(const pool_allocator<U> &) {}

  T *allocate(size_t n) {
    auto *res = (T *)(pool_allocator_base::instance.first_free -= sizeof(T) * n);
    if ((char *)res < pool_allocator_base::instance.bottom)
      throw std::bad_alloc();
    return res;
  }

  void deallocate(T *ptr, size_t n) {}
};

template <typename T, typename U>
inline bool operator==(const pool_allocator<T> &, const pool_allocator<U> &) {
  return true;
}

template <typename T, typename U>
inline bool operator!=(const pool_allocator<T> &a, const pool_allocator<U> &b) {
  return !(a == b);
}

#ifdef _WIN32
#include "psapi.h"
#include "windows.h"
static int64_t memory_usage(void) {
  PROCESS_MEMORY_COUNTERS_EX pmc;
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc,
                       sizeof(pmc));
  return pmc.PrivateUsage;
}
#else
#include <fstream>
using namespace std;
static uint64_t memory_usage(void) {
  enum { PAGE_SIZE = 4096 };
  ifstream ifs("/proc/self/statm");
  uint64_t m;
  ifs >> m;
  return m * PAGE_SIZE;
}
#endif

using namespace std;

struct Edge {
  Edge *next;
  int node_id;
};

template <typename Allocator = std::allocator<void>> class Graph {
  using Traits = std::allocator_traits<Allocator>;
  using PtrAlloc = typename Traits::template rebind_alloc<Edge *>;
  using EdgeAlloc = typename Traits::template rebind_alloc<Edge>;
  using EdgeTraits = std::allocator_traits<EdgeAlloc>;
  vector<Edge *, PtrAlloc> nodes;
  [[no_unique_address]] EdgeAlloc alloc;

public:
  Graph(int n, Allocator alloc = Allocator())
      : nodes(n, nullptr, alloc), alloc(alloc) {}
  ~Graph() {
    for (const auto &node : nodes) {
      for (Edge *p = node; p;) {
        Edge *next = p->next;
        EdgeTraits::destroy(alloc, p);
        EdgeTraits::deallocate(alloc, p, 1);
        p = next;
      }
    }
  }
  void connect(Edge *&from, int to) {
    auto *ptr = EdgeTraits::allocate(alloc, 1);
    EdgeTraits::construct(alloc, ptr, Edge{from, to});
    from = ptr;
  }
  void connect(int from, int to) { connect(nodes[from], to); }
  void build_complete_dighraph() {
    const auto n = nodes.size();
    for (int i = 0; i < n; i++)
      for (int j = 0; j < n; j++)
        if (i != j)
          connect(i, j);
  }
};

template <typename Allocator = std::allocator<void>>
static inline int64_t test(const Allocator &alloc = Allocator()) {
  const auto start = memory_usage();
  Graph<Allocator> g(10000, alloc);
  g.build_complete_dighraph();
  return memory_usage() - start;
}

int main(const int argc, const char *argv[]) {
  const auto start = chrono::high_resolution_clock::now();
  const auto memory_used = test(
#ifdef USE_POOL
    pool_allocator<void>()
#endif
  );
  const auto end = chrono::high_resolution_clock::now();
  cout << "Memory used: " << memory_used << " bytes\n";
  cout << "Time used: "
       << chrono::duration_cast<chrono::nanoseconds>(end - start).count()
       << " ns\n";
  return 0;
}

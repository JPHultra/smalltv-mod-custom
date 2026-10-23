// Peak heap of loading a theme package, counted the way the ESP8266 heap
// (umm_malloc) charges it: every allocation takes whole 8-byte blocks plus a
// 4-byte header. Build with -m32 so pointers, std::string and std::vector have
// the device's sizes. The device has roughly 6.5 KiB free while a theme runs,
// and listing or installing a theme loads a second one next to it.
#include "BlockList.h"
#include "ThemePackage.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <malloc.h>
#include <new>
using namespace smalltv;

static size_t live = 0, peak = 0, largest = 0;
static bool counting = false;
static size_t charge(void* p) { return p ? (malloc_usable_size(p) + 4 + 7) / 8 * 8 : 0; }
static void* track(void* p, size_t requested) {
  if (counting && p) {
    live += charge(p);
    if (live > peak) peak = live;
    if (requested > largest) largest = requested;
  }
  return p;
}
static void untrack(void* p) { if (counting && p) live -= charge(p); }
extern "C" {
void* __real_malloc(size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
void* __wrap_malloc(size_t n) { return track(__real_malloc(n), n); }
void __wrap_free(void* p) { untrack(p); __real_free(p); }
void* __wrap_realloc(void* p, size_t n) {
  untrack(p);
  return track(__real_realloc(p, n), n);
}
}
void* operator new(size_t n) {
  void* p = __wrap_malloc(n);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { __wrap_free(p); }
void operator delete(void* p, size_t) noexcept { __wrap_free(p); }

struct FileSource : Source {
  std::string bytes;
  explicit FileSource(const char* path) {
    std::ifstream in(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  uint32_t size() const override { return bytes.size(); }
  bool read(uint32_t offset, void* out, size_t count) override {
    if (offset > bytes.size() || count > bytes.size() - offset) return false;
    memcpy(out, bytes.data() + offset, count);
    return true;
  }
};

// Both limits come from a real SmallTV-Ultra. With the running theme
// released and the web server holding its 2 KiB upload buffer, the lowest
// free heap seen when an install starts validating was 8,400 bytes (three
// runs); a load must stay well inside that, so the budget leaves 3.3 KiB
// spare. And a load must never need a block so large that a fragmented heap
// cannot supply it: 2340-byte requests failed with 6.6 KiB free but no free
// block above 1.8 KiB.
constexpr size_t LoadPeakBudget = 5120;
constexpr size_t LargestBlockBudget = 1024;

struct Usage { size_t peak, running, theme, largest; };
// Peak while loading; what a running theme holds (the Theme plus the
// package's entry index, which ThemeMode keeps for reading assets); what the
// Theme alone holds; and the biggest single request (the device needs that
// much in one contiguous block).
static Usage load(const char* path) {
  FileSource source(path);
  assert(!source.bytes.empty());
  Theme theme;
  std::string error;
  Usage usage{};
  live = peak = largest = 0;
  counting = true;
  {
    Package package(source);
    bool ok = package.load(theme, error);
    if (!ok) std::cerr << path << ": " << error << "\n";
    assert(ok);
    usage.running = live;
  }
  usage.peak = peak;
  usage.theme = live;
  usage.largest = largest;
  counting = false;
  return usage;
}

// BlockList: the vector subset the engine uses, without one large block.
static void testBlockList() {
  BlockList<int> numbers;
  assert(numbers.size() == 0);
  numbers.reserve(40);
  assert(numbers.size() == 0);
  for (int i = 0; i < 40; ++i) numbers.push_back(i);
  assert(numbers.size() == 40 && numbers[0] == 0 && numbers[39] == 39);
  int expected = 0;
  for (int n : numbers) assert(n == expected++);
  assert(expected == 40);
  const BlockList<int>& constant = numbers;
  expected = 0;
  for (const int& n : constant) assert(n == expected++);
  numbers[5] = 50;
  BlockList<int> copy(numbers);
  assert(copy.size() == 40 && copy[5] == 50 && copy[39] == 39);
  copy.assign(3, 7);
  assert(copy.size() == 3 && copy[0] == 7 && copy[2] == 7 && numbers.size() == 40);
  copy = numbers;
  assert(copy.size() == 40 && copy[5] == 50);
  BlockList<int> moved(std::move(copy));
  assert(moved.size() == 40 && moved[39] == 39);
  BlockList<int> grown;
  int& made = grown.emplace_back();
  made = 9;
  assert(grown.size() == 1 && grown[0] == 9 && &grown[0] == &made);
  BlockList<int> listed = {1, 2, 3};
  assert(listed.size() == 3 && listed[2] == 3);
  listed = {4, 5};
  assert(listed.size() == 2 && listed[0] == 4 && listed[1] == 5);

  // A full theme's worth of layers: elements never move, and no single
  // request is larger than the budget, however many layers there are.
  live = peak = largest = 0;
  counting = true;
  {
    BlockList<Layer> layers;
    layers.reserve(MaxLayers);
    Layer first;
    first.id = "first";
    layers.push_back(first);
    const Layer* address = &layers[0];
    for (size_t i = 1; i < MaxLayers; ++i) layers.push_back(Layer());
    assert(&layers[0] == address && layers[0].id == "first" && layers.size() == MaxLayers);
  }
  counting = false;
  if (largest > LargestBlockBudget) fprintf(stderr, "BlockList<Layer>: largest block %zu B\n", largest);
  assert(largest <= LargestBlockBudget);

  // With the count reserved, a short list takes what its elements need, not
  // whole blocks: two blocks (4 + 1 layers) plus the small block index and
  // per-allocation headers, well under two full blocks (8 layers).
  live = 0;
  counting = true;
  {
    BlockList<Layer> five;
    five.reserve(5);
    for (int i = 0; i < 5; ++i) five.push_back(Layer());
    size_t kept = live;
    if (kept > 5 * sizeof(Layer) + 64) fprintf(stderr, "five layers keep %zu B, sizeof(Layer) %zu\n", kept, sizeof(Layer));
    assert(kept <= 5 * sizeof(Layer) + 64 && kept < 2 * BlockList<Layer>::PerBlock * sizeof(Layer));
  }
  counting = false;
}

int main(int argc, char** argv) {
  testBlockList();
  const char* examples[] = {"examples/themes/pixel-room.stheme", "examples/themes/live-status.stheme",
                            "examples/themes/terminal-ops.stheme"};
  bool report = argc > 1;
  for (int i = 1; i < argc; ++i) {
    Usage u = load(argv[i]);
    printf("%-36s peak %5zu B  running %5zu B  theme %5zu B  largest block %5zu B\n",
           argv[i], u.peak, u.running, u.theme, u.largest);
  }
  if (report) return 0;
  for (const char* path : examples) {
    Usage u = load(path);
    if (u.peak > LoadPeakBudget || u.largest > LargestBlockBudget)
      fprintf(stderr, "%s: peak %zu B (budget %zu), largest block %zu B (budget %zu)\n",
              path, u.peak, LoadPeakBudget, u.largest, LargestBlockBudget);
    assert(u.peak <= LoadPeakBudget);
    assert(u.largest <= LargestBlockBudget);
  }
  std::cout << "theme memory tests passed\n";
}

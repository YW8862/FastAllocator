# FastAllocator

`FastAllocator` 是一个用 C++ 实现的教学型高性能内存分配器，整体思路参考 `Thread Cache + Central Cache + Page Cache + Radix Tree` 的分层设计。

当前版本已经具备以下能力：

- 小对象分配与回收
- 大对象直接按页分配
- `malloc/free/realloc` 风格 API
- 调试模式：越界检测、double free 检测、分配/释放填充
- 统计系统：全局统计与线程级汇总
- 单元测试与基础 benchmark

## 目录结构

```text
include/allocator/   对外头文件与核心数据结构
src/                 分配器实现
test/                单元测试与 benchmark
doc/                 设计文档
```

## 当前模块

- `SizeClass`：负责 size class 映射、对齐与批量搬运参数
- `ThreadCache`：线程本地小对象缓存
- `CentralCache`：线程之间共享的中间层缓存
- `PageCache`：页级内存管理
- `RadixTree`：地址到 `Span` 的快速映射
- `allocator`：统一封装初始化、分配、释放、重分配、调试检测与统计

## 对外接口

头文件：`include/allocator/allocator.h`

主要接口如下：

```cpp
namespace fastalloc {

void Initialize();
void initialize();

void* Allocate(std::size_t size);
void Deallocate(void* ptr);
void* Reallocate(void* ptr, std::size_t size);

Stats GetStats();
Stats get_stats();

void* malloc(std::size_t size);
void free(void* ptr);
void* realloc(void* ptr, std::size_t size);

}  // namespace fastalloc
```

## Quick Start


### 方式一：直接拷贝到你的项目

假设你的新项目目录如下：

```text
your_project/
├── third_party/
│   └── FastAllocator/
│       ├── include/
│       └── src/
└── app/
    └── main.cpp
```

你可以把当前仓库中的 `include/` 和 `src/` 复制到 `third_party/FastAllocator/` 下，然后在你的工程里包含头文件：

```cpp
#include "allocator/allocator.h"
```

编译时把 FastAllocator 的源文件一起编进来：

```bash
g++ -std=c++17 -pthread \
    -Ithird_party/FastAllocator/include \
    app/main.cpp \
    third_party/FastAllocator/src/*.cpp \
    -o your_app
```

### 方式二：在现有工程中直接引用当前仓库源码

如果你的项目和当前仓库放在同一台机器上，也可以不拷贝源码，直接在编译命令里引用：

```bash
g++ -std=c++17 -pthread \
    -I/path/to/FastAllocator/include \
    app/main.cpp \
    /path/to/FastAllocator/src/*.cpp \
    -o your_app
```

这种方式适合本地实验，但不太适合长期维护，因为你项目会依赖外部路径。

### 方式三：先打包成静态库，再给外部项目链接

如果你希望像普通第三方库一样复用 `FastAllocator`，可以先把它编译成静态库 `libfastallocator.a`，然后在外部项目中链接这个库。

假设你先在当前仓库根目录执行：

```bash
mkdir -p build
g++ -std=c++17 -pthread -Iinclude -c src/*.cpp
ar rcs build/libfastallocator.a ./*.o
rm -f ./*.o
```

执行完成后，你会得到：

```text
FastAllocator/
├── build/
│   └── libfastallocator.a
└── include/
    └── allocator/
```

然后在你的外部项目中这样组织目录：

```text
your_project/
├── third_party/
│   └── FastAllocator/
│       ├── build/
│       │   └── libfastallocator.a
│       └── include/
└── app/
    └── main.cpp
```

在代码中照常包含头文件：

```cpp
#include "allocator/allocator.h"
```

编译外部项目时链接这个静态库：

```bash
g++ -std=c++17 -pthread \
    -Ithird_party/FastAllocator/include \
    app/main.cpp \
    third_party/FastAllocator/build/libfastallocator.a \
    -o your_app
```

这种方式更接近真实项目接入流程，优点是：

- 外部项目不需要每次重新编译 `FastAllocator` 的所有 `src/*.cpp`
- 工程依赖关系更清晰
- 更容易进一步演进成 `CMake` 或正式库发布方式

如果后续你修改了 `FastAllocator` 源码，需要重新执行一次打包命令，更新 `libfastallocator.a`。

### 最小使用示例

```cpp
#include "allocator/allocator.h"

#include <cstring>
#include <iostream>

int main() {
    fastalloc::initialize();

    void* p = fastalloc::malloc(128);
    if (p == nullptr) {
        return 1;
    }

    std::memset(p, 0, 128);

    p = fastalloc::realloc(p, 256);
    if (p == nullptr) {
        return 1;
    }

    fastalloc::Stats stats = fastalloc::get_stats();
    std::cout << "current_bytes = " << stats.current_bytes << '\n';
    std::cout << "peak_bytes = " << stats.peak_bytes << '\n';

    fastalloc::free(p);
    return 0;
}
```

### 推荐使用方式

通常建议优先使用下面这组接口：

- `fastalloc::initialize()`
- `fastalloc::malloc(size)`
- `fastalloc::free(ptr)`
- `fastalloc::realloc(ptr, new_size)`
- `fastalloc::get_stats()`

它们的命名更接近文档中的对外 API，也更适合作为项目接入层使用。

### API 行为说明

- `fastalloc::initialize()`：初始化全局分配器，也可以不手动调用，分配时会懒初始化
- `fastalloc::malloc(0)`：返回 `nullptr`
- `fastalloc::free(nullptr)`：安全，无副作用
- `fastalloc::realloc(nullptr, n)`：等价于 `malloc(n)`
- `fastalloc::realloc(ptr, 0)`：会释放 `ptr` 并返回 `nullptr`

### 注意事项

- 必须用 `fastalloc::free()` 释放由 `fastalloc::malloc()` 返回的指针，不要混用系统 `free()`
- 同理，不要对系统 `malloc()` 返回的指针调用 `fastalloc::free()`
- 当前实现主要面向学习和实验，不建议直接替代生产环境分配器
- 如果你想关闭调试开销，可以在 `include/allocator/config.h` 中把 `kEnableDebugMode` 设为 `false`

## 调试模式

调试模式配置位于 `include/allocator/config.h`。

当前默认开启，支持：

- 分配块前后加 guard，释放时检查越界
- 检测 double free
- 分配后填充 `0xAA`
- 释放后填充 `0xDD`

如果只想看纯分配路径，可以把 `kEnableDebugMode` 改为 `false`。

## 统计系统

`GetStats()` / `get_stats()` 目前返回以下信息：

- `total_allocated`
- `total_freed`
- `current_bytes`
- `peak_bytes`
- `thread_count`
- `active_thread_count`
- `thread_total_allocated`
- `thread_total_freed`
- `thread_current_bytes`
- `thread_peak_bytes`

其中线程级统计会按“分配发生的线程”归属，即使后续由其他线程释放，也能正确回收对应线程的使用量。

## 编译

当前仓库还没有接入 `CMakeLists.txt`，可以直接用 `g++` 编译。

### 编译单元测试

```bash
g++ -std=c++17 -pthread -Iinclude src/*.cpp test/test_size_class.cpp -o test_size_class
g++ -std=c++17 -pthread -Iinclude src/*.cpp test/test_allocator.cpp -o test_allocator
```

### 编译 benchmark

```bash
g++ -std=c++17 -pthread -Iinclude src/*.cpp test/bench_allocator.cpp -o bench_allocator
```

## 运行测试

### Size Class 测试

```bash
./test_size_class
```

覆盖内容包括：

- 对齐逻辑
- 单调递增映射
- 全范围请求覆盖
- 边界值：`0`、`1`、`256KB`、`256KB + 1`

### Allocator 测试

```bash
./test_allocator
```

覆盖内容包括：

- `Span` / `RadixTree` / `ThreadCache` / `CentralCache` 基础行为
- 小对象与大对象分配路径
- `realloc`
- 延迟初始化
- 调试模式下的越界与 double free 检测
- 统计系统正确性
- 多线程并发分配/释放

说明：

- 测试里会故意触发一次越界和一次 double free
- 你会看到类似 `buffer overrun detected`、`double free detected` 的输出
- 这是预期行为，测试通过依赖这些子进程被正确中止

## 运行 Benchmark

```bash
./bench_allocator
```

当前 benchmark 会对比：

- 系统 `malloc/free`
- `fastalloc::malloc/free`

当前场景包括：

- 单线程固定大小
- 单线程混合大小
- 多线程固定大小
- 多线程混合大小

输出字段说明：

- `ops`：总操作次数
- `qps`：每秒完成的 alloc+free 次数
- `p99(ns)`：单次 alloc+free 延迟的 P99
- `current`：当前仍在使用的字节数
- `peak`：运行过程中峰值字节数

## 当前限制

- 目前仍是教学/实验性质实现，尚未做生产级健壮性处理
- 还没有接入正式构建系统
- benchmark 目前只内置了系统分配器与本实现的对比
- 文档中的 `jemalloc` / `tcmalloc` 对比还没有接入
- 尚未系统接入 `ASan` / `TSan` / `valgrind` 脚本

## 后续建议

- 接入 `CMake`
- 增加 `jemalloc` / `tcmalloc` 基准对比
- 增加自动化 sanitizer 配置
- 补充更完整的多线程与长期运行压力测试
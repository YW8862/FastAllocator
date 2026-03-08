# C++ 高性能内存池设计文档

---

## 1. 设计背景与目标

在高性能 C++ 服务端系统、游戏引擎或高并发计算系统中，频繁调用系统默认的 `malloc/free` 或 `new/delete` 会带来以下问题：

### 1.1 核心问题

#### （1）锁竞争严重

多线程环境下，默认分配器通常使用全局锁或复杂同步机制，导致高并发时性能急剧下降。

#### （2）内存碎片严重

频繁分配和释放不同大小对象会造成：

- 内部碎片（Internal Fragmentation）
- 外部碎片（External Fragmentation）

#### （3）系统调用成本高

频繁调用 `brk/mmap` 导致：

- TLB miss
- 上下文切换
- 内核态切换成本

---

## 2. 总体架构

采用三层结构（类似 jemalloc / tcmalloc 设计思想）：

### 2.1 架构图

```
+---------------------+
|   Thread Cache      |  <-- 无锁
+----------+----------+
           |
+----------v----------+
|   Central Cache     |  <-- 每 size class 一把锁
+----------+----------+
           |
+----------v----------+
|   Page Cache        |  <-- 管理 Span + 页合并
+----------+----------+
           |
+----------v----------+
|   OS                |  <-- mmap / VirtualAlloc
+---------------------+
```

---

## 3. 关键概念说明

| 名称           | 说明             |
|----------------|------------------|
| Page           | 操作系统页（通常 4KB） |
| Span           | 若干连续 Page    |
| Size Class     | 固定对象大小分级 |
| Thread Cache   | 每线程私有缓存   |
| Central Cache  | 全局共享缓存     |
| Page Cache     | 页级管理与合并   |

---

## 4. Size Class 设计

### 4.1 设计目标

- 降低内部碎片
- 快速 O(1) 映射
- 控制分级数量

### 4.2 分级示例

- 8
- 16
- 24
- 32
- 48
- 64
- 96
- 128
- 256
- 512
- 1024
- 2048
- 4096
- …
- 256KB

### 4.3 核心算法

#### 对齐算法

```cpp
size = (size + align - 1) & ~(align - 1);
```

#### O(1) 映射

- 表驱动
- `constexpr` 编译期构建
- size → index 查表数组

---

## 5. Thread Cache 设计

### 5.1 目标

- 完全无锁
- O(1) 分配
- 批量转移降低竞争

### 5.2 数据结构

```cpp
struct FreeList {
    void* head;
    size_t size;
};

class ThreadCache {
private:
    FreeList free_lists_[MAX_SIZE_CLASS];
};
```

### 5.3 分配流程

1. 计算 size class
2. 从 free_list pop
3. 若为空：批量从 Central Cache 获取

### 5.4 回收策略

- 超过阈值 → 批量归还 Central Cache
- 避免单线程独占大量内存

### 5.5 技术点

| 技术         | 作用           |
|--------------|----------------|
| thread_local | 消除锁         |
| 批量分配     | 减少锁次数     |
| cache-line 对齐 | 避免 false sharing |

---

## 6. Central Cache 设计

### 6.1 作用

- 管理所有线程共享对象
- 管理 Span

### 6.2 数据结构

```cpp
struct Span {
    size_t page_id;
    size_t page_num;
    size_t object_size;
    void* free_list;
    std::atomic<size_t> use_count;
};

struct CentralBucket {
    std::mutex lock;
    Span* span_list;
};
```

### 6.3 批量分配

一次加锁 → 获取 32 个 → 释放锁

### 6.4 Span 生命周期

1. PageCache 分配
2. CentralCache 切分
3. use_count++
4. use_count == 0 → 归还 PageCache

---

## 7. Page Cache 设计

### 7.1 作用

- 管理连续页
- Span 合并
- 解决外部碎片

### 7.2 数据结构

```cpp
class PageCache {
    std::vector<Span*> free_lists_[MAX_PAGES];
    RadixTree span_map;
};
```

### 7.3 Span 合并机制

释放 Span 时：

1. 查找前后页
2. 若空闲 → 合并
3. 更新映射

### 7.4 Radix Tree 映射

**用途：**

- ptr → page_id → Span*

**特点：**

- 只读查询无锁
- 原子写入
- 不移动内存

---

## 8. 分配流程（完整）

### 8.1 小对象

```
ThreadCache
    ↓ (空)
CentralCache
    ↓ (空)
PageCache
    ↓
OS
```

### 8.2 大对象（>256KB）

直接：`mmap(size)`

释放：`munmap`

---

## 9. 关键路径伪代码

### Allocate

```cpp
void* Allocate(size_t size) {
    if (size > MAX_SMALL_OBJECT)
        return LargeAlloc(size);

    size_t idx = SizeClass::Index(size);
    auto& list = tls_cache.free_lists_[idx];

    if (!list.Empty())
        return list.Pop();

    FetchFromCentralCache(idx);
    return list.Pop();
}
```

### Deallocate

```cpp
void Deallocate(void* ptr) {
    Span* span = RadixLookup(ptr);

    if (span->object_size == 0)
        return LargeFree(ptr);

    size_t idx = SizeClass::Index(span->object_size);
    tls_cache.free_lists_[idx].Push(ptr);
}
```

---

## 10. 并发设计

| 层级        | 并发策略     |
|-------------|--------------|
| ThreadCache | 无锁         |
| CentralCache| 每桶独立锁   |
| PageCache   | 读无锁，写加锁 |

---

## 11. 性能优化技术

### 11.1 False Sharing 解决

```cpp
struct alignas(64) ThreadCache {};
```

### 11.2 批量操作

- 批量获取
- 批量归还

### 11.3 系统调用优化

使用：`mmap`、`munmap`、`VirtualAlloc`，而非 `malloc`。

### 11.4 预取优化

```cpp
__builtin_prefetch(ptr);
```

---

## 12. 调试模式设计

### 12.1 内存越界检测

```
[ header ][ user data ][ guard ]
```

Guard 使用 magic number。

### 12.2 双重释放检测

- bitmap 标记
- 标志位校验

### 12.3 填充模式

- 分配填充 `0xAA`
- 释放填充 `0xDD`

---

## 13. 统计系统

```cpp
struct MemoryStats {
    std::atomic<size_t> total_allocated;
    std::atomic<size_t> total_freed;
};
```

支持：

- 峰值
- 当前使用
- 每线程统计

---

## 14. 大对象策略

- 单独 Span
- 可选择缓存最近释放对象
- 支持 HugePage

---

## 15. 高级优化方向

- NUMA 感知
- HugePage
- Lock-free Central Cache
- 后台回收线程
- 延迟释放（Quarantine）
- CPU 亲和性

---

## 16. 测试与 Benchmark

### 16.1 单元测试

- 边界大小
- 多线程压力
- 重复释放检测

### 16.2 性能测试

**对比：**

- glibc malloc
- jemalloc
- tcmalloc

**指标：**

- QPS
- P99 延迟
- 内存占用

---

## 17. API 设计

```cpp
namespace my_alloc {
    void* malloc(size_t);
    void  free(void*);
    void* realloc(void*, size_t);
    void  initialize();
    Stats get_stats();
}
```

可：

- 重载 global new/delete
- 使用 LD_PRELOAD 替换系统 malloc

---

## 18. 实现顺序建议

1. SizeClass
2. ThreadCache
3. CentralCache
4. PageCache
5. RadixTree
6. Span 合并
7. 调试功能
8. Benchmark

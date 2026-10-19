#include "../mem_pool.h"
#include <string>
struct P { int a; double b; P() = default; P(int x, double y) : a(x), b(y) {} };
int main() {
    MemPool<P> p(2);
    auto *x = p.allocate(1, 2.0);
    auto *y = p.allocate(3, 4.0);
    auto *z = p.allocate(5, 6.0);
    MemPool<std::string> s(1);
    auto *t = s.allocate("hello world, long enough to heap allocate");
    return (x && y && !z && t && x->a == 1 && y->b == 4.0) ? 0 : 1;
}

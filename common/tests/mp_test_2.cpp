// test file for allocate and deallocate

#include <iostream>
#include <string>
#include "../mem_pool.h"

class Person {
public:
    std::string name;
    int age;

    Person(std::string n, int a)
        : name(std::move(n)), age(a) {
        std::cout << "Person constructed\n";
    }

    ~Person() {
        std::cout << "Person destroyed\n";
    }
};

// Assumes MemPool is fully implemented.
int main() {
    MemPool<Person> pool(3);

    Person* p1 = pool.allocate("Gary", 25);
    Person* p2 = pool.allocate("Alice", 30);
    Person* p3 = pool.allocate("Bob", 40);

    // std::cout << p1->name << '\n';
    // std::cout << p2->name << '\n';

    pool.deallocate(p3);
    pool.deallocate(p3);
    pool.allocate(p3);
    std::cout << p3->name << '\n';

    return 0;
}

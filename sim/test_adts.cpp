#include <iostream>
#include "core/dyn_array.hpp"

#include "core/hash_map.hpp"
#include "core/string_view.hpp"

using namespace core;
int main () {
    Arena arena (65636);
    Array<StringView> arr (arena, 23);
    arr.push_back (StringView ("Hello"));
    arr.push_back (StringView ("World"));
    arr.push_back (StringView ("Hello"));
    arr.push_back (StringView ("World"));

    for (auto x: arr) { std::cout << x << std::endl; }

    HashMap<int, StringView> map (arena);
    StringView str = "Hello";
    std::cout << str << std::endl;
    map.insert (2, str);
    str.remove_prefix (2);
    map.insert (3, str);

    std::cout << *map.find (2) << std::endl;
    std::cout << *map.find (3) << std::endl;

    map[7] = "f";
    std::cout << map[1] << std::endl;
}

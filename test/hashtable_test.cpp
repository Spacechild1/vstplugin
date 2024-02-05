#include "HashTable.h"

#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

constexpr size_t iterCount = 100;
constexpr size_t elementCount = 10000;
constexpr size_t maxStringSize = 64;

std::string randomString(std::mt19937& mt) {
    std::uniform_int_distribution<size_t> d(0, maxStringSize);
    std::uniform_int_distribution<char> d2;
    std::string result;

    auto size = d(mt);
    for (int i = 0; i < size; ++i) {
        result.push_back(d2(mt));
    }
    // std::cout << "randomString: " << result << std::endl;
    return result;
}

int randomInt(std::mt19937& mt) {
    std::uniform_int_distribution<int> d;
    return d(mt);
}

int main(int argc, const char *argv[]) {
    std::random_device rd;
    std::mt19937 mt(rd());

    for (int i = 0; i < iterCount; ++i) {
        std::unordered_map<std::string, int> source;
        vst::HashTable<std::string, int, std::string_view> dest;

        for (int i = 0; i < elementCount; ++i) {
            source.emplace(randomString(mt), randomInt(mt));
        }

        for (auto& [key, value] : source) {
            if (!dest.insert(key, value)) {
                std::cout << "could not insert key '" << key << "'!" << std::endl;
                return EXIT_FAILURE;
            }
        }

        for (auto& [key, value] : source) {
            auto result = dest.find(key);
            if (!result) {
                std::cout << "could not find key '" << key << "'!" << std::endl;
                return EXIT_FAILURE;
            }
            if (*result != value) {
                std::cout << "values (" << value << ", " << *result << ") do not match!" << std::endl;
                return EXIT_FAILURE;
            }
        }

        for (int i = 0; i < elementCount; ++i) {
            // make a key that is not contained in 'source'
            std::string key;
            do {
                key = randomString(mt);
            } while (source.count(key));

            if (dest.find(key)) {
                std::cout << "found key '" << key << "' that has not been inserted!" << std::endl;
                return EXIT_FAILURE;
            }
        }
    }

    std::cout << "all tests succeeded!" << std::endl;

    return EXIT_SUCCESS;
}

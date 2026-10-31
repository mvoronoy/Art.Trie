#include <numeric>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <type_traits>
#include <vector>
#include <cassert>

namespace
{

    constexpr std::size_t WarmupSamples = 10;
    constexpr std::size_t MeasuredSamples = 1000;

    /** Makes pseudo-random value ( Golden Ratio hash ) from key. */
    constexpr std::uint64_t payload(std::uint8_t key) noexcept
    {
        return 0x9E3779B97F4A7C15ULL *
            (static_cast<std::uint64_t>(key) + 1);
    }

    // Observable sink to prevent compiler dead-code elimination.
    volatile std::uint64_t g_sink = 0;

    struct Entry
    {
        std::uint8_t _key;
        std::uint64_t _value;

        constexpr bool operator < (const Entry& other) const noexcept
        {
            return _key < other._key;
        }
    };

    // Bitmap + compact ordered array.
    template <size_t Capacity>
    class BitmapIndex
    {
    public:
        using key_t = std::uint8_t;

        constexpr static inline size_t capacity_c = Capacity;
        bool insert(Entry kv) noexcept
        {
            if (_size == Capacity) return false;

            const auto word = kv._key / bits_c;
            const auto bit = kv._key % bits_c;
            const auto mask = std::uint64_t{ 1 } << bit;

            if (_presence[word] & mask) return false;

            const auto pos = rank(kv._key);
            std::shift_right(&_entries[pos], &_entries[_size + 1]/*out-of-range*/, 1);

            _entries[pos] = std::move(kv);
            _presence[word] |= mask;
            ++_size;
            return true;
        }

        bool erase(std::uint8_t key) noexcept
        {
            const auto word = key / bits_c;
            const auto bit = key % bits_c;
            const auto mask = std::uint64_t{ 1 } << bit;

            if (!(_presence[word] & mask)) return false;

            const auto pos = rank(key);
            std::shift_left(&_entries[pos], &_entries[_size]/*out-of-range*/, 1);

            --_size;
            _presence[word] &= ~mask;
            return true;
        }

        bool find(std::uint8_t key, Entry& value) const noexcept
        {
            const auto word = key / bits_c;
            const auto bit = key % bits_c;
            const auto mask = std::uint64_t{ 1 } << bit;

            if (!(_presence[word] & mask))
                return false;

            value = _entries[rank(key)];
            return true;
        }

        std::size_t size() const noexcept { return _size; }

    private:

        constexpr static inline size_t bits_c = std::numeric_limits<std::uint64_t>::digits; //=64
        constexpr static inline size_t presence_capacity_c = (Capacity + bits_c - 1) / bits_c;

        std::size_t rank(std::uint8_t key) const noexcept
        {
            const auto word = key / bits_c;
            const auto bit = key % bits_c;
            const auto mask = (std::uint64_t{ 1 } << bit) - 1;

            // calculate rank: how many bits are set before this one?
            size_t pos = 0;
            for (size_t i = 0; i < word; ++i)
            {
                pos += std::popcount(_presence[i]);
            }
            return pos + std::popcount(_presence[word] & mask);
        }

        std::array<std::uint64_t, presence_capacity_c> _presence{};
        std::array<Entry, Capacity> _entries{};
        std::size_t _size{};
    };

    // Sorted array baseline.
    template <size_t Capacity>
    class OrderedVector
    {
    public:

        constexpr static inline size_t capacity_c = Capacity;

        bool insert(Entry kv)
        {
            if (_size == Capacity) return false;
            const auto entries_end = &_entries[_size];
            auto it = std::lower_bound(
                &_entries[0], entries_end, kv);

            if (it != entries_end && it->_key == kv._key)
                return false;
            
            auto offset = it == entries_end ? _size : (it - &_entries[0]);
            std::shift_right(&_entries[offset], &_entries[_size + 1]/*out-of-range*/, 1);
            ++_size;

            _entries[offset] = std::move(kv);
            return true;
        }

        bool erase(std::uint8_t key)
        {
            const auto entries_end = &_entries[_size];
            auto it = std::lower_bound(
                &_entries[0], entries_end, Entry{ key, 0 });

            if (it == entries_end || it->_key != key)
                return false;

            std::shift_left(it, &_entries[_size]/*out-of-range*/, 1);
            --_size;

            return true;
        }

        bool find(std::uint8_t key, Entry& value) const
        {
            const auto entries_end = &_entries[_size];
            auto it = std::lower_bound(
                &_entries[0], entries_end, Entry{ key, 0 });

            if (it == entries_end || it->_key != key)
                return false;

            value = *it;
            return true;
        }

        std::size_t size() const noexcept { return _size; }

    private:
        std::array<Entry, Capacity> _entries{};
        size_t _size{};
    };


    /** Bulk mutation: construct identical initial states outside the timer.
    *   Each replica receives the same operation sequence.
    */
    template<class C, class Operation>
    double benchmark(
        const C& initial,
        Operation operation)
    {
        double samples = 0.0;

        for (std::size_t i = 0;
            i < WarmupSamples + MeasuredSamples; ++i)
        {
            auto dut_copy = initial;
            std::uint64_t checksum = 0;

            const auto start = std::chrono::steady_clock::now();

            checksum += operation(dut_copy) ? 1u : 0u;

            const auto stop = std::chrono::steady_clock::now();

            g_sink = g_sink ^ checksum; //artificial calc to force compiler keep the loop

            if (i >= WarmupSamples)
            {
                const double ns =
                    std::chrono::duration<double, std::nano>(
                        stop - start).count();
                samples += ns;
            }
        }

        return (samples / MeasuredSamples);
    }


    struct print_value
    {
        double _value;
        constexpr explicit print_value(double value) noexcept
            : _value(value) {
        }
        friend std::ostream& operator << (std::ostream& os, const print_value& pv)
        {
            os << std::setw(11)
                << std::fixed << std::setprecision(2)
                << pv._value;
            return os;
        }
    };

    template <size_t Capacity>
    void run(std::mt19937& rng)
    {

        auto bitmap = BitmapIndex<Capacity>{};
        auto vector = OrderedVector<Capacity>{};

        // Keys for insert operation test
        // There are (Capacity - 1) unique keys to prevent container populate last item
        // and during insert test each item pasted twice to simulate duplicate scenario
        std::array<std::uint8_t, Capacity - 1> keys;
        std::iota(keys.begin(), keys.end(), 0);
        std::shuffle(keys.begin(), keys.end(), rng);

        auto insert_test = [&](auto& container)->bool {
            bool result = true;

            for (size_t i = 0; i < keys.size(); ++i)
            {
                auto k = keys[i];
                //positive scenario
                result = result && container.insert(Entry{ k, payload(k) });
                assert(result); //must be true as key is unique
                for (size_t j = 0; j <= i; ++j)
                {
                    bool dup_res = container.insert(Entry{ keys[j], payload(keys[j]) });
                    assert(!dup_res); //must fail
                }
            }
            return result;
            };

        // Insert comparison
        const double bi = benchmark(bitmap, insert_test);
        const double vi = benchmark(vector, insert_test);

        // for Delete measures pre-populate origin containers
        for (auto k : keys)
        {
            bitmap.insert(Entry{ k, payload(k) });
            vector.insert(Entry{ k, payload(k) });
        }
        auto delete_test = [&](auto& container)->bool{
            bool result = true;
            for(size_t i = 0; i < keys.size(); ++i)
            {
                auto k = keys[i];
                //positive scenario
                result = result && container.erase(k);
                assert(result); //must be true as key is unique
                for(size_t j = 0; j <= i; ++j)
                {
                    bool dup_res = container.erase(keys[j]);
                    assert(!dup_res); //must fail since key already erased
                    result = result && !dup_res;
                }
            }
            return result;
        };
        // Delete comparison
        const double bd = benchmark(bitmap, delete_test);
        const double vd = benchmark(vector, delete_test);

        // Lookup comparison
        auto lookup_test = [&](auto& container)->bool {
            constexpr std::uint8_t not_exists_key = std::uint8_t{ Capacity - 1 };
            bool result = true;
            Entry out_value{};
            for (auto k: keys)
            {
                result = result && container.find(k, out_value);
                assert(result); //must exists
                assert(out_value._key == k && out_value._value == payload(k)); //associated value must match the key
                bool missing = container.find(not_exists_key, out_value);
                assert(!missing); //must not exists
            }
            return result;
            };

        const double bl = benchmark(bitmap, lookup_test);
        const double vl = benchmark(vector, lookup_test);

        std::cout << std::setw(4) << Capacity << " | ";
        std::cout
            << print_value(bi) << "|"
            << print_value(vi) << "|"
            << print_value(bd) << "|"
            << print_value(vd) << "|"
            << print_value(bl) << "|"
            << print_value(vl) << "|"
            << '\n'
            ;
    }

} // namespace

int main()
{
    std::mt19937 rng(0xD0C0FFEE);

    std::cout 
        << "Lookup queries/sample = " << MeasuredSamples
        << "\nMedian nanoseconds per operation\n"
        << " occ | bitmap ins | vector ins | bitmap del | vector del"
        << " | bitmap find | vector find\n"
        << "-----+------------+------------+------------+------------"
        << "-+-------------+------------\n";


    run<32>(rng);
    run<64>(rng);
    run<128>(rng);
    run<256>(rng);


    std::cerr << "Checksum: " << g_sink << '\n';
}

#include "prx/libc/include/CpuTopology.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "cpu topology test failed: %s\n", what);
    std::exit(1);
}

static std::uint64_t Parse(const char* text) {
    const auto path = std::filesystem::temp_directory_path() / ("anyps5_cpu_topology_test_cpus-" + std::to_string(std::random_device{}()));
    {
        std::ofstream file(path, std::ios::trunc);
        file << text;
    }
    const auto mask = CpuTopology::MaskFromCpuList(path.c_str());
    std::filesystem::remove(path);
    return mask;
}

static CpuTopology::CpuFacts TwoDomains(std::uint64_t firstBytes, std::uint64_t secondBytes, unsigned coresPerDomain, std::uint64_t firstRank, std::uint64_t secondRank) {
    CpuTopology::CpuFacts facts{};
    const unsigned cores = coresPerDomain * 2;
    std::uint64_t first = 0;
    for (unsigned core = 0; core < cores; ++core) {
        const auto pair = (std::uint64_t{1} << core) | (std::uint64_t{1} << (core + cores));
        facts.siblings[core] = pair;
        facts.siblings[core + cores] = pair;
        facts.process |= pair;
        if (core < coresPerDomain) first |= pair;
    }
    facts.domains.push_back({first, firstBytes, firstRank});
    facts.domains.push_back({facts.process & ~first, secondBytes, secondRank});
    return facts;
}

static void CacheDomainTests() {
    using CpuTopology::ChooseCacheDomain;
    const auto equal = ChooseCacheDomain(TwoDomains(32u << 20, 32u << 20, 8, 236, 201));
    Require(equal.cpus == 0x00ff00ffull && equal.cores == 8 && equal.domains == 2, "two equal caches: the domain with the best-ranked core");
    Require(ChooseCacheDomain(TwoDomains(32u << 20, 32u << 20, 8, 201, 236)).cpus == 0xff00ff00ull, "the better-ranked domain may be the second");
    Require(ChooseCacheDomain(TwoDomains(32u << 20, 32u << 20, 8, 0, 0)).cpus == 0x00ff00ffull, "no ranking: the domain of the lowest cpu");
    Require(ChooseCacheDomain(TwoDomains(96u << 20, 32u << 20, 8, 201, 236)).cpus == 0x00ff00ffull, "the larger cache wins over the ranking");
    Require(ChooseCacheDomain(TwoDomains(32u << 20, 32u << 20, 6, 236, 201)).cpus == 0, "domains of fewer than 8 cores are left alone");
    auto single = TwoDomains(32u << 20, 32u << 20, 8, 236, 201);
    single.domains = {{single.process, 32u << 20, 236}};
    Require(ChooseCacheDomain(single).cpus == 0, "one cache domain is left alone");
    auto hybrid = TwoDomains(32u << 20, 32u << 20, 8, 236, 201);
    hybrid.hybrid = true;
    Require(ChooseCacheDomain(hybrid).cpus == 0, "hybrid layouts keep their own pinning");
    auto noSiblings = TwoDomains(32u << 20, 32u << 20, 8, 236, 201);
    noSiblings.siblings.fill(0);
    Require(ChooseCacheDomain(noSiblings).cores == 16, "without sibling lists every cpu is a core");
}

int main() {
    Require(Parse("0-15\n") == 0xffffull, "range");
    Require(Parse("16-31\n") == 0xffff0000ull, "high range");
    Require(Parse("0,2,4-5\n") == 0x35ull, "list of cpus and ranges");
    Require(Parse("3") == 0x8ull, "single cpu without a newline");
    Require(Parse("62-70\n") == 0xc000000000000000ull, "cpus past 63 are left out");
    Require(Parse("") == 0, "empty list");
    Require(Parse("4-2\n") == 0, "reversed range");
    Require(Parse("0-x\n") == 0, "malformed range");
    Require(Parse("0;1\n") == 0, "malformed separator");
    Require(CpuTopology::MaskFromCpuList("/nonexistent/anyps5/cpus") == 0, "missing file");

    {
        std::uint64_t byClass[4] = {};
        bool seen[4] = {};
        const auto empty = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(!empty.hybrid && empty.efficient == 0 && empty.performant == 0, "empty classes");

        byClass[0] = 0xff;
        seen[0] = true;
        const auto single = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(!single.hybrid && single.efficient == 0 && single.performant == 0, "single class");

        byClass[1] = 0xff00;
        seen[1] = true;
        const auto twoTier = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(twoTier.hybrid && twoTier.efficient == 0xff && twoTier.performant == 0xff00, "two-tier classes");

        byClass[2] = 0xff0000;
        seen[2] = true;
        const auto threeTier = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(threeTier.hybrid && threeTier.efficient == 0xffff && threeTier.performant == 0xff0000, "three-tier classes aggregate efficient");
    }

    const auto& layout = CpuTopology::Get();
    Require(layout.process != 0, "process affinity read");
    Require(!layout.hybrid || (layout.performant & layout.efficient) == 0, "hybrid classes overlap");
    const std::uint64_t first = layout.process & (~layout.process + 1);
    Require(CpuTopology::Pin(nullptr, first) == first, "pin to one cpu of the process");
    cpu_set_t set;
    CPU_ZERO(&set);
    Require(pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0, "read thread affinity");
    Require(CPU_COUNT(&set) == 1 && CPU_ISSET(static_cast<unsigned>(__builtin_ctzll(first)), &set), "thread runs on the pinned cpu only");
    Require(CpuTopology::Pin(nullptr, ~layout.process) == 0, "mask outside the process is not applied");
    int dummy = 0;
    Require(CpuTopology::Pin(&dummy, first) == 0, "other threads are not pinned by handle");
    CacheDomainTests();
    return 0;
}

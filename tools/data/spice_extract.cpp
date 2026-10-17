// Single-process offline tool. Never linked into the application's CSPICE instance.
#include <cspice/SpiceUsr.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <picosha2.h>
#include <set>
#include <stdexcept>
#include <string>

namespace {
void check() {
    if (failed_c()) {
        char message[2048]{};
        getmsg_c("LONG", sizeof(message), message);
        reset_c();
        throw std::runtime_error(message);
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        const bool planets = argc == 5 && std::string(argv[4]) == "--all-planets";
        if (argc != 4 && !planets) {
            throw std::runtime_error("Usage: orbitforge_spice_extract de440s.bsp naif0012.tls "
                                     "NEW-output.bsp [--all-planets]");
        }
        if (std::filesystem::exists(argv[3])) {
            throw std::runtime_error("Refusing to replace an existing kernel");
        }
        const std::string hashes[]{
            "c1c7feeab882263fc493a9d5a5b2ddd71b54826cdf65d8d17a76126b260a49f2",
            "678e32bdb5a744117a467cd9601cd6b373f0e9bc9bbde1371d5eee39600a039b"};
        for (int i = 0; i < 2; ++i) {
            if (std::filesystem::file_size(argv[i + 1]) > 64 * 1024 * 1024) {
                throw std::runtime_error("Unexpected source kernel size");
            }
            std::ifstream input(argv[i + 1], std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(input), {}};
            if (picosha2::hash256_hex_string(bytes) != hashes[i]) {
                throw std::runtime_error("Input kernel does not match the pinned source SHA256");
            }
        }
        char action[] = "RETURN", report[] = "NONE";
        erract_c("SET", 0, action);
        errprt_c("SET", 0, report);
        furnsh_c(argv[2]);
        check();
        double begin = 0, end = 0;
        str2et_c("2016 DEC 27 00:00:00 TDB", &begin);
        str2et_c("2017 JAN 05 00:00:00 TDB", &end);
        check();
        SpiceInt input = 0, output = 0;
        dafopr_c(argv[1], &input);
        check();
        spkopn_c(argv[3], "OrbitForge derived DE440s validation sample", 4096, &output);
        check();
        // New file and comment area; original attribution is not copied into the modified kernel.
        char comments[][128] = {
            "Producer of this derived kernel: OrbitForge project.",
            "Created with CSPICE N0067 spksub; no numerical coefficients were refitted.",
            "Source: JPL DE440s, downloaded from the NAIF generic kernels directory.",
            "Source SHA256: c1c7feeab882263fc493a9d5a5b2ddd71b54826cdf65d8d17a76126b260a49f2",
            "Retained targets: 3, 10, 301, 399. Centers and J2000 axes unchanged.",
            "Coverage: 2016 DEC 27 to 2017 JAN 05, 00:00:00 TDB, inclusive.",
            "Purpose: small offline OrbitForge P0 validation sample; not a current-epoch "
            "ephemeris."};
        if (planets)
            std::snprintf(
                comments[4], 128, "%s",
                "Retained targets: 1..10, 199, 299, 301, 399; centres and J2000 unchanged.");
        dafac_c(output, 7, 128, comments);
        check();
        dafbfs_c(input);
        SpiceBoolean found = SPICEFALSE;
        daffna_c(&found);
        std::set<SpiceInt> remaining{3, 10, 301, 399};
        if (planets) {
            for (int id = 1; id <= 9; ++id)
                remaining.insert(id);
            remaining.insert(199);
            remaining.insert(299);
        }
        while (found) {
            SpiceDouble descriptor[5]{}, bounds[2]{};
            SpiceInt integers[6]{};
            dafgs_c(descriptor);
            dafus_c(descriptor, 2, 6, bounds, integers);
            check();
            if (remaining.contains(integers[0])) {
                if (integers[2] != 1 || integers[3] != 2 || bounds[0] > begin || bounds[1] < end) {
                    throw std::runtime_error("Expected a covering J2000 Type 2 DE440s segment");
                }
                const auto id = "OrbitForge DE440s subset body " + std::to_string(integers[0]);
                spksub_c(input, descriptor, id.c_str(), begin, end, output);
                check();
                remaining.erase(integers[0]);
            }
            daffna_c(&found);
            check();
        }
        if (!remaining.empty()) {
            throw std::runtime_error("Missing required Earth/Moon/Sun/EMB segment");
        }
        spkcls_c(output);
        dafcls_c(input);
        kclear_c();
        check();
        std::cout << "Created OrbitForge DE440s subset: " << argv[3] << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

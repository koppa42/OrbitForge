#pragma once
#include <stdexcept>
struct A1Mjd {
    double value;
    explicit A1Mjd(double v) : value(v) {}
    double ToUtcMjd() const {
        throw std::logic_error("Reference harness must pass UTC directly to scalar JR");
    }
};

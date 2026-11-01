#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    volatile double angle=539,turn=360;
    volatile float square=4,zero=0,one=1,hundred=100;
    assert(std::remainder(angle,turn)==179);
    assert(std::sqrt(square)==2);
    assert(std::abs(std::acos(zero)-1.57079632679f)<1e-6f);
    assert(std::abs(std::atan2(one,one)-.78539816339f)<1e-6f);
    assert(std::log10(hundred)==2);
    std::cout<<"Math compatibility: stable libm wrappers execute without recursion\n";
}

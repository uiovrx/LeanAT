#pragma once
#include <iostream>
#include <cstdlib>
#define LEANAT_CHECK(...) do { if(!(__VA_ARGS__)) { std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " << #__VA_ARGS__ << '\n'; std::exit(1); } } while(false)

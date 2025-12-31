#include "mathlib.h"
#include <cmath>

namespace MathLib {
    int Add(int a, int b) {
        return a + b;
    }

    int Subtract(int a, int b) {
        return a - b;
    }

    int Multiply(int a, int b) {
        return a * b;
    }

    double Divide(double a, double b) {
        if (b == 0.0) {
            return 0.0; // Simple error handling
        }
        return a / b;
    }

    int Power(int base, int exponent) {
        if (exponent < 0) {
            return 0; // Simple handling for negative exponents
        }
        if (exponent == 0) {
            return 1;
        }

        int result = 1;
        for (int i = 0; i < exponent; i++) {
            result *= base;
        }
        return result;
    }

    double SquareRoot(double value) {
        if (value < 0.0) {
            return 0.0; // Simple error handling
        }
        return std::sqrt(value);
    }

    const char* GetVersion() {
        return "MathLib v1.0.0";
    }

    bool IsPrime(int number) {
        if (number <= 1) {
            return false;
        }
        if (number <= 3) {
            return true;
        }
        if (number % 2 == 0 || number % 3 == 0) {
            return false;
        }

        for (int i = 5; i * i <= number; i += 6) {
            if (number % i == 0 || number % (i + 2) == 0) {
                return false;
            }
        }
        return true;
    }
}

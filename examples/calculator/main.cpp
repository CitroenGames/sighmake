#include <iostream>
#include "mathlib.h"

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Calculator Example - Using MathLib DLL" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Display library version
    std::cout << "Library: " << MathLib::GetVersion() << std::endl;
    std::cout << std::endl;

    // Test basic arithmetic
    std::cout << "Basic Arithmetic:" << std::endl;
    std::cout << "  10 + 5 = " << MathLib::Add(10, 5) << std::endl;
    std::cout << "  10 - 5 = " << MathLib::Subtract(10, 5) << std::endl;
    std::cout << "  10 * 5 = " << MathLib::Multiply(10, 5) << std::endl;
    std::cout << "  10 / 5 = " << MathLib::Divide(10.0, 5.0) << std::endl;
    std::cout << std::endl;

    // Test advanced operations
    std::cout << "Advanced Operations:" << std::endl;
    std::cout << "  2^8 = " << MathLib::Power(2, 8) << std::endl;
    std::cout << "  sqrt(144) = " << MathLib::SquareRoot(144.0) << std::endl;
    std::cout << "  sqrt(2) = " << MathLib::SquareRoot(2.0) << std::endl;
    std::cout << std::endl;

    // Test prime checking
    std::cout << "Prime Number Tests:" << std::endl;
    int numbers[] = {2, 7, 15, 17, 23, 24, 29, 100};
    for (int num : numbers) {
        bool isPrime = MathLib::IsPrime(num);
        std::cout << "  " << num << " is " << (isPrime ? "PRIME" : "NOT PRIME") << std::endl;
    }
    std::cout << std::endl;

    // Interactive calculation
    std::cout << "Interactive Mode:" << std::endl;
    std::cout << "Enter two numbers for calculation: ";

    int a, b;
    if (std::cin >> a >> b) {
        std::cout << std::endl;
        std::cout << "Results for " << a << " and " << b << ":" << std::endl;
        std::cout << "  Addition: " << MathLib::Add(a, b) << std::endl;
        std::cout << "  Subtraction: " << MathLib::Subtract(a, b) << std::endl;
        std::cout << "  Multiplication: " << MathLib::Multiply(a, b) << std::endl;

        if (b != 0) {
            std::cout << "  Division: " << MathLib::Divide(static_cast<double>(a), static_cast<double>(b)) << std::endl;
        } else {
            std::cout << "  Division: Cannot divide by zero!" << std::endl;
        }

        if (b >= 0 && b <= 20) {
            std::cout << "  Power (" << a << "^" << b << "): " << MathLib::Power(a, b) << std::endl;
        }
    }

    std::cout << std::endl;
    std::cout << "Press Enter to exit...";
    std::cin.ignore();
    std::cin.get();

    return 0;
}

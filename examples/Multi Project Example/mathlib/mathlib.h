#pragma once
#ifndef MATHLIB_H
#define MATHLIB_H

// DLL export/import macros
#ifdef MATHLIB_EXPORTS
    #define MATHLIB_API __declspec(dllexport)
#else
    #define MATHLIB_API __declspec(dllimport)
#endif

// Simple math library functions
namespace MathLib {
    // Basic arithmetic operations
    MATHLIB_API int Add(int a, int b);
    MATHLIB_API int Subtract(int a, int b);
    MATHLIB_API int Multiply(int a, int b);
    MATHLIB_API double Divide(double a, double b);

    // Advanced operations
    MATHLIB_API int Power(int base, int exponent);
    MATHLIB_API double SquareRoot(double value);

    // Utility functions
    MATHLIB_API const char* GetVersion();
    MATHLIB_API bool IsPrime(int number);
}

#endif // MATHLIB_H

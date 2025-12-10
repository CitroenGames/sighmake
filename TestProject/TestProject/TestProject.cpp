#include <iostream>

#include "framework.h"

int main()
{
#ifdef TESTTHING && STATICLIB_TEST
    std::cout << FORCEINCLUDE << std::endl;
#endif

#if defined RELEASEPRO
	// this is here to test out configuration dependent preprocessor definitions
	std::cout << "RELEASEPRO" << std::endl;
#endif

    framework obj;
    obj.fnStaticLib1();

	std::cout << "Press enter to continue";
	std::getchar();
	return 0;
}
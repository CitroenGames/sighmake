#pragma once

// Windows-specific headers (only on Windows)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Standard C++ Library - Most frequently used
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>

// Additional commonly used headers
#include <random>
#include <iomanip>
#include <cctype>

// Core project types - used by almost every file
#include "common/project_types.hpp"

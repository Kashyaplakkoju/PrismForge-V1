#pragma once

#include <ostream>
#include <iostream>
#include <fstream>
#include <mutex>
#include <filesystem>
#include "apppaths.h"

/*
* Provides custom logging service.
* https://stackoverflow.com/a/17597747
*/
class Logger
{
public:
	static const int CONSOLE = 0;
	static const int FILE = 1;
	static const int DISABLED = 2;

	Logger(int mode) : mode(mode), filestream(std::filesystem::path(PrismForge::ApplicationDirectory()) / "prismforge.log")
	{
	}

	// Overload for using std::endl
	// https://stackoverflow.com/a/10059337
	Logger& operator<<(std::ostream& (*f)(std::ostream&))
	{
		if (mode != DISABLED)
		{
			std::lock_guard<std::mutex> lock(mtx);
			stream() << std::endl;
		}
		return *this;
	}

	template<typename T>
	Logger& operator<<(const T& data)
	{
		if (mode != DISABLED)
		{
			std::lock_guard<std::mutex> lock(mtx);
			stream() << data;
			stream().flush();
		}
		return *this;
	}

private:
	int mode;
	std::ofstream filestream;
	std::mutex mtx;

	std::ostream& stream()
	{
		return mode == CONSOLE ? std::cout : filestream;
	}
};

extern Logger logger;
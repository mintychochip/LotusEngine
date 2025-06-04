#pragma once

constexpr size_t KB(size_t bytes)
{
	return 1024 * bytes;
}

constexpr size_t MB(size_t bytes)
{
	return 1024 * KB(bytes);
}
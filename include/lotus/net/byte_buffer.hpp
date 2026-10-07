#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace lotus {

class ByteWriter {
public:
	explicit ByteWriter(std::size_t reserve = 64);

	void u8(std::uint8_t value);
	void u16(std::uint16_t value);
	void u32(std::uint32_t value);
	void u64(std::uint64_t value);
	void i16(std::int16_t value);
	void i32(std::int32_t value);
	void varint(std::uint64_t value);
	void f32(float value);
	void bytes(std::span<const std::uint8_t> value);
	void string(const std::string& value);
	void position(const glm::vec3& value);
	void rotation(const glm::quat& value);

	std::vector<std::uint8_t>& data();
	const std::vector<std::uint8_t>& data() const;
	std::size_t size() const;
	void clear();
	void patch_u16(std::size_t offset, std::uint16_t value);

private:
	std::vector<std::uint8_t> buf_;
};

class ByteReader {
public:
	explicit ByteReader(std::span<const std::uint8_t> data);

	bool u8(std::uint8_t& out);
	bool u16(std::uint16_t& out);
	bool u32(std::uint32_t& out);
	bool u64(std::uint64_t& out);
	bool i16(std::int16_t& out);
	bool i32(std::int32_t& out);
	bool varint(std::uint64_t& out);
	bool f32(float& out);
	bool bytes(std::size_t count, std::span<const std::uint8_t>& out);
	bool string(std::string& out);
	bool position(glm::vec3& out);
	bool rotation(glm::quat& out);

	bool ok() const;
	std::size_t remaining() const;

private:
	std::span<const std::uint8_t> data_;
	std::size_t pos_{0};
	bool ok_{true};

	bool can_read(std::size_t n) const;
};

} // namespace lotus

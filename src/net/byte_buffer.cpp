#include "lotus/net/byte_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <cstring>

using namespace lotus;

// --- ByteWriter -------------------------------------------------------------

ByteWriter::ByteWriter(std::size_t reserve) { buf_.reserve(reserve); }

void ByteWriter::u8(std::uint8_t value) { buf_.push_back(value); }

void ByteWriter::u16(std::uint16_t value) {
	buf_.push_back(static_cast<std::uint8_t>(value & 0xFF));
	buf_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void ByteWriter::u32(std::uint32_t value) {
	for (int i = 0; i < 4; ++i)
		buf_.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
}

void ByteWriter::u64(std::uint64_t value) {
	for (int i = 0; i < 8; ++i)
		buf_.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
}

void ByteWriter::i16(std::int16_t value) {
	u16(static_cast<std::uint16_t>(value));
}

void ByteWriter::i32(std::int32_t value) {
	u32(static_cast<std::uint32_t>(value));
}

void ByteWriter::varint(std::uint64_t value) {
	do {
		std::uint8_t b = static_cast<std::uint8_t>(value & 0x7F);
		value >>= 7;
		if (value) b |= 0x80;
		buf_.push_back(b);
	} while (value);
}

void ByteWriter::f32(float value) {
	std::uint32_t bits;
	std::memcpy(&bits, &value, 4);
	u32(bits);
}

void ByteWriter::bytes(std::span<const std::uint8_t> value) {
	buf_.insert(buf_.end(), value.begin(), value.end());
}

void ByteWriter::string(const std::string& value) {
	varint(value.size());
	bytes(std::span<const std::uint8_t>(
	        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

void ByteWriter::position(const glm::vec3& value) {
	constexpr float kRange = 512.0f;
	auto quant = [kRange](float v) -> std::int16_t {
		float clamped = std::clamp(v, -kRange, kRange);
		return static_cast<std::int16_t>(clamped * 32767.0f / kRange);
	};
	i16(quant(value.x));
	i16(quant(value.y));
	i16(quant(value.z));
}

void ByteWriter::rotation(const glm::quat& value) {
	float comps[4] = {value.x, value.y, value.z, value.w};
	int largest   = 0;
	for (int i = 1; i < 4; ++i)
		if (std::abs(comps[i]) > std::abs(comps[largest])) largest = i;
	float sign = comps[largest] < 0.0f ? -1.0f : 1.0f;
	u8(static_cast<std::uint8_t>((largest << 1) | (sign < 0.0f ? 1 : 0)));
	constexpr float kScale = 32767.0f / 1.41421356f;
	for (int i = 0; i < 4; ++i) {
		if (i == largest) continue;
		i16(static_cast<std::int16_t>(comps[i] * sign * kScale));
	}
}

std::vector<std::uint8_t>& ByteWriter::data() { return buf_; }
const std::vector<std::uint8_t>& ByteWriter::data() const { return buf_; }
std::size_t ByteWriter::size() const { return buf_.size(); }
void ByteWriter::clear() { buf_.clear(); }

void ByteWriter::patch_u16(std::size_t offset, std::uint16_t value) {
	if (offset + 2 > buf_.size()) return;
	buf_[offset]     = static_cast<std::uint8_t>(value & 0xFF);
	buf_[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

// --- ByteReader -------------------------------------------------------------

ByteReader::ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

bool ByteReader::can_read(std::size_t n) const {
	return ok_ && pos_ + n <= data_.size();
}

bool ByteReader::u8(std::uint8_t& out) {
	if (!can_read(1)) {
		ok_ = false;
		return false;
	}
	out = data_[pos_++];
	return true;
}

bool ByteReader::u16(std::uint16_t& out) {
	if (!can_read(2)) {
		ok_ = false;
		return false;
	}
	out = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
	pos_ += 2;
	return true;
}

bool ByteReader::u32(std::uint32_t& out) {
	if (!can_read(4)) {
		ok_ = false;
		return false;
	}
	out = 0;
	for (int i = 0; i < 4; ++i)
		out |= static_cast<std::uint32_t>(data_[pos_ + i]) << (i * 8);
	pos_ += 4;
	return true;
}

bool ByteReader::u64(std::uint64_t& out) {
	if (!can_read(8)) {
		ok_ = false;
		return false;
	}
	out = 0;
	for (int i = 0; i < 8; ++i)
		out |= static_cast<std::uint64_t>(data_[pos_ + i]) << (i * 8);
	pos_ += 8;
	return true;
}

bool ByteReader::i16(std::int16_t& out) {
	std::uint16_t v;
	if (!u16(v)) return false;
	out = static_cast<std::int16_t>(v);
	return true;
}

bool ByteReader::i32(std::int32_t& out) {
	std::uint32_t v;
	if (!u32(v)) return false;
	out = static_cast<std::int32_t>(v);
	return true;
}

bool ByteReader::varint(std::uint64_t& out) {
	out      = 0;
	int shift = 0;
	for (int i = 0; i < 10; ++i) {
		std::uint8_t b;
		if (!u8(b)) return false;
		out |= static_cast<std::uint64_t>(b & 0x7F) << shift;
		if ((b & 0x80) == 0) return true;
		shift += 7;
	}
	ok_ = false;
	return false;
}

bool ByteReader::f32(float& out) {
	std::uint32_t bits;
	if (!u32(bits)) return false;
	std::memcpy(&out, &bits, 4);
	return true;
}

bool ByteReader::bytes(std::size_t count, std::span<const std::uint8_t>& out) {
	if (!can_read(count)) {
		ok_ = false;
		return false;
	}
	out = data_.subspan(pos_, count);
	pos_ += count;
	return true;
}

bool ByteReader::string(std::string& out) {
	std::uint64_t len = 0;
	if (!varint(len)) return false;
	if (len > data_.size() - pos_) {
		ok_ = false;
		return false;
	}
	out.assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
	pos_ += len;
	return true;
}

bool ByteReader::position(glm::vec3& out) {
	std::int16_t x, y, z;
	if (!i16(x) || !i16(y) || !i16(z)) return false;
	constexpr float kRange = 512.0f;
	out = {x * kRange / 32767.0f, y * kRange / 32767.0f, z * kRange / 32767.0f};
	return true;
}

bool ByteReader::rotation(glm::quat& out) {
	std::uint8_t header;
	if (!u8(header)) return false;
	int largest = (header >> 1) & 0x03;
	float sign  = (header & 0x01) ? -1.0f : 1.0f;
	float comps[4] = {0, 0, 0, 0};
	constexpr float kInvScale = 1.41421356f / 32767.0f;
	for (int i = 0; i < 4; ++i) {
		if (i == largest) continue;
		std::int16_t v;
		if (!i16(v)) return false;
		comps[i] = v * kInvScale * sign;
	}
	float sum = 0;
	for (int i = 0; i < 4; ++i)
		if (i != largest) sum += comps[i] * comps[i];
	float largest_val = std::sqrt(std::max(0.0f, 1.0f - sum)) * sign;
	if (std::isnan(largest_val)) largest_val = 0.0f;
	comps[largest] = largest_val;
	out            = glm::quat(comps[3], comps[0], comps[1], comps[2]);
	return true;
}

bool ByteReader::ok() const { return ok_; }
std::size_t ByteReader::remaining() const {
	return ok_ ? data_.size() - pos_ : 0;
}

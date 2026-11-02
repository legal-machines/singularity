#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

// Whole numbers of 128 bits, on every compiler.
//
// SemanticAlgebra works out exact fractions in 128-bit integers. GCC and Clang
// build those in on 64-bit targets (__int128), with checked arithmetic
// (__builtin_add_overflow ...) and bit counts (__builtin_clz); MSVC has none of
// them. So Int128 and UInt128 are the compiler's own types wherever it has
// them - macOS and Linux keep exactly the arithmetic they always ran - and
// otherwise the portable classes below: two's-complement numbers made of two
// 64-bit halves that give GCC's answer to every operation. The classes are
// compiled everywhere, so Int128.test.cpp holds them to the native types
// wherever those exist.
namespace singularity::platform {

namespace portable {

// The zero bits above the highest one bit of a 32-bit value; 32 for zero.
constexpr int count_leading_zeros(std::uint32_t value) {
	if (value == 0) return 32;
	int count = 0;
	for (; (value & 0x80000000U) == 0; value <<= 1) ++count;
	return count;
}

// The zero bits below the lowest one bit of a 32-bit value; 32 for zero.
constexpr int count_trailing_zeros(std::uint32_t value) {
	if (value == 0) return 32;
	int count = 0;
	for (; (value & 1U) == 0; value >>= 1) ++count;
	return count;
}

class Int128;

// An unsigned 128-bit integer: every operation is modulo 2^128, as for
// unsigned __int128.
class UInt128 {
public:
	constexpr UInt128() = default;
	// Any integer converts implicitly, as to the builtin type: a negative one
	// wraps around, so -1 is 2^128 - 1.
	template <typename Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
	constexpr UInt128(Integer value) : low_(static_cast<std::uint64_t>(value)), high_(sign_fill(value)) {}  // NOLINT
	// So does an Int128, so that the two together work out as unsigned, as the
	// builtin types do (Int128 + UInt128 is a UInt128); the other way is explicit.
	constexpr UInt128(const Int128 &value);  // NOLINT

	[[nodiscard]] static constexpr UInt128 from_halves(std::uint64_t high, std::uint64_t low) {
		UInt128 result;
		result.high_ = high;
		result.low_ = low;
		return result;
	}
	[[nodiscard]] constexpr std::uint64_t high() const { return high_; }
	[[nodiscard]] constexpr std::uint64_t low() const { return low_; }

	// Out to a smaller integer: its low bits (modulo 2^N), as a builtin cast.
	template <typename Integer, std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>, int> = 0>
	explicit constexpr operator Integer() const {
		return static_cast<Integer>(low_);
	}
	explicit constexpr operator bool() const { return (high_ | low_) != 0; }
	// The nearest double or float, ties to even, rounded once as
	// __floatuntidf and __floatuntisf round.
	explicit operator double() const { return nearest<double>(); }
	explicit operator float() const { return nearest<float>(); }

	friend constexpr UInt128 operator+(const UInt128 &a, const UInt128 &b) {
		const std::uint64_t low = a.low_ + b.low_;
		return from_halves(a.high_ + b.high_ + (low < a.low_ ? 1U : 0U), low);
	}
	friend constexpr UInt128 operator-(const UInt128 &a, const UInt128 &b) {
		return from_halves(a.high_ - b.high_ - (a.low_ < b.low_ ? 1U : 0U), a.low_ - b.low_);
	}
	friend constexpr UInt128 operator*(const UInt128 &a, const UInt128 &b) {
		const UInt128 low = product(a.low_, b.low_);
		return from_halves(low.high_ + a.high_ * b.low_ + a.low_ * b.high_, low.low_);
	}
	friend constexpr UInt128 operator/(const UInt128 &a, const UInt128 &b) {
		UInt128 quotient, remainder;
		divide(a, b, quotient, remainder);
		return quotient;
	}
	friend constexpr UInt128 operator%(const UInt128 &a, const UInt128 &b) {
		UInt128 quotient, remainder;
		divide(a, b, quotient, remainder);
		return remainder;
	}
	friend constexpr UInt128 operator&(const UInt128 &a, const UInt128 &b) {
		return from_halves(a.high_ & b.high_, a.low_ & b.low_);
	}
	friend constexpr UInt128 operator|(const UInt128 &a, const UInt128 &b) {
		return from_halves(a.high_ | b.high_, a.low_ | b.low_);
	}
	friend constexpr UInt128 operator^(const UInt128 &a, const UInt128 &b) {
		return from_halves(a.high_ ^ b.high_, a.low_ ^ b.low_);
	}
	// A count outside 0..127 is undefined for the builtin type; here it is
	// taken modulo 128.
	template <typename Count, std::enable_if_t<std::is_integral_v<Count>, int> = 0>
	friend constexpr UInt128 operator<<(const UInt128 &value, Count count) {
		const unsigned bits = static_cast<unsigned>(count) & 127U;
		if (bits == 0) return value;
		if (bits >= 64) return from_halves(value.low_ << (bits - 64), 0);
		return from_halves((value.high_ << bits) | (value.low_ >> (64 - bits)), value.low_ << bits);
	}
	template <typename Count, std::enable_if_t<std::is_integral_v<Count>, int> = 0>
	friend constexpr UInt128 operator>>(const UInt128 &value, Count count) {
		const unsigned bits = static_cast<unsigned>(count) & 127U;
		if (bits == 0) return value;
		if (bits >= 64) return from_halves(0, value.high_ >> (bits - 64));
		return from_halves(value.high_ >> bits, (value.low_ >> bits) | (value.high_ << (64 - bits)));
	}
	constexpr UInt128 operator+() const { return *this; }
	constexpr UInt128 operator-() const { return UInt128() - *this; }
	constexpr UInt128 operator~() const { return from_halves(~high_, ~low_); }

	friend constexpr bool operator==(const UInt128 &a, const UInt128 &b) { return a.high_ == b.high_ && a.low_ == b.low_; }
	friend constexpr bool operator!=(const UInt128 &a, const UInt128 &b) { return !(a == b); }
	friend constexpr bool operator<(const UInt128 &a, const UInt128 &b) {
		return a.high_ != b.high_ ? a.high_ < b.high_ : a.low_ < b.low_;
	}
	friend constexpr bool operator>(const UInt128 &a, const UInt128 &b) { return b < a; }
	friend constexpr bool operator<=(const UInt128 &a, const UInt128 &b) { return !(b < a); }
	friend constexpr bool operator>=(const UInt128 &a, const UInt128 &b) { return !(a < b); }

	constexpr UInt128 &operator+=(const UInt128 &other) { return *this = *this + other; }
	constexpr UInt128 &operator-=(const UInt128 &other) { return *this = *this - other; }
	constexpr UInt128 &operator*=(const UInt128 &other) { return *this = *this * other; }
	constexpr UInt128 &operator/=(const UInt128 &other) { return *this = *this / other; }
	constexpr UInt128 &operator%=(const UInt128 &other) { return *this = *this % other; }
	constexpr UInt128 &operator&=(const UInt128 &other) { return *this = *this & other; }
	constexpr UInt128 &operator|=(const UInt128 &other) { return *this = *this | other; }
	constexpr UInt128 &operator^=(const UInt128 &other) { return *this = *this ^ other; }
	template <typename Count, std::enable_if_t<std::is_integral_v<Count>, int> = 0>
	constexpr UInt128 &operator<<=(Count count) {
		return *this = *this << count;
	}
	template <typename Count, std::enable_if_t<std::is_integral_v<Count>, int> = 0>
	constexpr UInt128 &operator>>=(Count count) {
		return *this = *this >> count;
	}
	constexpr UInt128 &operator++() { return *this += 1; }
	constexpr UInt128 &operator--() { return *this -= 1; }
	constexpr UInt128 operator++(int) {
		const UInt128 old = *this;
		*this += 1;
		return old;
	}
	constexpr UInt128 operator--(int) {
		const UInt128 old = *this;
		*this -= 1;
		return old;
	}

private:
	template <typename Integer>
	static constexpr std::uint64_t sign_fill(Integer value) {
		if constexpr (std::is_signed_v<Integer>) {
			return value < 0 ? ~std::uint64_t{0} : 0;
		} else {
			static_cast<void>(value);
			return 0;
		}
	}

	// The whole 128-bit product of two 64-bit halves, from 32-bit pieces.
	static constexpr UInt128 product(std::uint64_t a, std::uint64_t b) {
		const std::uint64_t aLow = a & 0xFFFFFFFFU, aHigh = a >> 32, bLow = b & 0xFFFFFFFFU, bHigh = b >> 32;
		const std::uint64_t lowLow = aLow * bLow, highLow = aHigh * bLow, lowHigh = aLow * bHigh;
		// At most (2^32 - 2) + (2^32 - 1) + (2^32 - 1)^2: no carry is lost.
		const std::uint64_t middle = (lowLow >> 32) + (highLow & 0xFFFFFFFFU) + lowHigh;
		return from_halves(aHigh * bHigh + (highLow >> 32) + (middle >> 32), (middle << 32) | (lowLow & 0xFFFFFFFFU));
	}

	static constexpr int leading_zeros(const UInt128 &value) {
		const std::uint64_t half = value.high_ != 0 ? value.high_ : value.low_;
		const auto top = static_cast<std::uint32_t>(half >> 32);
		const int inHalf = top != 0 ? count_leading_zeros(top) : 32 + count_leading_zeros(static_cast<std::uint32_t>(half));
		return value.high_ != 0 ? inHalf : 64 + inHalf;
	}

	// a = quotient * b + remainder, remainder < b. A zero divisor is undefined
	// for the builtin type; here it gives quotient 0 and remainder a, so that
	// identity still holds, and nothing loops.
	static constexpr void divide(const UInt128 &a, const UInt128 &b, UInt128 &quotient, UInt128 &remainder) {
		quotient = UInt128();
		remainder = a;
		if (b == UInt128() || b > a) return;
		if (a.high_ == 0) {
			quotient = UInt128(a.low_ / b.low_);
			remainder = UInt128(a.low_ % b.low_);
			return;
		}
		// Long division, one quotient bit at a time from the highest it can have.
		const int shift = leading_zeros(b) - leading_zeros(a);
		UInt128 part = b << shift;
		for (int bit = shift; bit >= 0; --bit) {
			quotient <<= 1;
			if (remainder >= part) {
				remainder -= part;
				quotient |= 1U;
			}
			part >>= 1;
		}
	}

	template <typename Floating>
	Floating nearest() const {
		constexpr int digits = std::numeric_limits<Floating>::digits;
		const int bits = 128 - leading_zeros(*this);
		if (bits <= digits) return static_cast<Floating>(low_);  // exact
		const int dropped = bits - digits;
		auto significand = static_cast<std::uint64_t>(*this >> dropped);
		const UInt128 rest = *this - (UInt128(significand) << dropped);
		const UInt128 half = UInt128(1) << (dropped - 1);
		if (rest > half || (rest == half && (significand & 1U) != 0)) ++significand;
		// At most 2^digits: exact in Floating, and so is scaling it by 2^dropped
		// (float's range ends below 2^128, which is infinity, as the builtin gives).
		return std::ldexp(static_cast<Floating>(significand), dropped);
	}

	std::uint64_t low_ = 0;
	std::uint64_t high_ = 0;
};

// A signed 128-bit integer in two's complement, as __int128. +, -, *, unary -
// and << wrap modulo 2^128 where the builtin type's overflow is undefined; /
// truncates toward zero and % takes the dividend's sign; >> is arithmetic.
class Int128 {
public:
	constexpr Int128() = default;
	// Any integer converts implicitly, as to the builtin type.
	template <typename Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
	constexpr Int128(Integer value) : Int128(UInt128(value)) {}  // NOLINT
	// The same bits, read as signed: 2^128 - 1 is -1.
	explicit constexpr Int128(const UInt128 &value) : low_(value.low()), high_(value.high()) {}

	[[nodiscard]] static constexpr Int128 from_halves(std::uint64_t high, std::uint64_t low) {
		return Int128(UInt128::from_halves(high, low));
	}
	[[nodiscard]] constexpr std::uint64_t high() const { return high_; }
	[[nodiscard]] constexpr std::uint64_t low() const { return low_; }

	template <typename Integer, std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>, int> = 0>
	explicit constexpr operator Integer() const {
		return static_cast<Integer>(low_);
	}
	explicit constexpr operator bool() const { return (high_ | low_) != 0; }
	// Rounded as its magnitude is, then signed, as __floattidf and __floattisf do.
	explicit operator double() const { return nearest<double>(); }
	explicit operator float() const { return nearest<float>(); }

	friend constexpr Int128 operator+(const Int128 &a, const Int128 &b) { return Int128(UInt128(a) + UInt128(b)); }
	friend constexpr Int128 operator-(const Int128 &a, const Int128 &b) { return Int128(UInt128(a) - UInt128(b)); }
	friend constexpr Int128 operator*(const Int128 &a, const Int128 &b) { return Int128(UInt128(a) * UInt128(b)); }
	// Division by zero and INT128_MIN / -1 are undefined for the builtin type.
	// Here x / 0 is 0 and x % 0 is x; INT128_MIN / -1 wraps to INT128_MIN with
	// remainder 0: a / b * b + a % b == a for every pair, and nothing traps.
	friend constexpr Int128 operator/(const Int128 &a, const Int128 &b) {
		const UInt128 quotient = magnitude(a) / magnitude(b);
		return Int128(a.negative() != b.negative() ? -quotient : quotient);
	}
	friend constexpr Int128 operator%(const Int128 &a, const Int128 &b) {
		const UInt128 remainder = magnitude(a) % magnitude(b);
		return Int128(a.negative() ? -remainder : remainder);
	}
	friend constexpr Int128 operator&(const Int128 &a, const Int128 &b) { return Int128(UInt128(a) & UInt128(b)); }
	friend constexpr Int128 operator|(const Int128 &a, const Int128 &b) { return Int128(UInt128(a) | UInt128(b)); }
	friend constexpr Int128 operator^(const Int128 &a, const Int128 &b) { return Int128(UInt128(a) ^ UInt128(b)); }
	template <typename Count, std::enable_if_t<std::is_integral_v<Count>, int> = 0>
	friend constexpr Int128 operator<<(const Int128 &value, Count count) {
		return Int128(UInt128(value) << count);
	}
	// A negative value brings in ones from the top: ~(~x >> n) is x >> n.
	template <typename Count, std::enable_if_t<std::is_integral_v<Count>, int> = 0>
	friend constexpr Int128 operator>>(const Int128 &value, Count count) {
		return value.negative() ? ~Int128(~UInt128(value) >> count) : Int128(UInt128(value) >> count);
	}
	constexpr Int128 operator+() const { return *this; }
	constexpr Int128 operator-() const { return Int128(-UInt128(*this)); }
	constexpr Int128 operator~() const { return Int128(~UInt128(*this)); }

	friend constexpr bool operator==(const Int128 &a, const Int128 &b) { return a.high_ == b.high_ && a.low_ == b.low_; }
	friend constexpr bool operator!=(const Int128 &a, const Int128 &b) { return !(a == b); }
	// With the sign bit flipped, two's complement orders as unsigned.
	friend constexpr bool operator<(const Int128 &a, const Int128 &b) {
		return UInt128::from_halves(a.high_ ^ kSignBit, a.low_) < UInt128::from_halves(b.high_ ^ kSignBit, b.low_);
	}
	friend constexpr bool operator>(const Int128 &a, const Int128 &b) { return b < a; }
	friend constexpr bool operator<=(const Int128 &a, const Int128 &b) { return !(b < a); }
	friend constexpr bool operator>=(const Int128 &a, const Int128 &b) { return !(a < b); }

	constexpr Int128 &operator+=(const Int128 &other) { return *this = *this + other; }
	constexpr Int128 &operator-=(const Int128 &other) { return *this = *this - other; }
	constexpr Int128 &operator*=(const Int128 &other) { return *this = *this * other; }
	constexpr Int128 &operator/=(const Int128 &other) { return *this = *this / other; }
	constexpr Int128 &operator%=(const Int128 &other) { return *this = *this % other; }
	constexpr Int128 &operator&=(const Int128 &other) { return *this = *this & other; }
	constexpr Int128 &operator|=(const Int128 &other) { return *this = *this | other; }
	constexpr Int128 &operator^=(const Int128 &other) { return *this = *this ^ other; }
	template <typename Count, std::enable_if_t<std::is_integral_v<Count>, int> = 0>
	constexpr Int128 &operator<<=(Count count) {
		return *this = *this << count;
	}
	template <typename Count, std::enable_if_t<std::is_integral_v<Count>, int> = 0>
	constexpr Int128 &operator>>=(Count count) {
		return *this = *this >> count;
	}
	constexpr Int128 &operator++() { return *this += 1; }
	constexpr Int128 &operator--() { return *this -= 1; }
	constexpr Int128 operator++(int) {
		const Int128 old = *this;
		*this += 1;
		return old;
	}
	constexpr Int128 operator--(int) {
		const Int128 old = *this;
		*this -= 1;
		return old;
	}

private:
	static constexpr std::uint64_t kSignBit = std::uint64_t{1} << 63;

	[[nodiscard]] constexpr bool negative() const { return (high_ & kSignBit) != 0; }
	// |x| as unsigned: 2^127 for INT128_MIN.
	static constexpr UInt128 magnitude(const Int128 &value) { return value.negative() ? -UInt128(value) : UInt128(value); }

	template <typename Floating>
	Floating nearest() const {
		const auto rounded = static_cast<Floating>(magnitude(*this));
		return negative() ? -rounded : rounded;
	}

	std::uint64_t low_ = 0;
	std::uint64_t high_ = 0;
};

constexpr UInt128::UInt128(const Int128 &value) : low_(value.low()), high_(value.high()) {}

// __builtin_add_overflow's contract (and sub's and mul's): *result receives
// the result modulo 2^128, and the answer is whether the exact one did not fit.
// The operands are copies, so `result` may point at either.
inline bool add_overflow(Int128 a, Int128 b, Int128 *result) {
	*result = a + b;
	// Two numbers of one sign overflow when their sum shows the other sign.
	return (a < 0) == (b < 0) && (*result < 0) != (a < 0);
}

inline bool sub_overflow(Int128 a, Int128 b, Int128 *result) {
	*result = a - b;
	// Numbers of different signs overflow when the difference takes b's sign.
	return (a < 0) != (b < 0) && (*result < 0) != (a < 0);
}

inline bool mul_overflow(Int128 a, Int128 b, Int128 *result) {
	*result = a * b;
	if (a == 0 || b == 0) return false;
	const bool negative = (a < 0) != (b < 0);
	const UInt128 x = a < 0 ? -UInt128(a) : UInt128(a);
	const UInt128 y = b < 0 ? -UInt128(b) : UInt128(b);
	// The largest magnitude that fits: 2^127 for a negative product, 2^127 - 1
	// for a positive one. Two magnitudes under 2^64 multiply exactly in 128
	// bits; otherwise x * y fits exactly when y <= limit / x.
	const UInt128 limit = (UInt128(1) << 127) - (negative ? 0U : 1U);
	if (x.high() == 0 && y.high() == 0) return x * y > limit;
	return y > limit / x;
}

} // namespace portable

#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 Int128;
__extension__ typedef unsigned __int128 UInt128;

// Whether the exact a + b (a - b, a * b) does not fit in an Int128; *result
// receives it modulo 2^128 either way.
inline bool add_overflow(Int128 a, Int128 b, Int128 *result) { return __builtin_add_overflow(a, b, result); }
inline bool sub_overflow(Int128 a, Int128 b, Int128 *result) { return __builtin_sub_overflow(a, b, result); }
inline bool mul_overflow(Int128 a, Int128 b, Int128 *result) { return __builtin_mul_overflow(a, b, result); }
#else
using Int128 = portable::Int128;
using UInt128 = portable::UInt128;
using portable::add_overflow;
using portable::mul_overflow;
using portable::sub_overflow;
#endif

// The zero bits above the highest one bit (below the lowest one bit) of a
// 32-bit value that is not zero; for zero the builtins give no answer.
inline int count_leading_zeros(std::uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_clz(value);
#elif defined(_MSC_VER)
	unsigned long index = 0;
	_BitScanReverse(&index, value);
	return 31 - static_cast<int>(index);
#else
	return portable::count_leading_zeros(value);
#endif
}

inline int count_trailing_zeros(std::uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_ctz(value);
#elif defined(_MSC_VER)
	unsigned long index = 0;
	_BitScanForward(&index, value);
	return static_cast<int>(index);
#else
	return portable::count_trailing_zeros(value);
#endif
}

} // namespace singularity::platform

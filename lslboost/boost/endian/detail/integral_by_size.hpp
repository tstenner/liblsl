#ifndef BOOST_ENDIAN_DETAIL_INTEGRAL_BY_SIZE_HPP_INCLUDED
#define BOOST_ENDIAN_DETAIL_INTEGRAL_BY_SIZE_HPP_INCLUDED

// Copyright 2019 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0.
// http://www.boost.org/LICENSE_1_0.txt

#include <cstdint>
#include <cstddef>

namespace lslboost
{
namespace endian
{
namespace detail
{

template<std::size_t N> struct integral_by_size
{
};

template<> struct integral_by_size<1>
{
    typedef std::uint8_t type;
};

template<> struct integral_by_size<2>
{
    typedef std::uint16_t type;
};

template<> struct integral_by_size<4>
{
    typedef std::uint32_t type;
};

template<> struct integral_by_size<8>
{
    typedef std::uint64_t type;
};

#if defined(__SIZEOF_INT128__)

template<> struct integral_by_size<16>
{
    typedef __uint128_t type;
};

#endif

} // namespace detail
} // namespace endian
} // namespace lslboost

#endif  // BOOST_ENDIAN_DETAIL_INTEGRAL_BY_SIZE_HPP_INCLUDED

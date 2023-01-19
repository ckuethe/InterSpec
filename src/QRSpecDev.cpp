/* InterSpec: an application to analyze spectral gamma radiation data.
 
 Copyright 2018 National Technology & Engineering Solutions of Sandia, LLC
 (NTESS). Under the terms of Contract DE-NA0003525 with NTESS, the U.S.
 Government retains certain rights in this software.
 For questions contact William Johnson via email at wcjohns@sandia.gov, or
 alternative emails of interspec@sandia.gov.
 
 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.
 
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.
 
 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "InterSpec_config.h"

#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <stdexcept>

extern "C"{
#include <zlib.h>
}

#include <boost/crc.hpp>
#include <boost/endian/conversion.hpp>

#include <Wt/Utils>

#include "QR-Code-generator/cpp/qrcodegen.hpp"

#include "SpecUtils/SpecFile.h"
#include "SpecUtils/DateTime.h"
#include "SpecUtils/Filesystem.h"
#include "SpecUtils/ParseUtils.h"
#include "SpecUtils/StringAlgo.h"
#include "SpecUtils/EnergyCalibration.h"

#include "InterSpec/QrCode.h"
#include "InterSpec/QRSpecDev.h"
#include "InterSpec/PhysicalUnits.h"

#include "oroch/bitpck.h" //basic bit-packing codec
#include "oroch/bitfor.h" //bit-packing with a frame-of-reference technique
#include "oroch/bitpfr.h" //bit-packing with a frame-of-reference and patching
#include "oroch/varint.h"

#include "streamvbyte.h"
#include "streamvbytedelta.h"

using namespace std;
namespace
{
  const char * const sm_hex_digits = "0123456789ABCDEF";

  // From: https://datatracker.ietf.org/doc/rfc9285/ , table 1
  const char sm_base42_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
  
  // Implement Table 1 in rfc9285 as a switch; should maybe just switch to using a lookup table
  uint8_t b45_to_dec( const char i )
  {
    switch( i )
    {
      case '0': return 0;
      case '1': return 1;
      case '2': return 2;
      case '3': return 3;
      case '4': return 4;
      case '5': return 5;
      case '6': return 6;
      case '7': return 7;
      case '8': return 8;
      case '9': return 9;
     
      // I think the letters should always be uppercase, but we'll allow lower case, jic, for the moment
      case 'A': case 'a': return 10;
      case 'B': case 'b': return 11;
      case 'C': case 'c': return 12;
      case 'D': case 'd': return 13;
      case 'E': case 'e': return 14;
      case 'F': case 'f': return 15;
      case 'G': case 'g': return 16;
      case 'H': case 'h': return 17;
      case 'I': case 'i': return 18;
      case 'J': case 'j': return 19;
      case 'K': case 'k': return 20;
      case 'L': case 'l': return 21;
      case 'M': case 'm': return 22;
      case 'N': case 'n': return 23;
      case 'O': case 'o': return 24;
      case 'P': case 'p': return 25;
      case 'Q': case 'q': return 26;
      case 'R': case 'r': return 27;
      case 'S': case 's': return 28;
      case 'T': case 't': return 29;
      case 'U': case 'u': return 30;
      case 'V': case 'v': return 31;
      case 'W': case 'w': return 32;
      case 'X': case 'x': return 33;
      case 'Y': case 'y': return 34;
      case 'Z': case 'z': return 35;
      case ' ': return 36;
      case '$': return 37;
      case '%': return 38;
      case '*': return 39;
      case '+': return 40;
      case '-': return 41;
      case '.': return 42;
      case '/': return 43;
      case ':': return 44;
      
      default:
        throw std::runtime_error( "Invalid base-45 character with decimal value "
                               + std::to_string( static_cast<int>(i) ) );
    }//switch( i )
  
    assert( 0 );
    return 255;
  }//int b45_to_dec( char )

 uint8_t hex_to_dec( char v )
 {
   if( v >= '0' && v <= '9' )
     return static_cast<uint8_t>( v - '0' );
   
   switch( v )
   {
     case 'A': case 'a': return 0x0A;
     case 'B': case 'b': return 0x0B;
     case 'C': case 'c': return 0x0C;
     case 'D': case 'd': return 0x0D;
     case 'E': case 'e': return 0x0E;
     case 'F': case 'f': return 0x0F;
   }
   
   throw runtime_error( string("Invalid hex-digit '") + v + string("'") );
   return 0;
 }//uint8_t hex_to_dec( char v )


// We cant just use Wt::Utils::urlEncode(...) because it puts hex-values into lower-case, where
//  we need them in upper case, since QR codes alpha-numeric are upper-case only
template<class T>
string url_encode( const T &url )
{
  static_assert( sizeof(typename T::value_type) == 1, "Must be byte-based container" );
  
  const std::string invalid_chars = " $&+,:;=?@'\"<>#%{}|\\^~[]`/";
  
  string answer;
  answer.reserve( url.size()*2 ); //A large guess, to keep from copying a lot during resizes
  
  for( const typename T::value_type val : url )
  {
    unsigned char c = (unsigned char)val;
    
    if( (c <= 31) || (c >= 127) || (invalid_chars.find(c) != std::string::npos) )
    {
      answer += '%';
      answer += sm_hex_digits[ ((c >> 4) & 0x0F) ];
      answer += sm_hex_digits[ (c & 0x0F) ];
    }else
    {
      answer += val;
    }
  }//for( const string::value_type val : url )
  
  return answer;
}//url_encode(...)

// If we arent gzipping, and not base-45 encoding, and using channel data as ascii, then
//  we will make sure this URL can be encoded as a QR in ASCII mode, we will URL-encode
//  all non-base-42 characters.
// Note: The result of this encoding, will also get URL encoded, which is less than ideal
//       but its just a few extra bytes.
string url_encode_non_base45( const string &input )
{
  string answer;
  for( const char val : input )
  {
    if( std::find( begin(sm_base42_chars), end(sm_base42_chars), val ) == end(sm_base42_chars) )
    {
      unsigned char c = (unsigned char)val;
      answer += '%';
      answer += sm_hex_digits[ ((c >> 4) & 0x0F) ];
      answer += sm_hex_digits[ (c & 0x0F) ];
    }else
    {
      answer += val;
    }
  }//for( const char val : operator_notes )
  
  return answer;
};//url_encode_non_base45


template<class T>
std::string base45_encode_bytes( const T &input )
{
  static_assert( sizeof(typename T::value_type) == 1, "Must be byte-based container" );
  
  //From rfc9285:
  // """For encoding, two bytes [a, b] MUST be interpreted as a number n in
  // base 256, i.e. as an unsigned integer over 16 bits so that the number
  // n = (a * 256) + b.
  // This number n is converted to base 45 [c, d, e] so that n = c + (d *
  // 45) + (e * 45 * 45).  Note the order of c, d and e which are chosen
  // so that the left-most [c] is the least significant."""
  
  const size_t input_size = input.size();
  const size_t dest_bytes = 3 * (input_size / 2) + ((input_size % 2) ? 2 : 0);
  string answer( dest_bytes, ' ' );
  
  size_t out_pos = 0;
  for( size_t i = 0; i < input_size; i += 2 )
  {
    if( (i + 1) < input_size )
    {
      // We will process two bytes, storing them into three base-45 letters
      // n = c + (d * 45) + (e * 45 * 45)
      const uint16_t val_0 = reinterpret_cast<const uint8_t &>( input[i] );
      const uint16_t val_1 = reinterpret_cast<const uint8_t &>( input[i+1] );
      
      uint16_t n = (val_0 << 8) + val_1;
      
      //n may be 65535.   65535/(45 * 45)=32
      
      const uint8_t e = n / (45 * 45);
      n %= (45 * 45);
      const uint8_t d = n / 45;
      const uint8_t c = n % 45;
      
      assert( e < sizeof(sm_base42_chars) );
      assert( c < sizeof(sm_base42_chars) );
      assert( d < sizeof(sm_base42_chars) );
      
      answer[out_pos++] = reinterpret_cast<const typename T::value_type &>( sm_base42_chars[c] );
      answer[out_pos++] = reinterpret_cast<const typename T::value_type &>( sm_base42_chars[d] );
      answer[out_pos++] = reinterpret_cast<const typename T::value_type &>( sm_base42_chars[e] );
      assert( out_pos <= dest_bytes );
    }else
    {
      // We have one last dangling byte
      // a = c + (45 * d)
      const uint8_t a = reinterpret_cast<const uint8_t &>( input[i] );
      const uint8_t d = a / 45;
      const uint8_t c = a % 45;
      
      assert( c < sizeof(sm_base42_chars) );
      assert( d < sizeof(sm_base42_chars) );
      
      answer[out_pos++] = reinterpret_cast<const typename T::value_type &>( sm_base42_chars[c] );
      answer[out_pos++] = reinterpret_cast<const typename T::value_type &>( sm_base42_chars[d] );
      assert( out_pos <= dest_bytes );
    }
  }//for( size_t i = 0; i < input_size; i += 2 )
  
  assert( out_pos == dest_bytes );
  
#ifndef NDEBUG
  for( const auto val : answer )
  {
    const char c = static_cast<char>( val );
    assert( std::find( begin(sm_base42_chars), end(sm_base42_chars), c ) != end(sm_base42_chars) );
  }
#endif
  
  return answer;
}//std::string base45_encode_bytes( const vector<uint8_t> &input )


template<class T>
void deflate_compress_internal( const void *in_data, size_t in_data_size, T &out_data)
{
  static_assert( sizeof(typename T::value_type) == 1, "Must be byte-based container" );
  
  uLongf out_len = compressBound( in_data_size );
  T buffer( out_len, 0x0 );
  
  const int rval = compress2( (Bytef *)buffer.data(),  &out_len, (const Bytef *)in_data, in_data_size, Z_BEST_COMPRESSION );
  
  if( (rval != Z_OK) || (out_len == 0) )  //Other possible values: Z_MEM_ERROR, Z_BUF_ERROR
    throw runtime_error( "Error compressing data" );
  
  buffer.resize( out_len );
  out_data.swap( buffer );
}//void deflate_compress_internal( const void *in_data, size_t in_data_size, std::vector<uint8_t> &out_data)



template<class T>
void deflate_decompress_internal( void *in_data, size_t in_data_size, T &out_data )
{
  static_assert( sizeof(typename T::value_type) == 1, "Must be byte-based container" );
  
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  
  if( inflateInit(&zs) != Z_OK )
    throw(std::runtime_error("deflate_decompress: error from inflateInit while de-compressing."));
  
  zs.next_in = (Bytef*)in_data;
  zs.avail_in = static_cast<unsigned int>( in_data_size );
  
  int ret = Z_OK;
  typename T::value_type buffer[1024*16];
  
  T result;
  
  do
  {
    zs.next_out = reinterpret_cast<Bytef *>(buffer);
    zs.avail_out = sizeof(buffer);
    
    ret = inflate( &zs, 0 );
    
    if( result.size() < zs.total_out)
      result.insert( end(result), buffer, buffer + (zs.total_out - result.size()) );
  }while( ret == Z_OK );
  
  inflateEnd( &zs );
  
  if( ret != Z_STREAM_END )
    throw runtime_error( "deflate_decompress: Error decompressing : ("
                        + std::to_string(ret) + ") "
                        + (zs.msg ? string(zs.msg) : string()) );
  
  out_data.swap( result );
}//void deflate_decompress_internal( const void *in_data, size_t in_data_size, std::vector<uint8_t> &out_data)

vector<uint32_t> compress_to_counted_zeros( const vector<uint32_t> &input )
{
  vector<uint32_t> results;
  
  const size_t nBin = input.size();
  
  for( size_t bin = 0; bin < nBin; ++bin )
  {
    const bool isZero = (input[bin] == 0);
    
    if( !isZero ) results.push_back( input[bin] );
    else          results.push_back( 0 );
    
    if( isZero )
    {
      uint32_t nBinZeroes = 0;
      while( ( bin < nBin ) && (input[bin] == 0) )
      {
        ++nBinZeroes;
        ++bin;
      }//while more zero bins
      
      results.push_back( nBinZeroes );
      
      if( bin != nBin )
        --bin;
    }//if( input[bin] == 0.0 )
  }//for( size_t bin = 0; bin < input.size(); ++bin )
  
  return results;
}//void compress_to_counted_zeros(...)


vector<uint32_t> zero_compress_expand( const vector<uint32_t> &input )
{
  vector<uint32_t> expanded;
  const auto dstart = begin(input);
  const auto dend = end(input);
  
  for( auto iter = dstart; iter != dend; ++iter)
  {
    if( ((*iter) != 0) || ((iter+1)==dend) )
    {
      expanded.push_back(*iter);
    }else
    {
      iter++;
      if( (*iter) == 0 )
        throw runtime_error( "Invalid counted zeros: less than one number of zeros" );
      
      const uint32_t nZeroes = *iter;
      
      if( (expanded.size() + nZeroes) > 131072 )
        throw runtime_error( "Invalid counted zeros: too many total elements" );
      
      for( uint32_t k = 0; k < nZeroes; ++k )
        expanded.push_back( 0 );
    }//if( at a non-zero value, the last value, or the next value is zero) / else
  }//for( iterate over data, iter )
  
  return expanded;
};//zero_compress_expand


string to_hex_bytes_str( const string &input )
{
  string answer;
  for( size_t i = 0; i < input.size(); ++i )
  {
    if( i )
      answer += " ";
    uint8_t v = static_cast<uint8_t>(input[i]);
    answer += sm_hex_digits[((v >> 4) & 0x0F)];
    answer += sm_hex_digits[(v & 0x0F)];
  }
  
  return answer;
}


}//namespace


namespace QRSpecDev
{
std::string base45_encode( const std::vector<uint8_t> &input )
{
  return base45_encode_bytes( input );
}

std::string base45_encode( const std::string &input )
{
  return base45_encode_bytes( input );
}


vector<uint8_t> base45_decode( const string &input )
{
  const size_t input_size = input.size();
  
  if( (input_size == 1) || ((input_size % 3) && ((input_size - 2) % 3)) )
    throw runtime_error( "base45_decode: invalid input size (" + std::to_string(input_size) + ")" );
  
  const size_t output_size = ( 2*(input_size / 3) + ((input_size % 3) ? 1 : 0) );
  vector<uint8_t> answer( output_size );
  
  for( size_t i = 0, output_pos = 0; i < input_size; i += 3 )
  {
    assert( (i + 2) <= input_size );
    
    const uint32_t c = b45_to_dec( input[i] );
    const uint32_t d = b45_to_dec( input[i+1] );
    uint32_t n = c + 45 * d;
    
    if( (i + 2) < input_size )
    {
      uint32_t e = b45_to_dec( input[i+2] );
      
      n += e * 45 * 45;
      
      if( n >= 65536 )
        throw runtime_error( "base45_decode: Invalid three character sequence ("
                             + input.substr(i,3) + " -> " + std::to_string(n) + ")" );
      
      assert( (n / 256) <= 255 );
      
      answer[output_pos++] = static_cast<uint8_t>( n / 256 );
      n %= 256;
    }//if( (i + 2) < input_size )
    
    assert( n <= 255 );
    answer[output_pos++] = static_cast<uint8_t>( n );
  }//for( size_t i = 0, output_pos = 0; i < input_size; i+=3 )
  
  return answer;
}//vector<uint8_t> base45_decode( const string &input )

/** Performs the same encoding as `streamvbyte_encode` from https://github.com/lemire/streamvbyte,
 but pre-pended with a uint16_t to give number of integer entries, has a c++ interface, and is way,
 way slower.
 */
vector<uint8_t> encode_stream_vbyte( const vector<uint32_t> &input )
{
  // Performs the same encoding as `streamvbyte_encode` from https://github.com/lemire/streamvbyte,
  //  but prepends answer with a uint16_t to give number of integer entries.
  //  This niave implementation is based on README of that project.
  
  // I this function might be okay on big-endian machines, but untested, so we'll leave a
  //  compile time assert here
  static_assert( boost::endian::order::native == boost::endian::order::little, "This function not tested in big-endian" );
  
  const size_t count = input.size();
  
  // We'll limit the size, for our implementation, because we should never be seeing greater than
  //  64k channel spectra, I think.  The leading uint16_t is the only part of this implementation
  //  that is size-limited.
  if( count > std::numeric_limits<uint16_t>::max() )
    throw runtime_error( "encode_stream_vbyte: input too large" );
  
  // Encoded data starts with ((count + 3) / 4), two bit ints
  const size_t num_cntl_bytes = (count + 3) / 4;
  
  vector<uint8_t> answer;
  answer.reserve( 2 * input.size() ); // Assumes not-super-high-statistics spectra, and is probably a bit larger than needed
  answer.resize( num_cntl_bytes + 2, 0x00 );
  
  const uint16_t ncount = static_cast<uint16_t>( count );
  answer[0] = static_cast<uint8_t>( ncount & 0x00FF );
  answer[1] = static_cast<uint8_t>( (ncount & 0xFF00) >> 8 );
  
  uint16_t test_ncount;
  memcpy( &test_ncount, &(answer[0]), 2 );
  assert( test_ncount == ncount );
  
  if( input.empty() )
    return answer;
  
  for( size_t i = 0; i < count; ++i )
  {
    const uint32_t val = input[i];
    
    uint8_t ctrl_val = 0;
    answer.push_back( static_cast<uint8_t>( val & 0x000000FF ) );
    if( val < 256 )
    {
      ctrl_val = 0;
    }else if( val < 65536 )
    {
      ctrl_val = 1;
      answer.push_back( static_cast<uint8_t>( (val & 0x0000FF00 ) >> 8  ) );
    }else if( val < 16777216 )
    {
      ctrl_val = 2;
      answer.push_back( static_cast<uint8_t>( (val & 0x0000FF00 ) >> 8  ) );
      answer.push_back( static_cast<uint8_t>( (val & 0x00FF0000 ) >> 16 ) );
    }else
    {
      ctrl_val = 3;
      answer.push_back( static_cast<uint8_t>( (val & 0x0000FF00 ) >> 8  ) );
      answer.push_back( static_cast<uint8_t>( (val & 0x00FF0000 ) >> 16 ) );
      answer.push_back( static_cast<uint8_t>( (val & 0xFF000000 ) >> 24 ) );
    }//if( we can represent in 1 byte ) / else 2 byte / 3 / 4
    
    const size_t ctrl_byte = i / 4;
    const uint8_t ctrl_shift = 2 * (i % 4);
    
    assert( ctrl_byte < num_cntl_bytes );
    
    answer[2 + ctrl_byte] |= (ctrl_val << ctrl_shift);
  }//for( size_t i = 0; i < count; ++i )
  
  return answer;
}//vector<uint8_t> encode_stream_vbyte( const vector<uint32_t> &input )


template<class T>
size_t decode_stream_vbyte( const T * const input_begin, const size_t nbytes, vector<uint32_t> &answer )
{
  //Performs the same encoding as `streamvbyte_decode` from https://github.com/lemire/streamvbyte,
  //  except assumes first two bytes gives a uint16_t for the number of integer entries.
  //  This niave implementation is based on README of that project.
  
  static_assert( sizeof(T) == 1, "Must be byte-based container" );
  // Maybe fine on big-endian, but untested
  static_assert( boost::endian::order::native == boost::endian::order::little, "This function not tested in big-endian" );
  
  if( nbytes < 2 )
    throw runtime_error( "decode_stream_vbyte: input isnt long enough to give num integers." );
  
  size_t nints = reinterpret_cast<const uint8_t &>( input_begin[1] );
  nints = nints << 8;
  nints += reinterpret_cast<const uint8_t &>( input_begin[0] );
  
  answer.resize( nints );
  
  const size_t num_cntl_bytes = (nints + 3) / 4;
  
  if( nbytes < (num_cntl_bytes + 2) )
    throw runtime_error( "decode_stream_vbyte: input isnt long enough for control bytes." );
  
  const uint8_t *begin_input = (const uint8_t *)input_begin;
  const uint8_t *ctrl_begin = begin_input + 2;
  const uint8_t *data_pos = ctrl_begin + num_cntl_bytes;
  const uint8_t * const data_end = (const uint8_t *)(input_begin + nbytes);
  
  for( size_t i = 0; i < nints; ++i )
  {
    uint32_t &val = answer[i];
    const size_t ctrl_byte = i / 4;
    const uint8_t ctrl_shift = 2 * (i % 4);
    const uint8_t ctrl_val = ((ctrl_begin[ctrl_byte] >> ctrl_shift) & 0x00003);
    assert( (ctrl_val == 0) || (ctrl_val == 1) || (ctrl_val == 2) || (ctrl_val == 3) );
    
    if( (data_pos + ctrl_val + 1) > data_end )
      throw runtime_error( "decode_stream_vbyte: input shorter than needed for specified number of integers" );
    
    val = *data_pos++;
    if( ctrl_val >= 1 )
      val += (static_cast<uint32_t>( *(data_pos++) ) << 8);
    if( ctrl_val >= 2 )
      val += (static_cast<uint32_t>( *(data_pos++) ) << 16);
    if( ctrl_val == 3 )
      val += (static_cast<uint32_t>( *(data_pos++) ) << 24);
  }//for( size_t i = 0; i < nints; ++i )
  
  return data_pos - begin_input;
}//size_t decode_stream_vbyte( const T input, size_t inlen, vector<uint32_t> &answer )


size_t decode_stream_vbyte( const std::vector<uint8_t> &inbuff, std::vector<uint32_t> &answer )
{
  return decode_stream_vbyte( inbuff.data(), inbuff.size(), answer );
}


void test_base45()
{
  string encode_output, decode_input;
  vector<uint8_t> encode_input, decode_output;

  //Encoding example 1:
  encode_input = { 65, 66 };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "BB8" );
  
  //Encoding example 2:
  encode_input = { 72, 101, 108, 108, 111, 33, 33 };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "%69 VD92EX0" );
  
  //Encoding example 3:
  encode_input = { 98, 97, 115, 101, 45, 52, 53 };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "UJCLQE7W581" );
  
  //Decoding example 1:
  decode_output = base45_decode( "QED8WEX0" );
  const string expected_decode_output = "ietf!";
  
  assert( decode_output.size() == expected_decode_output.size() );
  for( size_t i = 0; i < decode_output.size(); ++i )
  {
    assert( static_cast<char>(decode_output[i]) == expected_decode_output[i] );
  }
  
  try
  {
    // invalid character
    base45_decode( "=~QED8WEX0" );
    assert( 0 );
  }catch( std::exception & )
  {
  }
  
  try
  {
    // invalid number characters
    base45_decode( "A" );
    assert( 0 );
  }catch( std::exception & )
  {
  }
  
  try
  {
    // invalid number characters
    base45_decode( "AAAA" );
    assert( 0 );
  }catch( std::exception & )
  {
  }
  
  try
  {
    // Triplet producing too large of a value (>=65536)
    base45_decode( "GGW" );
    assert( 0 );
  }catch( std::exception & )
  {
  }
  
  
  encode_input = { 0x80, 0x80 };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "1BG" );
  decode_output = base45_decode( encode_output );
  assert( decode_output == encode_input );
  
  
  encode_input = { 0x80, 0x80, 0x80 };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "1BG%2" );
  decode_output = base45_decode( encode_output );
  assert( decode_output == encode_input );
  
  
  encode_input = { 0x8F, 0x80, 0x8F };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "G6I83" );
  decode_output = base45_decode( encode_output );
  assert( decode_output == encode_input );
  
  
  encode_input = { 0x8F, 0x80, 0x8F, 0xF0 };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "G6I%8I" );
  decode_output = base45_decode( encode_output );
  assert( decode_output == encode_input );
  
  encode_input = { 0x01, 0xFF };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "GB0" );
  decode_output = base45_decode( encode_output );
  assert( decode_output == encode_input );
  
  encode_input = { 0xAA, 0xFF };
  encode_output = base45_encode( encode_input );
  assert( encode_output == "ZRL" );
  decode_output = base45_decode( encode_output );
  assert( decode_output == encode_input );
  
  for( uint8_t i = 0x00; true ; ++i )
  {
    const char val = reinterpret_cast<char &>( i );
    const string input( 2, val );
    const string encode_output = base45_encode( input );
    assert( encode_output.size() == 3 );
    const vector<uint8_t> decode_output_bytes = base45_decode( encode_output );
    string decode_output( decode_output_bytes.size(), '\0' );
    memcpy( &(decode_output[0]), &(decode_output_bytes[0]), decode_output_bytes.size() );
    assert( decode_output == input );
    
    if( i == 255 )
      break;
  }
  
  cout << "Done in test_base45()" << endl;
}//void test_base45()

void test_bitpacking()
{
  // Test case 1
  const vector<uint32_t> test_1_chan_cnts{
    0, 6, 38, 108, 156, 169, 219, 247, 243, 282, 307, 313, 318, 308, 295, 286, 269, 232, 214, 222,
    212, 206, 188, 196, 213, 296, 411, 428, 331, 206, 152, 83, 60, 49, 49, 42, 49, 35, 31, 32,
    15, 22, 29, 18, 17, 17, 19, 15, 18, 14, 12, 15, 15, 8, 9, 11, 14, 14, 7, 9,
    8, 3, 5, 3, 9, 9, 4, 8, 3, 4, 2, 4, 5, 3, 3, 7, 0, 1, 4, 5,
    5, 1, 3, 12, 4, 6, 4, 0, 1, 4, 1, 1, 8, 2, 6, 3, 2, 4, 3, 6,
    5, 1, 1, 1, 1, 3, 6, 1, 1, 2, 1, 3, 4, 4, 0, 1, 4, 1, 3, 2,
    1, 1, 0, 1, 1, 3, 2, 0, 1, 2, 1, 1, 2, 1, 2, 1, 1, 0, 1, 2,
    0, 1, 3, 1, 0, 1, 2, 1, 2, 0, 1, 2, 2, 1, 1, 0, 2, 1, 0, 2,
    2, 2, 0, 2, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0, 1, 2, 1, 0, 1,
    3, 2, 0, 2, 2, 0, 4, 1, 0, 1, 1, 2, 1, 0, 1, 1, 0, 3, 4, 2,
    0, 1, 1, 4, 4, 2, 0, 2, 1, 0, 2, 2, 2, 0, 4, 1, 1, 0, 1, 1,
    0, 7, 1, 0, 2, 2, 0, 6, 1, 0, 4, 1, 0, 2, 1, 2, 1, 1, 0, 2,
    1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 12, 1, 0, 1, 1,
    1, 0, 18, 2, 1, 1, 0, 8, 1, 0, 16, 1, 0, 9, 1, 0, 1, 1, 0, 23,
    1, 0, 2, 2, 0, 21, 1, 1, 0, 7, 1, 0, 1, 1, 1, 0, 3, 1, 0, 6,
    1, 0, 16, 1, 0, 14, 1, 0, 1, 1, 1, 0, 8, 1, 0, 2, 1, 0, 40, 1,
    0, 21  };
  assert( test_1_chan_cnts.size() == 322 );
  const vector<uint8_t> test_1_packed{
    66, 1, 0, 0, 84, 85, 1, 0, 84, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 38, 108, 156, 169, 219, 247, 243, 26, 1, 51, 1, 57, 1, 62, 1,
    52, 1, 39, 1, 30, 1, 13, 1, 232, 214, 222, 212, 206, 188, 196, 213, 40, 1, 155, 1, 172, 1, 75, 1, 206, 152, 83, 60, 49, 49, 42, 49, 35, 31, 32, 15, 22, 29, 18, 17, 17, 19, 15, 18, 14, 12, 15, 15, 8, 9,
    11, 14, 14, 7, 9, 8, 3, 5, 3, 9, 9, 4, 8, 3, 4, 2, 4, 5, 3, 3, 7, 0, 1, 4, 5, 5, 1, 3, 12, 4, 6, 4, 0, 1, 4, 1, 1, 8, 2, 6, 3, 2, 4, 3, 6, 5, 1, 1, 1, 1,
    3, 6, 1, 1, 2, 1, 3, 4, 4, 0, 1, 4, 1, 3, 2, 1, 1, 0, 1, 1, 3, 2, 0, 1, 2, 1, 1, 2, 1, 2, 1, 1, 0, 1, 2, 0, 1, 3, 1, 0, 1, 2, 1, 2, 0, 1, 2, 2, 1, 1,
    0, 2, 1, 0, 2, 2, 2, 0, 2, 1, 3, 1, 1, 1, 1, 1, 1, 3, 1, 0, 1, 2, 1, 0, 1, 3, 2, 0, 2, 2, 0, 4, 1, 0, 1, 1, 2, 1, 0, 1, 1, 0, 3, 4, 2, 0, 1, 1, 4, 4,
    2, 0, 2, 1, 0, 2, 2, 2, 0, 4, 1, 1, 0, 1, 1, 0, 7, 1, 0, 2, 2, 0, 6, 1, 0, 4, 1, 0, 2, 1, 2, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0,
    12, 1, 0, 1, 1, 1, 0, 18, 2, 1, 1, 0, 8, 1, 0, 16, 1, 0, 9, 1, 0, 1, 1, 0, 23, 1, 0, 2, 2, 0, 21, 1, 1, 0, 7, 1, 0, 1, 1, 1, 0, 3, 1, 0, 6, 1, 0, 16, 1, 0,
    14, 1, 0, 1, 1, 1, 0, 8, 1, 0, 2, 1, 0, 40, 1, 0, 21
  };
  assert( test_1_packed.size() == 417 );
  const vector<uint8_t> test_1_encoded = QRSpecDev::encode_stream_vbyte( test_1_chan_cnts );
  assert( test_1_encoded == test_1_packed );
  vector<uint32_t> test_1_dec;
  const size_t test_1_nbytedec = QRSpecDev::decode_stream_vbyte(test_1_encoded,test_1_dec);
  assert( test_1_nbytedec == test_1_packed.size() );
  assert( test_1_dec == test_1_chan_cnts );
  
  
  
  
  // Test case 2
  const vector<uint32_t> test_2_chan_cnts{
    0, 6, 36, 96, 143, 221, 231, 276, 286, 292, 313, 341, 331, 325, 296, 297, 261, 216, 198, 246,
    249, 206, 188, 183, 232, 282, 417, 420, 277, 168, 109, 80, 44, 37, 31, 40, 39, 24, 18, 32,
    17, 28, 18, 26, 18, 18, 19, 15, 26, 17, 17, 11, 10, 17, 13, 10, 6, 9, 9, 7,
    9, 8, 10, 9, 7, 9, 7, 4, 7, 7, 3, 4, 1, 1, 4, 7, 7, 5, 4, 4,
    3, 2, 3, 6, 1, 5, 1, 6, 1, 4, 2, 2, 4, 4, 2, 1, 7, 2, 4, 4,
    3, 1, 5, 1, 1, 2, 1, 1, 0, 1, 4, 1, 6, 3, 1, 2, 0, 1, 5, 1,
    1, 1, 1, 1, 1, 4, 0, 1, 1, 0, 2, 1, 3, 2, 2, 1, 3, 1, 1, 1,
    2, 0, 1, 3, 0, 1, 1, 3, 0, 3, 1, 2, 2, 1, 0, 1, 2, 3, 0, 1,
    3, 2, 0, 4, 1, 1, 0, 1, 1, 1, 3, 1, 0, 6, 4, 0, 4, 1, 1, 1,
    1, 2, 0, 1, 2, 1, 2, 0, 2, 2, 1, 1, 2, 0, 1, 1, 0, 2, 1, 1,
    0, 4, 2, 1, 1, 0, 8, 2, 1, 0, 4, 1, 0, 2, 1, 0, 2, 1, 0, 2,
    2, 0, 1, 1, 2, 1, 0, 1, 1, 1, 1, 1, 1, 2, 0, 1, 1, 1, 0, 3,
    1, 0, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 10, 1, 0, 1, 1, 0,
    26, 1, 0, 2, 1, 0, 18, 1, 0, 18, 1, 1, 0, 8, 1, 0, 28, 2, 1, 0,
    7, 1, 0, 15, 1, 0, 30, 1, 0, 6, 1, 0, 66  };
  assert( test_2_chan_cnts.size() == 293 );
  const vector<uint8_t> test_2_packed{
    37, 1, 0, 64, 85, 85, 1, 0, 84, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 36, 96, 143, 221, 231, 20, 1, 30, 1, 36, 1, 57, 1, 85, 1, 75, 1, 69, 1, 40, 1, 41,
    1, 5, 1, 216, 198, 246, 249, 206, 188, 183, 232, 26, 1, 161, 1, 164, 1, 21, 1, 168, 109, 80, 44, 37, 31, 40, 39, 24, 18, 32, 17, 28, 18, 26, 18, 18, 19, 15, 26, 17, 17, 11, 10, 17, 13, 10, 6, 9, 9, 7,
    9, 8, 10, 9, 7, 9, 7, 4, 7, 7, 3, 4, 1, 1, 4, 7, 7, 5, 4, 4, 3, 2, 3, 6, 1, 5, 1, 6, 1, 4, 2, 2, 4, 4, 2, 1, 7, 2, 4, 4, 3, 1, 5, 1, 1, 2, 1, 1, 0, 1,
    4, 1, 6, 3, 1, 2, 0, 1, 5, 1, 1, 1, 1, 1, 1, 4, 0, 1, 1, 0, 2, 1, 3, 2, 2, 1, 3, 1, 1, 1, 2, 0, 1, 3, 0, 1, 1, 3, 0, 3, 1, 2, 2, 1, 0, 1, 2, 3, 0, 1,
    3, 2, 0, 4, 1, 1, 0, 1, 1, 1, 3, 1, 0, 6, 4, 0, 4, 1, 1, 1, 1, 2, 0, 1, 2, 1, 2, 0, 2, 2, 1, 1, 2, 0, 1, 1, 0, 2, 1, 1, 0, 4, 2, 1, 1, 0, 8, 2, 1, 0,
    4, 1, 0, 2, 1, 0, 2, 1, 0, 2, 2, 0, 1, 1, 2, 1, 0, 1, 1, 1, 1, 1, 1, 2, 0, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 10, 1, 0, 1, 1, 0,
    26, 1, 0, 2, 1, 0, 18, 1, 0, 18, 1, 1, 0, 8, 1, 0, 28, 2, 1, 0, 7, 1, 0, 15, 1, 0, 30, 1, 0, 6, 1, 0, 66
  };
  assert( test_2_packed.size() == 383 );
  const vector<uint8_t> test_2_encoded = QRSpecDev::encode_stream_vbyte( test_2_chan_cnts );
  assert( test_2_encoded == test_2_packed );
  vector<uint32_t> test_2_dec;
  const size_t test_2_nbytedec = QRSpecDev::decode_stream_vbyte(test_2_encoded,test_2_dec);
  assert( test_2_nbytedec == test_2_packed.size() );
  assert( test_2_dec == test_2_chan_cnts );
  
  
  
  
  // Test case 3
  const vector<uint32_t> test_3_chan_cnts{
    0, 6, 39, 111, 136, 215, 238, 277, 296, 315, 292, 358, 331, 306, 324, 278, 224, 238, 225, 206,
    244, 200, 198, 196, 215, 314, 406, 389, 327, 188, 119, 79, 60, 48, 33, 49, 42, 27, 21, 30,
    17, 18, 20, 20, 17, 18, 9, 18, 11, 18, 12, 15, 9, 15, 10, 14, 7, 12, 11, 10,
    7, 3, 5, 7, 7, 3, 12, 7, 6, 5, 3, 12, 5, 2, 3, 5, 2, 4, 3, 3,
    3, 3, 5, 4, 2, 4, 2, 3, 7, 4, 2, 3, 4, 2, 3, 2, 4, 3, 5, 0,
    1, 3, 0, 1, 3, 2, 1, 1, 1, 2, 3, 1, 0, 2, 1, 0, 1, 3, 5, 1,
    5, 5, 5, 0, 1, 2, 0, 1, 3, 1, 4, 4, 4, 1, 1, 2, 3, 1, 2, 3,
    2, 2, 2, 1, 2, 2, 1, 0, 1, 1, 0, 4, 1, 0, 1, 1, 0, 1, 1, 1,
    2, 0, 1, 1, 0, 2, 1, 2, 1, 0, 1, 1, 2, 1, 0, 2, 1, 0, 1, 2,
    1, 0, 1, 1, 0, 1, 2, 1, 1, 2, 1, 3, 1, 0, 2, 1, 1, 1, 1, 0,
    1, 2, 2, 3, 1, 0, 1, 1, 0, 6, 1, 0, 2, 1, 0, 1, 2, 1, 0, 3,
    2, 1, 0, 1, 1, 0, 8, 2, 0, 5, 1, 0, 1, 1, 1, 1, 1, 2, 0, 1,
    1, 4, 1, 2, 0, 1, 1, 0, 1, 1, 2, 0, 1, 1, 1, 2, 0, 1, 1, 0,
    1, 1, 1, 1, 0, 7, 1, 0, 3, 1, 0, 20, 1, 1, 0, 1, 1, 0, 2, 1,
    0, 2, 1, 1, 0, 1, 1, 0, 9, 1, 0, 13, 1, 0, 4, 1, 0, 1, 2, 0,
    8, 1, 0, 8, 1, 0, 5, 1, 0, 3, 1, 0, 14, 1, 0, 5, 1, 0, 9, 2,
    0, 11, 2, 0, 19, 1, 0, 8, 1, 0, 7, 1, 0, 4, 1, 0, 32, 1, 0, 4,
    1, 0, 25  };
  assert( test_3_chan_cnts.size() == 343 );
  const vector<uint8_t> test_3_packed{
    87, 1, 0, 64, 85, 85, 0, 0, 84, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 39, 111, 136, 215, 238, 21, 1, 40, 1, 59,
    1, 36, 1, 102, 1, 75, 1, 50, 1, 68, 1, 22, 1, 224, 238, 225, 206, 244, 200, 198, 196, 215, 58, 1, 150, 1, 133, 1, 71, 1, 188, 119, 79, 60, 48, 33, 49, 42, 27, 21, 30, 17, 18, 20, 20, 17, 18, 9, 18, 11,
    18, 12, 15, 9, 15, 10, 14, 7, 12, 11, 10, 7, 3, 5, 7, 7, 3, 12, 7, 6, 5, 3, 12, 5, 2, 3, 5, 2, 4, 3, 3, 3, 3, 5, 4, 2, 4, 2, 3, 7, 4, 2, 3, 4, 2, 3, 2, 4, 3, 5,
    0, 1, 3, 0, 1, 3, 2, 1, 1, 1, 2, 3, 1, 0, 2, 1, 0, 1, 3, 5, 1, 5, 5, 5, 0, 1, 2, 0, 1, 3, 1, 4, 4, 4, 1, 1, 2, 3, 1, 2, 3, 2, 2, 2, 1, 2, 2, 1, 0, 1,
    1, 0, 4, 1, 0, 1, 1, 0, 1, 1, 1, 2, 0, 1, 1, 0, 2, 1, 2, 1, 0, 1, 1, 2, 1, 0, 2, 1, 0, 1, 2, 1, 0, 1, 1, 0, 1, 2, 1, 1, 2, 1, 3, 1, 0, 2, 1, 1, 1, 1,
    0, 1, 2, 2, 3, 1, 0, 1, 1, 0, 6, 1, 0, 2, 1, 0, 1, 2, 1, 0, 3, 2, 1, 0, 1, 1, 0, 8, 2, 0, 5, 1, 0, 1, 1, 1, 1, 1, 2, 0, 1, 1, 4, 1, 2, 0, 1, 1, 0, 1,
    1, 2, 0, 1, 1, 1, 2, 0, 1, 1, 0, 1, 1, 1, 1, 0, 7, 1, 0, 3, 1, 0, 20, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 1, 0, 1, 1, 0, 9, 1, 0, 13, 1, 0, 4, 1, 0, 1, 2,
    0, 8, 1, 0, 8, 1, 0, 5, 1, 0, 3, 1, 0, 14, 1, 0, 5, 1, 0, 9, 2, 0, 11, 2, 0, 19, 1, 0, 8, 1, 0, 7, 1, 0, 4, 1, 0, 32, 1, 0, 4, 1, 0, 25
  };
  assert( test_3_packed.size() == 444 );
  const vector<uint8_t> test_3_encoded = QRSpecDev::encode_stream_vbyte( test_3_chan_cnts );
  assert( test_3_encoded == test_3_packed );
  vector<uint32_t> test_3_dec;
  const size_t test_3_nbytedec = QRSpecDev::decode_stream_vbyte(test_3_encoded,test_3_dec);
  assert( test_3_nbytedec == test_3_packed.size() );
  assert( test_3_dec == test_3_chan_cnts );
  
  
  
  
  // Test case 4
  const vector<uint32_t> test_4_chan_cnts{
    0, 6, 42, 97, 132, 163, 242, 278, 288, 317, 303, 301, 322, 362, 298, 290, 228, 191, 236, 219,
    238, 223, 176, 196, 194, 277, 341, 400, 337, 219, 135, 73, 54, 47, 42, 34, 33, 36, 33, 29,
    23, 25, 19, 23, 11, 10, 20, 21, 18, 11, 12, 11, 14, 19, 22, 17, 7, 9, 3, 7,
    4, 9, 3, 5, 4, 6, 10, 3, 11, 8, 6, 8, 4, 8, 3, 5, 3, 5, 3, 4,
    4, 6, 5, 5, 5, 5, 2, 3, 1, 2, 3, 3, 4, 3, 5, 3, 3, 4, 0, 2,
    2, 1, 4, 1, 5, 4, 2, 1, 3, 2, 1, 1, 2, 2, 3, 3, 0, 1, 3, 2,
    2, 1, 0, 1, 1, 1, 1, 0, 1, 1, 2, 3, 3, 4, 1, 4, 0, 1, 1, 0,
    1, 2, 0, 1, 1, 1, 1, 4, 0, 2, 2, 0, 1, 2, 1, 2, 1, 2, 0, 1,
    1, 2, 2, 1, 3, 4, 2, 1, 2, 2, 1, 0, 2, 3, 1, 2, 1, 1, 4, 3,
    0, 2, 1, 0, 1, 1, 0, 1, 2, 0, 3, 1, 1, 0, 1, 1, 0, 4, 3, 1,
    5, 3, 0, 1, 3, 0, 1, 1, 0, 3, 1, 0, 1, 2, 1, 2, 0, 3, 1, 1,
    0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 1, 0, 6, 2, 1, 1, 0, 6,
    2, 3, 2, 2, 0, 1, 2, 1, 0, 3, 2, 0, 4, 1, 0, 2, 1, 0, 4, 1,
    0, 2, 2, 1, 0, 2, 1, 0, 5, 1, 0, 5, 1, 0, 14, 1, 0, 3, 1, 0,
    2, 1, 0, 1, 1, 0, 12, 1, 0, 4, 1, 0, 27, 1, 0, 5, 1, 0, 28, 1,
    0, 6, 2, 0, 17, 1, 0, 20, 1, 0, 8, 1, 0, 68  };
  assert( test_4_chan_cnts.size() == 314 );
  const vector<uint8_t> test_4_packed{
    58, 1, 0, 64, 85, 85, 0, 0, 84, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 42, 97, 132, 163, 242, 22, 1, 32, 1, 61, 1, 47, 1, 45, 1, 66, 1,
    106, 1, 42, 1, 34, 1, 228, 191, 236, 219, 238, 223, 176, 196, 194, 21, 1, 85, 1, 144, 1, 81, 1, 219, 135, 73, 54, 47, 42, 34, 33, 36, 33, 29, 23, 25, 19, 23, 11, 10, 20, 21, 18, 11, 12, 11, 14, 19, 22, 17,
    7, 9, 3, 7, 4, 9, 3, 5, 4, 6, 10, 3, 11, 8, 6, 8, 4, 8, 3, 5, 3, 5, 3, 4, 4, 6, 5, 5, 5, 5, 2, 3, 1, 2, 3, 3, 4, 3, 5, 3, 3, 4, 0, 2, 2, 1, 4, 1, 5, 4,
    2, 1, 3, 2, 1, 1, 2, 2, 3, 3, 0, 1, 3, 2, 2, 1, 0, 1, 1, 1, 1, 0, 1, 1, 2, 3, 3, 4, 1, 4, 0, 1, 1, 0, 1, 2, 0, 1, 1, 1, 1, 4, 0, 2, 2, 0, 1, 2, 1, 2,
    1, 2, 0, 1, 1, 2, 2, 1, 3, 4, 2, 1, 2, 2, 1, 0, 2, 3, 1, 2, 1, 1, 4, 3, 0, 2, 1, 0, 1, 1, 0, 1, 2, 0, 3, 1, 1, 0, 1, 1, 0, 4, 3, 1, 5, 3, 0, 1, 3, 0,
    1, 1, 0, 3, 1, 0, 1, 2, 1, 2, 0, 3, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 1, 0, 6, 2, 1, 1, 0, 6, 2, 3, 2, 2, 0, 1, 2, 1, 0, 3, 2, 0, 4, 1, 0, 2,
    1, 0, 4, 1, 0, 2, 2, 1, 0, 2, 1, 0, 5, 1, 0, 5, 1, 0, 14, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 12, 1, 0, 4, 1, 0, 27, 1, 0, 5, 1, 0, 28, 1, 0, 6, 2, 0, 17, 1,
    0, 20, 1, 0, 8, 1, 0, 68
  };
  assert( test_4_packed.size() == 408 );
  const vector<uint8_t> test_4_encoded = QRSpecDev::encode_stream_vbyte( test_4_chan_cnts );
  assert( test_4_encoded == test_4_packed );
  vector<uint32_t> test_4_dec;
  const size_t test_4_nbytedec = QRSpecDev::decode_stream_vbyte(test_4_encoded,test_4_dec);
  assert( test_4_nbytedec == test_4_packed.size() );
  assert( test_4_dec == test_4_chan_cnts );
  
  
  
  
  // Test case 5
  const vector<uint32_t> test_5_chan_cnts{
    0, 6, 41, 89, 143, 180, 248, 267, 272, 286, 286, 294, 331, 309, 277, 234, 249, 188, 192, 216,
    245, 191, 200, 172, 218, 300, 401, 351, 297, 146, 113, 78, 52, 50, 43, 36, 35, 29, 23, 24,
    29, 21, 14, 29, 17, 19, 19, 14, 15, 13, 15, 7, 17, 14, 18, 9, 3, 5, 10, 5,
    4, 7, 7, 1, 6, 6, 3, 3, 3, 7, 2, 4, 4, 6, 6, 2, 5, 8, 3, 3,
    8, 6, 3, 1, 3, 7, 4, 2, 2, 0, 1, 2, 3, 4, 3, 2, 4, 4, 4, 6,
    0, 1, 2, 1, 1, 3, 0, 1, 2, 2, 0, 1, 3, 1, 2, 2, 3, 2, 2, 3,
    3, 0, 3, 3, 1, 2, 1, 1, 2, 5, 1, 0, 1, 3, 3, 3, 2, 1, 2, 2,
    1, 2, 0, 2, 2, 0, 1, 3, 0, 3, 1, 1, 4, 2, 1, 0, 2, 1, 1, 1,
    0, 1, 1, 1, 2, 3, 0, 1, 1, 0, 1, 1, 1, 2, 0, 4, 1, 1, 1, 0,
    1, 1, 2, 0, 1, 1, 0, 1, 1, 1, 0, 1, 3, 0, 1, 2, 2, 0, 3, 1,
    0, 1, 1, 2, 0, 1, 1, 2, 0, 6, 1, 1, 2, 0, 1, 1, 0, 2, 2, 0,
    1, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 1, 0, 5, 1, 0, 1, 1, 0, 2,
    1, 1, 0, 1, 1, 3, 0, 2, 3, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0,
    1, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 7, 1, 0, 1, 1, 0, 2, 1,
    1, 0, 7, 1, 0, 2, 1, 1, 0, 6, 1, 1, 0, 6, 1, 0, 2, 1, 0, 7,
    1, 0, 32, 1, 0, 22, 1, 0, 3, 1, 0, 14, 1, 0, 8, 1, 0, 14, 1, 0,
    22, 1, 0, 6, 1, 0, 42, 1, 0, 23  };
  assert( test_5_chan_cnts.size() == 330 );
  const vector<uint8_t> test_5_packed{
    74, 1, 0, 64, 85, 21, 0, 0, 84, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 41, 89, 143, 180, 248, 11, 1, 16, 1, 30, 1, 30, 1,
    38, 1, 75, 1, 53, 1, 21, 1, 234, 249, 188, 192, 216, 245, 191, 200, 172, 218, 44, 1, 145, 1, 95, 1, 41, 1, 146, 113, 78, 52, 50, 43, 36, 35, 29, 23, 24, 29, 21, 14, 29, 17, 19, 19, 14, 15, 13, 15, 7, 17,
    14, 18, 9, 3, 5, 10, 5, 4, 7, 7, 1, 6, 6, 3, 3, 3, 7, 2, 4, 4, 6, 6, 2, 5, 8, 3, 3, 8, 6, 3, 1, 3, 7, 4, 2, 2, 0, 1, 2, 3, 4, 3, 2, 4, 4, 4, 6, 0, 1, 2,
    1, 1, 3, 0, 1, 2, 2, 0, 1, 3, 1, 2, 2, 3, 2, 2, 3, 3, 0, 3, 3, 1, 2, 1, 1, 2, 5, 1, 0, 1, 3, 3, 3, 2, 1, 2, 2, 1, 2, 0, 2, 2, 0, 1, 3, 0, 3, 1, 1, 4,
    2, 1, 0, 2, 1, 1, 1, 0, 1, 1, 1, 2, 3, 0, 1, 1, 0, 1, 1, 1, 2, 0, 4, 1, 1, 1, 0, 1, 1, 2, 0, 1, 1, 0, 1, 1, 1, 0, 1, 3, 0, 1, 2, 2, 0, 3, 1, 0, 1, 1,
    2, 0, 1, 1, 2, 0, 6, 1, 1, 2, 0, 1, 1, 0, 2, 2, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 3, 0, 2, 3, 1, 0, 1, 1,
    0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 7, 1, 0, 1, 1, 0, 2, 1, 1, 0, 7, 1, 0, 2, 1, 1, 0, 6, 1, 1, 0, 6, 1, 0, 2, 1, 0, 7, 1, 0, 32,
    1, 0, 22, 1, 0, 3, 1, 0, 14, 1, 0, 8, 1, 0, 14, 1, 0, 22, 1, 0, 6, 1, 0, 42, 1, 0, 23
  };
  assert( test_5_packed.size() == 427 );
  const vector<uint8_t> test_5_encoded = QRSpecDev::encode_stream_vbyte( test_5_chan_cnts );
  assert( test_5_encoded == test_5_packed );
  vector<uint32_t> test_5_dec;
  const size_t test_5_nbytedec = QRSpecDev::decode_stream_vbyte(test_5_encoded,test_5_dec);
  assert( test_5_nbytedec == test_5_packed.size() );
  assert( test_5_dec == test_5_chan_cnts );
  
  
  
  
  // Test case 6
  const vector<uint32_t> test_6_chan_cnts{
    0, 38, 4, 17, 35, 31, 40, 40, 41, 37, 34, 48, 40, 46, 41, 38, 26, 25, 31, 40,
    26, 29, 35, 35, 37, 33, 28, 46, 28, 36, 27, 28, 37, 30, 21, 31, 26, 33, 34, 36,
    30, 28, 25, 27, 22, 23, 34, 33, 33, 32, 27, 36, 37, 39, 28, 20, 31, 21, 27, 21,
    35, 22, 21, 25, 25, 28, 33, 16, 26, 30, 26, 25, 34, 23, 27, 22, 30, 24, 27, 26,
    27, 19, 25, 23, 22, 28, 15, 26, 26, 29, 24, 27, 20, 19, 22, 24, 22, 14, 13, 30,
    19, 26, 21, 21, 35, 30, 27, 29, 31, 21, 36, 31, 30, 31, 31, 34, 25, 41, 28, 31,
    33, 35, 23, 30, 40, 29, 35, 35, 26, 47, 35, 40, 41, 46, 32, 38, 41, 43, 37, 39,
    35, 33, 36, 35, 41, 47, 49, 50, 47, 51, 70, 59, 60, 42, 45, 53, 41, 41, 54, 55,
    57, 47, 41, 49, 52, 40, 39, 49, 57, 51, 52, 52, 52, 66, 67, 66, 76, 54, 49, 62,
    55, 56, 65, 57, 51, 47, 53, 56, 47, 46, 42, 58, 53, 69, 45, 59, 61, 47, 47, 62,
    49, 54, 59, 54, 50, 61, 53, 50, 62, 65, 56, 55, 59, 66, 67, 57, 53, 56, 66, 54,
    67, 60, 66, 52, 67, 63, 63, 58, 57, 62, 60, 66, 53, 56, 42, 55, 65, 58, 50, 62,
    68, 63, 57, 66, 66, 45, 56, 73, 41, 52, 56, 62, 64, 69, 66, 57, 64, 59, 64, 50,
    62, 70, 68, 46, 61, 62, 71, 59, 68, 61, 64, 73, 53, 56, 76, 53, 63, 49, 56, 59,
    62, 61, 71, 67, 57, 63, 61, 59, 58, 62, 64, 59, 70, 56, 62, 48, 52, 50, 65, 68,
    43, 55, 70, 62, 66, 68, 53, 71, 63, 54, 60, 43, 63, 55, 67, 71, 57, 61, 62, 62,
    62, 51, 49, 62, 57, 58, 50, 50, 58, 55, 56, 61, 46, 48, 46, 54, 71, 59, 59, 59,
    43, 48, 52, 55, 63, 47, 55, 44, 60, 50, 58, 47, 63, 58, 49, 65, 64, 52, 53, 45,
    51, 48, 48, 49, 36, 75, 41, 58, 36, 43, 45, 54, 51, 43, 52, 40, 48, 54, 46, 52,
    64, 44, 40, 57, 52, 39, 54, 57, 47, 44, 64, 49, 43, 38, 56, 55, 48, 54, 47, 53,
    53, 55, 47, 49, 27, 36, 52, 41, 60, 60, 44, 65, 65, 42, 43, 44, 46, 56, 47, 36,
    50, 60, 44, 45, 40, 54, 49, 61, 48, 43, 48, 33, 31, 43, 55, 40, 46, 45, 49, 43,
    56, 53, 45, 55, 41, 45, 42, 54, 39, 47, 52, 45, 41, 40, 35, 55, 43, 36, 46, 54,
    42, 53, 52, 30, 55, 50, 50, 60, 103, 164, 249, 256, 152, 87, 44, 33, 25, 31, 27, 30,
    18, 39, 25, 36, 27, 20, 31, 31, 30, 23, 28, 26, 31, 20, 23, 31, 23, 18, 29, 25,
    32, 20, 25, 27, 21, 26, 38, 29, 26, 37, 22, 14, 33, 25, 33, 29, 35, 33, 25, 28,
    14, 27, 33, 58, 66, 40, 37, 28, 22, 28, 33, 21, 29, 18, 25, 22, 31, 18, 22, 21,
    20, 21, 17, 25, 27, 17, 22, 26, 18, 28, 16, 28, 11, 25, 13, 27, 21, 18, 17, 25,
    21, 23, 26, 14, 22, 13, 24, 17, 23, 21, 17, 27, 12, 22, 17, 20, 18, 15, 23, 17,
    14, 14, 22, 19, 21, 18, 16, 17, 24, 20, 16, 24, 22, 23, 22, 14, 22, 19, 19, 19,
    20, 21, 27, 23, 19, 18, 26, 23, 11, 26, 9, 22, 28, 30, 28, 38, 34, 29, 16, 15,
    20, 24, 14, 24, 23, 15, 12, 16, 19, 13, 18, 14, 12, 16, 18, 18, 21, 9, 15, 10,
    11, 15, 11, 8, 9, 14, 14, 9, 14, 7, 16, 19, 15, 10, 12, 14, 9, 19, 11, 12,
    21, 15, 10, 15, 17, 13, 18, 14, 14, 9, 15, 19, 16, 18, 11, 19, 9, 14, 12, 14,
    10, 10, 12, 10, 14, 14, 13, 15, 18, 14, 10, 14, 20, 19, 14, 11, 12, 11, 16, 14,
    13, 11, 14, 18, 19, 12, 8, 13, 13, 18, 11, 11, 14, 15, 16, 6, 9, 12, 18, 11,
    13, 11, 11, 18, 15, 12, 10, 10, 11, 16, 11, 14, 12, 12, 15, 7, 8, 16, 8, 13,
    13, 8, 14, 10, 12, 12, 11, 8, 9, 16, 13, 13, 12, 10, 6, 18, 9, 17, 11, 10,
    12, 10, 12, 12, 11, 6, 14, 16, 23, 31, 19, 15, 15, 10, 15, 11, 5, 12, 6, 9,
    10, 10, 14, 6, 10, 8, 8, 13, 10, 10, 11, 13, 10, 14, 13, 11, 10, 10, 11, 9,
    8, 12, 10, 10, 7, 14, 5, 11, 8, 13, 8, 9, 10, 7, 7, 5, 10, 10, 7, 11,
    12, 11, 9, 14, 7, 11, 10, 6, 7, 13, 4, 14, 12, 8, 9, 13, 6, 5, 11, 9,
    11, 11, 12, 11, 6, 9, 7, 7, 9, 8, 9, 11, 9, 14, 13, 15, 11, 6, 12, 14,
    9, 15, 8, 5, 6, 9, 6, 7, 7, 18, 9, 8, 9, 4, 10, 9, 5, 5, 4, 8,
    8, 5, 13, 4, 12, 8, 6, 14, 17, 16, 10, 11, 11, 7, 11, 3, 11, 6, 11, 4,
    8, 6, 8, 6, 3, 5, 3, 8, 6, 8, 5, 9, 6, 6, 10, 8, 10, 8, 4, 7,
    11, 6, 12, 27, 18, 20, 16, 15, 11, 7, 5, 14, 9, 6, 4, 4, 4, 4, 9, 12,
    9, 7, 5, 7, 8, 4, 6, 13, 11, 5, 9, 4, 8, 5, 7, 11, 7, 3, 5, 7,
    8, 8, 4, 8, 1, 10, 5, 5, 6, 7, 6, 5, 2, 8, 5, 11, 5, 8, 8, 6,
    6, 13, 5, 7, 4, 8, 6, 8, 7, 6, 4, 3, 7, 7, 3, 7, 8, 9, 2, 9,
    6, 6, 5, 4, 7, 2, 4, 6, 10, 6, 9, 3, 5, 5, 10, 6, 5, 5, 7, 9,
    9, 5, 6, 5, 10, 5, 7, 4, 5, 5, 9, 5, 2, 4, 5, 5, 5, 6, 4, 4,
    8, 8, 5, 6, 4, 4, 4, 6, 2, 4, 6, 3, 3, 5, 3, 10, 11, 10, 9, 5,
    4, 5, 7, 4, 7, 9, 4, 11, 4, 7, 4, 7, 6, 4, 6, 5, 4, 8, 6, 8,
    8, 8, 5, 7, 7, 9, 4, 8, 4, 6, 5, 1, 8, 9, 9, 5, 4, 9, 3, 0,
    1, 4, 5, 6, 7, 5, 2, 6, 2, 5, 5, 7, 2, 5, 5, 9, 8, 10, 4, 5,
    6, 1, 7, 3, 3, 9, 6, 3, 5, 9, 4, 11, 5, 2, 4, 3, 6, 5, 3, 7,
    5, 4, 4, 6, 3, 3, 3, 3, 6, 4, 4, 1, 3, 5, 3, 3, 4, 2, 5, 2,
    2, 1, 1, 4, 7, 5, 12, 6, 3, 3, 2, 4, 3, 2, 3, 4, 6, 7, 5, 3,
    5, 5, 3, 6, 4, 4, 3, 6, 1, 6, 3, 5, 3, 9, 3, 5, 4, 4, 3, 6,
    4, 6, 1, 8, 4, 5, 5, 3, 4, 3, 3, 6, 7, 3, 6, 5, 2, 2, 2, 10,
    2, 2, 4, 6, 4, 1, 3, 7, 4, 4, 2, 6, 7, 4, 1, 5, 1, 8, 4, 1,
    5, 4, 4, 2, 3, 2, 5, 6, 4, 5, 1, 2, 4, 2, 4, 1, 6, 2, 2, 1,
    3, 6, 8, 5, 1, 6, 5, 5, 5, 3, 2, 4, 3, 4, 3, 10, 4, 6, 6, 3,
    2, 5, 6, 1, 3, 4, 3, 3, 6, 6, 5, 2, 5, 3, 2, 7, 4, 2, 1, 1,
    2, 4, 1, 3, 3, 5, 5, 2, 3, 2, 4, 1, 3, 1, 1, 3, 2, 2, 5, 3,
    1, 3, 1, 6, 1, 3, 3, 7, 1, 2, 3, 3, 5, 5, 0, 1, 3, 6, 2, 6,
    3, 3, 7, 1, 1, 3, 3, 4, 4, 5, 7, 4, 5, 2, 5, 2, 4, 9, 11, 10,
    13, 11, 11, 9, 10, 9, 7, 2, 6, 2, 4, 1, 2, 3, 3, 1, 5, 5, 3, 5,
    4, 6, 5, 8, 7, 3, 2, 3, 6, 6, 2, 3, 4, 4, 7, 5, 4, 4, 5, 5,
    2, 1, 2, 6, 3, 3, 7, 6, 3, 2, 3, 1, 2, 3, 5, 4, 5, 2, 1, 3,
    4, 2, 5, 2, 3, 6, 2, 2, 2, 4, 5, 3, 0, 1, 3, 1, 5, 4, 6, 3,
    2, 0, 1, 6, 2, 2, 3, 4, 3, 2, 4, 4, 7, 6, 5, 10, 5, 3, 3, 2,
    1, 3, 5, 3, 2, 3, 6, 0, 1, 3, 1, 2, 2, 4, 1, 3, 3, 1, 1, 3,
    4, 2, 4, 4, 2, 4, 4, 4, 2, 1, 3, 0, 1, 5, 2, 3, 2, 3, 0, 1,
    7, 2, 3, 2, 3, 2, 1, 2, 2, 5, 4, 1, 5, 1, 5, 3, 4, 3, 2, 1,
    4, 1, 4, 2, 3, 2, 2, 2, 4, 3, 1, 1, 1, 2, 0, 1, 5, 2, 4, 2,
    1, 5, 2, 3, 7, 4, 4, 3, 2, 2, 1, 2, 3, 2, 1, 10, 5, 3, 4, 1,
    3, 4, 8, 10, 17, 16, 11, 6, 6, 2, 2, 3, 4, 2, 2, 2, 0, 1, 3, 2,
    3, 4, 2, 1, 1, 0, 1, 7, 3, 4, 3, 2, 4, 2, 4, 0, 1, 2, 2, 2,
    1, 3, 3, 1, 2, 5, 2, 1, 0, 1, 2, 3, 4, 1, 4, 4, 1, 2, 6, 1,
    0, 1, 1, 3, 0, 1, 5, 6, 3, 8, 0, 1, 3, 6, 0, 1, 2, 3, 2, 2,
    4, 13, 18, 23, 35, 19, 15, 9, 5, 1, 2, 2, 3, 1, 0, 1, 1, 2, 1, 2,
    1, 0, 1, 4, 0, 2, 2, 2, 2, 1, 1, 3, 0, 1, 2, 5, 3, 1, 1, 3,
    5, 2, 5, 1, 3, 2, 1, 4, 3, 3, 3, 3, 2, 2, 5, 2, 1, 2, 1, 2,
    4, 2, 0, 1, 3, 2, 3, 5, 1, 3, 2, 2, 1, 0, 1, 4, 1, 3, 1, 1,
    6, 1, 4, 0, 1, 5, 4, 3, 2, 1, 2, 2, 2, 2, 4, 2, 3, 4, 1, 1,
    1, 6, 2, 2, 4, 3, 2, 3, 3, 1, 4, 1, 1, 2, 3, 3, 6, 1, 0, 1,
    5, 3, 2, 2, 2, 4, 0, 1, 1, 2, 5, 2, 5, 2, 2, 3, 1, 3, 0, 1,
    2, 0, 1, 3, 2, 3, 0, 2, 5, 3, 5, 4, 0, 1, 6, 4, 3, 3, 3, 2,
    1, 4, 3, 0, 1, 4, 3, 1, 1, 2, 2, 2, 3, 3, 2, 8, 3, 0, 1, 3,
    1, 5, 2, 3, 2, 1, 6, 0, 1, 2, 6, 2, 1, 1, 2, 4, 2, 1, 1, 2,
    1, 4, 2, 4, 2, 1, 2, 3, 1, 4, 2, 1, 1, 0, 1, 2, 3, 5, 1, 3,
    5, 3, 0, 1, 1, 1, 2, 3, 3, 0, 1, 2, 1, 2, 4, 0, 1, 1, 1, 3,
    1, 2, 0, 1, 4, 0, 1, 2, 2, 3, 3, 2, 1, 1, 1, 1, 0, 1, 2, 1,
    2, 3, 0, 1, 1, 1, 3, 5, 4, 2, 0, 1, 3, 5, 1, 1, 3, 6, 2, 1,
    2, 1, 2, 3, 4, 1, 2, 2, 1, 3, 4, 3, 1, 3, 3, 2, 2, 1, 2, 0,
    1, 4, 2, 1, 1, 2, 0, 1, 2, 2, 0, 1, 1, 2, 3, 1, 1, 4, 3, 0,
    1, 3, 1, 2, 0, 1, 1, 2, 2, 1, 1, 3, 4, 4, 2, 2, 0, 1, 1, 1,
    1, 4, 0, 1, 3, 1, 0, 1, 2, 2, 1, 5, 2, 4, 3, 2, 3, 9, 5, 1,
    2, 6, 1, 1, 4, 0, 1, 1, 4, 3, 0, 1, 5, 0, 1, 2, 4, 2, 3, 2,
    6, 0, 1, 1, 2, 0, 1, 2, 2, 1, 0, 1, 1, 2, 0, 1, 4, 1, 3, 1,
    2, 5, 5, 4, 1, 2, 2, 0, 1, 2, 1, 0, 1, 1, 1, 6, 0, 1, 1, 1,
    0, 1, 2, 4, 2, 1, 0, 1, 1, 1, 2, 6, 1, 3, 0, 1, 1, 0, 1, 2,
    1, 2, 2, 2, 3, 4, 1, 1, 3, 0, 1, 1, 2, 2, 0, 1, 2, 5, 1, 3,
    2, 0, 1, 4, 2, 2, 0, 1, 1, 3, 2, 1, 2, 3, 1, 0, 2, 3, 3, 1,
    1, 2, 4, 2, 3, 6, 5, 4, 5, 5, 1, 4, 3, 2, 2, 2, 3, 1, 3, 2,
    3, 3, 1, 5, 1, 3, 1, 1, 4, 1, 3, 4, 6, 1, 1, 1, 3, 1, 3, 0,
    2, 5, 3, 0, 1, 2, 2, 3, 1, 3, 0, 1, 2, 1, 3, 3, 3, 1, 1, 3,
    2, 3, 2, 3, 1, 3, 2, 2, 1, 0, 1, 4, 1, 1, 3, 0, 1, 3, 3, 4,
    0, 1, 2, 2, 5, 4, 3, 1, 5, 1, 2, 2, 3, 1, 1, 0, 1, 1, 0, 1,
    2, 3, 2, 1, 1, 3, 1, 3, 3, 2, 3, 2, 1, 4, 0, 1, 3, 2, 1, 1,
    1, 2, 4, 2, 0, 1, 3, 1, 0, 1, 2, 0, 1, 1, 0, 1, 1, 1, 0, 1,
    1, 0, 1, 1, 3, 0, 1, 1, 3, 1, 1, 1, 2, 1, 2, 1, 2, 0, 1, 3,
    0, 1, 1, 0, 1, 3, 1, 0, 1, 2, 1, 2, 3, 1, 0, 1, 6, 0, 1, 3,
    2, 2, 2, 5, 1, 1, 2, 2, 0, 1, 2, 2, 3, 0, 1, 2, 1, 3, 2, 2,
    3, 3, 2, 2, 1, 1, 1, 3, 2, 2, 2, 2, 2, 5, 1, 2, 4, 1, 0, 2,
    1, 5, 1, 2, 5, 1, 2, 4, 0, 1, 3, 1, 1, 0, 2, 1, 1, 1, 1, 0,
    2, 1, 2, 2, 4, 0, 1, 2, 2, 0, 2, 2, 1, 0, 2, 4, 2, 1, 3, 2,
    2, 3, 2, 1, 1, 2, 2, 0, 1, 2, 2, 1, 2, 1, 0, 1, 1, 2, 0, 1,
    4, 4, 2, 2, 5, 1, 3, 2, 2, 1, 0, 1, 1, 2, 3, 2, 2, 1, 3, 2,
    0, 1, 2, 1, 3, 0, 1, 3, 2, 1, 0, 2, 1, 2, 1, 0, 2, 1, 2, 0,
    1, 2, 4, 1, 0, 1, 1, 1, 2, 1, 2, 2, 3, 2, 2, 0, 1, 1, 2, 1,
    0, 1, 4, 2, 1, 1, 2, 5, 2, 0, 1, 2, 2, 0, 1, 1, 1, 2, 1, 0,
    1, 2, 0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 3, 2, 4, 1, 0, 1, 2, 0,
    4, 2, 1, 4, 1, 1, 3, 0, 1, 3, 3, 2, 2, 3, 2, 1, 0, 1, 1, 3,
    2, 1, 0, 2, 2, 1, 2, 2, 1, 0, 1, 2, 1, 2, 3, 0, 1, 3, 3, 0,
    1, 1, 6, 3, 3, 2, 2, 1, 0, 2, 1, 0, 2, 1, 3, 2, 1, 2, 6, 5,
    9, 10, 6, 8, 3, 4, 3, 3, 0, 1, 3, 1, 4, 0, 1, 2, 1, 1, 2, 1,
    3, 2, 0, 3, 1, 0, 1, 1, 2, 4, 2, 0, 1, 3, 0, 2, 3, 1, 2, 2,
    3, 1, 0, 1, 1, 4, 1, 2, 2, 2, 1, 1, 0, 1, 1, 1, 0, 2, 4, 3,
    2, 4, 1, 1, 4, 4, 2, 3, 3, 4, 1, 1, 3, 2, 1, 1, 0, 2, 3, 2,
    1, 0, 2, 2, 1, 1, 0, 2, 1, 1, 2, 1, 3, 0, 2, 2, 3, 3, 2, 3,
    4, 2, 2, 2, 1, 3, 1, 1, 0, 1, 3, 1, 4, 1, 3, 0, 1, 2, 3, 1,
    1, 0, 1, 1, 1, 0, 2, 1, 0, 2, 1, 2, 1, 2, 3, 1, 0, 1, 1, 2,
    4, 0, 1, 2, 0, 1, 2, 1, 1, 0, 1, 2, 2, 1, 1, 2, 1, 6, 4, 4,
    1, 6, 0, 1, 3, 1, 2, 0, 1, 1, 3, 3, 8, 6, 4, 6, 5, 1, 1, 3,
    0, 1, 1, 1, 1, 2, 0, 1, 1, 1, 0, 1, 3, 0, 1, 1, 3, 0, 1, 1,
    0, 1, 1, 0, 1, 1, 2, 0, 4, 1, 2, 1, 1, 3, 1, 0, 1, 2, 3, 1,
    1, 0, 1, 1, 1, 0, 2, 2, 0, 1, 4, 2, 0, 1, 2, 2, 1, 2, 2, 1,
    1, 0, 1, 1, 2, 0, 3, 2, 0, 1, 1, 1, 0, 2, 1, 2, 2, 2, 2, 1,
    1, 1, 1, 2, 0, 1, 4, 2, 0, 1, 5, 3, 1, 2, 1, 1, 2, 3, 2, 0,
    1, 1, 1, 1, 1, 2, 1, 0, 4, 1, 0, 1, 2, 0, 2, 3, 3, 0, 2, 1,
    2, 1, 1, 0, 1, 3, 0, 1, 1, 2, 2, 1, 0, 1, 1, 0, 1, 1, 4, 1,
    0, 1, 1, 0, 2, 3, 1, 0, 1, 2, 1, 0, 1, 4, 1, 2, 0, 1, 2, 1,
    0, 1, 2, 0, 1, 3, 2, 2, 2, 2, 3, 0, 4, 2, 3, 2, 1, 0, 1, 1,
    0, 1, 2, 0, 1, 2, 2, 2, 1, 1, 2, 2, 2, 1, 3, 1, 1, 3, 0, 1,
    2, 1, 4, 1, 3, 2, 3, 2, 4, 1, 1, 1, 0, 1, 1, 0, 2, 3, 0, 1,
    2, 1, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 4, 3, 3, 1, 2, 0, 1, 1,
    0, 2, 1, 0, 1, 1, 2, 1, 0, 1, 2, 0, 2, 3, 2, 2, 0, 1, 2, 0,
    2, 3, 0, 2, 2, 1, 2, 2, 0, 1, 1, 0, 1, 1, 0, 1, 3, 2, 0, 1,
    3, 1, 1, 0, 1, 1, 3, 0, 1, 1, 1, 2, 3, 1, 2, 2, 0, 1, 1, 0,
    3, 1, 0, 1, 1, 0, 1, 2, 1, 2, 1, 1, 1, 2, 2, 0, 1, 2, 4, 1,
    2, 1, 1, 2, 1, 1, 2, 2, 1, 1, 1, 2, 2, 0, 1, 1, 1, 0, 1, 2,
    0, 1, 2, 1, 1, 1, 0, 6, 1, 0, 1, 1, 2, 2, 0, 2, 4, 2, 0, 1,
    5, 4, 0, 2, 2, 2, 1, 1, 1, 2, 0, 2, 1, 2, 1, 0, 2, 1, 2, 1,
    1, 2, 3, 3, 0, 3, 1, 1, 4, 1, 2, 1, 1, 1, 2, 1, 0, 1, 1, 1,
    3, 1, 0, 1, 1, 1, 2, 1, 4, 1, 1, 1, 0, 1, 1, 0, 1, 2, 2, 0,
    1, 1, 0, 1, 2, 0, 1, 1, 0, 1, 4, 1, 2, 0, 3, 1, 1, 0, 1, 1,
    1, 0, 1, 1, 0, 1, 1, 0, 2, 3, 0, 1, 2, 3, 1, 3, 4, 5, 7, 4,
    4, 1, 0, 1, 3, 3, 2, 0, 1, 1, 1, 2, 0, 3, 1, 0, 1, 2, 1, 0,
    1, 1, 2, 2, 0, 1, 1, 1, 3, 1, 2, 1, 3, 0, 1, 3, 3, 2, 1, 0,
    2, 2, 1, 1, 1, 2, 2, 1, 4, 2, 0, 2, 2, 3, 2, 0, 2, 3, 2, 1,
    1, 1, 1, 2, 1, 0, 1, 1, 1, 0, 2, 1, 0, 1, 2, 1, 0, 1, 1, 1,
    1, 0, 1, 3, 3, 0, 1, 2, 1, 2, 1, 2, 0, 1, 1, 0, 1, 4, 3, 2,
    0, 1, 1, 0, 1, 2, 0, 1, 2, 1, 1, 2, 1, 0, 4, 1, 0, 1, 2, 2,
    1, 2, 2, 1, 0, 2, 2, 1, 2, 3, 2, 2, 1, 2, 2, 0, 1, 2, 2, 1,
    1, 1, 2, 1, 2, 2, 1, 1, 1, 3, 1, 0, 2, 2, 2, 1, 1, 2, 2, 1,
    0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 2, 2, 1, 0, 1,
    2, 1, 2, 0, 1, 2, 0, 1, 2, 0, 4, 1, 0, 3, 1, 2, 0, 1, 1, 3,
    1, 2, 2, 0, 2, 3, 1, 1, 2, 0, 2, 1, 2, 2, 0, 3, 2, 2, 0, 1,
    2, 1, 0, 2, 4, 2, 2, 1, 0, 3, 1, 2, 1, 1, 1, 3, 3, 1, 1, 0,
    2, 1, 3, 0, 2, 3, 1, 1, 3, 1, 2, 1, 3, 1, 1, 2, 3, 1, 1, 0,
    2, 1, 1, 2, 0, 2, 2, 1, 0, 2, 1, 2, 0, 1, 2, 1, 3, 2, 2, 3,
    1, 2, 1, 3, 1, 3, 2, 4, 2, 1, 1, 1, 0, 3, 2, 0, 1, 1, 2, 2,
    0, 1, 1, 0, 1, 2, 1, 2, 3, 3, 1, 2, 1, 1, 1, 2, 1, 1, 1, 0,
    6, 4, 1, 2, 2, 2, 0, 1, 1, 1, 0, 1, 2, 1, 0, 1, 2, 1, 2, 3,
    2, 3, 1, 0, 1, 1, 3, 1, 2, 1, 3, 1, 0, 3, 1, 0, 1, 2, 0, 1,
    1, 2, 2, 1, 3, 4, 0, 2, 1, 1, 1, 2, 2, 1, 0, 1, 1, 0, 1, 2,
    1, 0, 1, 2, 0, 6, 1, 1, 1, 2, 2, 0, 1, 1, 0, 1, 1, 1, 0, 2,
    1, 0, 2, 2, 0, 1, 1, 0, 2, 1, 0, 1, 2, 1, 0, 2, 1, 1, 2, 0,
    2, 1, 1, 2, 2, 0, 2, 2, 2, 0, 1, 1, 0, 3, 1, 1, 0, 3, 1, 0,
    1, 2, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 2, 2,
    1, 0, 1, 1, 2, 4, 1, 0, 2, 1, 3, 1, 1, 3, 0, 1, 1, 1, 1, 0,
    1, 3, 1, 2, 0, 2, 2, 0, 1, 1, 1, 1, 1, 2, 1, 3, 1, 0, 1, 2,
    0, 1, 3, 1, 1, 1, 3, 1, 1, 2, 3, 0, 2, 1, 0, 1, 1, 1, 0, 2,
    1, 1, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 0, 3, 2, 0, 3, 1, 0, 1,
    1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 5, 1, 0, 1, 2,
    0, 2, 1, 0, 3, 1, 1, 0, 2, 1, 0, 1, 1, 1, 3, 1, 1, 1, 5, 1,
    0, 3, 2, 1, 1, 0, 1, 2, 1, 0, 2, 1, 1, 0, 3, 2, 0, 1, 1, 2,
    1, 2, 1, 1, 2, 0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 1, 2, 0, 2, 1,
    3, 0, 1, 1, 1, 0, 2, 1, 0, 1, 1, 0, 2, 2, 1, 0, 1, 2, 0, 4,
    1, 1, 0, 2, 2, 0, 5, 1, 0, 2, 1, 0, 2, 1, 2, 2, 1, 2, 0, 1,
    2, 1, 1, 0, 2, 1, 1, 0, 1, 3, 0, 2, 1, 0, 3, 2, 0, 1, 2, 3,
    2, 2, 1, 0, 2, 1, 0, 2, 2, 0, 2, 3, 0, 4, 1, 0, 2, 1, 0, 2,
    1, 3, 0, 1, 1, 0, 2, 1, 1, 1, 0, 2, 2, 2, 0, 2, 1, 2, 1, 0,
    2, 1, 1, 1, 0, 6, 1, 0, 2, 1, 0, 4, 3, 1, 2, 0, 1, 3, 0, 6,
    1, 0, 2, 2, 1, 2, 0, 2, 2, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1,
    0, 4, 1, 1, 0, 5, 1, 0, 2, 1, 0, 1, 2, 0, 4, 1, 0, 2, 1, 1,
    0, 1, 1, 1, 0, 2, 1, 2, 0, 3, 1, 2, 4, 2, 0, 4, 1, 2, 0, 3,
    1, 1, 1, 1, 1, 1, 0, 1, 2, 1, 0, 1, 1, 0, 2, 1, 0, 7, 2, 1,
    0, 1, 1, 0, 3, 1, 1, 0, 3, 2, 0, 3, 1, 1, 1, 0, 4, 3, 0, 1,
    2, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 3, 0, 2, 1, 1, 0, 1,
    3, 1, 1, 0, 9, 1, 1, 0, 2, 2, 1, 0, 3, 1, 0, 1, 1, 2, 0, 2,
    1, 2, 1, 0, 2, 1, 0, 3, 1, 0, 2, 1, 1, 0, 6, 1, 0, 3, 2, 1,
    0, 8, 2, 0, 1, 1, 0, 5, 1, 1, 0, 3, 1, 0, 1, 1, 0, 3, 2, 4,
    17, 23, 48, 46, 60, 54, 36, 32, 17, 3, 1, 0, 2, 1, 0, 2, 1, 0, 13, 1,
    0, 13, 1, 0, 7, 1, 0, 13, 1, 0, 2, 1, 0, 6, 1, 0, 4, 1, 0, 7,
    1, 0, 2, 1, 0, 2, 2, 0, 3, 1, 0, 6, 1, 0, 3, 2, 1, 0, 2, 1,
    0, 2, 1, 0, 1, 1, 0, 6, 1, 1, 1, 2, 0, 4, 1, 3, 0, 1, 2, 1,
    1, 0, 6, 1, 0, 1, 1, 0, 10, 1, 0, 1, 1, 0, 5, 1, 0, 6, 2, 0,
    1, 1, 0, 14, 1, 0, 10, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 4, 2,
    0, 7, 1, 0, 5, 1, 0, 16, 1, 0, 3, 1, 1, 0, 2, 1, 0, 2, 1, 1,
    0, 5, 1, 1, 0, 5, 2, 0, 27, 1, 0, 5, 1, 0, 2, 1, 0, 2, 1, 2,
    1, 0, 3, 1, 0, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1,
    0, 4, 1, 0, 1, 1, 2, 1, 1, 0, 5, 1, 1, 1, 1, 1, 0, 2, 2, 1,
    0, 7, 1, 1, 0, 3, 3, 0, 6, 1, 0, 13, 1, 1, 0, 1, 1, 0, 1, 1,
    0, 6, 1, 0, 9, 1, 0, 11, 1, 1, 0, 14, 1, 1, 1, 1, 1, 1, 0, 5,
    1, 1, 0, 13, 1, 0, 2, 1, 0, 17, 1, 0, 3, 1, 1, 0, 5, 1, 1, 0,
    2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 17, 1, 0, 19, 1, 0, 4, 2,
    0, 11, 1, 0, 9, 1, 0, 3, 1, 0, 3, 1, 0, 4, 1, 1, 0, 7, 1, 0,
    12, 1, 0, 2, 1, 0, 11, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 0, 3, 1,
    0, 6, 1, 0, 13, 1, 0, 3, 1, 0, 8, 1, 0, 19, 1, 0, 5, 1, 0, 4,
    1, 0, 4, 1, 0, 11, 1, 0, 1, 1, 0, 1, 1, 0, 9, 1, 0, 2, 2, 3,
    1, 1, 1, 1, 0, 1, 1, 0, 16, 1, 0, 4, 1, 0, 7, 1, 0, 2, 1, 0,
    2, 1, 0, 1, 1, 0, 4, 1, 0, 8, 1, 0, 1, 1, 0, 2, 1, 0, 12, 1,
    1, 0, 2, 1, 0, 3, 1, 0, 6, 3, 2, 1, 5, 3, 3, 7, 3, 1, 0, 9,
    1, 0, 7, 1, 0, 3, 1, 0, 12, 1, 1, 0, 1, 2, 0, 7, 1, 0, 11, 1,
    0, 4, 1, 0, 4, 1, 0, 10, 3, 0, 4, 2, 0, 15, 1, 0, 4, 1, 1, 1,
    1, 0, 3, 1, 0, 9, 1, 0, 21, 1, 0, 2, 1, 0, 1, 1, 0, 8, 1, 1,
    0, 12, 1, 1, 0, 19, 1, 0, 7, 1, 0, 10, 1, 0, 3, 1, 0, 3, 1, 0,
    1, 1, 0, 9, 1, 0, 2, 1, 0, 1, 1, 1, 0, 5, 1, 1, 1, 0, 48, 2,
    2, 0, 5, 1, 0, 3, 1, 0, 2, 1, 0, 7, 1, 0, 3, 1, 0, 2, 1, 0,
    4, 1, 0, 5, 1, 0, 3, 1, 0, 5, 1, 0, 4, 1, 0, 12, 1, 0, 3, 1,
    0, 2, 2, 0, 1, 1, 0, 2, 1, 0, 6, 1, 0, 3, 1, 0, 2, 1, 0, 12,
    1, 0, 2, 1, 0, 1, 2, 0, 2, 1, 0, 9, 1, 0, 11, 1, 1, 0, 3, 1,
    0, 9, 1, 0, 4, 1, 1, 1, 0, 9, 2, 0, 4, 1, 0, 19, 1, 1, 0, 6,
    1, 0, 4, 1, 0, 2, 1, 0, 2, 1, 1, 0, 1, 1, 0, 6, 1, 0, 28, 1,
    0, 1, 1, 0, 3, 1, 0, 24, 1, 0, 8, 1, 0, 1, 1, 0, 5, 1, 0, 7,
    1, 0, 34, 1, 0, 6, 1, 0, 5, 1, 0, 11, 1, 0, 1, 2, 1, 0, 5, 1,
    0, 1, 1, 0, 7, 2, 0, 4, 1, 0, 26, 1, 0, 6, 1, 0, 4, 1, 0, 29,
    1, 0, 3, 1, 0, 7, 1, 0, 1, 1, 0, 2, 1, 0, 6, 1, 0, 2, 1, 0,
    7, 1, 0, 2, 1, 0, 32, 1, 0, 8, 1, 0, 12, 1, 0, 5, 1, 0, 4, 1,
    0, 1, 1, 0, 11, 1, 0, 8, 1, 0, 3, 1, 0, 15, 1, 0, 3, 1, 0, 1,
    1, 0, 4, 1, 1, 0, 4, 3, 2, 1, 1, 2, 0, 1, 1, 0, 2, 1, 1, 0,
    29, 1, 0, 2, 2, 1, 0, 1, 2, 0, 9, 1, 0, 2, 1, 0, 11, 1, 1, 1,
    0, 30, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 6, 1, 0, 2,
    1, 0, 25, 1, 0, 32, 1, 0, 1, 1, 0, 2, 1, 1, 1, 1, 0, 7, 1, 0,
    13, 1, 0, 1, 1, 0, 4, 2, 0, 12, 1, 0, 13, 1, 1, 0, 15, 1, 0, 10,
    3, 2, 0, 1, 1, 1, 1, 1, 2, 2, 0, 22, 1, 0, 1, 1, 0, 2, 1, 0,
    2, 1, 0, 34, 1, 0, 15, 1, 0, 12, 1, 0, 9, 1, 0, 28, 2, 0, 11, 1,
    0, 1, 2, 0, 8, 1, 0, 17, 1, 0, 14, 1, 0, 7, 1, 0, 3, 1, 0, 1,
    1, 0, 6, 1, 0, 10, 1, 0, 10, 2, 0, 3, 2, 0, 9, 1, 0, 2, 1, 0,
    12, 1, 0, 5, 2, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 11, 1, 0, 3, 1,
    0, 11, 1, 1, 0, 2, 1, 0, 18, 1, 0, 8, 1, 1, 0, 1, 1, 0, 5, 1,
    0, 1, 1, 1, 0, 4, 1, 0, 8, 1, 0, 7, 1, 0, 7, 1, 0, 4, 1, 1,
    0, 2, 1, 0, 4, 1, 0, 6, 1, 0, 2, 1, 0, 3, 1, 0, 4, 1, 1, 0,
    7, 2, 0, 4, 1, 2, 0, 1, 1, 0, 3, 1, 0, 7, 3, 0, 12, 1, 1, 0,
    4, 2, 0, 22, 1, 0, 5, 1, 0, 6, 2, 0, 7, 1, 0, 13, 1, 0, 5, 1,
    2, 0, 5, 1, 2, 0, 3, 1, 0, 8, 1, 0, 9, 1, 0, 10, 1, 0, 1, 1,
    0, 18, 1, 0, 13, 1, 0, 1, 1, 0, 4, 1, 0, 10, 2, 0, 13, 1, 0, 3,
    1, 0, 1, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 1, 0, 4, 2, 1, 0, 1,
    1, 0, 1, 1, 0, 27, 1, 0, 21, 1, 1, 0, 4, 1, 0, 2, 1, 0, 18, 1,
    0, 18, 1, 0, 1, 1, 0, 20, 2, 0, 18, 1, 0, 2, 1, 0, 2, 1, 1, 0,
    19, 1, 0, 35, 1, 0, 25, 1, 0, 3, 1, 0, 36, 1, 0, 24, 1, 0, 10, 1,
    0, 20, 1, 1, 0, 52, 1, 0, 35, 1, 0, 25, 2, 1, 2, 2, 3, 6, 4, 6,
    5, 8, 5, 4, 1, 1, 2, 0, 6, 1, 0, 76, 1, 0, 56, 1, 0, 38, 1, 0,
    76, 1, 0, 55, 1, 0, 43, 1, 0, 86, 1, 0, 73, 1, 0, 52, 1, 0, 98, 1,
    0, 131, 1, 0, 245  };
  assert( test_6_chan_cnts.size() == 5365 );
  const vector<uint8_t> test_6_packed{
    245, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 38, 4, 17, 35, 31,
    40, 40, 41, 37, 34, 48, 40, 46, 41, 38, 26, 25, 31, 40, 26, 29, 35, 35, 37, 33, 28, 46, 28, 36, 27, 28, 37, 30, 21, 31, 26, 33, 34, 36, 30, 28, 25, 27, 22, 23, 34, 33, 33, 32, 27, 36, 37, 39, 28, 20,
    31, 21, 27, 21, 35, 22, 21, 25, 25, 28, 33, 16, 26, 30, 26, 25, 34, 23, 27, 22, 30, 24, 27, 26, 27, 19, 25, 23, 22, 28, 15, 26, 26, 29, 24, 27, 20, 19, 22, 24, 22, 14, 13, 30, 19, 26, 21, 21, 35, 30,
    27, 29, 31, 21, 36, 31, 30, 31, 31, 34, 25, 41, 28, 31, 33, 35, 23, 30, 40, 29, 35, 35, 26, 47, 35, 40, 41, 46, 32, 38, 41, 43, 37, 39, 35, 33, 36, 35, 41, 47, 49, 50, 47, 51, 70, 59, 60, 42, 45, 53,
    41, 41, 54, 55, 57, 47, 41, 49, 52, 40, 39, 49, 57, 51, 52, 52, 52, 66, 67, 66, 76, 54, 49, 62, 55, 56, 65, 57, 51, 47, 53, 56, 47, 46, 42, 58, 53, 69, 45, 59, 61, 47, 47, 62, 49, 54, 59, 54, 50, 61,
    53, 50, 62, 65, 56, 55, 59, 66, 67, 57, 53, 56, 66, 54, 67, 60, 66, 52, 67, 63, 63, 58, 57, 62, 60, 66, 53, 56, 42, 55, 65, 58, 50, 62, 68, 63, 57, 66, 66, 45, 56, 73, 41, 52, 56, 62, 64, 69, 66, 57,
    64, 59, 64, 50, 62, 70, 68, 46, 61, 62, 71, 59, 68, 61, 64, 73, 53, 56, 76, 53, 63, 49, 56, 59, 62, 61, 71, 67, 57, 63, 61, 59, 58, 62, 64, 59, 70, 56, 62, 48, 52, 50, 65, 68, 43, 55, 70, 62, 66, 68,
    53, 71, 63, 54, 60, 43, 63, 55, 67, 71, 57, 61, 62, 62, 62, 51, 49, 62, 57, 58, 50, 50, 58, 55, 56, 61, 46, 48, 46, 54, 71, 59, 59, 59, 43, 48, 52, 55, 63, 47, 55, 44, 60, 50, 58, 47, 63, 58, 49, 65,
    64, 52, 53, 45, 51, 48, 48, 49, 36, 75, 41, 58, 36, 43, 45, 54, 51, 43, 52, 40, 48, 54, 46, 52, 64, 44, 40, 57, 52, 39, 54, 57, 47, 44, 64, 49, 43, 38, 56, 55, 48, 54, 47, 53, 53, 55, 47, 49, 27, 36,
    52, 41, 60, 60, 44, 65, 65, 42, 43, 44, 46, 56, 47, 36, 50, 60, 44, 45, 40, 54, 49, 61, 48, 43, 48, 33, 31, 43, 55, 40, 46, 45, 49, 43, 56, 53, 45, 55, 41, 45, 42, 54, 39, 47, 52, 45, 41, 40, 35, 55,
    43, 36, 46, 54, 42, 53, 52, 30, 55, 50, 50, 60, 103, 164, 249, 0, 1, 152, 87, 44, 33, 25, 31, 27, 30, 18, 39, 25, 36, 27, 20, 31, 31, 30, 23, 28, 26, 31, 20, 23, 31, 23, 18, 29, 25, 32, 20, 25, 27, 21,
    26, 38, 29, 26, 37, 22, 14, 33, 25, 33, 29, 35, 33, 25, 28, 14, 27, 33, 58, 66, 40, 37, 28, 22, 28, 33, 21, 29, 18, 25, 22, 31, 18, 22, 21, 20, 21, 17, 25, 27, 17, 22, 26, 18, 28, 16, 28, 11, 25, 13,
    27, 21, 18, 17, 25, 21, 23, 26, 14, 22, 13, 24, 17, 23, 21, 17, 27, 12, 22, 17, 20, 18, 15, 23, 17, 14, 14, 22, 19, 21, 18, 16, 17, 24, 20, 16, 24, 22, 23, 22, 14, 22, 19, 19, 19, 20, 21, 27, 23, 19,
    18, 26, 23, 11, 26, 9, 22, 28, 30, 28, 38, 34, 29, 16, 15, 20, 24, 14, 24, 23, 15, 12, 16, 19, 13, 18, 14, 12, 16, 18, 18, 21, 9, 15, 10, 11, 15, 11, 8, 9, 14, 14, 9, 14, 7, 16, 19, 15, 10, 12,
    14, 9, 19, 11, 12, 21, 15, 10, 15, 17, 13, 18, 14, 14, 9, 15, 19, 16, 18, 11, 19, 9, 14, 12, 14, 10, 10, 12, 10, 14, 14, 13, 15, 18, 14, 10, 14, 20, 19, 14, 11, 12, 11, 16, 14, 13, 11, 14, 18, 19,
    12, 8, 13, 13, 18, 11, 11, 14, 15, 16, 6, 9, 12, 18, 11, 13, 11, 11, 18, 15, 12, 10, 10, 11, 16, 11, 14, 12, 12, 15, 7, 8, 16, 8, 13, 13, 8, 14, 10, 12, 12, 11, 8, 9, 16, 13, 13, 12, 10, 6,
    18, 9, 17, 11, 10, 12, 10, 12, 12, 11, 6, 14, 16, 23, 31, 19, 15, 15, 10, 15, 11, 5, 12, 6, 9, 10, 10, 14, 6, 10, 8, 8, 13, 10, 10, 11, 13, 10, 14, 13, 11, 10, 10, 11, 9, 8, 12, 10, 10, 7,
    14, 5, 11, 8, 13, 8, 9, 10, 7, 7, 5, 10, 10, 7, 11, 12, 11, 9, 14, 7, 11, 10, 6, 7, 13, 4, 14, 12, 8, 9, 13, 6, 5, 11, 9, 11, 11, 12, 11, 6, 9, 7, 7, 9, 8, 9, 11, 9, 14, 13,
    15, 11, 6, 12, 14, 9, 15, 8, 5, 6, 9, 6, 7, 7, 18, 9, 8, 9, 4, 10, 9, 5, 5, 4, 8, 8, 5, 13, 4, 12, 8, 6, 14, 17, 16, 10, 11, 11, 7, 11, 3, 11, 6, 11, 4, 8, 6, 8, 6, 3,
    5, 3, 8, 6, 8, 5, 9, 6, 6, 10, 8, 10, 8, 4, 7, 11, 6, 12, 27, 18, 20, 16, 15, 11, 7, 5, 14, 9, 6, 4, 4, 4, 4, 9, 12, 9, 7, 5, 7, 8, 4, 6, 13, 11, 5, 9, 4, 8, 5, 7,
    11, 7, 3, 5, 7, 8, 8, 4, 8, 1, 10, 5, 5, 6, 7, 6, 5, 2, 8, 5, 11, 5, 8, 8, 6, 6, 13, 5, 7, 4, 8, 6, 8, 7, 6, 4, 3, 7, 7, 3, 7, 8, 9, 2, 9, 6, 6, 5, 4, 7,
    2, 4, 6, 10, 6, 9, 3, 5, 5, 10, 6, 5, 5, 7, 9, 9, 5, 6, 5, 10, 5, 7, 4, 5, 5, 9, 5, 2, 4, 5, 5, 5, 6, 4, 4, 8, 8, 5, 6, 4, 4, 4, 6, 2, 4, 6, 3, 3, 5, 3,
    10, 11, 10, 9, 5, 4, 5, 7, 4, 7, 9, 4, 11, 4, 7, 4, 7, 6, 4, 6, 5, 4, 8, 6, 8, 8, 8, 5, 7, 7, 9, 4, 8, 4, 6, 5, 1, 8, 9, 9, 5, 4, 9, 3, 0, 1, 4, 5, 6, 7,
    5, 2, 6, 2, 5, 5, 7, 2, 5, 5, 9, 8, 10, 4, 5, 6, 1, 7, 3, 3, 9, 6, 3, 5, 9, 4, 11, 5, 2, 4, 3, 6, 5, 3, 7, 5, 4, 4, 6, 3, 3, 3, 3, 6, 4, 4, 1, 3, 5, 3,
    3, 4, 2, 5, 2, 2, 1, 1, 4, 7, 5, 12, 6, 3, 3, 2, 4, 3, 2, 3, 4, 6, 7, 5, 3, 5, 5, 3, 6, 4, 4, 3, 6, 1, 6, 3, 5, 3, 9, 3, 5, 4, 4, 3, 6, 4, 6, 1, 8, 4,
    5, 5, 3, 4, 3, 3, 6, 7, 3, 6, 5, 2, 2, 2, 10, 2, 2, 4, 6, 4, 1, 3, 7, 4, 4, 2, 6, 7, 4, 1, 5, 1, 8, 4, 1, 5, 4, 4, 2, 3, 2, 5, 6, 4, 5, 1, 2, 4, 2, 4,
    1, 6, 2, 2, 1, 3, 6, 8, 5, 1, 6, 5, 5, 5, 3, 2, 4, 3, 4, 3, 10, 4, 6, 6, 3, 2, 5, 6, 1, 3, 4, 3, 3, 6, 6, 5, 2, 5, 3, 2, 7, 4, 2, 1, 1, 2, 4, 1, 3, 3,
    5, 5, 2, 3, 2, 4, 1, 3, 1, 1, 3, 2, 2, 5, 3, 1, 3, 1, 6, 1, 3, 3, 7, 1, 2, 3, 3, 5, 5, 0, 1, 3, 6, 2, 6, 3, 3, 7, 1, 1, 3, 3, 4, 4, 5, 7, 4, 5, 2, 5,
    2, 4, 9, 11, 10, 13, 11, 11, 9, 10, 9, 7, 2, 6, 2, 4, 1, 2, 3, 3, 1, 5, 5, 3, 5, 4, 6, 5, 8, 7, 3, 2, 3, 6, 6, 2, 3, 4, 4, 7, 5, 4, 4, 5, 5, 2, 1, 2, 6, 3,
    3, 7, 6, 3, 2, 3, 1, 2, 3, 5, 4, 5, 2, 1, 3, 4, 2, 5, 2, 3, 6, 2, 2, 2, 4, 5, 3, 0, 1, 3, 1, 5, 4, 6, 3, 2, 0, 1, 6, 2, 2, 3, 4, 3, 2, 4, 4, 7, 6, 5,
    10, 5, 3, 3, 2, 1, 3, 5, 3, 2, 3, 6, 0, 1, 3, 1, 2, 2, 4, 1, 3, 3, 1, 1, 3, 4, 2, 4, 4, 2, 4, 4, 4, 2, 1, 3, 0, 1, 5, 2, 3, 2, 3, 0, 1, 7, 2, 3, 2, 3,
    2, 1, 2, 2, 5, 4, 1, 5, 1, 5, 3, 4, 3, 2, 1, 4, 1, 4, 2, 3, 2, 2, 2, 4, 3, 1, 1, 1, 2, 0, 1, 5, 2, 4, 2, 1, 5, 2, 3, 7, 4, 4, 3, 2, 2, 1, 2, 3, 2, 1,
    10, 5, 3, 4, 1, 3, 4, 8, 10, 17, 16, 11, 6, 6, 2, 2, 3, 4, 2, 2, 2, 0, 1, 3, 2, 3, 4, 2, 1, 1, 0, 1, 7, 3, 4, 3, 2, 4, 2, 4, 0, 1, 2, 2, 2, 1, 3, 3, 1, 2,
    5, 2, 1, 0, 1, 2, 3, 4, 1, 4, 4, 1, 2, 6, 1, 0, 1, 1, 3, 0, 1, 5, 6, 3, 8, 0, 1, 3, 6, 0, 1, 2, 3, 2, 2, 4, 13, 18, 23, 35, 19, 15, 9, 5, 1, 2, 2, 3, 1, 0,
    1, 1, 2, 1, 2, 1, 0, 1, 4, 0, 2, 2, 2, 2, 1, 1, 3, 0, 1, 2, 5, 3, 1, 1, 3, 5, 2, 5, 1, 3, 2, 1, 4, 3, 3, 3, 3, 2, 2, 5, 2, 1, 2, 1, 2, 4, 2, 0, 1, 3,
    2, 3, 5, 1, 3, 2, 2, 1, 0, 1, 4, 1, 3, 1, 1, 6, 1, 4, 0, 1, 5, 4, 3, 2, 1, 2, 2, 2, 2, 4, 2, 3, 4, 1, 1, 1, 6, 2, 2, 4, 3, 2, 3, 3, 1, 4, 1, 1, 2, 3,
    3, 6, 1, 0, 1, 5, 3, 2, 2, 2, 4, 0, 1, 1, 2, 5, 2, 5, 2, 2, 3, 1, 3, 0, 1, 2, 0, 1, 3, 2, 3, 0, 2, 5, 3, 5, 4, 0, 1, 6, 4, 3, 3, 3, 2, 1, 4, 3, 0, 1,
    4, 3, 1, 1, 2, 2, 2, 3, 3, 2, 8, 3, 0, 1, 3, 1, 5, 2, 3, 2, 1, 6, 0, 1, 2, 6, 2, 1, 1, 2, 4, 2, 1, 1, 2, 1, 4, 2, 4, 2, 1, 2, 3, 1, 4, 2, 1, 1, 0, 1,
    2, 3, 5, 1, 3, 5, 3, 0, 1, 1, 1, 2, 3, 3, 0, 1, 2, 1, 2, 4, 0, 1, 1, 1, 3, 1, 2, 0, 1, 4, 0, 1, 2, 2, 3, 3, 2, 1, 1, 1, 1, 0, 1, 2, 1, 2, 3, 0, 1, 1,
    1, 3, 5, 4, 2, 0, 1, 3, 5, 1, 1, 3, 6, 2, 1, 2, 1, 2, 3, 4, 1, 2, 2, 1, 3, 4, 3, 1, 3, 3, 2, 2, 1, 2, 0, 1, 4, 2, 1, 1, 2, 0, 1, 2, 2, 0, 1, 1, 2, 3,
    1, 1, 4, 3, 0, 1, 3, 1, 2, 0, 1, 1, 2, 2, 1, 1, 3, 4, 4, 2, 2, 0, 1, 1, 1, 1, 4, 0, 1, 3, 1, 0, 1, 2, 2, 1, 5, 2, 4, 3, 2, 3, 9, 5, 1, 2, 6, 1, 1, 4,
    0, 1, 1, 4, 3, 0, 1, 5, 0, 1, 2, 4, 2, 3, 2, 6, 0, 1, 1, 2, 0, 1, 2, 2, 1, 0, 1, 1, 2, 0, 1, 4, 1, 3, 1, 2, 5, 5, 4, 1, 2, 2, 0, 1, 2, 1, 0, 1, 1, 1,
    6, 0, 1, 1, 1, 0, 1, 2, 4, 2, 1, 0, 1, 1, 1, 2, 6, 1, 3, 0, 1, 1, 0, 1, 2, 1, 2, 2, 2, 3, 4, 1, 1, 3, 0, 1, 1, 2, 2, 0, 1, 2, 5, 1, 3, 2, 0, 1, 4, 2,
    2, 0, 1, 1, 3, 2, 1, 2, 3, 1, 0, 2, 3, 3, 1, 1, 2, 4, 2, 3, 6, 5, 4, 5, 5, 1, 4, 3, 2, 2, 2, 3, 1, 3, 2, 3, 3, 1, 5, 1, 3, 1, 1, 4, 1, 3, 4, 6, 1, 1,
    1, 3, 1, 3, 0, 2, 5, 3, 0, 1, 2, 2, 3, 1, 3, 0, 1, 2, 1, 3, 3, 3, 1, 1, 3, 2, 3, 2, 3, 1, 3, 2, 2, 1, 0, 1, 4, 1, 1, 3, 0, 1, 3, 3, 4, 0, 1, 2, 2, 5,
    4, 3, 1, 5, 1, 2, 2, 3, 1, 1, 0, 1, 1, 0, 1, 2, 3, 2, 1, 1, 3, 1, 3, 3, 2, 3, 2, 1, 4, 0, 1, 3, 2, 1, 1, 1, 2, 4, 2, 0, 1, 3, 1, 0, 1, 2, 0, 1, 1, 0,
    1, 1, 1, 0, 1, 1, 0, 1, 1, 3, 0, 1, 1, 3, 1, 1, 1, 2, 1, 2, 1, 2, 0, 1, 3, 0, 1, 1, 0, 1, 3, 1, 0, 1, 2, 1, 2, 3, 1, 0, 1, 6, 0, 1, 3, 2, 2, 2, 5, 1,
    1, 2, 2, 0, 1, 2, 2, 3, 0, 1, 2, 1, 3, 2, 2, 3, 3, 2, 2, 1, 1, 1, 3, 2, 2, 2, 2, 2, 5, 1, 2, 4, 1, 0, 2, 1, 5, 1, 2, 5, 1, 2, 4, 0, 1, 3, 1, 1, 0, 2,
    1, 1, 1, 1, 0, 2, 1, 2, 2, 4, 0, 1, 2, 2, 0, 2, 2, 1, 0, 2, 4, 2, 1, 3, 2, 2, 3, 2, 1, 1, 2, 2, 0, 1, 2, 2, 1, 2, 1, 0, 1, 1, 2, 0, 1, 4, 4, 2, 2, 5,
    1, 3, 2, 2, 1, 0, 1, 1, 2, 3, 2, 2, 1, 3, 2, 0, 1, 2, 1, 3, 0, 1, 3, 2, 1, 0, 2, 1, 2, 1, 0, 2, 1, 2, 0, 1, 2, 4, 1, 0, 1, 1, 1, 2, 1, 2, 2, 3, 2, 2,
    0, 1, 1, 2, 1, 0, 1, 4, 2, 1, 1, 2, 5, 2, 0, 1, 2, 2, 0, 1, 1, 1, 2, 1, 0, 1, 2, 0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 3, 2, 4, 1, 0, 1, 2, 0, 4, 2, 1, 4, 1,
    1, 3, 0, 1, 3, 3, 2, 2, 3, 2, 1, 0, 1, 1, 3, 2, 1, 0, 2, 2, 1, 2, 2, 1, 0, 1, 2, 1, 2, 3, 0, 1, 3, 3, 0, 1, 1, 6, 3, 3, 2, 2, 1, 0, 2, 1, 0, 2, 1, 3,
    2, 1, 2, 6, 5, 9, 10, 6, 8, 3, 4, 3, 3, 0, 1, 3, 1, 4, 0, 1, 2, 1, 1, 2, 1, 3, 2, 0, 3, 1, 0, 1, 1, 2, 4, 2, 0, 1, 3, 0, 2, 3, 1, 2, 2, 3, 1, 0, 1, 1,
    4, 1, 2, 2, 2, 1, 1, 0, 1, 1, 1, 0, 2, 4, 3, 2, 4, 1, 1, 4, 4, 2, 3, 3, 4, 1, 1, 3, 2, 1, 1, 0, 2, 3, 2, 1, 0, 2, 2, 1, 1, 0, 2, 1, 1, 2, 1, 3, 0, 2,
    2, 3, 3, 2, 3, 4, 2, 2, 2, 1, 3, 1, 1, 0, 1, 3, 1, 4, 1, 3, 0, 1, 2, 3, 1, 1, 0, 1, 1, 1, 0, 2, 1, 0, 2, 1, 2, 1, 2, 3, 1, 0, 1, 1, 2, 4, 0, 1, 2, 0,
    1, 2, 1, 1, 0, 1, 2, 2, 1, 1, 2, 1, 6, 4, 4, 1, 6, 0, 1, 3, 1, 2, 0, 1, 1, 3, 3, 8, 6, 4, 6, 5, 1, 1, 3, 0, 1, 1, 1, 1, 2, 0, 1, 1, 1, 0, 1, 3, 0, 1,
    1, 3, 0, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 4, 1, 2, 1, 1, 3, 1, 0, 1, 2, 3, 1, 1, 0, 1, 1, 1, 0, 2, 2, 0, 1, 4, 2, 0, 1, 2, 2, 1, 2, 2, 1, 1, 0, 1, 1, 2,
    0, 3, 2, 0, 1, 1, 1, 0, 2, 1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 0, 1, 4, 2, 0, 1, 5, 3, 1, 2, 1, 1, 2, 3, 2, 0, 1, 1, 1, 1, 1, 2, 1, 0, 4, 1, 0, 1, 2, 0, 2,
    3, 3, 0, 2, 1, 2, 1, 1, 0, 1, 3, 0, 1, 1, 2, 2, 1, 0, 1, 1, 0, 1, 1, 4, 1, 0, 1, 1, 0, 2, 3, 1, 0, 1, 2, 1, 0, 1, 4, 1, 2, 0, 1, 2, 1, 0, 1, 2, 0, 1,
    3, 2, 2, 2, 2, 3, 0, 4, 2, 3, 2, 1, 0, 1, 1, 0, 1, 2, 0, 1, 2, 2, 2, 1, 1, 2, 2, 2, 1, 3, 1, 1, 3, 0, 1, 2, 1, 4, 1, 3, 2, 3, 2, 4, 1, 1, 1, 0, 1, 1,
    0, 2, 3, 0, 1, 2, 1, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 4, 3, 3, 1, 2, 0, 1, 1, 0, 2, 1, 0, 1, 1, 2, 1, 0, 1, 2, 0, 2, 3, 2, 2, 0, 1, 2, 0, 2, 3, 0, 2, 2,
    1, 2, 2, 0, 1, 1, 0, 1, 1, 0, 1, 3, 2, 0, 1, 3, 1, 1, 0, 1, 1, 3, 0, 1, 1, 1, 2, 3, 1, 2, 2, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 1, 2, 1, 2, 1, 1, 1, 2, 2,
    0, 1, 2, 4, 1, 2, 1, 1, 2, 1, 1, 2, 2, 1, 1, 1, 2, 2, 0, 1, 1, 1, 0, 1, 2, 0, 1, 2, 1, 1, 1, 0, 6, 1, 0, 1, 1, 2, 2, 0, 2, 4, 2, 0, 1, 5, 4, 0, 2, 2,
    2, 1, 1, 1, 2, 0, 2, 1, 2, 1, 0, 2, 1, 2, 1, 1, 2, 3, 3, 0, 3, 1, 1, 4, 1, 2, 1, 1, 1, 2, 1, 0, 1, 1, 1, 3, 1, 0, 1, 1, 1, 2, 1, 4, 1, 1, 1, 0, 1, 1,
    0, 1, 2, 2, 0, 1, 1, 0, 1, 2, 0, 1, 1, 0, 1, 4, 1, 2, 0, 3, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 3, 0, 1, 2, 3, 1, 3, 4, 5, 7, 4, 4, 1, 0, 1, 3,
    3, 2, 0, 1, 1, 1, 2, 0, 3, 1, 0, 1, 2, 1, 0, 1, 1, 2, 2, 0, 1, 1, 1, 3, 1, 2, 1, 3, 0, 1, 3, 3, 2, 1, 0, 2, 2, 1, 1, 1, 2, 2, 1, 4, 2, 0, 2, 2, 3, 2,
    0, 2, 3, 2, 1, 1, 1, 1, 2, 1, 0, 1, 1, 1, 0, 2, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 0, 1, 3, 3, 0, 1, 2, 1, 2, 1, 2, 0, 1, 1, 0, 1, 4, 3, 2, 0, 1, 1, 0, 1,
    2, 0, 1, 2, 1, 1, 2, 1, 0, 4, 1, 0, 1, 2, 2, 1, 2, 2, 1, 0, 2, 2, 1, 2, 3, 2, 2, 1, 2, 2, 0, 1, 2, 2, 1, 1, 1, 2, 1, 2, 2, 1, 1, 1, 3, 1, 0, 2, 2, 2,
    1, 1, 2, 2, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 2, 2, 1, 0, 1, 2, 1, 2, 0, 1, 2, 0, 1, 2, 0, 4, 1, 0, 3, 1, 2, 0, 1, 1, 3, 1, 2, 2, 0, 2,
    3, 1, 1, 2, 0, 2, 1, 2, 2, 0, 3, 2, 2, 0, 1, 2, 1, 0, 2, 4, 2, 2, 1, 0, 3, 1, 2, 1, 1, 1, 3, 3, 1, 1, 0, 2, 1, 3, 0, 2, 3, 1, 1, 3, 1, 2, 1, 3, 1, 1,
    2, 3, 1, 1, 0, 2, 1, 1, 2, 0, 2, 2, 1, 0, 2, 1, 2, 0, 1, 2, 1, 3, 2, 2, 3, 1, 2, 1, 3, 1, 3, 2, 4, 2, 1, 1, 1, 0, 3, 2, 0, 1, 1, 2, 2, 0, 1, 1, 0, 1,
    2, 1, 2, 3, 3, 1, 2, 1, 1, 1, 2, 1, 1, 1, 0, 6, 4, 1, 2, 2, 2, 0, 1, 1, 1, 0, 1, 2, 1, 0, 1, 2, 1, 2, 3, 2, 3, 1, 0, 1, 1, 3, 1, 2, 1, 3, 1, 0, 3, 1,
    0, 1, 2, 0, 1, 1, 2, 2, 1, 3, 4, 0, 2, 1, 1, 1, 2, 2, 1, 0, 1, 1, 0, 1, 2, 1, 0, 1, 2, 0, 6, 1, 1, 1, 2, 2, 0, 1, 1, 0, 1, 1, 1, 0, 2, 1, 0, 2, 2, 0,
    1, 1, 0, 2, 1, 0, 1, 2, 1, 0, 2, 1, 1, 2, 0, 2, 1, 1, 2, 2, 0, 2, 2, 2, 0, 1, 1, 0, 3, 1, 1, 0, 3, 1, 0, 1, 2, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 1, 0, 1,
    1, 0, 1, 2, 2, 1, 0, 1, 1, 2, 4, 1, 0, 2, 1, 3, 1, 1, 3, 0, 1, 1, 1, 1, 0, 1, 3, 1, 2, 0, 2, 2, 0, 1, 1, 1, 1, 1, 2, 1, 3, 1, 0, 1, 2, 0, 1, 3, 1, 1,
    1, 3, 1, 1, 2, 3, 0, 2, 1, 0, 1, 1, 1, 0, 2, 1, 1, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 0, 3, 2, 0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0,
    5, 1, 0, 1, 2, 0, 2, 1, 0, 3, 1, 1, 0, 2, 1, 0, 1, 1, 1, 3, 1, 1, 1, 5, 1, 0, 3, 2, 1, 1, 0, 1, 2, 1, 0, 2, 1, 1, 0, 3, 2, 0, 1, 1, 2, 1, 2, 1, 1, 2,
    0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 1, 2, 0, 2, 1, 3, 0, 1, 1, 1, 0, 2, 1, 0, 1, 1, 0, 2, 2, 1, 0, 1, 2, 0, 4, 1, 1, 0, 2, 2, 0, 5, 1, 0, 2, 1, 0, 2, 1, 2,
    2, 1, 2, 0, 1, 2, 1, 1, 0, 2, 1, 1, 0, 1, 3, 0, 2, 1, 0, 3, 2, 0, 1, 2, 3, 2, 2, 1, 0, 2, 1, 0, 2, 2, 0, 2, 3, 0, 4, 1, 0, 2, 1, 0, 2, 1, 3, 0, 1, 1,
    0, 2, 1, 1, 1, 0, 2, 2, 2, 0, 2, 1, 2, 1, 0, 2, 1, 1, 1, 0, 6, 1, 0, 2, 1, 0, 4, 3, 1, 2, 0, 1, 3, 0, 6, 1, 0, 2, 2, 1, 2, 0, 2, 2, 1, 1, 0, 1, 1, 0,
    2, 1, 0, 1, 1, 0, 4, 1, 1, 0, 5, 1, 0, 2, 1, 0, 1, 2, 0, 4, 1, 0, 2, 1, 1, 0, 1, 1, 1, 0, 2, 1, 2, 0, 3, 1, 2, 4, 2, 0, 4, 1, 2, 0, 3, 1, 1, 1, 1, 1,
    1, 0, 1, 2, 1, 0, 1, 1, 0, 2, 1, 0, 7, 2, 1, 0, 1, 1, 0, 3, 1, 1, 0, 3, 2, 0, 3, 1, 1, 1, 0, 4, 3, 0, 1, 2, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 3, 0,
    2, 1, 1, 0, 1, 3, 1, 1, 0, 9, 1, 1, 0, 2, 2, 1, 0, 3, 1, 0, 1, 1, 2, 0, 2, 1, 2, 1, 0, 2, 1, 0, 3, 1, 0, 2, 1, 1, 0, 6, 1, 0, 3, 2, 1, 0, 8, 2, 0, 1,
    1, 0, 5, 1, 1, 0, 3, 1, 0, 1, 1, 0, 3, 2, 4, 17, 23, 48, 46, 60, 54, 36, 32, 17, 3, 1, 0, 2, 1, 0, 2, 1, 0, 13, 1, 0, 13, 1, 0, 7, 1, 0, 13, 1, 0, 2, 1, 0, 6, 1,
    0, 4, 1, 0, 7, 1, 0, 2, 1, 0, 2, 2, 0, 3, 1, 0, 6, 1, 0, 3, 2, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 0, 6, 1, 1, 1, 2, 0, 4, 1, 3, 0, 1, 2, 1, 1, 0, 6, 1, 0,
    1, 1, 0, 10, 1, 0, 1, 1, 0, 5, 1, 0, 6, 2, 0, 1, 1, 0, 14, 1, 0, 10, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 4, 2, 0, 7, 1, 0, 5, 1, 0, 16, 1, 0, 3, 1, 1, 0, 2,
    1, 0, 2, 1, 1, 0, 5, 1, 1, 0, 5, 2, 0, 27, 1, 0, 5, 1, 0, 2, 1, 0, 2, 1, 2, 1, 0, 3, 1, 0, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 0, 4, 1, 0, 1,
    1, 2, 1, 1, 0, 5, 1, 1, 1, 1, 1, 0, 2, 2, 1, 0, 7, 1, 1, 0, 3, 3, 0, 6, 1, 0, 13, 1, 1, 0, 1, 1, 0, 1, 1, 0, 6, 1, 0, 9, 1, 0, 11, 1, 1, 0, 14, 1, 1, 1,
    1, 1, 1, 0, 5, 1, 1, 0, 13, 1, 0, 2, 1, 0, 17, 1, 0, 3, 1, 1, 0, 5, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 17, 1, 0, 19, 1, 0, 4, 2, 0, 11, 1, 0, 9,
    1, 0, 3, 1, 0, 3, 1, 0, 4, 1, 1, 0, 7, 1, 0, 12, 1, 0, 2, 1, 0, 11, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 0, 3, 1, 0, 6, 1, 0, 13, 1, 0, 3, 1, 0, 8, 1, 0, 19, 1,
    0, 5, 1, 0, 4, 1, 0, 4, 1, 0, 11, 1, 0, 1, 1, 0, 1, 1, 0, 9, 1, 0, 2, 2, 3, 1, 1, 1, 1, 0, 1, 1, 0, 16, 1, 0, 4, 1, 0, 7, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1,
    0, 4, 1, 0, 8, 1, 0, 1, 1, 0, 2, 1, 0, 12, 1, 1, 0, 2, 1, 0, 3, 1, 0, 6, 3, 2, 1, 5, 3, 3, 7, 3, 1, 0, 9, 1, 0, 7, 1, 0, 3, 1, 0, 12, 1, 1, 0, 1, 2, 0,
    7, 1, 0, 11, 1, 0, 4, 1, 0, 4, 1, 0, 10, 3, 0, 4, 2, 0, 15, 1, 0, 4, 1, 1, 1, 1, 0, 3, 1, 0, 9, 1, 0, 21, 1, 0, 2, 1, 0, 1, 1, 0, 8, 1, 1, 0, 12, 1, 1, 0,
    19, 1, 0, 7, 1, 0, 10, 1, 0, 3, 1, 0, 3, 1, 0, 1, 1, 0, 9, 1, 0, 2, 1, 0, 1, 1, 1, 0, 5, 1, 1, 1, 0, 48, 2, 2, 0, 5, 1, 0, 3, 1, 0, 2, 1, 0, 7, 1, 0, 3,
    1, 0, 2, 1, 0, 4, 1, 0, 5, 1, 0, 3, 1, 0, 5, 1, 0, 4, 1, 0, 12, 1, 0, 3, 1, 0, 2, 2, 0, 1, 1, 0, 2, 1, 0, 6, 1, 0, 3, 1, 0, 2, 1, 0, 12, 1, 0, 2, 1, 0,
    1, 2, 0, 2, 1, 0, 9, 1, 0, 11, 1, 1, 0, 3, 1, 0, 9, 1, 0, 4, 1, 1, 1, 0, 9, 2, 0, 4, 1, 0, 19, 1, 1, 0, 6, 1, 0, 4, 1, 0, 2, 1, 0, 2, 1, 1, 0, 1, 1, 0,
    6, 1, 0, 28, 1, 0, 1, 1, 0, 3, 1, 0, 24, 1, 0, 8, 1, 0, 1, 1, 0, 5, 1, 0, 7, 1, 0, 34, 1, 0, 6, 1, 0, 5, 1, 0, 11, 1, 0, 1, 2, 1, 0, 5, 1, 0, 1, 1, 0, 7,
    2, 0, 4, 1, 0, 26, 1, 0, 6, 1, 0, 4, 1, 0, 29, 1, 0, 3, 1, 0, 7, 1, 0, 1, 1, 0, 2, 1, 0, 6, 1, 0, 2, 1, 0, 7, 1, 0, 2, 1, 0, 32, 1, 0, 8, 1, 0, 12, 1, 0,
    5, 1, 0, 4, 1, 0, 1, 1, 0, 11, 1, 0, 8, 1, 0, 3, 1, 0, 15, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 1, 0, 4, 3, 2, 1, 1, 2, 0, 1, 1, 0, 2, 1, 1, 0, 29, 1, 0, 2, 2,
    1, 0, 1, 2, 0, 9, 1, 0, 2, 1, 0, 11, 1, 1, 1, 0, 30, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 6, 1, 0, 2, 1, 0, 25, 1, 0, 32, 1, 0, 1, 1, 0, 2, 1, 1, 1,
    1, 0, 7, 1, 0, 13, 1, 0, 1, 1, 0, 4, 2, 0, 12, 1, 0, 13, 1, 1, 0, 15, 1, 0, 10, 3, 2, 0, 1, 1, 1, 1, 1, 2, 2, 0, 22, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 34, 1,
    0, 15, 1, 0, 12, 1, 0, 9, 1, 0, 28, 2, 0, 11, 1, 0, 1, 2, 0, 8, 1, 0, 17, 1, 0, 14, 1, 0, 7, 1, 0, 3, 1, 0, 1, 1, 0, 6, 1, 0, 10, 1, 0, 10, 2, 0, 3, 2, 0, 9,
    1, 0, 2, 1, 0, 12, 1, 0, 5, 2, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 11, 1, 0, 3, 1, 0, 11, 1, 1, 0, 2, 1, 0, 18, 1, 0, 8, 1, 1, 0, 1, 1, 0, 5, 1, 0, 1, 1, 1, 0,
    4, 1, 0, 8, 1, 0, 7, 1, 0, 7, 1, 0, 4, 1, 1, 0, 2, 1, 0, 4, 1, 0, 6, 1, 0, 2, 1, 0, 3, 1, 0, 4, 1, 1, 0, 7, 2, 0, 4, 1, 2, 0, 1, 1, 0, 3, 1, 0, 7, 3,
    0, 12, 1, 1, 0, 4, 2, 0, 22, 1, 0, 5, 1, 0, 6, 2, 0, 7, 1, 0, 13, 1, 0, 5, 1, 2, 0, 5, 1, 2, 0, 3, 1, 0, 8, 1, 0, 9, 1, 0, 10, 1, 0, 1, 1, 0, 18, 1, 0, 13,
    1, 0, 1, 1, 0, 4, 1, 0, 10, 2, 0, 13, 1, 0, 3, 1, 0, 1, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 1, 0, 4, 2, 1, 0, 1, 1, 0, 1, 1, 0, 27, 1, 0, 21, 1, 1, 0, 4, 1, 0,
    2, 1, 0, 18, 1, 0, 18, 1, 0, 1, 1, 0, 20, 2, 0, 18, 1, 0, 2, 1, 0, 2, 1, 1, 0, 19, 1, 0, 35, 1, 0, 25, 1, 0, 3, 1, 0, 36, 1, 0, 24, 1, 0, 10, 1, 0, 20, 1, 1, 0,
    52, 1, 0, 35, 1, 0, 25, 2, 1, 2, 2, 3, 6, 4, 6, 5, 8, 5, 4, 1, 1, 2, 0, 6, 1, 0, 76, 1, 0, 56, 1, 0, 38, 1, 0, 76, 1, 0, 55, 1, 0, 43, 1, 0, 86, 1, 0, 73, 1, 0,
    52, 1, 0, 98, 1, 0, 131, 1, 0, 245
  };
  assert( test_6_packed.size() == 6710 );
  const vector<uint8_t> test_6_encoded = QRSpecDev::encode_stream_vbyte( test_6_chan_cnts );
  assert( test_6_encoded == test_6_packed );
  vector<uint32_t> test_6_dec;
  const size_t test_6_nbytedec = QRSpecDev::decode_stream_vbyte(test_6_encoded,test_6_dec);
  assert( test_6_nbytedec == test_6_packed.size() );
  assert( test_6_dec == test_6_chan_cnts );
  
  
  
  
  // Test case 7
  const vector<uint32_t> test_7_chan_cnts{
    0, 40, 33, 91, 76, 87, 96, 87, 79, 69, 67, 73, 71, 79, 70, 72, 61, 59, 58, 73,
    73, 85, 68, 76, 70, 78, 75, 68, 65, 52, 74, 66, 70, 50, 49, 64, 61, 70, 60, 67,
    59, 42, 61, 52, 48, 43, 49, 63, 53, 55, 54, 72, 66, 58, 52, 54, 47, 60, 53, 57,
    50, 47, 46, 56, 69, 61, 46, 40, 61, 63, 40, 52, 40, 62, 38, 52, 44, 46, 61, 49,
    54, 62, 52, 41, 61, 52, 60, 62, 50, 48, 42, 50, 42, 49, 58, 58, 43, 46, 49, 54,
    56, 57, 51, 49, 52, 50, 56, 69, 55, 45, 44, 54, 60, 59, 41, 59, 58, 47, 51, 43,
    64, 70, 67, 54, 63, 63, 69, 61, 50, 68, 64, 55, 59, 74, 60, 71, 63, 77, 88, 77,
    76, 82, 71, 82, 108, 97, 86, 94, 97, 100, 110, 97, 83, 95, 84, 90, 72, 95, 86, 105,
    89, 89, 93, 108, 89, 115, 97, 95, 96, 94, 90, 119, 113, 117, 128, 115, 111, 113, 99, 88,
    92, 108, 86, 113, 99, 97, 90, 97, 101, 108, 115, 113, 135, 103, 95, 115, 120, 141, 113, 101,
    115, 132, 133, 128, 127, 131, 135, 126, 143, 112, 130, 118, 141, 119, 148, 141, 182, 151, 183, 186,
    163, 145, 148, 136, 143, 131, 126, 150, 153, 187, 209, 209, 188, 151, 129, 125, 124, 149, 128, 136,
    121, 119, 126, 147, 150, 131, 142, 156, 142, 156, 175, 158, 144, 126, 156, 131, 150, 144, 172, 174,
    188, 194, 167, 176, 192, 203, 191, 185, 161, 133, 142, 151, 141, 177, 179, 171, 185, 169, 142, 129,
    153, 132, 165, 161, 153, 121, 134, 140, 126, 127, 140, 151, 168, 131, 142, 118, 152, 141, 135, 129,
    139, 120, 136, 145, 126, 113, 134, 126, 126, 148, 138, 117, 132, 112, 119, 122, 105, 121, 131, 129,
    138, 120, 118, 143, 137, 118, 116, 126, 98, 103, 116, 96, 125, 118, 120, 107, 107, 98, 113, 105,
    111, 116, 115, 115, 110, 128, 112, 121, 110, 130, 121, 129, 174, 263, 350, 370, 257, 185, 130, 123,
    100, 114, 108, 110, 106, 114, 104, 124, 102, 98, 114, 96, 114, 127, 98, 100, 98, 111, 100, 101,
    108, 113, 114, 122, 101, 111, 110, 117, 108, 121, 122, 104, 110, 114, 84, 110, 98, 106, 110, 113,
    99, 104, 88, 104, 117, 119, 179, 242, 307, 244, 190, 132, 114, 114, 115, 97, 145, 84, 113, 103,
    97, 93, 108, 87, 118, 107, 106, 107, 95, 105, 103, 106, 108, 90, 92, 122, 105, 99, 95, 107,
    85, 88, 113, 131, 101, 94, 118, 118, 111, 100, 97, 103, 106, 105, 113, 104, 117, 125, 115, 110,
    117, 126, 113, 107, 106, 141, 287, 742, 1522, 2291, 2208, 1338, 568, 172, 59, 44, 53, 32, 33, 29,
    37, 38, 35, 35, 40, 46, 35, 37, 33, 35, 44, 25, 32, 51, 77, 57, 58, 49, 29, 30,
    35, 38, 29, 33, 38, 37, 30, 52, 35, 32, 25, 35, 50, 71, 70, 87, 61, 33, 34, 30,
    58, 127, 214, 231, 212, 116, 56, 28, 30, 18, 17, 22, 31, 26, 30, 29, 21, 15, 27, 16,
    28, 30, 22, 21, 25, 25, 30, 19, 25, 21, 22, 29, 19, 19, 30, 16, 20, 26, 21, 21,
    32, 20, 21, 20, 34, 27, 32, 24, 28, 25, 23, 18, 23, 16, 14, 26, 18, 18, 16, 26,
    17, 24, 19, 16, 24, 24, 21, 23, 19, 18, 24, 17, 18, 24, 20, 16, 18, 20, 14, 21,
    22, 24, 37, 26, 20, 27, 15, 26, 17, 22, 19, 22, 38, 53, 46, 46, 31, 25, 19, 22,
    17, 25, 20, 18, 27, 12, 12, 19, 24, 15, 19, 13, 17, 16, 23, 15, 16, 24, 19, 18,
    12, 11, 19, 22, 22, 13, 20, 9, 15, 18, 11, 11, 18, 19, 8, 16, 21, 9, 14, 7,
    18, 17, 14, 16, 19, 11, 17, 12, 18, 16, 15, 16, 14, 10, 13, 9, 11, 10, 11, 11,
    18, 16, 13, 16, 13, 11, 17, 16, 10, 14, 19, 19, 18, 13, 12, 14, 18, 8, 14, 13,
    10, 15, 10, 13, 11, 14, 15, 11, 13, 14, 18, 12, 11, 13, 12, 12, 17, 6, 11, 8,
    24, 16, 15, 8, 12, 20, 14, 13, 10, 11, 18, 16, 9, 16, 15, 13, 15, 19, 10, 11,
    13, 11, 9, 11, 9, 14, 9, 18, 18, 9, 15, 7, 13, 15, 19, 8, 14, 3, 13, 7,
    17, 22, 6, 15, 10, 15, 9, 14, 24, 21, 16, 14, 12, 7, 14, 7, 11, 5, 8, 9,
    14, 14, 13, 18, 9, 7, 13, 4, 8, 7, 10, 18, 10, 6, 7, 13, 10, 8, 11, 11,
    8, 13, 12, 7, 11, 9, 7, 10, 7, 10, 8, 11, 7, 8, 8, 8, 8, 12, 7, 17,
    6, 7, 10, 10, 11, 11, 7, 12, 7, 14, 6, 10, 4, 7, 5, 15, 4, 10, 13, 11,
    9, 8, 8, 9, 4, 8, 11, 8, 7, 9, 10, 10, 5, 6, 4, 8, 12, 11, 14, 7,
    9, 14, 15, 7, 11, 6, 18, 6, 11, 8, 12, 16, 8, 9, 6, 14, 10, 6, 14, 16,
    4, 2, 11, 14, 9, 13, 17, 15, 8, 13, 7, 4, 10, 5, 5, 7, 13, 14, 7, 11,
    15, 8, 9, 12, 8, 5, 11, 7, 12, 7, 12, 7, 7, 11, 13, 8, 9, 7, 3, 3,
    8, 11, 23, 27, 28, 15, 15, 12, 8, 7, 7, 7, 6, 7, 4, 7, 6, 15, 8, 11,
    8, 5, 9, 6, 7, 5, 9, 9, 6, 5, 4, 10, 4, 7, 4, 9, 4, 12, 6, 6,
    3, 6, 4, 3, 3, 6, 6, 6, 8, 12, 6, 6, 5, 8, 4, 8, 6, 8, 7, 8,
    7, 7, 7, 11, 5, 3, 3, 10, 4, 7, 3, 7, 6, 7, 5, 11, 7, 2, 7, 9,
    11, 4, 4, 8, 11, 7, 6, 6, 9, 4, 5, 5, 5, 2, 3, 5, 6, 4, 5, 4,
    6, 13, 4, 10, 6, 9, 0, 1, 4, 7, 9, 8, 8, 5, 4, 8, 6, 3, 5, 8,
    5, 2, 8, 7, 5, 4, 5, 6, 6, 7, 4, 4, 6, 5, 3, 6, 6, 6, 4, 7,
    4, 7, 3, 5, 7, 6, 6, 3, 6, 4, 5, 9, 2, 5, 6, 6, 9, 8, 8, 6,
    4, 4, 9, 4, 5, 8, 5, 4, 2, 9, 5, 2, 8, 4, 6, 5, 2, 10, 5, 8,
    6, 4, 4, 5, 11, 3, 7, 3, 6, 6, 5, 11, 6, 4, 5, 8, 7, 4, 7, 3,
    4, 3, 3, 3, 3, 5, 6, 9, 4, 10, 5, 6, 5, 8, 4, 6, 6, 8, 7, 6,
    8, 5, 6, 6, 6, 4, 7, 0, 1, 8, 5, 2, 6, 13, 3, 2, 3, 7, 12, 4,
    5, 5, 7, 4, 7, 6, 5, 4, 1, 9, 5, 5, 4, 4, 3, 7, 8, 7, 6, 6,
    6, 5, 3, 1, 5, 5, 4, 8, 5, 2, 3, 5, 5, 4, 3, 4, 6, 4, 5, 3,
    5, 7, 6, 5, 3, 3, 5, 5, 3, 5, 7, 6, 3, 5, 9, 5, 3, 7, 6, 9,
    5, 3, 10, 4, 4, 2, 9, 7, 9, 3, 8, 6, 5, 7, 3, 2, 3, 4, 8, 0,
    1, 5, 2, 5, 0, 1, 4, 3, 1, 0, 1, 5, 3, 3, 6, 2, 3, 5, 5, 6,
    5, 1, 3, 1, 4, 7, 4, 5, 6, 7, 1, 6, 8, 3, 6, 3, 4, 4, 2, 5,
    3, 4, 4, 6, 5, 7, 3, 3, 1, 3, 5, 4, 4, 3, 1, 2, 5, 6, 4, 5,
    0, 1, 6, 4, 5, 2, 3, 1, 0, 1, 3, 3, 5, 2, 1, 3, 6, 6, 3, 5,
    0, 1, 2, 2, 2, 6, 8, 6, 7, 0, 1, 4, 2, 4, 2, 5, 5, 7, 3, 4,
    3, 9, 5, 4, 6, 4, 3, 3, 1, 3, 2, 1, 2, 6, 2, 2, 6, 5, 6, 3,
    2, 1, 5, 8, 9, 8, 9, 15, 11, 6, 5, 2, 8, 5, 4, 4, 6, 2, 3, 5,
    2, 4, 2, 1, 2, 2, 6, 3, 1, 4, 3, 2, 6, 6, 3, 7, 5, 7, 3, 4,
    8, 2, 5, 4, 2, 3, 3, 2, 5, 4, 0, 1, 5, 6, 1, 4, 5, 9, 3, 5,
    10, 3, 5, 4, 3, 4, 5, 7, 3, 3, 2, 4, 4, 1, 2, 2, 8, 5, 2, 2,
    2, 2, 5, 2, 3, 6, 5, 2, 2, 3, 3, 4, 0, 1, 3, 4, 6, 2, 6, 0,
    1, 4, 2, 6, 2, 9, 6, 5, 2, 3, 2, 4, 3, 5, 3, 8, 1, 2, 7, 7,
    4, 5, 2, 6, 2, 5, 5, 5, 6, 2, 2, 3, 0, 1, 5, 1, 2, 2, 0, 1,
    4, 5, 3, 5, 2, 1, 3, 3, 3, 5, 3, 7, 6, 1, 2, 4, 2, 6, 3, 1,
    1, 0, 1, 7, 0, 1, 4, 3, 3, 1, 6, 4, 4, 3, 2, 4, 4, 4, 4, 2,
    3, 3, 7, 2, 2, 3, 1, 2, 6, 1, 2, 3, 2, 2, 3, 2, 6, 3, 2, 2,
    6, 5, 1, 4, 5, 1, 4, 3, 7, 12, 11, 14, 12, 10, 8, 3, 2, 3, 2, 3,
    1, 3, 0, 1, 4, 3, 4, 2, 4, 2, 1, 1, 2, 3, 2, 1, 1, 1, 2, 1,
    4, 3, 2, 3, 4, 4, 2, 2, 2, 4, 4, 0, 1, 2, 5, 2, 1, 5, 1, 3,
    4, 1, 0, 1, 3, 4, 2, 2, 2, 0, 1, 5, 2, 4, 5, 1, 4, 2, 3, 2,
    5, 2, 4, 2, 10, 22, 27, 30, 29, 10, 4, 5, 2, 1, 3, 2, 1, 6, 4, 1,
    2, 2, 1, 0, 1, 1, 2, 3, 6, 1, 7, 0, 1, 3, 1, 1, 1, 1, 6, 2,
    1, 0, 1, 2, 4, 4, 2, 2, 2, 3, 1, 7, 3, 2, 1, 6, 3, 3, 0, 1,
    6, 2, 4, 1, 7, 2, 0, 1, 2, 5, 2, 1, 5, 2, 4, 1, 2, 1, 4, 1,
    2, 3, 6, 2, 2, 5, 5, 3, 3, 3, 1, 3, 0, 1, 1, 1, 3, 2, 1, 1,
    1, 0, 1, 3, 1, 3, 3, 0, 1, 1, 2, 3, 1, 2, 2, 3, 3, 4, 3, 4,
    2, 0, 1, 2, 1, 2, 2, 4, 5, 2, 4, 2, 3, 4, 4, 3, 4, 3, 3, 3,
    1, 1, 1, 1, 2, 2, 6, 0, 1, 4, 1, 4, 4, 4, 3, 2, 3, 5, 2, 3,
    1, 1, 1, 2, 3, 0, 1, 6, 3, 4, 6, 2, 1, 8, 0, 1, 2, 3, 2, 0,
    1, 2, 1, 1, 4, 4, 2, 3, 1, 3, 3, 4, 2, 2, 4, 3, 3, 3, 2, 3,
    2, 2, 1, 2, 6, 5, 3, 1, 2, 2, 2, 0, 1, 2, 3, 2, 3, 1, 1, 2,
    3, 1, 0, 1, 3, 1, 1, 2, 2, 2, 2, 1, 1, 10, 4, 1, 1, 0, 1, 3,
    0, 1, 4, 3, 2, 2, 3, 2, 1, 3, 1, 3, 3, 1, 1, 6, 1, 4, 2, 7,
    0, 1, 2, 1, 3, 2, 2, 2, 1, 4, 2, 3, 2, 3, 3, 2, 5, 1, 0, 1,
    1, 2, 6, 2, 4, 1, 2, 6, 2, 2, 2, 3, 4, 2, 3, 0, 1, 1, 2, 2,
    0, 1, 3, 2, 4, 4, 3, 2, 3, 4, 3, 4, 2, 3, 6, 3, 2, 1, 2, 2,
    1, 4, 1, 2, 0, 1, 3, 3, 1, 0, 1, 3, 1, 3, 3, 2, 4, 2, 1, 2,
    2, 0, 1, 1, 1, 1, 0, 2, 4, 2, 2, 5, 5, 7, 7, 6, 2, 4, 1, 0,
    1, 3, 1, 5, 2, 2, 1, 4, 2, 2, 2, 0, 2, 2, 3, 3, 3, 2, 2, 2,
    4, 0, 1, 2, 0, 1, 3, 0, 1, 1, 1, 2, 3, 1, 2, 1, 4, 0, 1, 3,
    5, 1, 2, 1, 0, 1, 4, 4, 4, 1, 3, 1, 4, 1, 2, 2, 3, 2, 2, 2,
    1, 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 3, 2, 4, 2, 2, 3, 1,
    2, 2, 4, 2, 1, 1, 0, 1, 2, 2, 0, 1, 3, 2, 1, 1, 0, 2, 1, 5,
    4, 5, 2, 3, 1, 1, 1, 1, 3, 3, 3, 6, 1, 2, 5, 5, 4, 3, 3, 5,
    2, 1, 1, 3, 0, 1, 3, 1, 3, 2, 3, 3, 2, 1, 0, 2, 2, 1, 1, 2,
    1, 0, 1, 1, 2, 0, 1, 3, 0, 1, 2, 0, 1, 4, 4, 2, 4, 3, 2, 1,
    1, 0, 1, 1, 1, 2, 2, 2, 1, 5, 2, 4, 2, 1, 0, 1, 4, 3, 2, 0,
    1, 3, 2, 1, 2, 1, 1, 3, 2, 3, 1, 3, 1, 7, 2, 2, 2, 7, 8, 5,
    1, 1, 1, 1, 4, 1, 0, 1, 3, 3, 3, 3, 3, 2, 1, 3, 2, 2, 1, 1,
    4, 2, 1, 1, 2, 0, 1, 2, 0, 1, 1, 2, 1, 0, 1, 2, 4, 0, 2, 1,
    2, 0, 2, 2, 1, 2, 4, 2, 0, 2, 2, 3, 2, 1, 1, 3, 2, 3, 3, 2,
    2, 3, 2, 0, 2, 3, 0, 1, 1, 2, 3, 2, 3, 0, 1, 3, 0, 1, 2, 2,
    0, 1, 1, 0, 1, 5, 1, 0, 1, 2, 1, 2, 1, 2, 1, 1, 1, 1, 0, 1,
    1, 0, 1, 1, 2, 2, 4, 0, 1, 2, 2, 1, 0, 1, 3, 6, 1, 3, 2, 0,
    1, 5, 2, 2, 2, 1, 5, 1, 2, 1, 0, 2, 3, 2, 2, 2, 2, 1, 2, 1,
    3, 6, 2, 1, 2, 3, 2, 2, 2, 3, 3, 2, 0, 1, 1, 1, 2, 2, 0, 2,
    1, 1, 3, 0, 1, 2, 1, 0, 1, 1, 0, 2, 2, 0, 1, 1, 2, 1, 1, 3,
    2, 0, 1, 2, 0, 3, 2, 1, 2, 1, 1, 2, 2, 2, 6, 5, 3, 2, 3, 3,
    1, 2, 0, 1, 1, 2, 3, 1, 1, 3, 2, 2, 1, 1, 1, 2, 1, 0, 1, 4,
    3, 2, 1, 2, 2, 2, 0, 1, 3, 1, 2, 2, 1, 1, 2, 2, 1, 1, 5, 5,
    3, 0, 1, 4, 1, 0, 1, 2, 0, 1, 2, 1, 2, 2, 2, 1, 1, 3, 6, 1,
    1, 3, 2, 0, 1, 2, 2, 2, 3, 3, 1, 1, 1, 1, 0, 2, 2, 1, 0, 1,
    2, 1, 1, 0, 2, 3, 3, 0, 1, 1, 4, 1, 2, 0, 1, 3, 3, 2, 1, 0,
    2, 1, 0, 1, 1, 0, 1, 2, 3, 2, 1, 1, 0, 1, 1, 1, 2, 1, 2, 3,
    5, 3, 2, 3, 5, 2, 1, 2, 4, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1,
    1, 2, 0, 2, 5, 2, 4, 7, 7, 7, 6, 7, 4, 1, 3, 2, 1, 1, 3, 1,
    3, 1, 0, 1, 1, 1, 1, 0, 1, 1, 3, 4, 1, 1, 1, 6, 2, 0, 2, 1,
    3, 2, 1, 0, 1, 1, 4, 1, 0, 1, 1, 1, 0, 1, 4, 1, 3, 3, 3, 1,
    3, 2, 2, 0, 1, 2, 1, 2, 2, 3, 3, 2, 0, 2, 3, 3, 3, 0, 1, 3,
    3, 1, 0, 1, 3, 1, 3, 1, 1, 0, 1, 2, 2, 1, 3, 0, 4, 1, 2, 2,
    2, 2, 0, 2, 1, 2, 1, 0, 1, 2, 0, 3, 1, 2, 1, 3, 1, 2, 1, 0,
    2, 1, 0, 1, 2, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 5, 2, 0, 1, 2,
    4, 1, 1, 2, 2, 0, 1, 1, 3, 3, 3, 4, 1, 2, 1, 2, 0, 1, 1, 2,
    1, 2, 3, 1, 1, 4, 1, 3, 2, 2, 1, 1, 1, 3, 3, 4, 3, 5, 10, 7,
    5, 7, 3, 1, 3, 1, 1, 2, 2, 1, 1, 2, 1, 2, 1, 2, 3, 1, 0, 1,
    2, 3, 2, 0, 1, 2, 0, 1, 1, 0, 1, 2, 2, 3, 1, 0, 2, 2, 0, 1,
    2, 1, 0, 1, 4, 1, 3, 1, 2, 1, 1, 1, 2, 0, 1, 3, 3, 1, 1, 0,
    1, 3, 3, 4, 0, 1, 1, 3, 2, 1, 2, 0, 1, 3, 3, 1, 3, 2, 2, 1,
    0, 1, 3, 1, 0, 1, 2, 2, 0, 2, 2, 2, 5, 2, 1, 4, 2, 11, 3, 4,
    2, 4, 1, 3, 0, 1, 1, 3, 0, 2, 2, 4, 2, 0, 1, 2, 1, 3, 2, 3,
    2, 0, 1, 1, 0, 1, 2, 2, 2, 2, 0, 1, 1, 2, 0, 1, 1, 1, 1, 1,
    2, 1, 1, 1, 1, 0, 1, 1, 0, 3, 1, 1, 2, 0, 1, 1, 1, 2, 4, 1,
    3, 2, 1, 0, 1, 2, 0, 1, 2, 2, 2, 3, 0, 3, 1, 2, 1, 1, 5, 2,
    1, 0, 1, 1, 0, 2, 1, 1, 1, 1, 1, 0, 1, 2, 0, 1, 4, 1, 1, 4,
    1, 1, 2, 1, 0, 2, 1, 3, 1, 4, 0, 2, 3, 2, 2, 0, 1, 1, 1, 1,
    1, 4, 0, 1, 4, 1, 0, 1, 2, 0, 2, 2, 2, 1, 0, 1, 1, 1, 0, 1,
    1, 0, 1, 2, 0, 1, 1, 0, 1, 3, 2, 1, 2, 1, 4, 2, 2, 1, 1, 3,
    0, 1, 1, 3, 2, 3, 3, 1, 3, 2, 1, 2, 2, 1, 1, 3, 1, 1, 1, 0,
    1, 1, 3, 2, 1, 3, 3, 2, 1, 2, 2, 4, 0, 2, 1, 0, 1, 1, 1, 3,
    3, 3, 3, 1, 1, 0, 2, 1, 1, 3, 1, 0, 1, 3, 2, 0, 1, 1, 2, 0,
    1, 3, 0, 1, 3, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 2, 0, 1, 2, 2,
    1, 1, 0, 3, 1, 2, 1, 1, 0, 2, 1, 0, 1, 2, 0, 2, 1, 1, 2, 0,
    2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 2, 1, 1, 3, 0, 1, 1, 2, 2, 1,
    1, 1, 1, 0, 1, 1, 1, 1, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 2, 1,
    2, 1, 1, 3, 2, 2, 0, 1, 1, 2, 0, 1, 2, 2, 1, 0, 1, 1, 1, 0,
    1, 2, 3, 2, 1, 0, 3, 1, 2, 1, 2, 3, 1, 5, 0, 2, 4, 1, 0, 3,
    3, 2, 1, 1, 0, 1, 1, 1, 3, 2, 3, 1, 0, 1, 1, 0, 1, 1, 0, 1,
    2, 2, 2, 2, 5, 4, 8, 4, 7, 5, 4, 1, 0, 1, 2, 0, 1, 2, 1, 2,
    0, 1, 1, 2, 0, 1, 1, 1, 1, 2, 3, 1, 1, 1, 2, 0, 1, 1, 0, 1,
    2, 1, 0, 2, 1, 0, 2, 1, 3, 0, 1, 1, 0, 1, 3, 1, 1, 1, 3, 3,
    3, 0, 1, 1, 2, 0, 1, 1, 3, 0, 1, 1, 1, 1, 0, 1, 2, 1, 0, 1,
    1, 1, 1, 1, 1, 5, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 1, 2,
    1, 2, 2, 3, 1, 1, 2, 1, 0, 2, 1, 1, 1, 0, 1, 3, 1, 1, 2, 2,
    0, 1, 3, 1, 0, 1, 1, 1, 1, 0, 1, 1, 3, 2, 0, 1, 3, 1, 0, 1,
    2, 2, 1, 1, 1, 1, 0, 1, 5, 1, 1, 3, 0, 1, 1, 1, 0, 2, 2, 4,
    2, 0, 2, 1, 1, 0, 1, 4, 0, 1, 2, 1, 2, 1, 1, 2, 1, 1, 3, 1,
    0, 1, 1, 2, 3, 0, 1, 1, 3, 1, 1, 1, 0, 1, 3, 2, 3, 1, 1, 1,
    0, 2, 1, 2, 1, 4, 2, 1, 3, 0, 1, 2, 4, 2, 0, 1, 1, 1, 0, 1,
    3, 2, 4, 1, 0, 1, 1, 1, 1, 2, 2, 3, 1, 1, 0, 1, 2, 1, 0, 4,
    2, 0, 1, 1, 2, 0, 1, 3, 0, 1, 2, 2, 3, 1, 0, 1, 2, 1, 1, 1,
    1, 0, 1, 1, 1, 1, 0, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 0, 3, 2,
    4, 2, 1, 2, 2, 3, 3, 2, 3, 0, 1, 2, 3, 0, 3, 1, 0, 1, 1, 0,
    1, 2, 1, 1, 2, 1, 2, 1, 2, 0, 1, 3, 2, 0, 1, 2, 0, 1, 1, 4,
    2, 1, 0, 1, 2, 2, 2, 3, 0, 1, 1, 0, 1, 3, 1, 5, 1, 4, 0, 1,
    2, 2, 1, 0, 3, 1, 1, 1, 1, 2, 2, 0, 1, 4, 0, 2, 2, 0, 1, 1,
    3, 2, 1, 0, 1, 2, 2, 1, 2, 0, 2, 1, 2, 3, 3, 4, 4, 3, 4, 0,
    1, 2, 0, 1, 3, 0, 2, 1, 0, 1, 1, 2, 2, 0, 2, 1, 0, 1, 3, 3,
    3, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 0, 1,
    2, 2, 0, 1, 2, 0, 1, 2, 2, 2, 3, 1, 0, 1, 2, 0, 1, 1, 0, 1,
    2, 0, 3, 1, 2, 1, 0, 1, 1, 0, 1, 2, 0, 1, 1, 1, 2, 0, 1, 2,
    1, 2, 0, 1, 1, 2, 3, 1, 0, 1, 2, 0, 1, 2, 2, 1, 0, 2, 4, 0,
    2, 1, 0, 2, 1, 2, 0, 1, 1, 2, 0, 1, 2, 0, 1, 1, 0, 1, 1, 2,
    0, 1, 2, 0, 1, 2, 1, 2, 0, 3, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1,
    1, 3, 1, 4, 0, 3, 1, 1, 2, 0, 1, 1, 1, 2, 2, 0, 1, 1, 3, 2,
    1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 3, 1, 1, 2, 1, 0, 1, 1, 1, 1,
    0, 5, 4, 0, 2, 1, 1, 1, 1, 0, 1, 1, 1, 1, 3, 1, 2, 1, 2, 0,
    1, 3, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0,
    2, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1, 1, 0, 1, 1, 0,
    4, 1, 0, 4, 1, 0, 3, 1, 1, 0, 2, 1, 1, 1, 0, 1, 2, 0, 7, 1,
    2, 2, 0, 6, 1, 1, 1, 0, 1, 1, 0, 1, 2, 1, 0, 1, 1, 1, 0, 1,
    1, 1, 1, 2, 0, 1, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 3, 1,
    0, 5, 2, 1, 0, 2, 1, 0, 4, 2, 0, 1, 1, 3, 0, 1, 2, 0, 2, 1,
    0, 1, 2, 2, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 2, 0,
    4, 2, 0, 3, 1, 0, 2, 1, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 1,
    2, 0, 2, 1, 0, 5, 1, 1, 0, 3, 2, 0, 1, 2, 2, 0, 1, 1, 2, 0,
    3, 1, 0, 5, 2, 0, 4, 1, 1, 0, 1, 1, 0, 2, 2, 4, 3, 1, 1, 2,
    1, 1, 1, 1, 1, 1, 2, 0, 4, 1, 1, 3, 0, 1, 2, 2, 0, 2, 2, 0,
    5, 1, 0, 1, 1, 0, 5, 1, 0, 4, 2, 1, 0, 6, 1, 0, 2, 2, 1, 0,
    1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 4, 2, 1, 0, 1, 1,
    0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 5, 2, 1, 0, 7, 1, 0, 1,
    2, 1, 0, 1, 1, 1, 0, 2, 1, 1, 0, 3, 1, 2, 0, 2, 2, 0, 4, 1,
    1, 0, 2, 1, 1, 0, 10, 2, 0, 2, 1, 1, 1, 0, 1, 1, 0, 3, 2, 1,
    0, 1, 1, 1, 0, 2, 1, 0, 4, 4, 0, 1, 1, 1, 2, 0, 1, 1, 0, 1,
    2, 0, 15, 1, 0, 3, 3, 0, 2, 1, 2, 0, 7, 1, 2, 1, 3, 0, 6, 2,
    0, 1, 1, 1, 1, 0, 2, 1, 1, 0, 1, 1, 1, 3, 0, 1, 4, 4, 10, 23,
    45, 47, 49, 58, 42, 26, 18, 6, 4, 0, 2, 1, 0, 1, 1, 0, 1, 2, 0, 1,
    2, 2, 0, 3, 1, 0, 13, 1, 0, 2, 1, 1, 0, 14, 1, 0, 2, 1, 0, 4,
    1, 0, 1, 1, 0, 3, 1, 0, 6, 1, 0, 3, 1, 0, 4, 1, 1, 0, 11, 2,
    0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 9, 1, 0, 2, 2, 0, 7, 1, 1, 1,
    0, 2, 1, 3, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 14, 2, 1, 0, 2, 1,
    0, 3, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1,
    0, 15, 1, 0, 4, 1, 0, 4, 2, 0, 6, 1, 0, 2, 1, 0, 1, 1, 1, 0,
    3, 1, 0, 6, 2, 1, 0, 1, 1, 0, 1, 1, 0, 9, 1, 0, 15, 2, 0, 4,
    1, 0, 3, 1, 0, 5, 1, 0, 7, 1, 0, 2, 1, 0, 4, 1, 0, 1, 1, 0,
    4, 2, 1, 0, 2, 1, 0, 2, 1, 0, 7, 1, 0, 3, 1, 1, 0, 6, 1, 0,
    3, 1, 0, 3, 1, 0, 1, 2, 2, 0, 1, 2, 0, 5, 1, 1, 0, 1, 1, 1,
    0, 1, 2, 2, 0, 1, 1, 1, 0, 5, 1, 0, 1, 2, 0, 1, 1, 2, 0, 3,
    1, 0, 6, 1, 0, 1, 1, 0, 1, 1, 1, 0, 6, 1, 0, 10, 1, 1, 0, 6,
    1, 0, 3, 1, 0, 2, 1, 1, 0, 5, 1, 0, 10, 1, 1, 1, 0, 4, 3, 1,
    0, 1, 1, 0, 4, 1, 0, 6, 1, 0, 1, 1, 0, 8, 1, 1, 0, 1, 2, 1,
    0, 5, 1, 1, 0, 3, 1, 0, 2, 1, 1, 0, 8, 1, 1, 0, 9, 1, 0, 5,
    1, 0, 4, 1, 0, 6, 1, 0, 14, 1, 0, 8, 1, 0, 3, 1, 0, 11, 1, 0,
    1, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 0, 8, 1, 0, 4, 1, 0, 4, 1,
    0, 4, 1, 0, 5, 1, 0, 5, 2, 0, 5, 1, 0, 4, 1, 0, 2, 1, 0, 3,
    1, 0, 16, 2, 0, 7, 1, 0, 11, 1, 0, 9, 1, 0, 2, 1, 0, 14, 1, 0,
    4, 1, 0, 2, 1, 1, 0, 9, 1, 0, 6, 1, 0, 5, 1, 1, 0, 6, 3, 1,
    1, 3, 1, 0, 2, 1, 0, 2, 1, 0, 9, 1, 0, 3, 1, 0, 18, 1, 0, 8,
    2, 0, 3, 1, 0, 6, 2, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 0, 16, 1,
    0, 1, 1, 1, 1, 1, 4, 8, 4, 6, 7, 4, 1, 0, 2, 1, 0, 9, 1, 1,
    0, 11, 1, 0, 3, 1, 0, 2, 1, 0, 4, 1, 0, 2, 1, 0, 8, 1, 0, 34,
    1, 0, 1, 1, 0, 17, 1, 0, 4, 2, 0, 2, 1, 1, 0, 4, 1, 0, 1, 1,
    0, 3, 1, 0, 8, 1, 0, 6, 1, 0, 34, 1, 0, 1, 1, 0, 11, 1, 0, 17,
    1, 0, 6, 1, 0, 5, 1, 2, 1, 0, 2, 1, 0, 9, 1, 0, 3, 1, 0, 12,
    1, 0, 12, 1, 1, 0, 7, 1, 0, 1, 1, 0, 7, 1, 0, 3, 2, 0, 6, 1,
    0, 6, 1, 0, 5, 1, 0, 4, 1, 0, 1, 1, 0, 6, 1, 0, 6, 1, 0, 8,
    1, 0, 5, 1, 0, 7, 1, 0, 8, 1, 0, 22, 1, 0, 7, 1, 0, 2, 1, 0,
    18, 1, 0, 3, 2, 1, 0, 6, 1, 0, 21, 1, 0, 9, 1, 1, 0, 1, 1, 0,
    1, 1, 0, 1, 1, 0, 23, 1, 0, 7, 1, 1, 0, 2, 3, 0, 3, 1, 0, 4,
    1, 1, 0, 17, 1, 0, 1, 1, 0, 1, 1, 0, 4, 2, 1, 0, 19, 1, 0, 17,
    1, 0, 12, 1, 0, 1, 1, 0, 35, 1, 0, 3, 1, 0, 6, 1, 0, 9, 1, 0,
    13, 2, 0, 3, 1, 0, 7, 1, 0, 3, 1, 0, 5, 1, 0, 3, 1, 0, 6, 1,
    0, 4, 1, 0, 13, 1, 1, 0, 4, 1, 1, 0, 1, 1, 0, 2, 1, 0, 3, 1,
    0, 18, 1, 0, 19, 1, 0, 14, 1, 0, 1, 1, 1, 0, 7, 1, 1, 0, 7, 1,
    0, 6, 1, 0, 7, 2, 1, 0, 7, 1, 0, 2, 1, 0, 4, 1, 2, 0, 1, 1,
    0, 10, 2, 0, 15, 1, 0, 1, 1, 0, 22, 2, 0, 11, 1, 0, 5, 1, 0, 1,
    1, 0, 4, 2, 2, 1, 0, 2, 2, 1, 2, 0, 5, 1, 0, 8, 1, 0, 40, 1,
    0, 11, 1, 0, 20, 1, 0, 13, 1, 0, 4, 1, 0, 4, 1, 0, 3, 1, 0, 3,
    1, 0, 8, 1, 0, 28, 1, 0, 3, 1, 1, 0, 4, 1, 0, 7, 3, 0, 3, 1,
    0, 23, 2, 0, 1, 1, 1, 0, 7, 1, 0, 8, 1, 1, 0, 4, 1, 0, 3, 1,
    0, 3, 1, 0, 19, 1, 0, 3, 1, 0, 2, 1, 0, 2, 1, 2, 2, 0, 1, 2,
    1, 0, 2, 1, 0, 6, 1, 0, 7, 1, 0, 4, 1, 0, 12, 1, 0, 7, 1, 0,
    6, 1, 0, 40, 1, 0, 7, 1, 0, 7, 1, 1, 0, 13, 1, 0, 4, 1, 0, 7,
    1, 0, 15, 1, 0, 8, 1, 0, 9, 1, 1, 0, 5, 1, 0, 6, 1, 0, 7, 1,
    0, 1, 1, 0, 3, 1, 0, 3, 1, 0, 5, 1, 0, 15, 2, 0, 10, 1, 0, 6,
    1, 0, 6, 1, 0, 5, 1, 0, 10, 1, 0, 5, 1, 0, 4, 1, 1, 1, 0, 11,
    1, 1, 0, 1, 1, 0, 2, 1, 0, 12, 1, 0, 5, 1, 0, 33, 1, 0, 4, 2,
    0, 2, 2, 0, 10, 2, 0, 6, 1, 0, 8, 1, 1, 0, 7, 1, 1, 0, 6, 1,
    0, 1, 1, 1, 0, 19, 1, 0, 2, 1, 0, 4, 1, 0, 3, 1, 0, 8, 1, 0,
    1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 6, 1, 0, 1, 1, 0, 15, 1,
    0, 2, 1, 0, 2, 1, 0, 3, 1, 0, 5, 1, 0, 3, 1, 0, 3, 1, 1, 0,
    4, 1, 0, 8, 1, 0, 9, 1, 0, 1, 1, 1, 0, 6, 1, 0, 7, 2, 0, 7,
    1, 0, 15, 1, 0, 8, 1, 0, 4, 1, 0, 30, 1, 0, 9, 1, 0, 2, 1, 1,
    1, 0, 13, 1, 0, 12, 1, 0, 12, 1, 0, 3, 1, 1, 0, 18, 1, 0, 38, 1,
    0, 19, 1, 0, 19, 1, 0, 24, 1, 0, 63, 1, 0, 61, 1, 0, 24, 1, 0, 34,
    1, 0, 60, 1, 0, 38, 1, 0, 12, 1, 0, 5, 1, 0, 3, 1, 1, 1, 1, 1,
    4, 7, 3, 7, 11, 5, 4, 2, 1, 1, 0, 113, 1, 0, 38, 1, 0, 374, 1, 0,
    196, 1, 0, 26, 1, 0, 100, 1, 0, 166, 1, 0, 26  };
  assert( test_7_chan_cnts.size() == 5373 );
  const vector<uint8_t> test_7_packed{
    253, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 84, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 85, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 40, 33, 91,
    76, 87, 96, 87, 79, 69, 67, 73, 71, 79, 70, 72, 61, 59, 58, 73, 73, 85, 68, 76, 70, 78, 75, 68, 65, 52, 74, 66, 70, 50, 49, 64, 61, 70, 60, 67, 59, 42, 61, 52, 48, 43, 49, 63, 53, 55, 54, 72, 66, 58,
    52, 54, 47, 60, 53, 57, 50, 47, 46, 56, 69, 61, 46, 40, 61, 63, 40, 52, 40, 62, 38, 52, 44, 46, 61, 49, 54, 62, 52, 41, 61, 52, 60, 62, 50, 48, 42, 50, 42, 49, 58, 58, 43, 46, 49, 54, 56, 57, 51, 49,
    52, 50, 56, 69, 55, 45, 44, 54, 60, 59, 41, 59, 58, 47, 51, 43, 64, 70, 67, 54, 63, 63, 69, 61, 50, 68, 64, 55, 59, 74, 60, 71, 63, 77, 88, 77, 76, 82, 71, 82, 108, 97, 86, 94, 97, 100, 110, 97, 83, 95,
    84, 90, 72, 95, 86, 105, 89, 89, 93, 108, 89, 115, 97, 95, 96, 94, 90, 119, 113, 117, 128, 115, 111, 113, 99, 88, 92, 108, 86, 113, 99, 97, 90, 97, 101, 108, 115, 113, 135, 103, 95, 115, 120, 141, 113, 101, 115, 132, 133, 128,
    127, 131, 135, 126, 143, 112, 130, 118, 141, 119, 148, 141, 182, 151, 183, 186, 163, 145, 148, 136, 143, 131, 126, 150, 153, 187, 209, 209, 188, 151, 129, 125, 124, 149, 128, 136, 121, 119, 126, 147, 150, 131, 142, 156, 142, 156, 175, 158, 144, 126,
    156, 131, 150, 144, 172, 174, 188, 194, 167, 176, 192, 203, 191, 185, 161, 133, 142, 151, 141, 177, 179, 171, 185, 169, 142, 129, 153, 132, 165, 161, 153, 121, 134, 140, 126, 127, 140, 151, 168, 131, 142, 118, 152, 141, 135, 129, 139, 120, 136, 145,
    126, 113, 134, 126, 126, 148, 138, 117, 132, 112, 119, 122, 105, 121, 131, 129, 138, 120, 118, 143, 137, 118, 116, 126, 98, 103, 116, 96, 125, 118, 120, 107, 107, 98, 113, 105, 111, 116, 115, 115, 110, 128, 112, 121, 110, 130, 121, 129, 174, 7,
    1, 94, 1, 114, 1, 1, 1, 185, 130, 123, 100, 114, 108, 110, 106, 114, 104, 124, 102, 98, 114, 96, 114, 127, 98, 100, 98, 111, 100, 101, 108, 113, 114, 122, 101, 111, 110, 117, 108, 121, 122, 104, 110, 114, 84, 110, 98, 106, 110, 113,
    99, 104, 88, 104, 117, 119, 179, 242, 51, 1, 244, 190, 132, 114, 114, 115, 97, 145, 84, 113, 103, 97, 93, 108, 87, 118, 107, 106, 107, 95, 105, 103, 106, 108, 90, 92, 122, 105, 99, 95, 107, 85, 88, 113, 131, 101, 94, 118, 118, 111,
    100, 97, 103, 106, 105, 113, 104, 117, 125, 115, 110, 117, 126, 113, 107, 106, 141, 31, 1, 230, 2, 242, 5, 243, 8, 160, 8, 58, 5, 56, 2, 172, 59, 44, 53, 32, 33, 29, 37, 38, 35, 35, 40, 46, 35, 37, 33, 35, 44, 25,
    32, 51, 77, 57, 58, 49, 29, 30, 35, 38, 29, 33, 38, 37, 30, 52, 35, 32, 25, 35, 50, 71, 70, 87, 61, 33, 34, 30, 58, 127, 214, 231, 212, 116, 56, 28, 30, 18, 17, 22, 31, 26, 30, 29, 21, 15, 27, 16, 28, 30,
    22, 21, 25, 25, 30, 19, 25, 21, 22, 29, 19, 19, 30, 16, 20, 26, 21, 21, 32, 20, 21, 20, 34, 27, 32, 24, 28, 25, 23, 18, 23, 16, 14, 26, 18, 18, 16, 26, 17, 24, 19, 16, 24, 24, 21, 23, 19, 18, 24, 17,
    18, 24, 20, 16, 18, 20, 14, 21, 22, 24, 37, 26, 20, 27, 15, 26, 17, 22, 19, 22, 38, 53, 46, 46, 31, 25, 19, 22, 17, 25, 20, 18, 27, 12, 12, 19, 24, 15, 19, 13, 17, 16, 23, 15, 16, 24, 19, 18, 12, 11,
    19, 22, 22, 13, 20, 9, 15, 18, 11, 11, 18, 19, 8, 16, 21, 9, 14, 7, 18, 17, 14, 16, 19, 11, 17, 12, 18, 16, 15, 16, 14, 10, 13, 9, 11, 10, 11, 11, 18, 16, 13, 16, 13, 11, 17, 16, 10, 14, 19, 19,
    18, 13, 12, 14, 18, 8, 14, 13, 10, 15, 10, 13, 11, 14, 15, 11, 13, 14, 18, 12, 11, 13, 12, 12, 17, 6, 11, 8, 24, 16, 15, 8, 12, 20, 14, 13, 10, 11, 18, 16, 9, 16, 15, 13, 15, 19, 10, 11, 13, 11,
    9, 11, 9, 14, 9, 18, 18, 9, 15, 7, 13, 15, 19, 8, 14, 3, 13, 7, 17, 22, 6, 15, 10, 15, 9, 14, 24, 21, 16, 14, 12, 7, 14, 7, 11, 5, 8, 9, 14, 14, 13, 18, 9, 7, 13, 4, 8, 7, 10, 18,
    10, 6, 7, 13, 10, 8, 11, 11, 8, 13, 12, 7, 11, 9, 7, 10, 7, 10, 8, 11, 7, 8, 8, 8, 8, 12, 7, 17, 6, 7, 10, 10, 11, 11, 7, 12, 7, 14, 6, 10, 4, 7, 5, 15, 4, 10, 13, 11, 9, 8,
    8, 9, 4, 8, 11, 8, 7, 9, 10, 10, 5, 6, 4, 8, 12, 11, 14, 7, 9, 14, 15, 7, 11, 6, 18, 6, 11, 8, 12, 16, 8, 9, 6, 14, 10, 6, 14, 16, 4, 2, 11, 14, 9, 13, 17, 15, 8, 13, 7, 4,
    10, 5, 5, 7, 13, 14, 7, 11, 15, 8, 9, 12, 8, 5, 11, 7, 12, 7, 12, 7, 7, 11, 13, 8, 9, 7, 3, 3, 8, 11, 23, 27, 28, 15, 15, 12, 8, 7, 7, 7, 6, 7, 4, 7, 6, 15, 8, 11, 8, 5,
    9, 6, 7, 5, 9, 9, 6, 5, 4, 10, 4, 7, 4, 9, 4, 12, 6, 6, 3, 6, 4, 3, 3, 6, 6, 6, 8, 12, 6, 6, 5, 8, 4, 8, 6, 8, 7, 8, 7, 7, 7, 11, 5, 3, 3, 10, 4, 7, 3, 7,
    6, 7, 5, 11, 7, 2, 7, 9, 11, 4, 4, 8, 11, 7, 6, 6, 9, 4, 5, 5, 5, 2, 3, 5, 6, 4, 5, 4, 6, 13, 4, 10, 6, 9, 0, 1, 4, 7, 9, 8, 8, 5, 4, 8, 6, 3, 5, 8, 5, 2,
    8, 7, 5, 4, 5, 6, 6, 7, 4, 4, 6, 5, 3, 6, 6, 6, 4, 7, 4, 7, 3, 5, 7, 6, 6, 3, 6, 4, 5, 9, 2, 5, 6, 6, 9, 8, 8, 6, 4, 4, 9, 4, 5, 8, 5, 4, 2, 9, 5, 2,
    8, 4, 6, 5, 2, 10, 5, 8, 6, 4, 4, 5, 11, 3, 7, 3, 6, 6, 5, 11, 6, 4, 5, 8, 7, 4, 7, 3, 4, 3, 3, 3, 3, 5, 6, 9, 4, 10, 5, 6, 5, 8, 4, 6, 6, 8, 7, 6, 8, 5,
    6, 6, 6, 4, 7, 0, 1, 8, 5, 2, 6, 13, 3, 2, 3, 7, 12, 4, 5, 5, 7, 4, 7, 6, 5, 4, 1, 9, 5, 5, 4, 4, 3, 7, 8, 7, 6, 6, 6, 5, 3, 1, 5, 5, 4, 8, 5, 2, 3, 5,
    5, 4, 3, 4, 6, 4, 5, 3, 5, 7, 6, 5, 3, 3, 5, 5, 3, 5, 7, 6, 3, 5, 9, 5, 3, 7, 6, 9, 5, 3, 10, 4, 4, 2, 9, 7, 9, 3, 8, 6, 5, 7, 3, 2, 3, 4, 8, 0, 1, 5,
    2, 5, 0, 1, 4, 3, 1, 0, 1, 5, 3, 3, 6, 2, 3, 5, 5, 6, 5, 1, 3, 1, 4, 7, 4, 5, 6, 7, 1, 6, 8, 3, 6, 3, 4, 4, 2, 5, 3, 4, 4, 6, 5, 7, 3, 3, 1, 3, 5, 4,
    4, 3, 1, 2, 5, 6, 4, 5, 0, 1, 6, 4, 5, 2, 3, 1, 0, 1, 3, 3, 5, 2, 1, 3, 6, 6, 3, 5, 0, 1, 2, 2, 2, 6, 8, 6, 7, 0, 1, 4, 2, 4, 2, 5, 5, 7, 3, 4, 3, 9,
    5, 4, 6, 4, 3, 3, 1, 3, 2, 1, 2, 6, 2, 2, 6, 5, 6, 3, 2, 1, 5, 8, 9, 8, 9, 15, 11, 6, 5, 2, 8, 5, 4, 4, 6, 2, 3, 5, 2, 4, 2, 1, 2, 2, 6, 3, 1, 4, 3, 2,
    6, 6, 3, 7, 5, 7, 3, 4, 8, 2, 5, 4, 2, 3, 3, 2, 5, 4, 0, 1, 5, 6, 1, 4, 5, 9, 3, 5, 10, 3, 5, 4, 3, 4, 5, 7, 3, 3, 2, 4, 4, 1, 2, 2, 8, 5, 2, 2, 2, 2,
    5, 2, 3, 6, 5, 2, 2, 3, 3, 4, 0, 1, 3, 4, 6, 2, 6, 0, 1, 4, 2, 6, 2, 9, 6, 5, 2, 3, 2, 4, 3, 5, 3, 8, 1, 2, 7, 7, 4, 5, 2, 6, 2, 5, 5, 5, 6, 2, 2, 3,
    0, 1, 5, 1, 2, 2, 0, 1, 4, 5, 3, 5, 2, 1, 3, 3, 3, 5, 3, 7, 6, 1, 2, 4, 2, 6, 3, 1, 1, 0, 1, 7, 0, 1, 4, 3, 3, 1, 6, 4, 4, 3, 2, 4, 4, 4, 4, 2, 3, 3,
    7, 2, 2, 3, 1, 2, 6, 1, 2, 3, 2, 2, 3, 2, 6, 3, 2, 2, 6, 5, 1, 4, 5, 1, 4, 3, 7, 12, 11, 14, 12, 10, 8, 3, 2, 3, 2, 3, 1, 3, 0, 1, 4, 3, 4, 2, 4, 2, 1, 1,
    2, 3, 2, 1, 1, 1, 2, 1, 4, 3, 2, 3, 4, 4, 2, 2, 2, 4, 4, 0, 1, 2, 5, 2, 1, 5, 1, 3, 4, 1, 0, 1, 3, 4, 2, 2, 2, 0, 1, 5, 2, 4, 5, 1, 4, 2, 3, 2, 5, 2,
    4, 2, 10, 22, 27, 30, 29, 10, 4, 5, 2, 1, 3, 2, 1, 6, 4, 1, 2, 2, 1, 0, 1, 1, 2, 3, 6, 1, 7, 0, 1, 3, 1, 1, 1, 1, 6, 2, 1, 0, 1, 2, 4, 4, 2, 2, 2, 3, 1, 7,
    3, 2, 1, 6, 3, 3, 0, 1, 6, 2, 4, 1, 7, 2, 0, 1, 2, 5, 2, 1, 5, 2, 4, 1, 2, 1, 4, 1, 2, 3, 6, 2, 2, 5, 5, 3, 3, 3, 1, 3, 0, 1, 1, 1, 3, 2, 1, 1, 1, 0,
    1, 3, 1, 3, 3, 0, 1, 1, 2, 3, 1, 2, 2, 3, 3, 4, 3, 4, 2, 0, 1, 2, 1, 2, 2, 4, 5, 2, 4, 2, 3, 4, 4, 3, 4, 3, 3, 3, 1, 1, 1, 1, 2, 2, 6, 0, 1, 4, 1, 4,
    4, 4, 3, 2, 3, 5, 2, 3, 1, 1, 1, 2, 3, 0, 1, 6, 3, 4, 6, 2, 1, 8, 0, 1, 2, 3, 2, 0, 1, 2, 1, 1, 4, 4, 2, 3, 1, 3, 3, 4, 2, 2, 4, 3, 3, 3, 2, 3, 2, 2,
    1, 2, 6, 5, 3, 1, 2, 2, 2, 0, 1, 2, 3, 2, 3, 1, 1, 2, 3, 1, 0, 1, 3, 1, 1, 2, 2, 2, 2, 1, 1, 10, 4, 1, 1, 0, 1, 3, 0, 1, 4, 3, 2, 2, 3, 2, 1, 3, 1, 3,
    3, 1, 1, 6, 1, 4, 2, 7, 0, 1, 2, 1, 3, 2, 2, 2, 1, 4, 2, 3, 2, 3, 3, 2, 5, 1, 0, 1, 1, 2, 6, 2, 4, 1, 2, 6, 2, 2, 2, 3, 4, 2, 3, 0, 1, 1, 2, 2, 0, 1,
    3, 2, 4, 4, 3, 2, 3, 4, 3, 4, 2, 3, 6, 3, 2, 1, 2, 2, 1, 4, 1, 2, 0, 1, 3, 3, 1, 0, 1, 3, 1, 3, 3, 2, 4, 2, 1, 2, 2, 0, 1, 1, 1, 1, 0, 2, 4, 2, 2, 5,
    5, 7, 7, 6, 2, 4, 1, 0, 1, 3, 1, 5, 2, 2, 1, 4, 2, 2, 2, 0, 2, 2, 3, 3, 3, 2, 2, 2, 4, 0, 1, 2, 0, 1, 3, 0, 1, 1, 1, 2, 3, 1, 2, 1, 4, 0, 1, 3, 5, 1,
    2, 1, 0, 1, 4, 4, 4, 1, 3, 1, 4, 1, 2, 2, 3, 2, 2, 2, 1, 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 3, 2, 4, 2, 2, 3, 1, 2, 2, 4, 2, 1, 1, 0, 1, 2, 2, 0, 1,
    3, 2, 1, 1, 0, 2, 1, 5, 4, 5, 2, 3, 1, 1, 1, 1, 3, 3, 3, 6, 1, 2, 5, 5, 4, 3, 3, 5, 2, 1, 1, 3, 0, 1, 3, 1, 3, 2, 3, 3, 2, 1, 0, 2, 2, 1, 1, 2, 1, 0,
    1, 1, 2, 0, 1, 3, 0, 1, 2, 0, 1, 4, 4, 2, 4, 3, 2, 1, 1, 0, 1, 1, 1, 2, 2, 2, 1, 5, 2, 4, 2, 1, 0, 1, 4, 3, 2, 0, 1, 3, 2, 1, 2, 1, 1, 3, 2, 3, 1, 3,
    1, 7, 2, 2, 2, 7, 8, 5, 1, 1, 1, 1, 4, 1, 0, 1, 3, 3, 3, 3, 3, 2, 1, 3, 2, 2, 1, 1, 4, 2, 1, 1, 2, 0, 1, 2, 0, 1, 1, 2, 1, 0, 1, 2, 4, 0, 2, 1, 2, 0,
    2, 2, 1, 2, 4, 2, 0, 2, 2, 3, 2, 1, 1, 3, 2, 3, 3, 2, 2, 3, 2, 0, 2, 3, 0, 1, 1, 2, 3, 2, 3, 0, 1, 3, 0, 1, 2, 2, 0, 1, 1, 0, 1, 5, 1, 0, 1, 2, 1, 2,
    1, 2, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 2, 4, 0, 1, 2, 2, 1, 0, 1, 3, 6, 1, 3, 2, 0, 1, 5, 2, 2, 2, 1, 5, 1, 2, 1, 0, 2, 3, 2, 2, 2, 2, 1, 2, 1, 3, 6,
    2, 1, 2, 3, 2, 2, 2, 3, 3, 2, 0, 1, 1, 1, 2, 2, 0, 2, 1, 1, 3, 0, 1, 2, 1, 0, 1, 1, 0, 2, 2, 0, 1, 1, 2, 1, 1, 3, 2, 0, 1, 2, 0, 3, 2, 1, 2, 1, 1, 2,
    2, 2, 6, 5, 3, 2, 3, 3, 1, 2, 0, 1, 1, 2, 3, 1, 1, 3, 2, 2, 1, 1, 1, 2, 1, 0, 1, 4, 3, 2, 1, 2, 2, 2, 0, 1, 3, 1, 2, 2, 1, 1, 2, 2, 1, 1, 5, 5, 3, 0,
    1, 4, 1, 0, 1, 2, 0, 1, 2, 1, 2, 2, 2, 1, 1, 3, 6, 1, 1, 3, 2, 0, 1, 2, 2, 2, 3, 3, 1, 1, 1, 1, 0, 2, 2, 1, 0, 1, 2, 1, 1, 0, 2, 3, 3, 0, 1, 1, 4, 1,
    2, 0, 1, 3, 3, 2, 1, 0, 2, 1, 0, 1, 1, 0, 1, 2, 3, 2, 1, 1, 0, 1, 1, 1, 2, 1, 2, 3, 5, 3, 2, 3, 5, 2, 1, 2, 4, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 2,
    0, 2, 5, 2, 4, 7, 7, 7, 6, 7, 4, 1, 3, 2, 1, 1, 3, 1, 3, 1, 0, 1, 1, 1, 1, 0, 1, 1, 3, 4, 1, 1, 1, 6, 2, 0, 2, 1, 3, 2, 1, 0, 1, 1, 4, 1, 0, 1, 1, 1,
    0, 1, 4, 1, 3, 3, 3, 1, 3, 2, 2, 0, 1, 2, 1, 2, 2, 3, 3, 2, 0, 2, 3, 3, 3, 0, 1, 3, 3, 1, 0, 1, 3, 1, 3, 1, 1, 0, 1, 2, 2, 1, 3, 0, 4, 1, 2, 2, 2, 2,
    0, 2, 1, 2, 1, 0, 1, 2, 0, 3, 1, 2, 1, 3, 1, 2, 1, 0, 2, 1, 0, 1, 2, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 5, 2, 0, 1, 2, 4, 1, 1, 2, 2, 0, 1, 1, 3, 3, 3, 4,
    1, 2, 1, 2, 0, 1, 1, 2, 1, 2, 3, 1, 1, 4, 1, 3, 2, 2, 1, 1, 1, 3, 3, 4, 3, 5, 10, 7, 5, 7, 3, 1, 3, 1, 1, 2, 2, 1, 1, 2, 1, 2, 1, 2, 3, 1, 0, 1, 2, 3,
    2, 0, 1, 2, 0, 1, 1, 0, 1, 2, 2, 3, 1, 0, 2, 2, 0, 1, 2, 1, 0, 1, 4, 1, 3, 1, 2, 1, 1, 1, 2, 0, 1, 3, 3, 1, 1, 0, 1, 3, 3, 4, 0, 1, 1, 3, 2, 1, 2, 0,
    1, 3, 3, 1, 3, 2, 2, 1, 0, 1, 3, 1, 0, 1, 2, 2, 0, 2, 2, 2, 5, 2, 1, 4, 2, 11, 3, 4, 2, 4, 1, 3, 0, 1, 1, 3, 0, 2, 2, 4, 2, 0, 1, 2, 1, 3, 2, 3, 2, 0,
    1, 1, 0, 1, 2, 2, 2, 2, 0, 1, 1, 2, 0, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0, 1, 1, 0, 3, 1, 1, 2, 0, 1, 1, 1, 2, 4, 1, 3, 2, 1, 0, 1, 2, 0, 1, 2, 2, 2, 3,
    0, 3, 1, 2, 1, 1, 5, 2, 1, 0, 1, 1, 0, 2, 1, 1, 1, 1, 1, 0, 1, 2, 0, 1, 4, 1, 1, 4, 1, 1, 2, 1, 0, 2, 1, 3, 1, 4, 0, 2, 3, 2, 2, 0, 1, 1, 1, 1, 1, 4,
    0, 1, 4, 1, 0, 1, 2, 0, 2, 2, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 2, 0, 1, 1, 0, 1, 3, 2, 1, 2, 1, 4, 2, 2, 1, 1, 3, 0, 1, 1, 3, 2, 3, 3, 1, 3, 2, 1, 2,
    2, 1, 1, 3, 1, 1, 1, 0, 1, 1, 3, 2, 1, 3, 3, 2, 1, 2, 2, 4, 0, 2, 1, 0, 1, 1, 1, 3, 3, 3, 3, 1, 1, 0, 2, 1, 1, 3, 1, 0, 1, 3, 2, 0, 1, 1, 2, 0, 1, 3,
    0, 1, 3, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 2, 0, 1, 2, 2, 1, 1, 0, 3, 1, 2, 1, 1, 0, 2, 1, 0, 1, 2, 0, 2, 1, 1, 2, 0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 2, 1,
    1, 3, 0, 1, 1, 2, 2, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 2, 1, 2, 1, 1, 3, 2, 2, 0, 1, 1, 2, 0, 1, 2, 2, 1, 0, 1, 1, 1, 0, 1, 2,
    3, 2, 1, 0, 3, 1, 2, 1, 2, 3, 1, 5, 0, 2, 4, 1, 0, 3, 3, 2, 1, 1, 0, 1, 1, 1, 3, 2, 3, 1, 0, 1, 1, 0, 1, 1, 0, 1, 2, 2, 2, 2, 5, 4, 8, 4, 7, 5, 4, 1,
    0, 1, 2, 0, 1, 2, 1, 2, 0, 1, 1, 2, 0, 1, 1, 1, 1, 2, 3, 1, 1, 1, 2, 0, 1, 1, 0, 1, 2, 1, 0, 2, 1, 0, 2, 1, 3, 0, 1, 1, 0, 1, 3, 1, 1, 1, 3, 3, 3, 0,
    1, 1, 2, 0, 1, 1, 3, 0, 1, 1, 1, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 1, 1, 5, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 1, 2, 1, 2, 2, 3, 1, 1, 2, 1, 0, 2, 1, 1,
    1, 0, 1, 3, 1, 1, 2, 2, 0, 1, 3, 1, 0, 1, 1, 1, 1, 0, 1, 1, 3, 2, 0, 1, 3, 1, 0, 1, 2, 2, 1, 1, 1, 1, 0, 1, 5, 1, 1, 3, 0, 1, 1, 1, 0, 2, 2, 4, 2, 0,
    2, 1, 1, 0, 1, 4, 0, 1, 2, 1, 2, 1, 1, 2, 1, 1, 3, 1, 0, 1, 1, 2, 3, 0, 1, 1, 3, 1, 1, 1, 0, 1, 3, 2, 3, 1, 1, 1, 0, 2, 1, 2, 1, 4, 2, 1, 3, 0, 1, 2,
    4, 2, 0, 1, 1, 1, 0, 1, 3, 2, 4, 1, 0, 1, 1, 1, 1, 2, 2, 3, 1, 1, 0, 1, 2, 1, 0, 4, 2, 0, 1, 1, 2, 0, 1, 3, 0, 1, 2, 2, 3, 1, 0, 1, 2, 1, 1, 1, 1, 0,
    1, 1, 1, 1, 0, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 0, 3, 2, 4, 2, 1, 2, 2, 3, 3, 2, 3, 0, 1, 2, 3, 0, 3, 1, 0, 1, 1, 0, 1, 2, 1, 1, 2, 1, 2, 1, 2, 0, 1, 3,
    2, 0, 1, 2, 0, 1, 1, 4, 2, 1, 0, 1, 2, 2, 2, 3, 0, 1, 1, 0, 1, 3, 1, 5, 1, 4, 0, 1, 2, 2, 1, 0, 3, 1, 1, 1, 1, 2, 2, 0, 1, 4, 0, 2, 2, 0, 1, 1, 3, 2,
    1, 0, 1, 2, 2, 1, 2, 0, 2, 1, 2, 3, 3, 4, 4, 3, 4, 0, 1, 2, 0, 1, 3, 0, 2, 1, 0, 1, 1, 2, 2, 0, 2, 1, 0, 1, 3, 3, 3, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 2,
    1, 0, 1, 1, 1, 1, 0, 1, 2, 2, 0, 1, 2, 0, 1, 2, 2, 2, 3, 1, 0, 1, 2, 0, 1, 1, 0, 1, 2, 0, 3, 1, 2, 1, 0, 1, 1, 0, 1, 2, 0, 1, 1, 1, 2, 0, 1, 2, 1, 2,
    0, 1, 1, 2, 3, 1, 0, 1, 2, 0, 1, 2, 2, 1, 0, 2, 4, 0, 2, 1, 0, 2, 1, 2, 0, 1, 1, 2, 0, 1, 2, 0, 1, 1, 0, 1, 1, 2, 0, 1, 2, 0, 1, 2, 1, 2, 0, 3, 1, 1,
    0, 1, 1, 0, 2, 1, 0, 1, 1, 3, 1, 4, 0, 3, 1, 1, 2, 0, 1, 1, 1, 2, 2, 0, 1, 1, 3, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 3, 1, 1, 2, 1, 0, 1, 1, 1, 1, 0, 5,
    4, 0, 2, 1, 1, 1, 1, 0, 1, 1, 1, 1, 3, 1, 2, 1, 2, 0, 1, 3, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1,
    0, 3, 1, 1, 0, 1, 1, 0, 4, 1, 0, 4, 1, 0, 3, 1, 1, 0, 2, 1, 1, 1, 0, 1, 2, 0, 7, 1, 2, 2, 0, 6, 1, 1, 1, 0, 1, 1, 0, 1, 2, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    1, 2, 0, 1, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 3, 1, 0, 5, 2, 1, 0, 2, 1, 0, 4, 2, 0, 1, 1, 3, 0, 1, 2, 0, 2, 1, 0, 1, 2, 2, 1, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 1, 1, 1, 1, 2, 0, 4, 2, 0, 3, 1, 0, 2, 1, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0, 2, 1, 0, 5, 1, 1, 0, 3, 2, 0, 1, 2, 2, 0, 1, 1, 2, 0, 3, 1,
    0, 5, 2, 0, 4, 1, 1, 0, 1, 1, 0, 2, 2, 4, 3, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 0, 4, 1, 1, 3, 0, 1, 2, 2, 0, 2, 2, 0, 5, 1, 0, 1, 1, 0, 5, 1, 0, 4, 2, 1,
    0, 6, 1, 0, 2, 2, 1, 0, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 4, 2, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 5, 2, 1, 0, 7, 1, 0, 1, 2, 1,
    0, 1, 1, 1, 0, 2, 1, 1, 0, 3, 1, 2, 0, 2, 2, 0, 4, 1, 1, 0, 2, 1, 1, 0, 10, 2, 0, 2, 1, 1, 1, 0, 1, 1, 0, 3, 2, 1, 0, 1, 1, 1, 0, 2, 1, 0, 4, 4, 0, 1,
    1, 1, 2, 0, 1, 1, 0, 1, 2, 0, 15, 1, 0, 3, 3, 0, 2, 1, 2, 0, 7, 1, 2, 1, 3, 0, 6, 2, 0, 1, 1, 1, 1, 0, 2, 1, 1, 0, 1, 1, 1, 3, 0, 1, 4, 4, 10, 23, 45, 47,
    49, 58, 42, 26, 18, 6, 4, 0, 2, 1, 0, 1, 1, 0, 1, 2, 0, 1, 2, 2, 0, 3, 1, 0, 13, 1, 0, 2, 1, 1, 0, 14, 1, 0, 2, 1, 0, 4, 1, 0, 1, 1, 0, 3, 1, 0, 6, 1, 0, 3,
    1, 0, 4, 1, 1, 0, 11, 2, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 9, 1, 0, 2, 2, 0, 7, 1, 1, 1, 0, 2, 1, 3, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 14, 2, 1, 0, 2, 1, 0, 3,
    1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 15, 1, 0, 4, 1, 0, 4, 2, 0, 6, 1, 0, 2, 1, 0, 1, 1, 1, 0, 3, 1, 0, 6, 2, 1, 0, 1, 1, 0, 1, 1,
    0, 9, 1, 0, 15, 2, 0, 4, 1, 0, 3, 1, 0, 5, 1, 0, 7, 1, 0, 2, 1, 0, 4, 1, 0, 1, 1, 0, 4, 2, 1, 0, 2, 1, 0, 2, 1, 0, 7, 1, 0, 3, 1, 1, 0, 6, 1, 0, 3, 1,
    0, 3, 1, 0, 1, 2, 2, 0, 1, 2, 0, 5, 1, 1, 0, 1, 1, 1, 0, 1, 2, 2, 0, 1, 1, 1, 0, 5, 1, 0, 1, 2, 0, 1, 1, 2, 0, 3, 1, 0, 6, 1, 0, 1, 1, 0, 1, 1, 1, 0,
    6, 1, 0, 10, 1, 1, 0, 6, 1, 0, 3, 1, 0, 2, 1, 1, 0, 5, 1, 0, 10, 1, 1, 1, 0, 4, 3, 1, 0, 1, 1, 0, 4, 1, 0, 6, 1, 0, 1, 1, 0, 8, 1, 1, 0, 1, 2, 1, 0, 5,
    1, 1, 0, 3, 1, 0, 2, 1, 1, 0, 8, 1, 1, 0, 9, 1, 0, 5, 1, 0, 4, 1, 0, 6, 1, 0, 14, 1, 0, 8, 1, 0, 3, 1, 0, 11, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 0,
    8, 1, 0, 4, 1, 0, 4, 1, 0, 4, 1, 0, 5, 1, 0, 5, 2, 0, 5, 1, 0, 4, 1, 0, 2, 1, 0, 3, 1, 0, 16, 2, 0, 7, 1, 0, 11, 1, 0, 9, 1, 0, 2, 1, 0, 14, 1, 0, 4, 1,
    0, 2, 1, 1, 0, 9, 1, 0, 6, 1, 0, 5, 1, 1, 0, 6, 3, 1, 1, 3, 1, 0, 2, 1, 0, 2, 1, 0, 9, 1, 0, 3, 1, 0, 18, 1, 0, 8, 2, 0, 3, 1, 0, 6, 2, 1, 0, 2, 1, 0,
    2, 1, 0, 1, 1, 0, 16, 1, 0, 1, 1, 1, 1, 1, 4, 8, 4, 6, 7, 4, 1, 0, 2, 1, 0, 9, 1, 1, 0, 11, 1, 0, 3, 1, 0, 2, 1, 0, 4, 1, 0, 2, 1, 0, 8, 1, 0, 34, 1, 0,
    1, 1, 0, 17, 1, 0, 4, 2, 0, 2, 1, 1, 0, 4, 1, 0, 1, 1, 0, 3, 1, 0, 8, 1, 0, 6, 1, 0, 34, 1, 0, 1, 1, 0, 11, 1, 0, 17, 1, 0, 6, 1, 0, 5, 1, 2, 1, 0, 2, 1,
    0, 9, 1, 0, 3, 1, 0, 12, 1, 0, 12, 1, 1, 0, 7, 1, 0, 1, 1, 0, 7, 1, 0, 3, 2, 0, 6, 1, 0, 6, 1, 0, 5, 1, 0, 4, 1, 0, 1, 1, 0, 6, 1, 0, 6, 1, 0, 8, 1, 0,
    5, 1, 0, 7, 1, 0, 8, 1, 0, 22, 1, 0, 7, 1, 0, 2, 1, 0, 18, 1, 0, 3, 2, 1, 0, 6, 1, 0, 21, 1, 0, 9, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 23, 1, 0, 7, 1, 1,
    0, 2, 3, 0, 3, 1, 0, 4, 1, 1, 0, 17, 1, 0, 1, 1, 0, 1, 1, 0, 4, 2, 1, 0, 19, 1, 0, 17, 1, 0, 12, 1, 0, 1, 1, 0, 35, 1, 0, 3, 1, 0, 6, 1, 0, 9, 1, 0, 13, 2,
    0, 3, 1, 0, 7, 1, 0, 3, 1, 0, 5, 1, 0, 3, 1, 0, 6, 1, 0, 4, 1, 0, 13, 1, 1, 0, 4, 1, 1, 0, 1, 1, 0, 2, 1, 0, 3, 1, 0, 18, 1, 0, 19, 1, 0, 14, 1, 0, 1, 1,
    1, 0, 7, 1, 1, 0, 7, 1, 0, 6, 1, 0, 7, 2, 1, 0, 7, 1, 0, 2, 1, 0, 4, 1, 2, 0, 1, 1, 0, 10, 2, 0, 15, 1, 0, 1, 1, 0, 22, 2, 0, 11, 1, 0, 5, 1, 0, 1, 1, 0,
    4, 2, 2, 1, 0, 2, 2, 1, 2, 0, 5, 1, 0, 8, 1, 0, 40, 1, 0, 11, 1, 0, 20, 1, 0, 13, 1, 0, 4, 1, 0, 4, 1, 0, 3, 1, 0, 3, 1, 0, 8, 1, 0, 28, 1, 0, 3, 1, 1, 0,
    4, 1, 0, 7, 3, 0, 3, 1, 0, 23, 2, 0, 1, 1, 1, 0, 7, 1, 0, 8, 1, 1, 0, 4, 1, 0, 3, 1, 0, 3, 1, 0, 19, 1, 0, 3, 1, 0, 2, 1, 0, 2, 1, 2, 2, 0, 1, 2, 1, 0,
    2, 1, 0, 6, 1, 0, 7, 1, 0, 4, 1, 0, 12, 1, 0, 7, 1, 0, 6, 1, 0, 40, 1, 0, 7, 1, 0, 7, 1, 1, 0, 13, 1, 0, 4, 1, 0, 7, 1, 0, 15, 1, 0, 8, 1, 0, 9, 1, 1, 0,
    5, 1, 0, 6, 1, 0, 7, 1, 0, 1, 1, 0, 3, 1, 0, 3, 1, 0, 5, 1, 0, 15, 2, 0, 10, 1, 0, 6, 1, 0, 6, 1, 0, 5, 1, 0, 10, 1, 0, 5, 1, 0, 4, 1, 1, 1, 0, 11, 1, 1,
    0, 1, 1, 0, 2, 1, 0, 12, 1, 0, 5, 1, 0, 33, 1, 0, 4, 2, 0, 2, 2, 0, 10, 2, 0, 6, 1, 0, 8, 1, 1, 0, 7, 1, 1, 0, 6, 1, 0, 1, 1, 1, 0, 19, 1, 0, 2, 1, 0, 4,
    1, 0, 3, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 6, 1, 0, 1, 1, 0, 15, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1, 0, 5, 1, 0, 3, 1, 0, 3, 1, 1, 0, 4, 1,
    0, 8, 1, 0, 9, 1, 0, 1, 1, 1, 0, 6, 1, 0, 7, 2, 0, 7, 1, 0, 15, 1, 0, 8, 1, 0, 4, 1, 0, 30, 1, 0, 9, 1, 0, 2, 1, 1, 1, 0, 13, 1, 0, 12, 1, 0, 12, 1, 0, 3,
    1, 1, 0, 18, 1, 0, 38, 1, 0, 19, 1, 0, 19, 1, 0, 24, 1, 0, 63, 1, 0, 61, 1, 0, 24, 1, 0, 34, 1, 0, 60, 1, 0, 38, 1, 0, 12, 1, 0, 5, 1, 0, 3, 1, 1, 1, 1, 1, 4, 7,
    3, 7, 11, 5, 4, 2, 1, 1, 0, 113, 1, 0, 38, 1, 0, 118, 1, 1, 0, 196, 1, 0, 26, 1, 0, 100, 1, 0, 166, 1, 0, 26
  };
  assert( test_7_packed.size() == 6732 );
  const vector<uint8_t> test_7_encoded = QRSpecDev::encode_stream_vbyte( test_7_chan_cnts );
  assert( test_7_encoded == test_7_packed );
  vector<uint32_t> test_7_dec;
  const size_t test_7_nbytedec = QRSpecDev::decode_stream_vbyte(test_7_encoded,test_7_dec);
  assert( test_7_nbytedec == test_7_packed.size() );
  assert( test_7_dec == test_7_chan_cnts );
  
  
  
  
  // Test case 8
  const vector<uint32_t> test_8_chan_cnts{
    0, 39, 24, 50, 50, 56, 56, 52, 48, 54, 57, 46, 40, 30, 36, 37, 48, 35, 42, 39,
    42, 35, 38, 32, 44, 37, 43, 48, 43, 38, 36, 33, 38, 42, 40, 28, 37, 33, 30, 37,
    29, 53, 29, 26, 29, 35, 32, 27, 38, 33, 34, 33, 34, 38, 33, 42, 21, 36, 32, 37,
    28, 29, 39, 31, 24, 41, 42, 24, 34, 31, 35, 28, 36, 40, 40, 24, 25, 24, 41, 32,
    30, 39, 31, 28, 27, 25, 30, 29, 31, 24, 32, 41, 29, 32, 23, 25, 31, 33, 34, 32,
    32, 28, 32, 22, 34, 37, 29, 37, 38, 27, 23, 38, 34, 35, 35, 36, 28, 38, 36, 42,
    36, 40, 49, 39, 32, 45, 38, 42, 26, 35, 49, 53, 35, 39, 47, 55, 56, 54, 44, 43,
    40, 47, 61, 40, 61, 63, 67, 58, 66, 70, 68, 64, 55, 49, 47, 43, 35, 66, 63, 53,
    66, 46, 47, 61, 61, 57, 63, 59, 67, 59, 69, 67, 82, 61, 66, 68, 65, 75, 62, 62,
    55, 54, 51, 51, 68, 65, 60, 55, 54, 50, 71, 75, 54, 64, 60, 78, 64, 65, 70, 59,
    77, 69, 59, 59, 77, 73, 56, 60, 73, 71, 84, 67, 73, 67, 84, 82, 73, 63, 58, 66,
    68, 70, 75, 79, 74, 63, 81, 67, 60, 65, 82, 79, 84, 84, 79, 61, 67, 58, 67, 64,
    90, 72, 70, 83, 77, 78, 85, 91, 84, 61, 95, 58, 98, 78, 84, 67, 78, 79, 81, 91,
    80, 90, 89, 81, 73, 72, 82, 75, 87, 79, 104, 82, 86, 69, 77, 83, 97, 76, 80, 79,
    68, 80, 84, 76, 77, 65, 73, 88, 65, 76, 69, 77, 79, 76, 72, 80, 88, 52, 81, 83,
    72, 62, 74, 67, 72, 68, 78, 79, 70, 73, 80, 72, 61, 71, 93, 70, 70, 82, 67, 74,
    77, 70, 72, 84, 85, 78, 79, 80, 75, 58, 66, 75, 75, 72, 63, 61, 73, 67, 75, 87,
    73, 72, 73, 72, 63, 64, 64, 66, 63, 63, 58, 69, 77, 86, 119, 98, 92, 66, 65, 63,
    66, 73, 74, 54, 70, 62, 73, 69, 74, 65, 56, 74, 69, 67, 65, 54, 67, 67, 58, 69,
    70, 62, 58, 65, 51, 62, 68, 58, 54, 56, 60, 68, 57, 65, 49, 81, 72, 57, 70, 69,
    67, 69, 62, 67, 71, 69, 80, 83, 83, 96, 101, 75, 63, 57, 62, 68, 71, 68, 68, 61,
    70, 56, 71, 57, 46, 69, 71, 70, 64, 68, 58, 79, 66, 76, 66, 58, 52, 60, 62, 56,
    61, 56, 60, 70, 71, 73, 60, 68, 56, 52, 65, 86, 61, 72, 55, 50, 65, 64, 57, 67,
    75, 48, 75, 66, 69, 72, 104, 199, 431, 575, 601, 328, 157, 55, 36, 37, 36, 27, 27, 25,
    41, 37, 30, 26, 33, 25, 40, 29, 31, 29, 41, 36, 24, 22, 37, 41, 34, 17, 32, 31,
    28, 23, 25, 24, 20, 43, 28, 14, 21, 29, 22, 32, 26, 49, 33, 38, 36, 23, 27, 27,
    43, 51, 65, 96, 84, 55, 37, 27, 20, 23, 27, 24, 30, 20, 17, 31, 22, 23, 20, 27,
    24, 14, 24, 21, 27, 24, 17, 24, 17, 24, 29, 23, 19, 22, 28, 14, 31, 29, 22, 21,
    19, 23, 20, 14, 22, 25, 26, 31, 19, 21, 26, 18, 19, 23, 22, 26, 28, 19, 18, 16,
    20, 23, 25, 29, 22, 24, 22, 21, 21, 14, 22, 20, 16, 22, 17, 15, 14, 21, 18, 16,
    21, 25, 24, 21, 10, 21, 16, 19, 21, 12, 20, 30, 28, 53, 36, 42, 29, 25, 21, 19,
    25, 17, 27, 24, 18, 17, 18, 15, 12, 21, 21, 14, 11, 13, 14, 11, 11, 22, 21, 17,
    13, 15, 14, 20, 18, 18, 12, 9, 18, 6, 17, 18, 26, 19, 14, 13, 16, 18, 16, 24,
    20, 12, 8, 6, 18, 22, 16, 14, 13, 11, 16, 14, 3, 12, 17, 10, 10, 16, 12, 10,
    10, 18, 19, 15, 13, 12, 12, 16, 10, 8, 14, 17, 13, 7, 13, 19, 13, 13, 17, 11,
    18, 14, 9, 14, 13, 12, 13, 11, 10, 10, 9, 8, 13, 13, 16, 13, 15, 10, 20, 15,
    14, 14, 11, 16, 6, 16, 9, 17, 14, 12, 10, 13, 4, 10, 8, 11, 13, 7, 13, 14,
    13, 12, 13, 14, 14, 19, 15, 12, 11, 13, 10, 6, 13, 21, 8, 9, 8, 10, 13, 10,
    16, 7, 12, 8, 9, 5, 15, 17, 21, 21, 19, 14, 14, 10, 15, 24, 14, 16, 10, 14,
    10, 10, 13, 6, 9, 14, 13, 9, 9, 7, 8, 12, 7, 10, 5, 9, 8, 12, 11, 9,
    6, 9, 11, 8, 8, 8, 10, 7, 5, 3, 4, 12, 8, 10, 9, 10, 8, 12, 13, 9,
    13, 7, 11, 10, 8, 7, 8, 6, 8, 5, 6, 11, 5, 12, 11, 13, 16, 6, 7, 7,
    7, 4, 9, 7, 10, 6, 13, 11, 13, 11, 12, 10, 3, 12, 10, 4, 9, 11, 9, 9,
    10, 11, 7, 9, 8, 10, 8, 11, 12, 5, 6, 13, 9, 12, 11, 6, 9, 10, 9, 8,
    12, 3, 5, 7, 5, 10, 19, 11, 10, 7, 14, 8, 8, 5, 10, 4, 6, 8, 7, 11,
    6, 6, 7, 8, 4, 5, 7, 12, 5, 6, 9, 13, 9, 9, 6, 13, 7, 6, 12, 6,
    10, 11, 25, 27, 21, 21, 15, 10, 10, 11, 6, 10, 4, 6, 8, 6, 4, 1, 7, 11,
    6, 3, 5, 9, 7, 11, 9, 3, 9, 8, 8, 10, 9, 8, 10, 9, 7, 10, 4, 11,
    1, 9, 12, 10, 10, 9, 4, 8, 8, 8, 7, 4, 8, 12, 5, 9, 4, 7, 7, 9,
    7, 5, 9, 5, 9, 9, 9, 10, 6, 9, 10, 8, 11, 5, 7, 4, 4, 9, 3, 6,
    2, 5, 8, 4, 4, 9, 8, 7, 4, 6, 6, 5, 3, 4, 5, 6, 3, 8, 8, 11,
    5, 9, 5, 3, 8, 13, 5, 7, 4, 7, 5, 9, 5, 4, 9, 5, 3, 5, 6, 6,
    6, 5, 7, 6, 2, 5, 6, 4, 6, 2, 5, 6, 9, 5, 6, 7, 8, 7, 6, 6,
    6, 9, 7, 6, 4, 13, 10, 7, 5, 5, 8, 7, 3, 4, 8, 4, 5, 4, 8, 8,
    6, 5, 9, 6, 9, 1, 9, 7, 3, 4, 3, 9, 11, 7, 7, 5, 7, 5, 7, 12,
    6, 1, 7, 7, 7, 6, 4, 7, 3, 8, 3, 6, 6, 6, 7, 7, 5, 5, 5, 6,
    6, 4, 2, 2, 14, 7, 6, 6, 2, 2, 6, 4, 4, 7, 10, 5, 3, 5, 7, 8,
    6, 5, 5, 7, 6, 5, 6, 2, 6, 4, 10, 3, 5, 3, 5, 2, 3, 5, 6, 6,
    4, 5, 2, 5, 1, 5, 7, 7, 6, 7, 3, 2, 2, 3, 6, 6, 4, 6, 6, 2,
    2, 2, 11, 3, 2, 3, 3, 7, 9, 4, 8, 3, 3, 11, 5, 3, 5, 6, 3, 5,
    6, 4, 2, 2, 4, 3, 4, 3, 5, 2, 3, 6, 5, 9, 4, 3, 3, 6, 9, 2,
    2, 2, 3, 6, 3, 4, 7, 7, 2, 4, 2, 5, 3, 8, 2, 2, 7, 2, 8, 2,
    3, 6, 4, 4, 3, 8, 6, 1, 2, 3, 3, 3, 7, 3, 5, 2, 4, 4, 2, 4,
    4, 5, 4, 3, 8, 7, 5, 2, 4, 6, 3, 5, 6, 2, 3, 2, 5, 3, 3, 8,
    4, 6, 3, 4, 1, 4, 4, 4, 5, 3, 5, 1, 3, 4, 1, 7, 4, 3, 1, 5,
    4, 8, 4, 2, 3, 5, 5, 2, 5, 5, 2, 6, 4, 5, 4, 6, 2, 3, 1, 4,
    4, 4, 5, 4, 1, 4, 7, 5, 3, 1, 7, 4, 1, 6, 2, 1, 4, 3, 1, 2,
    3, 2, 2, 4, 3, 1, 1, 1, 8, 8, 5, 7, 4, 8, 8, 5, 12, 15, 6, 4,
    15, 5, 4, 2, 1, 3, 5, 3, 4, 1, 3, 7, 4, 8, 4, 2, 2, 2, 5, 3,
    3, 5, 6, 4, 4, 2, 7, 1, 3, 2, 2, 4, 3, 4, 5, 3, 3, 3, 2, 2,
    3, 2, 3, 1, 3, 3, 1, 4, 4, 3, 3, 3, 1, 6, 5, 5, 4, 3, 1, 3,
    4, 4, 0, 1, 3, 1, 1, 2, 1, 4, 1, 0, 1, 5, 2, 3, 7, 1, 3, 5,
    1, 3, 0, 1, 3, 3, 3, 6, 3, 1, 3, 2, 6, 6, 2, 1, 4, 2, 3, 5,
    3, 1, 3, 1, 2, 2, 1, 1, 5, 3, 4, 2, 5, 5, 5, 3, 5, 3, 2, 2,
    4, 3, 2, 4, 2, 4, 1, 2, 3, 3, 6, 1, 3, 6, 0, 1, 6, 3, 3, 5,
    2, 2, 4, 3, 4, 6, 9, 3, 0, 1, 3, 1, 3, 2, 1, 7, 5, 5, 4, 3,
    4, 3, 2, 5, 2, 2, 2, 5, 3, 1, 2, 0, 1, 3, 3, 2, 5, 1, 5, 3,
    2, 2, 4, 5, 4, 2, 2, 1, 3, 0, 1, 3, 2, 2, 2, 5, 2, 1, 2, 4,
    11, 15, 12, 19, 13, 4, 5, 5, 0, 1, 5, 1, 0, 1, 3, 1, 2, 4, 5, 0,
    2, 2, 1, 2, 3, 1, 5, 2, 3, 2, 5, 2, 6, 5, 2, 0, 1, 4, 2, 3,
    3, 3, 3, 1, 2, 1, 3, 2, 1, 3, 3, 1, 2, 2, 4, 1, 2, 3, 2, 0,
    1, 1, 2, 0, 1, 3, 5, 4, 4, 1, 2, 3, 1, 4, 6, 2, 6, 17, 15, 39,
    12, 14, 7, 4, 4, 2, 3, 3, 3, 1, 4, 3, 3, 2, 4, 3, 4, 3, 3, 0,
    1, 7, 3, 1, 2, 4, 4, 2, 1, 2, 1, 8, 2, 2, 3, 3, 2, 3, 3, 3,
    3, 4, 3, 2, 0, 1, 1, 2, 4, 3, 4, 1, 1, 1, 2, 5, 3, 2, 4, 3,
    2, 4, 3, 4, 2, 0, 3, 2, 4, 5, 3, 4, 1, 2, 2, 2, 3, 3, 2, 2,
    4, 4, 2, 0, 1, 3, 2, 3, 4, 0, 1, 4, 4, 2, 3, 4, 0, 1, 4, 2,
    1, 1, 2, 1, 5, 1, 0, 1, 1, 7, 1, 2, 0, 1, 1, 2, 1, 2, 4, 5,
    1, 3, 2, 2, 5, 4, 5, 3, 1, 2, 4, 1, 4, 2, 2, 3, 3, 5, 3, 2,
    1, 2, 1, 1, 3, 0, 2, 1, 3, 2, 2, 1, 2, 3, 5, 5, 1, 6, 7, 1,
    4, 2, 3, 4, 4, 10, 3, 5, 1, 1, 0, 1, 2, 1, 3, 2, 3, 2, 1, 4,
    4, 0, 1, 1, 0, 1, 1, 1, 1, 4, 1, 4, 2, 1, 1, 3, 0, 1, 2, 3,
    1, 2, 3, 0, 1, 3, 0, 1, 2, 4, 3, 1, 1, 1, 3, 2, 0, 2, 3, 2,
    0, 1, 3, 3, 2, 2, 2, 1, 1, 3, 1, 2, 1, 2, 1, 1, 3, 4, 3, 1,
    0, 2, 5, 4, 2, 2, 1, 6, 1, 0, 1, 4, 0, 1, 3, 1, 2, 0, 1, 2,
    1, 1, 3, 3, 2, 6, 4, 3, 4, 1, 0, 1, 3, 1, 1, 3, 5, 4, 1, 0,
    1, 1, 1, 1, 2, 1, 1, 0, 1, 4, 1, 5, 1, 3, 3, 3, 4, 0, 1, 3,
    3, 1, 5, 1, 5, 3, 0, 1, 2, 1, 2, 0, 1, 1, 2, 1, 1, 1, 2, 1,
    1, 1, 2, 1, 2, 1, 2, 5, 3, 1, 4, 1, 1, 4, 1, 3, 2, 1, 2, 2,
    1, 4, 5, 2, 5, 4, 2, 3, 2, 5, 2, 1, 2, 3, 4, 3, 2, 3, 1, 2,
    0, 1, 3, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 1, 5, 5, 1, 3, 2,
    2, 2, 2, 1, 4, 1, 3, 1, 2, 1, 0, 2, 1, 3, 3, 0, 1, 2, 1, 3,
    2, 3, 0, 1, 1, 1, 0, 1, 1, 0, 1, 5, 2, 2, 2, 2, 0, 1, 1, 1,
    0, 1, 1, 1, 1, 3, 1, 4, 2, 2, 2, 3, 5, 3, 0, 1, 1, 2, 3, 3,
    2, 3, 3, 3, 1, 0, 1, 1, 2, 0, 1, 1, 2, 1, 1, 4, 2, 0, 1, 2,
    0, 1, 3, 2, 3, 1, 3, 6, 1, 1, 5, 2, 3, 2, 3, 2, 3, 4, 4, 1,
    1, 4, 3, 0, 1, 5, 2, 2, 1, 1, 1, 3, 1, 4, 0, 1, 1, 3, 7, 2,
    1, 2, 1, 0, 1, 2, 4, 0, 1, 1, 2, 0, 1, 1, 3, 1, 1, 4, 2, 2,
    1, 3, 9, 3, 2, 1, 3, 1, 1, 5, 0, 2, 1, 0, 1, 1, 1, 4, 2, 0,
    1, 1, 0, 1, 1, 1, 1, 1, 2, 1, 3, 3, 2, 1, 5, 0, 1, 2, 1, 0,
    1, 5, 4, 1, 0, 1, 3, 2, 0, 1, 3, 2, 1, 1, 0, 4, 2, 3, 1, 3,
    1, 5, 2, 5, 1, 2, 0, 1, 1, 3, 0, 1, 1, 2, 1, 1, 1, 0, 1, 1,
    1, 2, 1, 1, 2, 2, 3, 3, 1, 3, 0, 1, 3, 0, 1, 2, 5, 1, 0, 1,
    1, 2, 0, 1, 4, 2, 0, 1, 2, 3, 0, 1, 3, 0, 1, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 0, 2, 1, 1, 1, 3, 4, 1, 0, 1, 2, 0, 2, 1, 5, 0,
    1, 1, 2, 2, 0, 1, 3, 0, 1, 3, 0, 1, 1, 0, 1, 2, 1, 3, 2, 1,
    0, 1, 1, 1, 1, 5, 0, 1, 1, 1, 1, 0, 1, 2, 3, 2, 1, 2, 0, 1,
    1, 3, 1, 2, 0, 1, 1, 2, 0, 1, 1, 0, 1, 1, 0, 2, 1, 2, 0, 1,
    1, 1, 3, 3, 5, 3, 1, 1, 1, 1, 2, 1, 2, 3, 0, 1, 1, 0, 1, 3,
    0, 2, 3, 1, 1, 1, 2, 2, 4, 3, 5, 0, 1, 2, 2, 3, 3, 1, 1, 5,
    4, 2, 0, 1, 3, 3, 3, 2, 3, 0, 3, 2, 0, 2, 1, 2, 2, 1, 0, 1,
    1, 2, 2, 2, 2, 0, 1, 3, 1, 1, 1, 1, 2, 1, 0, 5, 1, 1, 0, 2,
    1, 2, 1, 0, 4, 1, 3, 2, 3, 1, 1, 1, 3, 3, 0, 1, 2, 1, 1, 1,
    4, 0, 1, 4, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 5, 0, 1, 1, 5,
    4, 1, 2, 2, 0, 1, 2, 3, 2, 1, 2, 1, 2, 0, 1, 4, 0, 1, 2, 3,
    1, 0, 1, 2, 4, 0, 1, 2, 1, 1, 0, 1, 3, 1, 1, 3, 0, 1, 3, 1,
    0, 1, 2, 0, 1, 2, 2, 1, 2, 2, 5, 0, 2, 2, 2, 1, 3, 4, 0, 1,
    6, 0, 2, 5, 1, 3, 4, 1, 14, 8, 7, 4, 5, 5, 0, 1, 1, 3, 0, 1,
    2, 1, 3, 1, 2, 0, 2, 2, 2, 7, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    1, 0, 1, 2, 2, 0, 1, 4, 1, 2, 1, 1, 0, 2, 3, 1, 0, 1, 2, 1,
    0, 2, 2, 1, 0, 1, 1, 2, 0, 1, 1, 1, 3, 2, 0, 1, 4, 0, 1, 1,
    5, 3, 1, 2, 4, 4, 1, 2, 1, 1, 0, 1, 3, 2, 0, 2, 1, 4, 1, 0,
    1, 1, 2, 0, 1, 2, 0, 1, 2, 2, 2, 3, 1, 1, 3, 1, 1, 2, 1, 1,
    0, 1, 3, 1, 2, 1, 2, 4, 2, 2, 0, 1, 2, 4, 1, 0, 3, 2, 3, 1,
    1, 2, 2, 0, 3, 2, 1, 0, 1, 1, 0, 3, 1, 1, 1, 2, 1, 1, 3, 3,
    3, 0, 1, 3, 3, 1, 6, 1, 4, 3, 1, 3, 4, 3, 2, 1, 1, 2, 1, 3,
    4, 0, 1, 7, 10, 8, 4, 5, 1, 1, 1, 0, 3, 2, 0, 1, 1, 1, 0, 3,
    1, 1, 3, 3, 0, 5, 1, 1, 0, 1, 1, 1, 1, 1, 2, 2, 1, 3, 1, 0,
    1, 3, 2, 4, 3, 3, 1, 1, 2, 1, 0, 3, 1, 5, 2, 2, 0, 2, 2, 1,
    1, 3, 0, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 3, 1, 1, 2, 2, 2, 1,
    0, 1, 2, 1, 1, 1, 2, 2, 4, 1, 2, 2, 2, 3, 4, 2, 3, 1, 1, 1,
    0, 1, 1, 1, 4, 3, 2, 0, 1, 1, 5, 4, 0, 1, 1, 1, 4, 2, 0, 2,
    1, 3, 1, 0, 1, 1, 3, 1, 4, 0, 1, 1, 0, 2, 2, 3, 0, 1, 4, 1,
    0, 1, 1, 0, 1, 2, 3, 2, 4, 3, 2, 1, 1, 2, 2, 4, 2, 0, 1, 3,
    1, 1, 0, 1, 3, 2, 0, 2, 3, 0, 2, 1, 2, 2, 1, 2, 1, 0, 1, 2,
    0, 1, 1, 2, 3, 2, 2, 1, 1, 0, 2, 4, 3, 2, 3, 1, 1, 1, 1, 2,
    2, 2, 1, 0, 1, 1, 1, 0, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 2, 0,
    2, 2, 1, 0, 2, 1, 1, 1, 1, 1, 3, 1, 2, 0, 1, 1, 0, 2, 1, 2,
    3, 1, 1, 2, 1, 2, 1, 3, 2, 1, 2, 0, 1, 3, 0, 1, 2, 0, 1, 1,
    1, 4, 1, 0, 1, 2, 0, 1, 3, 1, 2, 0, 1, 4, 4, 0, 1, 1, 1, 1,
    0, 3, 2, 2, 0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 2, 4, 0, 3, 2, 1,
    1, 0, 1, 2, 0, 2, 2, 0, 2, 3, 0, 1, 2, 0, 1, 2, 1, 0, 1, 1,
    2, 1, 0, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 2, 2, 0, 1,
    1, 2, 2, 4, 3, 0, 2, 1, 1, 0, 1, 3, 0, 1, 2, 2, 1, 0, 1, 2,
    0, 1, 2, 3, 2, 4, 1, 4, 1, 2, 0, 1, 2, 0, 2, 1, 1, 0, 2, 3,
    0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 5, 1, 1, 0, 2, 4, 0, 1,
    3, 1, 0, 1, 2, 1, 2, 1, 0, 1, 1, 1, 0, 2, 1, 2, 3, 3, 1, 0,
    1, 3, 1, 0, 1, 4, 0, 1, 3, 2, 0, 1, 3, 3, 0, 2, 3, 0, 2, 2,
    0, 1, 2, 2, 1, 1, 0, 1, 1, 4, 0, 1, 1, 2, 3, 2, 1, 0, 1, 2,
    0, 1, 2, 1, 3, 9, 10, 7, 6, 1, 2, 1, 2, 1, 0, 1, 1, 1, 1, 2,
    0, 2, 1, 0, 3, 2, 2, 0, 4, 2, 1, 0, 1, 2, 1, 0, 3, 1, 2, 0,
    1, 2, 0, 2, 1, 4, 0, 2, 1, 0, 2, 3, 1, 1, 3, 3, 1, 3, 0, 1,
    5, 1, 1, 0, 1, 2, 1, 2, 1, 0, 1, 1, 2, 3, 0, 2, 1, 2, 2, 2,
    0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 3, 1, 3, 1, 0, 2, 1, 2, 1, 0,
    1, 1, 0, 1, 1, 3, 3, 2, 1, 0, 1, 4, 2, 2, 1, 4, 1, 2, 1, 1,
    1, 0, 1, 1, 1, 2, 2, 2, 1, 0, 2, 1, 1, 2, 0, 2, 3, 2, 0, 1,
    1, 1, 1, 1, 0, 1, 2, 1, 3, 2, 2, 2, 0, 1, 4, 2, 0, 1, 2, 1,
    3, 1, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 1, 0, 3, 1, 2, 1, 2, 2,
    1, 0, 1, 2, 2, 1, 1, 0, 2, 2, 4, 2, 0, 1, 2, 1, 2, 1, 1, 0,
    1, 2, 1, 1, 0, 1, 1, 3, 0, 1, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0,
    1, 2, 0, 1, 3, 1, 2, 3, 2, 1, 2, 0, 2, 1, 1, 1, 0, 3, 1, 3,
    0, 1, 2, 1, 1, 0, 2, 3, 1, 2, 2, 2, 2, 0, 1, 2, 1, 1, 2, 1,
    2, 1, 0, 1, 3, 2, 1, 1, 1, 2, 0, 2, 4, 1, 1, 2, 2, 0, 1, 1,
    2, 0, 3, 1, 3, 1, 1, 0, 1, 1, 0, 2, 2, 4, 2, 0, 2, 1, 3, 3,
    3, 2, 0, 2, 1, 1, 2, 2, 3, 1, 1, 1, 2, 0, 1, 1, 0, 1, 1, 2,
    0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 2, 3, 7, 0,
    1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 3, 2,
    1, 2, 2, 5, 0, 2, 1, 1, 1, 2, 3, 1, 0, 2, 2, 3, 0, 1, 1, 0,
    1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 2, 1, 2, 3, 2, 0, 2, 1,
    2, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 5, 1, 1, 1, 0, 2, 1, 0,
    4, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 1, 3, 3, 1, 0, 1, 2, 1,
    0, 2, 1, 0, 3, 1, 1, 1, 1, 1, 3, 0, 1, 1, 1, 1, 0, 3, 1, 0,
    8, 3, 0, 2, 1, 0, 1, 1, 0, 1, 2, 1, 1, 1, 1, 2, 1, 3, 1, 1,
    2, 1, 0, 1, 1, 1, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1,
    1, 0, 5, 1, 0, 2, 4, 0, 1, 1, 1, 2, 0, 1, 1, 0, 2, 1, 0, 1,
    2, 1, 0, 2, 2, 1, 1, 0, 1, 2, 0, 1, 2, 0, 4, 2, 0, 2, 1, 1,
    1, 2, 1, 4, 2, 0, 1, 1, 1, 1, 1, 1, 0, 4, 1, 1, 0, 3, 2, 0,
    1, 1, 1, 0, 3, 1, 0, 2, 3, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2,
    0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 0, 1, 2, 1, 1, 0, 1, 2, 3, 1,
    1, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 3, 0, 1, 3, 0, 3, 2, 1,
    0, 1, 2, 0, 1, 1, 0, 4, 2, 0, 1, 2, 0, 1, 1, 1, 0, 1, 3, 2,
    2, 3, 0, 4, 1, 1, 1, 3, 0, 6, 1, 0, 2, 2, 1, 1, 0, 3, 1, 0,
    3, 1, 0, 2, 1, 0, 3, 1, 0, 2, 1, 2, 1, 2, 0, 1, 1, 1, 1, 0,
    2, 1, 1, 2, 0, 2, 1, 0, 5, 2, 1, 0, 1, 1, 1, 0, 2, 2, 0, 1,
    1, 0, 1, 1, 0, 2, 1, 0, 4, 2, 0, 6, 1, 0, 2, 1, 1, 0, 1, 3,
    0, 1, 1, 1, 0, 1, 2, 0, 5, 1, 0, 1, 1, 2, 0, 2, 1, 0, 1, 1,
    1, 2, 2, 1, 3, 1, 3, 0, 1, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 1,
    1, 0, 4, 1, 1, 0, 1, 1, 1, 2, 1, 0, 1, 1, 0, 5, 1, 0, 1, 1,
    0, 3, 2, 0, 2, 1, 0, 3, 1, 0, 2, 1, 0, 4, 2, 0, 3, 1, 0, 3,
    1, 1, 1, 2, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 0, 3,
    1, 0, 2, 2, 2, 0, 1, 1, 2, 0, 5, 1, 0, 3, 1, 0, 1, 1, 1, 0,
    2, 1, 1, 2, 1, 0, 3, 1, 1, 0, 2, 1, 0, 3, 1, 0, 2, 3, 0, 1,
    1, 1, 0, 1, 1, 1, 0, 4, 1, 0, 2, 2, 0, 1, 2, 0, 2, 1, 0, 3,
    1, 1, 0, 2, 1, 0, 1, 1, 1, 2, 0, 6, 1, 1, 1, 0, 9, 1, 1, 0,
    1, 2, 0, 2, 1, 2, 1, 0, 1, 1, 2, 0, 2, 1, 0, 2, 1, 0, 3, 1,
    2, 0, 4, 2, 0, 2, 2, 0, 1, 1, 0, 1, 3, 1, 0, 1, 1, 0, 2, 1,
    1, 0, 1, 1, 3, 0, 2, 2, 0, 1, 1, 2, 11, 18, 26, 35, 54, 53, 69, 44,
    27, 12, 12, 3, 0, 5, 1, 0, 4, 1, 1, 0, 1, 1, 0, 4, 1, 0, 2, 1,
    0, 1, 1, 0, 6, 1, 1, 0, 2, 2, 1, 0, 5, 1, 0, 4, 1, 0, 8, 1,
    0, 2, 1, 0, 8, 2, 0, 1, 1, 2, 0, 2, 1, 1, 0, 4, 1, 0, 2, 2,
    0, 4, 1, 0, 10, 1, 0, 8, 1, 0, 6, 1, 0, 5, 1, 2, 0, 3, 1, 2,
    2, 2, 0, 4, 1, 2, 0, 1, 2, 1, 0, 2, 1, 1, 2, 0, 4, 1, 1, 0,
    7, 1, 0, 4, 1, 0, 1, 1, 0, 9, 1, 0, 1, 1, 1, 0, 4, 1, 2, 0,
    1, 2, 0, 7, 1, 0, 2, 1, 0, 3, 1, 1, 0, 4, 1, 1, 0, 8, 1, 1,
    0, 11, 2, 0, 6, 1, 0, 2, 1, 0, 6, 1, 0, 1, 2, 1, 0, 3, 2, 0,
    6, 2, 1, 0, 1, 1, 0, 14, 2, 0, 12, 1, 0, 6, 1, 0, 2, 1, 0, 3,
    1, 0, 2, 2, 0, 3, 1, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 1, 0,
    5, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 3, 1, 1, 2, 2, 0, 1,
    1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 2, 1, 0, 2, 2, 0, 1, 1, 0, 2,
    1, 1, 0, 6, 2, 1, 0, 3, 1, 0, 2, 1, 1, 0, 1, 1, 0, 8, 1, 0,
    1, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 1, 0, 4, 1, 0, 7,
    1, 0, 7, 1, 0, 3, 1, 0, 4, 1, 0, 4, 1, 0, 5, 1, 1, 0, 1, 1,
    1, 0, 1, 1, 1, 1, 1, 0, 15, 1, 0, 3, 2, 2, 0, 1, 1, 0, 4, 1,
    0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 4, 1,
    0, 1, 1, 0, 10, 1, 1, 1, 1, 0, 3, 1, 1, 0, 7, 1, 0, 1, 1, 0,
    4, 1, 0, 7, 1, 0, 7, 1, 0, 3, 1, 1, 0, 2, 2, 0, 1, 2, 0, 21,
    1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 10, 1, 0, 9, 1, 0, 2, 1,
    0, 11, 1, 0, 2, 2, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 9, 1, 0, 5,
    1, 0, 11, 1, 0, 9, 1, 0, 22, 2, 0, 1, 1, 0, 8, 1, 1, 2, 1, 0,
    4, 1, 1, 2, 0, 1, 1, 0, 19, 1, 1, 0, 26, 1, 1, 1, 0, 10, 1, 1,
    0, 8, 1, 0, 1, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 1, 2, 5, 6, 7,
    8, 2, 5, 3, 0, 1, 1, 1, 0, 2, 1, 0, 2, 1, 0, 14, 1, 0, 3, 1,
    0, 12, 1, 0, 1, 1, 0, 1, 1, 0, 16, 1, 0, 15, 1, 0, 7, 1, 0, 11,
    1, 0, 1, 1, 1, 1, 0, 17, 2, 0, 18, 1, 0, 1, 1, 0, 16, 1, 0, 3,
    1, 1, 0, 2, 1, 0, 3, 1, 0, 12, 1, 0, 22, 1, 0, 8, 1, 0, 4, 2,
    1, 0, 8, 1, 0, 13, 1, 0, 4, 1, 0, 15, 1, 0, 1, 1, 0, 2, 1, 1,
    0, 1, 1, 0, 12, 1, 0, 15, 1, 0, 15, 1, 0, 2, 1, 0, 19, 1, 0, 2,
    1, 0, 2, 1, 0, 3, 2, 1, 0, 1, 1, 0, 4, 1, 0, 9, 1, 0, 2, 1,
    0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 10, 1, 0, 36, 1, 0, 2, 1, 0, 3,
    1, 0, 1, 1, 0, 13, 1, 0, 11, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0,
    2, 1, 0, 10, 1, 0, 2, 1, 0, 13, 1, 0, 5, 2, 1, 1, 0, 4, 1, 0,
    6, 2, 0, 6, 1, 0, 17, 1, 0, 14, 1, 0, 6, 1, 0, 6, 1, 0, 4, 1,
    0, 21, 1, 0, 38, 1, 0, 3, 1, 0, 2, 2, 1, 0, 9, 1, 0, 2, 1, 0,
    1, 1, 0, 1, 1, 0, 3, 1, 0, 5, 1, 0, 8, 1, 0, 2, 1, 0, 16, 1,
    0, 5, 1, 0, 5, 1, 0, 1, 1, 0, 11, 1, 0, 2, 1, 0, 6, 1, 0, 3,
    1, 0, 2, 1, 0, 4, 1, 0, 7, 1, 0, 9, 1, 0, 9, 1, 0, 3, 1, 1,
    0, 8, 1, 0, 3, 1, 0, 7, 1, 0, 9, 2, 0, 20, 1, 0, 1, 1, 0, 1,
    1, 0, 3, 1, 0, 3, 1, 0, 24, 1, 0, 7, 1, 0, 4, 1, 1, 1, 0, 10,
    1, 0, 6, 1, 0, 3, 1, 2, 2, 2, 1, 2, 0, 1, 2, 0, 8, 1, 0, 9,
    2, 0, 6, 1, 0, 8, 1, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 0, 11, 2,
    0, 5, 1, 0, 6, 2, 0, 23, 1, 0, 5, 1, 1, 0, 3, 1, 0, 8, 1, 1,
    0, 7, 1, 0, 1, 1, 1, 0, 1, 1, 0, 18, 2, 1, 0, 3, 1, 0, 5, 1,
    0, 2, 1, 2, 0, 2, 1, 0, 5, 1, 0, 1, 1, 1, 1, 0, 7, 1, 1, 2,
    1, 0, 5, 1, 0, 6, 1, 0, 6, 1, 0, 2, 1, 0, 15, 1, 0, 11, 1, 0,
    7, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 1,
    2, 0, 3, 3, 2, 2, 2, 1, 1, 0, 1, 1, 0, 1, 1, 0, 10, 1, 0, 17,
    1, 0, 9, 1, 0, 4, 2, 0, 7, 1, 0, 4, 1, 0, 4, 1, 0, 26, 1, 0,
    16, 1, 0, 4, 1, 0, 10, 1, 0, 34, 2, 0, 24, 2, 1, 0, 5, 1, 0, 17,
    1, 0, 4, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 6, 2, 0, 3, 1, 1, 0,
    5, 1, 0, 14, 1, 1, 0, 3, 1, 0, 7, 1, 0, 5, 2, 0, 8, 1, 1, 0,
    10, 1, 1, 0, 8, 1, 0, 16, 1, 1, 0, 8, 1, 0, 2, 1, 0, 29, 1, 0,
    7, 2, 0, 9, 1, 0, 5, 1, 0, 11, 1, 0, 9, 1, 0, 4, 1, 0, 15, 1,
    0, 10, 1, 0, 3, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 0, 5, 2, 0, 10,
    1, 0, 17, 1, 0, 9, 1, 0, 4, 2, 0, 1, 1, 0, 6, 1, 0, 11, 1, 0,
    3, 1, 0, 18, 1, 0, 3, 1, 0, 27, 1, 0, 6, 2, 0, 34, 1, 0, 2, 1,
    0, 9, 1, 0, 13, 1, 0, 6, 3, 0, 1, 1, 0, 13, 1, 0, 7, 1, 0, 3,
    1, 0, 16, 1, 0, 18, 1, 0, 2, 1, 1, 0, 24, 1, 0, 6, 1, 0, 6, 1,
    0, 12, 1, 0, 46, 1, 0, 1, 1, 0, 12, 1, 0, 37, 1, 0, 9, 1, 0, 4,
    1, 1, 0, 26, 1, 0, 27, 1, 0, 23, 1, 0, 19, 1, 0, 28, 1, 0, 7, 1,
    0, 73, 1, 0, 2, 1, 0, 1, 2, 2, 10, 4, 3, 6, 9, 5, 0, 1, 1, 2,
    1, 0, 40, 1, 0, 17, 1, 0, 7, 1, 0, 1, 1, 0, 72, 1, 0, 49, 1, 0,
    10, 1, 0, 11, 1, 0, 95, 1, 0, 87, 1, 0, 1, 1, 0, 71, 1, 0, 60, 1,
    0, 6, 1, 0, 123, 1, 0, 11, 1, 0, 121, 1, 0, 248  };
  assert( test_8_chan_cnts.size() == 5454 );
  const vector<uint8_t> test_8_packed{
    78, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 39, 24, 50, 50, 56, 56, 52, 48, 54, 57, 46, 40, 30, 36, 37, 48, 35, 42, 39, 42, 35, 38, 32, 44, 37, 43, 48, 43, 38, 36, 33, 38, 42,
    40, 28, 37, 33, 30, 37, 29, 53, 29, 26, 29, 35, 32, 27, 38, 33, 34, 33, 34, 38, 33, 42, 21, 36, 32, 37, 28, 29, 39, 31, 24, 41, 42, 24, 34, 31, 35, 28, 36, 40, 40, 24, 25, 24, 41, 32, 30, 39, 31, 28,
    27, 25, 30, 29, 31, 24, 32, 41, 29, 32, 23, 25, 31, 33, 34, 32, 32, 28, 32, 22, 34, 37, 29, 37, 38, 27, 23, 38, 34, 35, 35, 36, 28, 38, 36, 42, 36, 40, 49, 39, 32, 45, 38, 42, 26, 35, 49, 53, 35, 39,
    47, 55, 56, 54, 44, 43, 40, 47, 61, 40, 61, 63, 67, 58, 66, 70, 68, 64, 55, 49, 47, 43, 35, 66, 63, 53, 66, 46, 47, 61, 61, 57, 63, 59, 67, 59, 69, 67, 82, 61, 66, 68, 65, 75, 62, 62, 55, 54, 51, 51,
    68, 65, 60, 55, 54, 50, 71, 75, 54, 64, 60, 78, 64, 65, 70, 59, 77, 69, 59, 59, 77, 73, 56, 60, 73, 71, 84, 67, 73, 67, 84, 82, 73, 63, 58, 66, 68, 70, 75, 79, 74, 63, 81, 67, 60, 65, 82, 79, 84, 84,
    79, 61, 67, 58, 67, 64, 90, 72, 70, 83, 77, 78, 85, 91, 84, 61, 95, 58, 98, 78, 84, 67, 78, 79, 81, 91, 80, 90, 89, 81, 73, 72, 82, 75, 87, 79, 104, 82, 86, 69, 77, 83, 97, 76, 80, 79, 68, 80, 84, 76,
    77, 65, 73, 88, 65, 76, 69, 77, 79, 76, 72, 80, 88, 52, 81, 83, 72, 62, 74, 67, 72, 68, 78, 79, 70, 73, 80, 72, 61, 71, 93, 70, 70, 82, 67, 74, 77, 70, 72, 84, 85, 78, 79, 80, 75, 58, 66, 75, 75, 72,
    63, 61, 73, 67, 75, 87, 73, 72, 73, 72, 63, 64, 64, 66, 63, 63, 58, 69, 77, 86, 119, 98, 92, 66, 65, 63, 66, 73, 74, 54, 70, 62, 73, 69, 74, 65, 56, 74, 69, 67, 65, 54, 67, 67, 58, 69, 70, 62, 58, 65,
    51, 62, 68, 58, 54, 56, 60, 68, 57, 65, 49, 81, 72, 57, 70, 69, 67, 69, 62, 67, 71, 69, 80, 83, 83, 96, 101, 75, 63, 57, 62, 68, 71, 68, 68, 61, 70, 56, 71, 57, 46, 69, 71, 70, 64, 68, 58, 79, 66, 76,
    66, 58, 52, 60, 62, 56, 61, 56, 60, 70, 71, 73, 60, 68, 56, 52, 65, 86, 61, 72, 55, 50, 65, 64, 57, 67, 75, 48, 75, 66, 69, 72, 104, 199, 175, 1, 63, 2, 89, 2, 72, 1, 157, 55, 36, 37, 36, 27, 27, 25,
    41, 37, 30, 26, 33, 25, 40, 29, 31, 29, 41, 36, 24, 22, 37, 41, 34, 17, 32, 31, 28, 23, 25, 24, 20, 43, 28, 14, 21, 29, 22, 32, 26, 49, 33, 38, 36, 23, 27, 27, 43, 51, 65, 96, 84, 55, 37, 27, 20, 23,
    27, 24, 30, 20, 17, 31, 22, 23, 20, 27, 24, 14, 24, 21, 27, 24, 17, 24, 17, 24, 29, 23, 19, 22, 28, 14, 31, 29, 22, 21, 19, 23, 20, 14, 22, 25, 26, 31, 19, 21, 26, 18, 19, 23, 22, 26, 28, 19, 18, 16,
    20, 23, 25, 29, 22, 24, 22, 21, 21, 14, 22, 20, 16, 22, 17, 15, 14, 21, 18, 16, 21, 25, 24, 21, 10, 21, 16, 19, 21, 12, 20, 30, 28, 53, 36, 42, 29, 25, 21, 19, 25, 17, 27, 24, 18, 17, 18, 15, 12, 21,
    21, 14, 11, 13, 14, 11, 11, 22, 21, 17, 13, 15, 14, 20, 18, 18, 12, 9, 18, 6, 17, 18, 26, 19, 14, 13, 16, 18, 16, 24, 20, 12, 8, 6, 18, 22, 16, 14, 13, 11, 16, 14, 3, 12, 17, 10, 10, 16, 12, 10,
    10, 18, 19, 15, 13, 12, 12, 16, 10, 8, 14, 17, 13, 7, 13, 19, 13, 13, 17, 11, 18, 14, 9, 14, 13, 12, 13, 11, 10, 10, 9, 8, 13, 13, 16, 13, 15, 10, 20, 15, 14, 14, 11, 16, 6, 16, 9, 17, 14, 12,
    10, 13, 4, 10, 8, 11, 13, 7, 13, 14, 13, 12, 13, 14, 14, 19, 15, 12, 11, 13, 10, 6, 13, 21, 8, 9, 8, 10, 13, 10, 16, 7, 12, 8, 9, 5, 15, 17, 21, 21, 19, 14, 14, 10, 15, 24, 14, 16, 10, 14,
    10, 10, 13, 6, 9, 14, 13, 9, 9, 7, 8, 12, 7, 10, 5, 9, 8, 12, 11, 9, 6, 9, 11, 8, 8, 8, 10, 7, 5, 3, 4, 12, 8, 10, 9, 10, 8, 12, 13, 9, 13, 7, 11, 10, 8, 7, 8, 6, 8, 5,
    6, 11, 5, 12, 11, 13, 16, 6, 7, 7, 7, 4, 9, 7, 10, 6, 13, 11, 13, 11, 12, 10, 3, 12, 10, 4, 9, 11, 9, 9, 10, 11, 7, 9, 8, 10, 8, 11, 12, 5, 6, 13, 9, 12, 11, 6, 9, 10, 9, 8,
    12, 3, 5, 7, 5, 10, 19, 11, 10, 7, 14, 8, 8, 5, 10, 4, 6, 8, 7, 11, 6, 6, 7, 8, 4, 5, 7, 12, 5, 6, 9, 13, 9, 9, 6, 13, 7, 6, 12, 6, 10, 11, 25, 27, 21, 21, 15, 10, 10, 11,
    6, 10, 4, 6, 8, 6, 4, 1, 7, 11, 6, 3, 5, 9, 7, 11, 9, 3, 9, 8, 8, 10, 9, 8, 10, 9, 7, 10, 4, 11, 1, 9, 12, 10, 10, 9, 4, 8, 8, 8, 7, 4, 8, 12, 5, 9, 4, 7, 7, 9,
    7, 5, 9, 5, 9, 9, 9, 10, 6, 9, 10, 8, 11, 5, 7, 4, 4, 9, 3, 6, 2, 5, 8, 4, 4, 9, 8, 7, 4, 6, 6, 5, 3, 4, 5, 6, 3, 8, 8, 11, 5, 9, 5, 3, 8, 13, 5, 7, 4, 7,
    5, 9, 5, 4, 9, 5, 3, 5, 6, 6, 6, 5, 7, 6, 2, 5, 6, 4, 6, 2, 5, 6, 9, 5, 6, 7, 8, 7, 6, 6, 6, 9, 7, 6, 4, 13, 10, 7, 5, 5, 8, 7, 3, 4, 8, 4, 5, 4, 8, 8,
    6, 5, 9, 6, 9, 1, 9, 7, 3, 4, 3, 9, 11, 7, 7, 5, 7, 5, 7, 12, 6, 1, 7, 7, 7, 6, 4, 7, 3, 8, 3, 6, 6, 6, 7, 7, 5, 5, 5, 6, 6, 4, 2, 2, 14, 7, 6, 6, 2, 2,
    6, 4, 4, 7, 10, 5, 3, 5, 7, 8, 6, 5, 5, 7, 6, 5, 6, 2, 6, 4, 10, 3, 5, 3, 5, 2, 3, 5, 6, 6, 4, 5, 2, 5, 1, 5, 7, 7, 6, 7, 3, 2, 2, 3, 6, 6, 4, 6, 6, 2,
    2, 2, 11, 3, 2, 3, 3, 7, 9, 4, 8, 3, 3, 11, 5, 3, 5, 6, 3, 5, 6, 4, 2, 2, 4, 3, 4, 3, 5, 2, 3, 6, 5, 9, 4, 3, 3, 6, 9, 2, 2, 2, 3, 6, 3, 4, 7, 7, 2, 4,
    2, 5, 3, 8, 2, 2, 7, 2, 8, 2, 3, 6, 4, 4, 3, 8, 6, 1, 2, 3, 3, 3, 7, 3, 5, 2, 4, 4, 2, 4, 4, 5, 4, 3, 8, 7, 5, 2, 4, 6, 3, 5, 6, 2, 3, 2, 5, 3, 3, 8,
    4, 6, 3, 4, 1, 4, 4, 4, 5, 3, 5, 1, 3, 4, 1, 7, 4, 3, 1, 5, 4, 8, 4, 2, 3, 5, 5, 2, 5, 5, 2, 6, 4, 5, 4, 6, 2, 3, 1, 4, 4, 4, 5, 4, 1, 4, 7, 5, 3, 1,
    7, 4, 1, 6, 2, 1, 4, 3, 1, 2, 3, 2, 2, 4, 3, 1, 1, 1, 8, 8, 5, 7, 4, 8, 8, 5, 12, 15, 6, 4, 15, 5, 4, 2, 1, 3, 5, 3, 4, 1, 3, 7, 4, 8, 4, 2, 2, 2, 5, 3,
    3, 5, 6, 4, 4, 2, 7, 1, 3, 2, 2, 4, 3, 4, 5, 3, 3, 3, 2, 2, 3, 2, 3, 1, 3, 3, 1, 4, 4, 3, 3, 3, 1, 6, 5, 5, 4, 3, 1, 3, 4, 4, 0, 1, 3, 1, 1, 2, 1, 4,
    1, 0, 1, 5, 2, 3, 7, 1, 3, 5, 1, 3, 0, 1, 3, 3, 3, 6, 3, 1, 3, 2, 6, 6, 2, 1, 4, 2, 3, 5, 3, 1, 3, 1, 2, 2, 1, 1, 5, 3, 4, 2, 5, 5, 5, 3, 5, 3, 2, 2,
    4, 3, 2, 4, 2, 4, 1, 2, 3, 3, 6, 1, 3, 6, 0, 1, 6, 3, 3, 5, 2, 2, 4, 3, 4, 6, 9, 3, 0, 1, 3, 1, 3, 2, 1, 7, 5, 5, 4, 3, 4, 3, 2, 5, 2, 2, 2, 5, 3, 1,
    2, 0, 1, 3, 3, 2, 5, 1, 5, 3, 2, 2, 4, 5, 4, 2, 2, 1, 3, 0, 1, 3, 2, 2, 2, 5, 2, 1, 2, 4, 11, 15, 12, 19, 13, 4, 5, 5, 0, 1, 5, 1, 0, 1, 3, 1, 2, 4, 5, 0,
    2, 2, 1, 2, 3, 1, 5, 2, 3, 2, 5, 2, 6, 5, 2, 0, 1, 4, 2, 3, 3, 3, 3, 1, 2, 1, 3, 2, 1, 3, 3, 1, 2, 2, 4, 1, 2, 3, 2, 0, 1, 1, 2, 0, 1, 3, 5, 4, 4, 1,
    2, 3, 1, 4, 6, 2, 6, 17, 15, 39, 12, 14, 7, 4, 4, 2, 3, 3, 3, 1, 4, 3, 3, 2, 4, 3, 4, 3, 3, 0, 1, 7, 3, 1, 2, 4, 4, 2, 1, 2, 1, 8, 2, 2, 3, 3, 2, 3, 3, 3,
    3, 4, 3, 2, 0, 1, 1, 2, 4, 3, 4, 1, 1, 1, 2, 5, 3, 2, 4, 3, 2, 4, 3, 4, 2, 0, 3, 2, 4, 5, 3, 4, 1, 2, 2, 2, 3, 3, 2, 2, 4, 4, 2, 0, 1, 3, 2, 3, 4, 0,
    1, 4, 4, 2, 3, 4, 0, 1, 4, 2, 1, 1, 2, 1, 5, 1, 0, 1, 1, 7, 1, 2, 0, 1, 1, 2, 1, 2, 4, 5, 1, 3, 2, 2, 5, 4, 5, 3, 1, 2, 4, 1, 4, 2, 2, 3, 3, 5, 3, 2,
    1, 2, 1, 1, 3, 0, 2, 1, 3, 2, 2, 1, 2, 3, 5, 5, 1, 6, 7, 1, 4, 2, 3, 4, 4, 10, 3, 5, 1, 1, 0, 1, 2, 1, 3, 2, 3, 2, 1, 4, 4, 0, 1, 1, 0, 1, 1, 1, 1, 4,
    1, 4, 2, 1, 1, 3, 0, 1, 2, 3, 1, 2, 3, 0, 1, 3, 0, 1, 2, 4, 3, 1, 1, 1, 3, 2, 0, 2, 3, 2, 0, 1, 3, 3, 2, 2, 2, 1, 1, 3, 1, 2, 1, 2, 1, 1, 3, 4, 3, 1,
    0, 2, 5, 4, 2, 2, 1, 6, 1, 0, 1, 4, 0, 1, 3, 1, 2, 0, 1, 2, 1, 1, 3, 3, 2, 6, 4, 3, 4, 1, 0, 1, 3, 1, 1, 3, 5, 4, 1, 0, 1, 1, 1, 1, 2, 1, 1, 0, 1, 4,
    1, 5, 1, 3, 3, 3, 4, 0, 1, 3, 3, 1, 5, 1, 5, 3, 0, 1, 2, 1, 2, 0, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 2, 1, 2, 1, 2, 5, 3, 1, 4, 1, 1, 4, 1, 3, 2, 1, 2, 2,
    1, 4, 5, 2, 5, 4, 2, 3, 2, 5, 2, 1, 2, 3, 4, 3, 2, 3, 1, 2, 0, 1, 3, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 1, 5, 5, 1, 3, 2, 2, 2, 2, 1, 4, 1, 3, 1, 2, 1,
    0, 2, 1, 3, 3, 0, 1, 2, 1, 3, 2, 3, 0, 1, 1, 1, 0, 1, 1, 0, 1, 5, 2, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 1, 3, 1, 4, 2, 2, 2, 3, 5, 3, 0, 1, 1, 2, 3, 3,
    2, 3, 3, 3, 1, 0, 1, 1, 2, 0, 1, 1, 2, 1, 1, 4, 2, 0, 1, 2, 0, 1, 3, 2, 3, 1, 3, 6, 1, 1, 5, 2, 3, 2, 3, 2, 3, 4, 4, 1, 1, 4, 3, 0, 1, 5, 2, 2, 1, 1,
    1, 3, 1, 4, 0, 1, 1, 3, 7, 2, 1, 2, 1, 0, 1, 2, 4, 0, 1, 1, 2, 0, 1, 1, 3, 1, 1, 4, 2, 2, 1, 3, 9, 3, 2, 1, 3, 1, 1, 5, 0, 2, 1, 0, 1, 1, 1, 4, 2, 0,
    1, 1, 0, 1, 1, 1, 1, 1, 2, 1, 3, 3, 2, 1, 5, 0, 1, 2, 1, 0, 1, 5, 4, 1, 0, 1, 3, 2, 0, 1, 3, 2, 1, 1, 0, 4, 2, 3, 1, 3, 1, 5, 2, 5, 1, 2, 0, 1, 1, 3,
    0, 1, 1, 2, 1, 1, 1, 0, 1, 1, 1, 2, 1, 1, 2, 2, 3, 3, 1, 3, 0, 1, 3, 0, 1, 2, 5, 1, 0, 1, 1, 2, 0, 1, 4, 2, 0, 1, 2, 3, 0, 1, 3, 0, 1, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 0, 2, 1, 1, 1, 3, 4, 1, 0, 1, 2, 0, 2, 1, 5, 0, 1, 1, 2, 2, 0, 1, 3, 0, 1, 3, 0, 1, 1, 0, 1, 2, 1, 3, 2, 1, 0, 1, 1, 1, 1, 5, 0, 1, 1, 1,
    1, 0, 1, 2, 3, 2, 1, 2, 0, 1, 1, 3, 1, 2, 0, 1, 1, 2, 0, 1, 1, 0, 1, 1, 0, 2, 1, 2, 0, 1, 1, 1, 3, 3, 5, 3, 1, 1, 1, 1, 2, 1, 2, 3, 0, 1, 1, 0, 1, 3,
    0, 2, 3, 1, 1, 1, 2, 2, 4, 3, 5, 0, 1, 2, 2, 3, 3, 1, 1, 5, 4, 2, 0, 1, 3, 3, 3, 2, 3, 0, 3, 2, 0, 2, 1, 2, 2, 1, 0, 1, 1, 2, 2, 2, 2, 0, 1, 3, 1, 1,
    1, 1, 2, 1, 0, 5, 1, 1, 0, 2, 1, 2, 1, 0, 4, 1, 3, 2, 3, 1, 1, 1, 3, 3, 0, 1, 2, 1, 1, 1, 4, 0, 1, 4, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 5, 0, 1, 1, 5,
    4, 1, 2, 2, 0, 1, 2, 3, 2, 1, 2, 1, 2, 0, 1, 4, 0, 1, 2, 3, 1, 0, 1, 2, 4, 0, 1, 2, 1, 1, 0, 1, 3, 1, 1, 3, 0, 1, 3, 1, 0, 1, 2, 0, 1, 2, 2, 1, 2, 2,
    5, 0, 2, 2, 2, 1, 3, 4, 0, 1, 6, 0, 2, 5, 1, 3, 4, 1, 14, 8, 7, 4, 5, 5, 0, 1, 1, 3, 0, 1, 2, 1, 3, 1, 2, 0, 2, 2, 2, 7, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    1, 0, 1, 2, 2, 0, 1, 4, 1, 2, 1, 1, 0, 2, 3, 1, 0, 1, 2, 1, 0, 2, 2, 1, 0, 1, 1, 2, 0, 1, 1, 1, 3, 2, 0, 1, 4, 0, 1, 1, 5, 3, 1, 2, 4, 4, 1, 2, 1, 1,
    0, 1, 3, 2, 0, 2, 1, 4, 1, 0, 1, 1, 2, 0, 1, 2, 0, 1, 2, 2, 2, 3, 1, 1, 3, 1, 1, 2, 1, 1, 0, 1, 3, 1, 2, 1, 2, 4, 2, 2, 0, 1, 2, 4, 1, 0, 3, 2, 3, 1,
    1, 2, 2, 0, 3, 2, 1, 0, 1, 1, 0, 3, 1, 1, 1, 2, 1, 1, 3, 3, 3, 0, 1, 3, 3, 1, 6, 1, 4, 3, 1, 3, 4, 3, 2, 1, 1, 2, 1, 3, 4, 0, 1, 7, 10, 8, 4, 5, 1, 1,
    1, 0, 3, 2, 0, 1, 1, 1, 0, 3, 1, 1, 3, 3, 0, 5, 1, 1, 0, 1, 1, 1, 1, 1, 2, 2, 1, 3, 1, 0, 1, 3, 2, 4, 3, 3, 1, 1, 2, 1, 0, 3, 1, 5, 2, 2, 0, 2, 2, 1,
    1, 3, 0, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 3, 1, 1, 2, 2, 2, 1, 0, 1, 2, 1, 1, 1, 2, 2, 4, 1, 2, 2, 2, 3, 4, 2, 3, 1, 1, 1, 0, 1, 1, 1, 4, 3, 2, 0, 1, 1,
    5, 4, 0, 1, 1, 1, 4, 2, 0, 2, 1, 3, 1, 0, 1, 1, 3, 1, 4, 0, 1, 1, 0, 2, 2, 3, 0, 1, 4, 1, 0, 1, 1, 0, 1, 2, 3, 2, 4, 3, 2, 1, 1, 2, 2, 4, 2, 0, 1, 3,
    1, 1, 0, 1, 3, 2, 0, 2, 3, 0, 2, 1, 2, 2, 1, 2, 1, 0, 1, 2, 0, 1, 1, 2, 3, 2, 2, 1, 1, 0, 2, 4, 3, 2, 3, 1, 1, 1, 1, 2, 2, 2, 1, 0, 1, 1, 1, 0, 1, 2,
    1, 2, 1, 1, 1, 1, 1, 1, 2, 0, 2, 2, 1, 0, 2, 1, 1, 1, 1, 1, 3, 1, 2, 0, 1, 1, 0, 2, 1, 2, 3, 1, 1, 2, 1, 2, 1, 3, 2, 1, 2, 0, 1, 3, 0, 1, 2, 0, 1, 1,
    1, 4, 1, 0, 1, 2, 0, 1, 3, 1, 2, 0, 1, 4, 4, 0, 1, 1, 1, 1, 0, 3, 2, 2, 0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 2, 4, 0, 3, 2, 1, 1, 0, 1, 2, 0, 2, 2, 0, 2, 3,
    0, 1, 2, 0, 1, 2, 1, 0, 1, 1, 2, 1, 0, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 2, 2, 0, 1, 1, 2, 2, 4, 3, 0, 2, 1, 1, 0, 1, 3, 0, 1, 2, 2, 1, 0, 1, 2,
    0, 1, 2, 3, 2, 4, 1, 4, 1, 2, 0, 1, 2, 0, 2, 1, 1, 0, 2, 3, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 5, 1, 1, 0, 2, 4, 0, 1, 3, 1, 0, 1, 2, 1, 2, 1, 0, 1,
    1, 1, 0, 2, 1, 2, 3, 3, 1, 0, 1, 3, 1, 0, 1, 4, 0, 1, 3, 2, 0, 1, 3, 3, 0, 2, 3, 0, 2, 2, 0, 1, 2, 2, 1, 1, 0, 1, 1, 4, 0, 1, 1, 2, 3, 2, 1, 0, 1, 2,
    0, 1, 2, 1, 3, 9, 10, 7, 6, 1, 2, 1, 2, 1, 0, 1, 1, 1, 1, 2, 0, 2, 1, 0, 3, 2, 2, 0, 4, 2, 1, 0, 1, 2, 1, 0, 3, 1, 2, 0, 1, 2, 0, 2, 1, 4, 0, 2, 1, 0,
    2, 3, 1, 1, 3, 3, 1, 3, 0, 1, 5, 1, 1, 0, 1, 2, 1, 2, 1, 0, 1, 1, 2, 3, 0, 2, 1, 2, 2, 2, 0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 3, 1, 3, 1, 0, 2, 1, 2, 1, 0,
    1, 1, 0, 1, 1, 3, 3, 2, 1, 0, 1, 4, 2, 2, 1, 4, 1, 2, 1, 1, 1, 0, 1, 1, 1, 2, 2, 2, 1, 0, 2, 1, 1, 2, 0, 2, 3, 2, 0, 1, 1, 1, 1, 1, 0, 1, 2, 1, 3, 2,
    2, 2, 0, 1, 4, 2, 0, 1, 2, 1, 3, 1, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 1, 0, 3, 1, 2, 1, 2, 2, 1, 0, 1, 2, 2, 1, 1, 0, 2, 2, 4, 2, 0, 1, 2, 1, 2, 1, 1, 0,
    1, 2, 1, 1, 0, 1, 1, 3, 0, 1, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0, 1, 2, 0, 1, 3, 1, 2, 3, 2, 1, 2, 0, 2, 1, 1, 1, 0, 3, 1, 3, 0, 1, 2, 1, 1, 0, 2, 3, 1, 2,
    2, 2, 2, 0, 1, 2, 1, 1, 2, 1, 2, 1, 0, 1, 3, 2, 1, 1, 1, 2, 0, 2, 4, 1, 1, 2, 2, 0, 1, 1, 2, 0, 3, 1, 3, 1, 1, 0, 1, 1, 0, 2, 2, 4, 2, 0, 2, 1, 3, 3,
    3, 2, 0, 2, 1, 1, 2, 2, 3, 1, 1, 1, 2, 0, 1, 1, 0, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 2, 3, 7, 0, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1,
    0, 1, 1, 0, 1, 2, 1, 2, 3, 2, 1, 2, 2, 5, 0, 2, 1, 1, 1, 2, 3, 1, 0, 2, 2, 3, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 2, 1, 2, 3, 2, 0, 2, 1,
    2, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 5, 1, 1, 1, 0, 2, 1, 0, 4, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 1, 3, 3, 1, 0, 1, 2, 1, 0, 2, 1, 0, 3, 1, 1, 1, 1, 1,
    3, 0, 1, 1, 1, 1, 0, 3, 1, 0, 8, 3, 0, 2, 1, 0, 1, 1, 0, 1, 2, 1, 1, 1, 1, 2, 1, 3, 1, 1, 2, 1, 0, 1, 1, 1, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1,
    1, 0, 5, 1, 0, 2, 4, 0, 1, 1, 1, 2, 0, 1, 1, 0, 2, 1, 0, 1, 2, 1, 0, 2, 2, 1, 1, 0, 1, 2, 0, 1, 2, 0, 4, 2, 0, 2, 1, 1, 1, 2, 1, 4, 2, 0, 1, 1, 1, 1,
    1, 1, 0, 4, 1, 1, 0, 3, 2, 0, 1, 1, 1, 0, 3, 1, 0, 2, 3, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 0, 1, 2, 1, 1, 0, 1, 2, 3, 1,
    1, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 3, 0, 1, 3, 0, 3, 2, 1, 0, 1, 2, 0, 1, 1, 0, 4, 2, 0, 1, 2, 0, 1, 1, 1, 0, 1, 3, 2, 2, 3, 0, 4, 1, 1, 1, 3, 0, 6,
    1, 0, 2, 2, 1, 1, 0, 3, 1, 0, 3, 1, 0, 2, 1, 0, 3, 1, 0, 2, 1, 2, 1, 2, 0, 1, 1, 1, 1, 0, 2, 1, 1, 2, 0, 2, 1, 0, 5, 2, 1, 0, 1, 1, 1, 0, 2, 2, 0, 1,
    1, 0, 1, 1, 0, 2, 1, 0, 4, 2, 0, 6, 1, 0, 2, 1, 1, 0, 1, 3, 0, 1, 1, 1, 0, 1, 2, 0, 5, 1, 0, 1, 1, 2, 0, 2, 1, 0, 1, 1, 1, 2, 2, 1, 3, 1, 3, 0, 1, 1,
    1, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 0, 4, 1, 1, 0, 1, 1, 1, 2, 1, 0, 1, 1, 0, 5, 1, 0, 1, 1, 0, 3, 2, 0, 2, 1, 0, 3, 1, 0, 2, 1, 0, 4, 2, 0, 3, 1, 0, 3,
    1, 1, 1, 2, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 2, 2, 2, 0, 1, 1, 2, 0, 5, 1, 0, 3, 1, 0, 1, 1, 1, 0, 2, 1, 1, 2, 1, 0, 3, 1, 1, 0,
    2, 1, 0, 3, 1, 0, 2, 3, 0, 1, 1, 1, 0, 1, 1, 1, 0, 4, 1, 0, 2, 2, 0, 1, 2, 0, 2, 1, 0, 3, 1, 1, 0, 2, 1, 0, 1, 1, 1, 2, 0, 6, 1, 1, 1, 0, 9, 1, 1, 0,
    1, 2, 0, 2, 1, 2, 1, 0, 1, 1, 2, 0, 2, 1, 0, 2, 1, 0, 3, 1, 2, 0, 4, 2, 0, 2, 2, 0, 1, 1, 0, 1, 3, 1, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 3, 0, 2, 2, 0, 1,
    1, 2, 11, 18, 26, 35, 54, 53, 69, 44, 27, 12, 12, 3, 0, 5, 1, 0, 4, 1, 1, 0, 1, 1, 0, 4, 1, 0, 2, 1, 0, 1, 1, 0, 6, 1, 1, 0, 2, 2, 1, 0, 5, 1, 0, 4, 1, 0, 8, 1,
    0, 2, 1, 0, 8, 2, 0, 1, 1, 2, 0, 2, 1, 1, 0, 4, 1, 0, 2, 2, 0, 4, 1, 0, 10, 1, 0, 8, 1, 0, 6, 1, 0, 5, 1, 2, 0, 3, 1, 2, 2, 2, 0, 4, 1, 2, 0, 1, 2, 1,
    0, 2, 1, 1, 2, 0, 4, 1, 1, 0, 7, 1, 0, 4, 1, 0, 1, 1, 0, 9, 1, 0, 1, 1, 1, 0, 4, 1, 2, 0, 1, 2, 0, 7, 1, 0, 2, 1, 0, 3, 1, 1, 0, 4, 1, 1, 0, 8, 1, 1,
    0, 11, 2, 0, 6, 1, 0, 2, 1, 0, 6, 1, 0, 1, 2, 1, 0, 3, 2, 0, 6, 2, 1, 0, 1, 1, 0, 14, 2, 0, 12, 1, 0, 6, 1, 0, 2, 1, 0, 3, 1, 0, 2, 2, 0, 3, 1, 1, 1, 0,
    2, 1, 0, 2, 1, 0, 1, 1, 1, 0, 5, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 3, 1, 1, 2, 2, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 2, 1, 0, 2, 2, 0, 1, 1, 0, 2,
    1, 1, 0, 6, 2, 1, 0, 3, 1, 0, 2, 1, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 1, 0, 4, 1, 0, 7, 1, 0, 7, 1, 0, 3, 1, 0, 4, 1,
    0, 4, 1, 0, 5, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 15, 1, 0, 3, 2, 2, 0, 1, 1, 0, 4, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 4, 1,
    0, 1, 1, 0, 10, 1, 1, 1, 1, 0, 3, 1, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 0, 7, 1, 0, 7, 1, 0, 3, 1, 1, 0, 2, 2, 0, 1, 2, 0, 21, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2,
    1, 0, 10, 1, 0, 9, 1, 0, 2, 1, 0, 11, 1, 0, 2, 2, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 9, 1, 0, 5, 1, 0, 11, 1, 0, 9, 1, 0, 22, 2, 0, 1, 1, 0, 8, 1, 1, 2, 1, 0,
    4, 1, 1, 2, 0, 1, 1, 0, 19, 1, 1, 0, 26, 1, 1, 1, 0, 10, 1, 1, 0, 8, 1, 0, 1, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 1, 2, 5, 6, 7, 8, 2, 5, 3, 0, 1, 1, 1, 0, 2,
    1, 0, 2, 1, 0, 14, 1, 0, 3, 1, 0, 12, 1, 0, 1, 1, 0, 1, 1, 0, 16, 1, 0, 15, 1, 0, 7, 1, 0, 11, 1, 0, 1, 1, 1, 1, 0, 17, 2, 0, 18, 1, 0, 1, 1, 0, 16, 1, 0, 3,
    1, 1, 0, 2, 1, 0, 3, 1, 0, 12, 1, 0, 22, 1, 0, 8, 1, 0, 4, 2, 1, 0, 8, 1, 0, 13, 1, 0, 4, 1, 0, 15, 1, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 0, 12, 1, 0, 15, 1, 0,
    15, 1, 0, 2, 1, 0, 19, 1, 0, 2, 1, 0, 2, 1, 0, 3, 2, 1, 0, 1, 1, 0, 4, 1, 0, 9, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 10, 1, 0, 36, 1, 0, 2, 1, 0, 3,
    1, 0, 1, 1, 0, 13, 1, 0, 11, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 2, 1, 0, 10, 1, 0, 2, 1, 0, 13, 1, 0, 5, 2, 1, 1, 0, 4, 1, 0, 6, 2, 0, 6, 1, 0, 17, 1, 0, 14,
    1, 0, 6, 1, 0, 6, 1, 0, 4, 1, 0, 21, 1, 0, 38, 1, 0, 3, 1, 0, 2, 2, 1, 0, 9, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 5, 1, 0, 8, 1, 0, 2, 1, 0, 16, 1,
    0, 5, 1, 0, 5, 1, 0, 1, 1, 0, 11, 1, 0, 2, 1, 0, 6, 1, 0, 3, 1, 0, 2, 1, 0, 4, 1, 0, 7, 1, 0, 9, 1, 0, 9, 1, 0, 3, 1, 1, 0, 8, 1, 0, 3, 1, 0, 7, 1, 0,
    9, 2, 0, 20, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 3, 1, 0, 24, 1, 0, 7, 1, 0, 4, 1, 1, 1, 0, 10, 1, 0, 6, 1, 0, 3, 1, 2, 2, 2, 1, 2, 0, 1, 2, 0, 8, 1, 0, 9,
    2, 0, 6, 1, 0, 8, 1, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 0, 11, 2, 0, 5, 1, 0, 6, 2, 0, 23, 1, 0, 5, 1, 1, 0, 3, 1, 0, 8, 1, 1, 0, 7, 1, 0, 1, 1, 1, 0, 1, 1,
    0, 18, 2, 1, 0, 3, 1, 0, 5, 1, 0, 2, 1, 2, 0, 2, 1, 0, 5, 1, 0, 1, 1, 1, 1, 0, 7, 1, 1, 2, 1, 0, 5, 1, 0, 6, 1, 0, 6, 1, 0, 2, 1, 0, 15, 1, 0, 11, 1, 0,
    7, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 1, 2, 0, 3, 3, 2, 2, 2, 1, 1, 0, 1, 1, 0, 1, 1, 0, 10, 1, 0, 17, 1, 0, 9, 1, 0, 4, 2, 0, 7, 1,
    0, 4, 1, 0, 4, 1, 0, 26, 1, 0, 16, 1, 0, 4, 1, 0, 10, 1, 0, 34, 2, 0, 24, 2, 1, 0, 5, 1, 0, 17, 1, 0, 4, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 6, 2, 0, 3, 1, 1, 0,
    5, 1, 0, 14, 1, 1, 0, 3, 1, 0, 7, 1, 0, 5, 2, 0, 8, 1, 1, 0, 10, 1, 1, 0, 8, 1, 0, 16, 1, 1, 0, 8, 1, 0, 2, 1, 0, 29, 1, 0, 7, 2, 0, 9, 1, 0, 5, 1, 0, 11,
    1, 0, 9, 1, 0, 4, 1, 0, 15, 1, 0, 10, 1, 0, 3, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 0, 5, 2, 0, 10, 1, 0, 17, 1, 0, 9, 1, 0, 4, 2, 0, 1, 1, 0, 6, 1, 0, 11, 1, 0,
    3, 1, 0, 18, 1, 0, 3, 1, 0, 27, 1, 0, 6, 2, 0, 34, 1, 0, 2, 1, 0, 9, 1, 0, 13, 1, 0, 6, 3, 0, 1, 1, 0, 13, 1, 0, 7, 1, 0, 3, 1, 0, 16, 1, 0, 18, 1, 0, 2, 1,
    1, 0, 24, 1, 0, 6, 1, 0, 6, 1, 0, 12, 1, 0, 46, 1, 0, 1, 1, 0, 12, 1, 0, 37, 1, 0, 9, 1, 0, 4, 1, 1, 0, 26, 1, 0, 27, 1, 0, 23, 1, 0, 19, 1, 0, 28, 1, 0, 7, 1,
    0, 73, 1, 0, 2, 1, 0, 1, 2, 2, 10, 4, 3, 6, 9, 5, 0, 1, 1, 2, 1, 0, 40, 1, 0, 17, 1, 0, 7, 1, 0, 1, 1, 0, 72, 1, 0, 49, 1, 0, 10, 1, 0, 11, 1, 0, 95, 1, 0, 87,
    1, 0, 1, 1, 0, 71, 1, 0, 60, 1, 0, 6, 1, 0, 123, 1, 0, 11, 1, 0, 121, 1, 0, 248
  };
  assert( test_8_packed.size() == 6824 );
  const vector<uint8_t> test_8_encoded = QRSpecDev::encode_stream_vbyte( test_8_chan_cnts );
  assert( test_8_encoded == test_8_packed );
  vector<uint32_t> test_8_dec;
  const size_t test_8_nbytedec = QRSpecDev::decode_stream_vbyte(test_8_encoded,test_8_dec);
  assert( test_8_nbytedec == test_8_packed.size() );
  assert( test_8_dec == test_8_chan_cnts );
  
  
  
  
  // Test case 9
  const vector<uint32_t> test_9_chan_cnts{
    0, 39, 1, 30, 59, 62, 58, 57, 58, 65, 62, 54, 62, 50, 61, 52, 59, 52, 44, 61,
    72, 57, 51, 56, 59, 45, 40, 42, 47, 56, 48, 53, 38, 55, 43, 47, 41, 42, 64, 42,
    46, 49, 42, 49, 41, 50, 31, 53, 46, 44, 44, 39, 46, 42, 39, 49, 46, 45, 40, 28,
    37, 40, 32, 39, 40, 43, 40, 37, 48, 37, 35, 37, 46, 45, 43, 35, 40, 33, 38, 32,
    40, 38, 41, 42, 35, 41, 45, 32, 44, 37, 43, 33, 27, 34, 40, 33, 26, 48, 45, 33,
    38, 31, 32, 44, 42, 46, 35, 41, 33, 31, 41, 33, 49, 39, 46, 38, 43, 37, 45, 34,
    42, 53, 51, 42, 35, 35, 55, 49, 42, 57, 56, 70, 44, 50, 40, 68, 51, 52, 50, 70,
    49, 56, 50, 74, 61, 74, 80, 61, 66, 82, 90, 96, 78, 67, 58, 57, 58, 66, 66, 74,
    73, 80, 71, 75, 71, 68, 70, 76, 76, 78, 81, 69, 78, 76, 94, 85, 86, 85, 87, 78,
    69, 77, 76, 79, 82, 65, 62, 80, 78, 75, 83, 79, 82, 77, 87, 97, 84, 77, 70, 87,
    80, 80, 81, 72, 90, 86, 65, 89, 96, 77, 81, 103, 88, 72, 81, 97, 79, 90, 98, 86,
    107, 88, 110, 80, 79, 102, 94, 92, 96, 96, 105, 117, 103, 82, 94, 84, 91, 93, 103, 90,
    101, 102, 113, 109, 88, 97, 98, 95, 108, 85, 88, 118, 112, 101, 104, 108, 131, 110, 110, 103,
    119, 116, 116, 123, 106, 130, 133, 118, 110, 108, 108, 95, 93, 103, 102, 111, 92, 127, 92, 94,
    106, 106, 118, 87, 107, 98, 89, 102, 102, 106, 93, 109, 95, 110, 102, 86, 92, 95, 94, 87,
    100, 89, 89, 92, 96, 97, 87, 95, 93, 85, 87, 88, 84, 77, 105, 91, 108, 98, 95, 92,
    81, 91, 101, 80, 100, 83, 93, 86, 85, 89, 91, 73, 84, 86, 80, 85, 95, 90, 82, 89,
    93, 74, 93, 103, 86, 68, 93, 84, 74, 89, 82, 89, 82, 95, 143, 184, 173, 135, 122, 81,
    82, 74, 99, 83, 92, 88, 88, 83, 71, 96, 80, 89, 78, 98, 85, 80, 78, 103, 84, 90,
    80, 76, 79, 97, 105, 88, 87, 78, 79, 72, 91, 94, 91, 85, 106, 83, 79, 89, 91, 75,
    77, 91, 76, 84, 93, 83, 94, 106, 149, 154, 170, 113, 80, 100, 95, 73, 79, 95, 99, 77,
    82, 73, 75, 95, 100, 85, 77, 90, 82, 88, 87, 83, 90, 92, 68, 69, 95, 85, 70, 91,
    76, 79, 67, 70, 82, 81, 81, 83, 80, 87, 91, 85, 90, 89, 80, 79, 70, 72, 76, 104,
    82, 91, 99, 86, 85, 75, 104, 173, 430, 843, 1172, 1062, 690, 295, 108, 41, 24, 38, 23, 23,
    34, 42, 41, 36, 30, 24, 30, 34, 25, 31, 32, 31, 22, 51, 24, 43, 43, 38, 38, 34,
    38, 37, 28, 38, 26, 34, 24, 33, 30, 29, 25, 34, 36, 40, 54, 57, 46, 44, 34, 33,
    33, 47, 70, 123, 134, 119, 62, 33, 22, 28, 29, 38, 39, 32, 22, 19, 33, 30, 23, 17,
    25, 22, 20, 20, 23, 15, 28, 24, 27, 26, 23, 17, 30, 17, 17, 18, 19, 24, 24, 31,
    21, 22, 10, 20, 20, 25, 24, 23, 34, 28, 9, 25, 24, 17, 23, 17, 17, 17, 26, 14,
    15, 18, 16, 23, 27, 14, 21, 14, 27, 14, 19, 22, 14, 19, 17, 16, 16, 19, 26, 13,
    20, 20, 24, 20, 14, 18, 22, 13, 18, 21, 18, 23, 25, 37, 49, 41, 40, 24, 29, 20,
    31, 17, 27, 29, 27, 25, 14, 20, 19, 25, 13, 22, 16, 13, 17, 16, 25, 16, 15, 22,
    12, 23, 13, 16, 16, 14, 15, 14, 17, 17, 14, 15, 20, 10, 17, 10, 18, 14, 16, 15,
    20, 14, 14, 14, 13, 16, 17, 18, 15, 11, 16, 18, 17, 17, 15, 17, 15, 14, 12, 14,
    11, 15, 11, 16, 12, 12, 9, 12, 14, 11, 16, 9, 13, 19, 16, 16, 14, 10, 12, 15,
    16, 14, 15, 17, 16, 9, 17, 16, 11, 13, 24, 13, 8, 12, 23, 19, 15, 11, 13, 16,
    11, 14, 14, 18, 10, 6, 10, 15, 8, 13, 13, 16, 15, 15, 12, 13, 14, 14, 7, 15,
    10, 19, 6, 4, 9, 16, 12, 10, 11, 16, 6, 15, 14, 10, 7, 7, 14, 9, 11, 13,
    10, 16, 13, 13, 8, 10, 14, 19, 25, 31, 23, 22, 14, 7, 15, 9, 12, 18, 8, 20,
    10, 9, 10, 7, 16, 15, 14, 12, 12, 7, 12, 3, 15, 9, 11, 10, 9, 11, 12, 6,
    6, 15, 12, 10, 14, 11, 8, 8, 10, 12, 11, 15, 12, 16, 10, 11, 14, 9, 6, 10,
    10, 14, 5, 6, 12, 16, 11, 13, 12, 12, 12, 13, 9, 14, 8, 3, 8, 9, 11, 8,
    8, 10, 15, 8, 14, 9, 8, 7, 14, 8, 8, 7, 12, 9, 14, 7, 10, 6, 7, 7,
    10, 6, 7, 12, 12, 5, 9, 8, 5, 10, 11, 6, 6, 13, 10, 7, 7, 7, 8, 7,
    6, 9, 3, 6, 11, 15, 13, 6, 10, 10, 13, 9, 8, 11, 9, 13, 10, 9, 13, 9,
    5, 7, 4, 6, 11, 6, 7, 11, 12, 13, 6, 7, 4, 14, 9, 3, 8, 14, 6, 9,
    6, 12, 18, 20, 33, 23, 19, 15, 7, 4, 8, 8, 9, 4, 7, 4, 7, 5, 6, 6,
    6, 5, 6, 6, 7, 6, 7, 7, 6, 7, 8, 3, 8, 5, 9, 2, 3, 7, 10, 5,
    8, 6, 9, 7, 6, 7, 5, 5, 8, 13, 13, 5, 4, 5, 4, 5, 9, 5, 6, 7,
    8, 2, 5, 7, 3, 8, 5, 7, 9, 5, 7, 8, 6, 6, 7, 8, 7, 7, 7, 8,
    8, 6, 5, 6, 8, 8, 0, 1, 6, 4, 5, 6, 8, 5, 4, 9, 4, 6, 9, 8,
    8, 6, 5, 6, 5, 6, 6, 8, 9, 6, 2, 6, 7, 7, 3, 3, 4, 3, 11, 3,
    7, 5, 10, 7, 6, 4, 6, 8, 11, 11, 5, 7, 3, 8, 5, 5, 5, 2, 4, 1,
    2, 3, 5, 5, 6, 7, 5, 3, 3, 3, 5, 8, 5, 4, 2, 7, 6, 10, 3, 6,
    3, 7, 4, 4, 6, 8, 4, 6, 6, 8, 5, 6, 7, 3, 6, 6, 3, 9, 6, 7,
    6, 7, 3, 4, 6, 5, 10, 6, 7, 4, 5, 4, 6, 5, 8, 3, 5, 3, 7, 6,
    5, 4, 1, 5, 7, 5, 9, 6, 6, 2, 7, 4, 4, 3, 4, 6, 3, 4, 3, 3,
    4, 4, 5, 7, 3, 6, 4, 9, 7, 1, 3, 2, 4, 6, 3, 6, 4, 7, 6, 6,
    5, 6, 5, 6, 7, 5, 5, 1, 3, 5, 7, 7, 3, 5, 4, 2, 6, 4, 6, 4,
    6, 3, 3, 2, 3, 1, 8, 5, 4, 2, 5, 5, 5, 2, 4, 8, 2, 3, 8, 1,
    7, 5, 2, 5, 3, 1, 4, 5, 4, 3, 5, 2, 3, 7, 5, 5, 6, 2, 2, 8,
    4, 3, 5, 2, 5, 2, 6, 4, 1, 8, 7, 4, 5, 2, 2, 4, 2, 6, 8, 8,
    2, 4, 4, 2, 3, 6, 1, 3, 4, 4, 3, 4, 3, 1, 2, 2, 6, 2, 2, 5,
    3, 7, 7, 1, 4, 3, 3, 4, 6, 4, 3, 10, 4, 5, 3, 3, 7, 4, 3, 3,
    2, 6, 3, 5, 2, 3, 4, 6, 4, 6, 8, 2, 3, 5, 6, 4, 3, 4, 8, 4,
    7, 4, 3, 3, 4, 4, 2, 3, 1, 4, 3, 2, 6, 1, 2, 4, 3, 6, 4, 3,
    5, 3, 3, 3, 8, 3, 1, 4, 5, 5, 3, 3, 4, 7, 4, 3, 0, 1, 1, 2,
    4, 2, 4, 5, 4, 2, 1, 1, 3, 6, 8, 1, 2, 4, 3, 8, 4, 3, 8, 11,
    11, 6, 17, 5, 7, 7, 5, 3, 3, 4, 5, 4, 3, 1, 3, 7, 8, 4, 2, 5,
    5, 7, 8, 3, 6, 2, 0, 1, 1, 7, 3, 4, 3, 2, 3, 4, 6, 2, 3, 2,
    5, 5, 5, 4, 4, 4, 5, 2, 4, 5, 4, 3, 2, 5, 3, 3, 3, 2, 8, 5,
    6, 2, 2, 1, 6, 3, 3, 5, 1, 3, 6, 3, 2, 5, 4, 9, 3, 6, 2, 2,
    4, 1, 6, 1, 3, 6, 3, 3, 2, 2, 0, 1, 2, 3, 2, 2, 5, 2, 4, 4,
    4, 2, 6, 1, 3, 3, 2, 4, 3, 1, 6, 5, 2, 1, 2, 2, 4, 2, 3, 6,
    4, 3, 1, 3, 1, 1, 3, 1, 1, 5, 4, 1, 5, 7, 5, 1, 2, 1, 3, 5,
    5, 5, 3, 5, 3, 2, 2, 4, 3, 2, 4, 5, 2, 3, 1, 4, 4, 2, 4, 4,
    2, 4, 3, 5, 2, 5, 0, 1, 3, 2, 6, 3, 4, 3, 0, 1, 9, 2, 2, 5,
    2, 2, 2, 1, 1, 7, 3, 1, 6, 3, 3, 4, 1, 5, 3, 3, 1, 2, 3, 4,
    8, 17, 13, 15, 12, 7, 9, 2, 4, 2, 2, 5, 2, 3, 5, 3, 2, 3, 6, 3,
    1, 5, 4, 1, 4, 4, 1, 3, 2, 1, 2, 4, 4, 3, 2, 2, 4, 1, 7, 2,
    3, 2, 1, 3, 0, 1, 1, 0, 1, 6, 4, 2, 0, 1, 1, 2, 3, 3, 3, 2,
    5, 2, 2, 3, 3, 2, 3, 5, 3, 4, 3, 2, 5, 4, 17, 17, 34, 21, 22, 7,
    5, 4, 2, 1, 3, 1, 3, 3, 4, 5, 3, 1, 1, 2, 1, 2, 2, 1, 2, 2,
    4, 3, 3, 1, 2, 1, 2, 5, 2, 0, 1, 1, 1, 1, 2, 4, 2, 2, 3, 3,
    1, 3, 2, 1, 3, 2, 2, 3, 1, 2, 4, 4, 6, 2, 4, 2, 3, 4, 3, 5,
    3, 3, 3, 1, 2, 0, 1, 2, 0, 1, 1, 3, 0, 1, 1, 4, 0, 1, 4, 1,
    4, 4, 2, 4, 1, 5, 1, 2, 2, 4, 6, 2, 2, 3, 2, 1, 1, 1, 2, 5,
    1, 0, 1, 3, 4, 5, 2, 2, 2, 2, 1, 1, 3, 1, 4, 2, 4, 4, 0, 2,
    4, 0, 1, 1, 7, 3, 4, 1, 1, 3, 0, 1, 1, 1, 2, 1, 4, 2, 3, 4,
    1, 3, 3, 6, 4, 1, 2, 3, 5, 4, 2, 1, 3, 2, 2, 2, 2, 5, 4, 4,
    2, 0, 2, 1, 2, 2, 6, 1, 1, 5, 2, 1, 5, 3, 2, 2, 3, 4, 1, 1,
    6, 1, 5, 3, 2, 6, 0, 1, 1, 1, 1, 1, 2, 4, 3, 0, 1, 1, 2, 2,
    3, 3, 3, 3, 1, 2, 0, 2, 3, 3, 4, 2, 3, 6, 1, 2, 2, 3, 3, 2,
    0, 1, 2, 1, 0, 1, 2, 4, 3, 3, 0, 1, 5, 2, 2, 2, 6, 1, 1, 1,
    0, 1, 3, 1, 4, 3, 0, 1, 4, 5, 2, 3, 5, 2, 1, 3, 5, 3, 3, 2,
    1, 3, 3, 4, 2, 3, 1, 6, 1, 4, 4, 5, 2, 2, 1, 1, 2, 3, 2, 0,
    1, 4, 2, 0, 1, 3, 2, 0, 1, 1, 4, 2, 3, 1, 3, 0, 2, 1, 2, 1,
    4, 2, 3, 0, 1, 1, 2, 4, 3, 1, 1, 0, 1, 2, 1, 1, 4, 1, 1, 2,
    2, 3, 2, 0, 1, 2, 1, 3, 2, 3, 0, 1, 3, 4, 2, 2, 3, 5, 3, 2,
    2, 7, 2, 3, 3, 1, 3, 1, 4, 3, 1, 3, 2, 1, 0, 1, 4, 2, 3, 3,
    2, 4, 2, 2, 0, 1, 2, 1, 0, 1, 1, 4, 4, 1, 0, 1, 2, 3, 3, 2,
    2, 3, 0, 1, 2, 2, 2, 2, 1, 1, 3, 1, 1, 0, 2, 2, 2, 6, 2, 2,
    2, 1, 1, 3, 2, 3, 2, 4, 3, 2, 2, 0, 1, 2, 3, 1, 4, 1, 2, 0,
    1, 1, 3, 6, 3, 3, 3, 6, 1, 1, 0, 2, 2, 1, 0, 1, 1, 1, 1, 2,
    2, 1, 2, 1, 3, 2, 4, 3, 0, 1, 1, 0, 1, 4, 3, 4, 1, 4, 2, 3,
    1, 4, 1, 1, 2, 5, 4, 2, 0, 1, 1, 4, 5, 1, 0, 1, 2, 2, 2, 5,
    0, 1, 1, 2, 4, 0, 2, 3, 2, 2, 0, 1, 2, 2, 2, 0, 1, 3, 1, 2,
    3, 0, 1, 2, 0, 1, 1, 0, 1, 3, 4, 3, 1, 2, 1, 4, 1, 3, 2, 4,
    2, 2, 2, 3, 1, 2, 2, 4, 1, 3, 0, 1, 2, 3, 1, 1, 4, 2, 1, 1,
    4, 0, 1, 5, 1, 3, 2, 8, 2, 2, 1, 1, 1, 2, 2, 1, 0, 1, 2, 2,
    3, 3, 4, 0, 1, 3, 0, 1, 2, 0, 1, 1, 1, 1, 1, 4, 2, 0, 2, 3,
    3, 3, 3, 1, 1, 3, 2, 2, 1, 1, 3, 0, 1, 1, 3, 2, 5, 2, 0, 1,
    2, 2, 4, 3, 1, 0, 1, 3, 1, 0, 2, 2, 1, 2, 1, 0, 1, 2, 1, 1,
    2, 2, 1, 4, 1, 1, 0, 1, 2, 3, 1, 1, 3, 1, 2, 1, 1, 0, 1, 2,
    2, 3, 1, 0, 1, 3, 2, 2, 0, 1, 5, 2, 5, 4, 3, 4, 2, 1, 4, 3,
    0, 1, 3, 1, 1, 0, 1, 2, 0, 1, 2, 1, 3, 4, 4, 0, 1, 2, 0, 2,
    2, 2, 1, 4, 4, 1, 3, 2, 0, 2, 3, 2, 1, 0, 2, 1, 6, 0, 2, 4,
    0, 2, 2, 1, 1, 1, 0, 1, 2, 0, 2, 2, 0, 1, 3, 1, 1, 0, 1, 2,
    2, 2, 1, 0, 1, 2, 3, 0, 2, 1, 1, 1, 1, 0, 1, 4, 0, 1, 2, 0,
    1, 4, 0, 1, 2, 2, 4, 4, 2, 4, 3, 3, 0, 1, 2, 0, 1, 1, 0, 1,
    2, 1, 2, 2, 2, 2, 2, 1, 2, 0, 2, 2, 1, 1, 1, 3, 1, 0, 1, 2,
    1, 0, 1, 2, 4, 1, 1, 3, 2, 0, 1, 2, 0, 1, 3, 3, 1, 2, 1, 2,
    1, 4, 1, 2, 1, 3, 0, 1, 1, 3, 1, 3, 4, 3, 2, 1, 1, 4, 2, 4,
    3, 3, 2, 2, 0, 1, 1, 0, 1, 1, 5, 0, 1, 2, 2, 2, 1, 2, 0, 2,
    1, 5, 1, 2, 2, 0, 1, 2, 1, 2, 2, 0, 1, 3, 2, 1, 1, 2, 0, 1,
    3, 1, 2, 3, 1, 0, 1, 2, 0, 1, 2, 1, 0, 2, 1, 1, 2, 1, 2, 1,
    0, 1, 4, 2, 0, 1, 3, 1, 0, 1, 1, 1, 0, 1, 1, 2, 2, 0, 1, 3,
    1, 0, 1, 4, 3, 3, 3, 10, 8, 9, 4, 9, 4, 3, 1, 2, 1, 1, 1, 2,
    1, 0, 1, 2, 4, 1, 0, 1, 3, 3, 1, 0, 1, 2, 3, 1, 0, 1, 1, 1,
    1, 1, 2, 3, 1, 2, 1, 2, 1, 2, 0, 1, 2, 0, 1, 3, 3, 1, 1, 4,
    0, 1, 2, 2, 1, 5, 0, 1, 2, 1, 0, 4, 2, 1, 1, 4, 0, 1, 4, 3,
    3, 3, 1, 2, 1, 1, 1, 1, 1, 2, 2, 2, 1, 3, 4, 1, 0, 1, 1, 2,
    4, 1, 1, 1, 3, 0, 1, 1, 0, 1, 3, 1, 1, 5, 2, 2, 2, 0, 1, 1,
    0, 2, 1, 3, 0, 1, 1, 1, 1, 2, 3, 2, 1, 2, 0, 2, 1, 0, 1, 1,
    1, 0, 1, 2, 2, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 1, 1, 4, 1, 0,
    1, 1, 1, 0, 1, 1, 2, 1, 2, 1, 2, 1, 3, 3, 4, 7, 1, 2, 0, 2,
    1, 1, 3, 4, 7, 3, 11, 8, 4, 2, 5, 4, 1, 1, 0, 2, 2, 2, 1, 0,
    1, 2, 0, 1, 1, 2, 1, 3, 0, 1, 2, 2, 0, 3, 1, 1, 1, 1, 0, 1,
    2, 1, 1, 0, 1, 3, 1, 0, 1, 2, 0, 1, 1, 0, 1, 1, 3, 1, 0, 1,
    3, 4, 3, 1, 0, 2, 2, 1, 2, 1, 1, 0, 1, 4, 2, 1, 1, 0, 2, 1,
    2, 0, 3, 2, 1, 5, 1, 1, 0, 1, 1, 3, 0, 2, 1, 0, 1, 2, 1, 2,
    1, 4, 3, 3, 3, 4, 2, 1, 0, 3, 2, 3, 2, 0, 2, 1, 0, 2, 2, 0,
    1, 1, 0, 1, 1, 3, 1, 2, 2, 0, 4, 1, 2, 1, 1, 1, 1, 1, 1, 2,
    1, 0, 2, 1, 1, 0, 1, 3, 2, 1, 3, 2, 1, 3, 1, 1, 0, 1, 2, 1,
    2, 3, 0, 2, 1, 2, 2, 1, 0, 1, 1, 1, 0, 1, 1, 2, 4, 2, 0, 1,
    1, 0, 2, 1, 1, 1, 0, 2, 1, 1, 0, 1, 1, 1, 1, 2, 1, 1, 1, 0,
    1, 4, 2, 1, 3, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 3, 1, 1, 0, 1, 2, 0, 1, 2, 1, 2, 3, 2, 2, 1, 2, 4, 0,
    1, 1, 2, 0, 1, 1, 3, 2, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1,
    2, 1, 3, 1, 1, 0, 2, 1, 0, 2, 1, 1, 2, 1, 4, 0, 2, 2, 5, 4,
    2, 0, 1, 2, 0, 1, 2, 4, 4, 2, 0, 1, 2, 0, 2, 1, 1, 2, 1, 1,
    1, 1, 2, 3, 1, 1, 2, 3, 1, 0, 1, 3, 0, 1, 2, 1, 0, 2, 3, 0,
    1, 1, 1, 2, 0, 1, 2, 1, 0, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 1,
    2, 1, 2, 2, 0, 1, 2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 3, 1, 0, 1,
    2, 1, 0, 1, 2, 2, 1, 2, 1, 3, 2, 1, 0, 1, 2, 0, 1, 1, 0, 1,
    1, 3, 2, 0, 1, 1, 1, 0, 2, 1, 2, 1, 3, 0, 2, 1, 1, 1, 1, 1,
    0, 1, 2, 1, 1, 0, 3, 1, 2, 1, 0, 1, 2, 0, 1, 1, 0, 1, 1, 3,
    2, 2, 0, 1, 1, 0, 1, 1, 0, 3, 2, 4, 1, 1, 2, 0, 2, 1, 2, 0,
    3, 1, 3, 1, 3, 2, 2, 1, 1, 3, 2, 2, 3, 1, 2, 0, 1, 1, 0, 1,
    2, 0, 1, 1, 2, 2, 7, 8, 7, 6, 2, 8, 2, 0, 1, 1, 2, 2, 1, 0,
    1, 1, 2, 0, 5, 2, 1, 2, 2, 0, 1, 3, 1, 1, 2, 0, 1, 3, 1, 2,
    0, 1, 2, 0, 2, 2, 0, 1, 1, 1, 0, 1, 3, 2, 2, 1, 0, 2, 1, 3,
    1, 1, 0, 2, 2, 2, 1, 1, 0, 1, 3, 3, 0, 1, 1, 2, 1, 1, 1, 0,
    1, 2, 1, 2, 1, 1, 3, 1, 1, 3, 0, 3, 2, 0, 1, 3, 1, 0, 5, 2,
    0, 1, 1, 1, 4, 1, 0, 1, 1, 2, 3, 2, 2, 0, 1, 2, 0, 1, 3, 2,
    0, 1, 1, 3, 0, 1, 2, 0, 1, 3, 1, 2, 1, 3, 2, 0, 2, 1, 0, 2,
    3, 0, 1, 1, 1, 2, 1, 4, 1, 1, 0, 1, 2, 2, 1, 3, 0, 1, 2, 2,
    1, 2, 0, 1, 1, 3, 1, 3, 2, 1, 3, 0, 2, 4, 2, 3, 1, 2, 0, 2,
    2, 2, 1, 2, 1, 1, 1, 1, 0, 1, 2, 0, 1, 1, 2, 2, 1, 2, 1, 4,
    0, 1, 2, 0, 1, 2, 0, 1, 2, 3, 2, 0, 1, 4, 1, 0, 1, 1, 0, 3,
    2, 0, 1, 1, 1, 2, 1, 1, 3, 3, 1, 2, 0, 1, 2, 0, 1, 1, 1, 2,
    3, 0, 1, 1, 1, 3, 1, 0, 1, 3, 0, 1, 1, 0, 1, 1, 3, 0, 1, 2,
    0, 1, 1, 1, 1, 0, 3, 1, 2, 1, 1, 1, 1, 1, 2, 1, 4, 3, 0, 1,
    2, 1, 2, 0, 1, 2, 3, 1, 1, 2, 4, 0, 1, 4, 7, 1, 1, 1, 0, 1,
    1, 0, 1, 4, 1, 0, 2, 3, 3, 1, 3, 1, 0, 1, 1, 0, 2, 2, 1, 1,
    1, 2, 1, 3, 0, 1, 1, 1, 0, 1, 1, 2, 3, 1, 3, 0, 1, 2, 2, 2,
    1, 1, 0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 1, 1, 3, 1, 1, 0, 1, 2,
    1, 3, 2, 2, 2, 2, 0, 1, 2, 2, 1, 2, 4, 4, 4, 2, 0, 1, 1, 3,
    2, 3, 3, 4, 1, 2, 1, 2, 0, 2, 1, 2, 3, 0, 1, 2, 0, 1, 1, 1,
    2, 0, 1, 2, 1, 1, 1, 0, 1, 2, 2, 0, 1, 1, 0, 1, 2, 1, 1, 1,
    1, 0, 1, 1, 1, 2, 0, 1, 1, 0, 2, 2, 2, 2, 0, 1, 2, 1, 1, 3,
    3, 0, 7, 2, 2, 0, 2, 1, 4, 1, 2, 3, 2, 1, 0, 2, 2, 2, 1, 0,
    1, 1, 0, 6, 1, 1, 1, 2, 2, 2, 1, 0, 3, 1, 1, 0, 1, 1, 0, 3,
    1, 1, 2, 0, 2, 1, 2, 1, 0, 1, 1, 0, 1, 3, 1, 2, 1, 1, 1, 1,
    2, 2, 0, 4, 1, 1, 1, 2, 3, 1, 0, 2, 2, 4, 1, 4, 0, 1, 1, 1,
    1, 0, 2, 1, 2, 1, 1, 1, 0, 1, 1, 0, 4, 2, 0, 3, 1, 1, 2, 1,
    1, 2, 0, 1, 2, 0, 2, 1, 0, 4, 2, 1, 1, 1, 0, 1, 1, 2, 2, 1,
    1, 2, 1, 0, 4, 1, 2, 0, 2, 2, 2, 1, 1, 1, 3, 1, 0, 1, 2, 1,
    0, 3, 1, 1, 2, 2, 2, 0, 1, 1, 0, 3, 1, 0, 1, 1, 1, 0, 3, 1,
    0, 1, 1, 0, 2, 2, 0, 3, 4, 1, 1, 2, 0, 2, 1, 0, 1, 1, 0, 1,
    1, 0, 1, 1, 1, 0, 2, 2, 2, 2, 2, 2, 1, 1, 1, 0, 1, 3, 0, 3,
    1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 3, 1, 1, 1, 3, 0, 3,
    2, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 8, 1, 2, 3, 0, 3, 1, 1, 0,
    5, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 2, 1, 0, 3, 2, 0, 2, 1,
    0, 4, 2, 0, 2, 2, 0, 2, 1, 1, 1, 1, 2, 0, 3, 1, 0, 1, 1, 0,
    3, 1, 0, 4, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 3, 1, 0, 6, 1, 1,
    1, 1, 2, 2, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0,
    3, 3, 1, 1, 1, 0, 1, 1, 2, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 5,
    1, 0, 4, 1, 1, 0, 2, 1, 0, 2, 1, 1, 1, 0, 2, 1, 0, 3, 1, 0,
    2, 1, 1, 1, 2, 0, 1, 2, 0, 1, 1, 2, 1, 2, 0, 2, 2, 0, 1, 1,
    0, 2, 1, 0, 2, 3, 0, 2, 1, 2, 0, 1, 1, 0, 2, 2, 1, 0, 3, 1,
    0, 1, 1, 1, 1, 0, 1, 1, 2, 0, 1, 1, 0, 1, 2, 0, 1, 1, 1, 1,
    1, 1, 0, 4, 2, 0, 3, 1, 2, 0, 2, 1, 1, 0, 3, 1, 1, 1, 1, 0,
    3, 1, 3, 0, 1, 1, 1, 1, 1, 1, 0, 4, 1, 1, 0, 2, 1, 1, 0, 3,
    1, 2, 1, 0, 4, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 3, 2, 2, 0,
    4, 2, 0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 9, 2, 1, 2, 0,
    1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 2, 2, 1, 5, 4, 15, 21, 31, 54, 39,
    49, 37, 23, 13, 3, 1, 0, 1, 1, 1, 0, 4, 1, 1, 0, 15, 1, 0, 1, 1,
    0, 15, 1, 1, 0, 3, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1, 1, 2, 0,
    1, 1, 0, 2, 1, 0, 3, 1, 0, 4, 1, 1, 0, 3, 1, 0, 7, 1, 0, 7,
    1, 0, 3, 1, 2, 1, 0, 6, 1, 0, 2, 1, 0, 2, 1, 1, 0, 7, 1, 1,
    0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 7, 2, 0, 4, 1, 0, 6, 1, 0, 4,
    1, 1, 1, 0, 7, 2, 0, 4, 1, 0, 16, 1, 0, 2, 1, 0, 2, 1, 2, 0,
    7, 1, 0, 8, 1, 0, 1, 1, 0, 3, 1, 1, 0, 1, 1, 0, 5, 1, 0, 1,
    2, 0, 2, 1, 0, 10, 2, 0, 1, 1, 0, 5, 1, 0, 2, 1, 0, 1, 1, 0,
    8, 1, 0, 10, 1, 0, 7, 1, 0, 9, 1, 0, 3, 1, 0, 12, 1, 0, 3, 1,
    0, 2, 2, 0, 2, 1, 0, 3, 1, 0, 4, 1, 0, 10, 1, 1, 0, 1, 1, 0,
    5, 2, 3, 0, 3, 1, 0, 1, 1, 1, 1, 0, 4, 1, 0, 5, 1, 0, 2, 1,
    0, 7, 1, 1, 0, 10, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0,
    4, 1, 0, 1, 1, 1, 0, 1, 1, 0, 4, 1, 0, 5, 1, 0, 2, 1, 2, 0,
    1, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 5, 1, 0, 3, 1, 1, 0, 1,
    2, 0, 1, 1, 0, 1, 1, 0, 14, 1, 1, 0, 2, 1, 0, 3, 1, 0, 6, 1,
    0, 8, 1, 0, 1, 1, 0, 6, 1, 2, 0, 2, 1, 1, 0, 12, 1, 0, 3, 1,
    0, 7, 1, 0, 2, 2, 0, 1, 2, 0, 1, 1, 0, 6, 1, 1, 0, 10, 1, 1,
    0, 11, 1, 0, 1, 1, 0, 4, 1, 1, 0, 1, 1, 1, 0, 12, 2, 0, 5, 1,
    2, 0, 10, 1, 1, 0, 3, 1, 0, 19, 1, 0, 8, 1, 0, 6, 1, 0, 1, 1,
    0, 1, 1, 0, 6, 1, 1, 0, 16, 1, 0, 1, 2, 0, 13, 1, 0, 8, 1, 0,
    5, 1, 1, 0, 1, 2, 0, 1, 1, 0, 2, 1, 1, 0, 2, 2, 0, 5, 3, 0,
    2, 1, 0, 2, 1, 1, 0, 1, 1, 0, 9, 1, 0, 9, 1, 0, 7, 1, 0, 1,
    1, 0, 4, 1, 0, 4, 1, 0, 4, 1, 0, 1, 1, 0, 10, 1, 0, 4, 1, 0,
    7, 3, 3, 3, 4, 7, 2, 5, 4, 2, 1, 1, 0, 1, 1, 0, 4, 1, 0, 11,
    1, 0, 1, 1, 0, 5, 1, 0, 6, 1, 0, 4, 1, 0, 15, 1, 0, 2, 2, 0,
    1, 1, 0, 7, 1, 0, 8, 1, 0, 8, 1, 0, 4, 1, 0, 7, 1, 0, 16, 2,
    0, 6, 1, 0, 14, 1, 0, 12, 1, 0, 1, 1, 1, 0, 22, 1, 0, 8, 1, 0,
    10, 1, 0, 1, 1, 0, 1, 1, 0, 5, 1, 0, 4, 1, 0, 4, 1, 0, 2, 1,
    1, 1, 0, 13, 1, 1, 0, 3, 1, 0, 7, 1, 0, 32, 1, 0, 17, 1, 0, 4,
    1, 0, 1, 1, 0, 8, 1, 0, 2, 1, 0, 7, 1, 0, 4, 1, 0, 4, 1, 0,
    1, 2, 0, 3, 1, 0, 6, 1, 0, 6, 2, 0, 5, 1, 1, 0, 9, 1, 0, 1,
    2, 0, 20, 1, 0, 6, 1, 0, 1, 2, 0, 4, 1, 0, 4, 1, 0, 2, 1, 0,
    14, 1, 0, 13, 1, 0, 8, 1, 0, 12, 1, 2, 0, 1, 1, 0, 19, 1, 0, 9,
    1, 0, 1, 2, 1, 0, 20, 1, 0, 2, 2, 0, 1, 2, 0, 1, 1, 0, 9, 1,
    0, 3, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 0, 10, 2, 0, 5, 2, 0, 3,
    1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 15, 1, 0, 5, 1, 0, 1, 1, 1,
    0, 3, 1, 0, 2, 1, 0, 2, 1, 0, 26, 1, 0, 23, 1, 0, 13, 1, 0, 17,
    1, 0, 3, 1, 0, 11, 1, 0, 61, 1, 0, 5, 1, 0, 7, 1, 0, 4, 1, 0,
    11, 1, 0, 2, 1, 0, 9, 1, 0, 14, 1, 0, 3, 1, 0, 25, 1, 0, 2, 2,
    0, 9, 1, 0, 4, 1, 0, 5, 1, 0, 6, 1, 0, 2, 2, 0, 5, 1, 2, 0,
    2, 1, 0, 15, 1, 0, 1, 2, 0, 7, 1, 0, 7, 1, 1, 0, 5, 1, 0, 4,
    1, 1, 0, 1, 1, 0, 8, 1, 0, 12, 1, 0, 33, 1, 2, 0, 4, 2, 0, 20,
    1, 0, 21, 1, 0, 10, 1, 0, 4, 1, 0, 6, 1, 0, 3, 1, 0, 5, 1, 0,
    18, 1, 1, 0, 4, 1, 0, 14, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 3,
    2, 0, 13, 2, 0, 1, 1, 0, 2, 1, 0, 4, 2, 2, 0, 1, 1, 2, 2, 1,
    2, 0, 1, 2, 0, 1, 2, 0, 1, 1, 0, 26, 1, 0, 12, 1, 0, 2, 2, 0,
    21, 1, 0, 1, 1, 0, 4, 1, 0, 7, 1, 0, 1, 1, 3, 1, 0, 1, 1, 0,
    1, 1, 0, 7, 1, 0, 2, 1, 0, 15, 1, 0, 16, 1, 0, 4, 1, 0, 4, 1,
    0, 4, 1, 0, 24, 1, 0, 1, 1, 0, 9, 1, 0, 1, 2, 0, 14, 1, 0, 4,
    1, 0, 7, 1, 0, 3, 1, 0, 7, 1, 0, 6, 1, 0, 2, 1, 0, 6, 1, 0,
    15, 1, 0, 1, 1, 1, 0, 4, 1, 0, 6, 1, 0, 8, 1, 0, 1, 1, 0, 6,
    1, 1, 1, 0, 17, 1, 0, 11, 1, 0, 3, 1, 0, 12, 1, 0, 4, 1, 0, 7,
    1, 0, 1, 1, 0, 4, 1, 0, 2, 1, 0, 19, 1, 0, 3, 1, 0, 10, 1, 0,
    4, 1, 0, 1, 3, 0, 5, 1, 1, 0, 6, 1, 0, 11, 1, 2, 0, 9, 1, 0,
    3, 1, 0, 18, 1, 0, 15, 1, 0, 12, 1, 0, 7, 1, 1, 0, 2, 1, 0, 12,
    1, 0, 7, 1, 0, 15, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 23, 1, 0,
    2, 1, 0, 12, 1, 0, 7, 1, 0, 3, 1, 0, 12, 1, 0, 1, 1, 0, 2, 1,
    0, 1, 1, 0, 71, 1, 0, 6, 1, 0, 6, 1, 0, 4, 1, 0, 8, 1, 0, 7,
    1, 0, 7, 1, 1, 0, 16, 1, 0, 23, 1, 1, 0, 44, 1, 0, 30, 1, 0, 8,
    1, 0, 40, 1, 0, 8, 1, 0, 4, 1, 0, 30, 1, 0, 22, 1, 0, 66, 1, 0,
    4, 1, 0, 54, 1, 0, 1, 1, 2, 2, 2, 6, 6, 8, 3, 4, 4, 5, 2, 0,
    53, 1, 0, 251, 1, 0, 55, 1, 0, 15, 1, 0, 20, 1, 0, 24, 1, 0, 13, 1,
    0, 126, 1, 0, 44, 1, 0, 123, 1, 0, 10, 1, 0, 43, 1, 0, 16, 1, 0, 188,
    1, 0, 20, 1, 0, 1, 1, 0, 30  };
  assert( test_9_chan_cnts.size() == 5409 );
  const vector<uint8_t> test_9_packed{
    33, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 39, 1, 30, 59, 62, 58, 57, 58, 65, 62, 54, 62, 50, 61, 52, 59, 52, 44, 61, 72, 57, 51, 56, 59, 45, 40, 42, 47, 56, 48, 53, 38, 55, 43, 47, 41, 42, 64, 42, 46, 49, 42, 49, 41,
    50, 31, 53, 46, 44, 44, 39, 46, 42, 39, 49, 46, 45, 40, 28, 37, 40, 32, 39, 40, 43, 40, 37, 48, 37, 35, 37, 46, 45, 43, 35, 40, 33, 38, 32, 40, 38, 41, 42, 35, 41, 45, 32, 44, 37, 43, 33, 27, 34, 40,
    33, 26, 48, 45, 33, 38, 31, 32, 44, 42, 46, 35, 41, 33, 31, 41, 33, 49, 39, 46, 38, 43, 37, 45, 34, 42, 53, 51, 42, 35, 35, 55, 49, 42, 57, 56, 70, 44, 50, 40, 68, 51, 52, 50, 70, 49, 56, 50, 74, 61,
    74, 80, 61, 66, 82, 90, 96, 78, 67, 58, 57, 58, 66, 66, 74, 73, 80, 71, 75, 71, 68, 70, 76, 76, 78, 81, 69, 78, 76, 94, 85, 86, 85, 87, 78, 69, 77, 76, 79, 82, 65, 62, 80, 78, 75, 83, 79, 82, 77, 87,
    97, 84, 77, 70, 87, 80, 80, 81, 72, 90, 86, 65, 89, 96, 77, 81, 103, 88, 72, 81, 97, 79, 90, 98, 86, 107, 88, 110, 80, 79, 102, 94, 92, 96, 96, 105, 117, 103, 82, 94, 84, 91, 93, 103, 90, 101, 102, 113, 109, 88,
    97, 98, 95, 108, 85, 88, 118, 112, 101, 104, 108, 131, 110, 110, 103, 119, 116, 116, 123, 106, 130, 133, 118, 110, 108, 108, 95, 93, 103, 102, 111, 92, 127, 92, 94, 106, 106, 118, 87, 107, 98, 89, 102, 102, 106, 93, 109, 95, 110, 102,
    86, 92, 95, 94, 87, 100, 89, 89, 92, 96, 97, 87, 95, 93, 85, 87, 88, 84, 77, 105, 91, 108, 98, 95, 92, 81, 91, 101, 80, 100, 83, 93, 86, 85, 89, 91, 73, 84, 86, 80, 85, 95, 90, 82, 89, 93, 74, 93, 103, 86,
    68, 93, 84, 74, 89, 82, 89, 82, 95, 143, 184, 173, 135, 122, 81, 82, 74, 99, 83, 92, 88, 88, 83, 71, 96, 80, 89, 78, 98, 85, 80, 78, 103, 84, 90, 80, 76, 79, 97, 105, 88, 87, 78, 79, 72, 91, 94, 91, 85, 106,
    83, 79, 89, 91, 75, 77, 91, 76, 84, 93, 83, 94, 106, 149, 154, 170, 113, 80, 100, 95, 73, 79, 95, 99, 77, 82, 73, 75, 95, 100, 85, 77, 90, 82, 88, 87, 83, 90, 92, 68, 69, 95, 85, 70, 91, 76, 79, 67, 70, 82,
    81, 81, 83, 80, 87, 91, 85, 90, 89, 80, 79, 70, 72, 76, 104, 82, 91, 99, 86, 85, 75, 104, 173, 174, 1, 75, 3, 148, 4, 38, 4, 178, 2, 39, 1, 108, 41, 24, 38, 23, 23, 34, 42, 41, 36, 30, 24, 30, 34, 25,
    31, 32, 31, 22, 51, 24, 43, 43, 38, 38, 34, 38, 37, 28, 38, 26, 34, 24, 33, 30, 29, 25, 34, 36, 40, 54, 57, 46, 44, 34, 33, 33, 47, 70, 123, 134, 119, 62, 33, 22, 28, 29, 38, 39, 32, 22, 19, 33, 30, 23,
    17, 25, 22, 20, 20, 23, 15, 28, 24, 27, 26, 23, 17, 30, 17, 17, 18, 19, 24, 24, 31, 21, 22, 10, 20, 20, 25, 24, 23, 34, 28, 9, 25, 24, 17, 23, 17, 17, 17, 26, 14, 15, 18, 16, 23, 27, 14, 21, 14, 27,
    14, 19, 22, 14, 19, 17, 16, 16, 19, 26, 13, 20, 20, 24, 20, 14, 18, 22, 13, 18, 21, 18, 23, 25, 37, 49, 41, 40, 24, 29, 20, 31, 17, 27, 29, 27, 25, 14, 20, 19, 25, 13, 22, 16, 13, 17, 16, 25, 16, 15,
    22, 12, 23, 13, 16, 16, 14, 15, 14, 17, 17, 14, 15, 20, 10, 17, 10, 18, 14, 16, 15, 20, 14, 14, 14, 13, 16, 17, 18, 15, 11, 16, 18, 17, 17, 15, 17, 15, 14, 12, 14, 11, 15, 11, 16, 12, 12, 9, 12, 14,
    11, 16, 9, 13, 19, 16, 16, 14, 10, 12, 15, 16, 14, 15, 17, 16, 9, 17, 16, 11, 13, 24, 13, 8, 12, 23, 19, 15, 11, 13, 16, 11, 14, 14, 18, 10, 6, 10, 15, 8, 13, 13, 16, 15, 15, 12, 13, 14, 14, 7,
    15, 10, 19, 6, 4, 9, 16, 12, 10, 11, 16, 6, 15, 14, 10, 7, 7, 14, 9, 11, 13, 10, 16, 13, 13, 8, 10, 14, 19, 25, 31, 23, 22, 14, 7, 15, 9, 12, 18, 8, 20, 10, 9, 10, 7, 16, 15, 14, 12, 12,
    7, 12, 3, 15, 9, 11, 10, 9, 11, 12, 6, 6, 15, 12, 10, 14, 11, 8, 8, 10, 12, 11, 15, 12, 16, 10, 11, 14, 9, 6, 10, 10, 14, 5, 6, 12, 16, 11, 13, 12, 12, 12, 13, 9, 14, 8, 3, 8, 9, 11,
    8, 8, 10, 15, 8, 14, 9, 8, 7, 14, 8, 8, 7, 12, 9, 14, 7, 10, 6, 7, 7, 10, 6, 7, 12, 12, 5, 9, 8, 5, 10, 11, 6, 6, 13, 10, 7, 7, 7, 8, 7, 6, 9, 3, 6, 11, 15, 13, 6, 10,
    10, 13, 9, 8, 11, 9, 13, 10, 9, 13, 9, 5, 7, 4, 6, 11, 6, 7, 11, 12, 13, 6, 7, 4, 14, 9, 3, 8, 14, 6, 9, 6, 12, 18, 20, 33, 23, 19, 15, 7, 4, 8, 8, 9, 4, 7, 4, 7, 5, 6,
    6, 6, 5, 6, 6, 7, 6, 7, 7, 6, 7, 8, 3, 8, 5, 9, 2, 3, 7, 10, 5, 8, 6, 9, 7, 6, 7, 5, 5, 8, 13, 13, 5, 4, 5, 4, 5, 9, 5, 6, 7, 8, 2, 5, 7, 3, 8, 5, 7, 9,
    5, 7, 8, 6, 6, 7, 8, 7, 7, 7, 8, 8, 6, 5, 6, 8, 8, 0, 1, 6, 4, 5, 6, 8, 5, 4, 9, 4, 6, 9, 8, 8, 6, 5, 6, 5, 6, 6, 8, 9, 6, 2, 6, 7, 7, 3, 3, 4, 3, 11,
    3, 7, 5, 10, 7, 6, 4, 6, 8, 11, 11, 5, 7, 3, 8, 5, 5, 5, 2, 4, 1, 2, 3, 5, 5, 6, 7, 5, 3, 3, 3, 5, 8, 5, 4, 2, 7, 6, 10, 3, 6, 3, 7, 4, 4, 6, 8, 4, 6, 6,
    8, 5, 6, 7, 3, 6, 6, 3, 9, 6, 7, 6, 7, 3, 4, 6, 5, 10, 6, 7, 4, 5, 4, 6, 5, 8, 3, 5, 3, 7, 6, 5, 4, 1, 5, 7, 5, 9, 6, 6, 2, 7, 4, 4, 3, 4, 6, 3, 4, 3,
    3, 4, 4, 5, 7, 3, 6, 4, 9, 7, 1, 3, 2, 4, 6, 3, 6, 4, 7, 6, 6, 5, 6, 5, 6, 7, 5, 5, 1, 3, 5, 7, 7, 3, 5, 4, 2, 6, 4, 6, 4, 6, 3, 3, 2, 3, 1, 8, 5, 4,
    2, 5, 5, 5, 2, 4, 8, 2, 3, 8, 1, 7, 5, 2, 5, 3, 1, 4, 5, 4, 3, 5, 2, 3, 7, 5, 5, 6, 2, 2, 8, 4, 3, 5, 2, 5, 2, 6, 4, 1, 8, 7, 4, 5, 2, 2, 4, 2, 6, 8,
    8, 2, 4, 4, 2, 3, 6, 1, 3, 4, 4, 3, 4, 3, 1, 2, 2, 6, 2, 2, 5, 3, 7, 7, 1, 4, 3, 3, 4, 6, 4, 3, 10, 4, 5, 3, 3, 7, 4, 3, 3, 2, 6, 3, 5, 2, 3, 4, 6, 4,
    6, 8, 2, 3, 5, 6, 4, 3, 4, 8, 4, 7, 4, 3, 3, 4, 4, 2, 3, 1, 4, 3, 2, 6, 1, 2, 4, 3, 6, 4, 3, 5, 3, 3, 3, 8, 3, 1, 4, 5, 5, 3, 3, 4, 7, 4, 3, 0, 1, 1,
    2, 4, 2, 4, 5, 4, 2, 1, 1, 3, 6, 8, 1, 2, 4, 3, 8, 4, 3, 8, 11, 11, 6, 17, 5, 7, 7, 5, 3, 3, 4, 5, 4, 3, 1, 3, 7, 8, 4, 2, 5, 5, 7, 8, 3, 6, 2, 0, 1, 1,
    7, 3, 4, 3, 2, 3, 4, 6, 2, 3, 2, 5, 5, 5, 4, 4, 4, 5, 2, 4, 5, 4, 3, 2, 5, 3, 3, 3, 2, 8, 5, 6, 2, 2, 1, 6, 3, 3, 5, 1, 3, 6, 3, 2, 5, 4, 9, 3, 6, 2,
    2, 4, 1, 6, 1, 3, 6, 3, 3, 2, 2, 0, 1, 2, 3, 2, 2, 5, 2, 4, 4, 4, 2, 6, 1, 3, 3, 2, 4, 3, 1, 6, 5, 2, 1, 2, 2, 4, 2, 3, 6, 4, 3, 1, 3, 1, 1, 3, 1, 1,
    5, 4, 1, 5, 7, 5, 1, 2, 1, 3, 5, 5, 5, 3, 5, 3, 2, 2, 4, 3, 2, 4, 5, 2, 3, 1, 4, 4, 2, 4, 4, 2, 4, 3, 5, 2, 5, 0, 1, 3, 2, 6, 3, 4, 3, 0, 1, 9, 2, 2,
    5, 2, 2, 2, 1, 1, 7, 3, 1, 6, 3, 3, 4, 1, 5, 3, 3, 1, 2, 3, 4, 8, 17, 13, 15, 12, 7, 9, 2, 4, 2, 2, 5, 2, 3, 5, 3, 2, 3, 6, 3, 1, 5, 4, 1, 4, 4, 1, 3, 2,
    1, 2, 4, 4, 3, 2, 2, 4, 1, 7, 2, 3, 2, 1, 3, 0, 1, 1, 0, 1, 6, 4, 2, 0, 1, 1, 2, 3, 3, 3, 2, 5, 2, 2, 3, 3, 2, 3, 5, 3, 4, 3, 2, 5, 4, 17, 17, 34, 21, 22,
    7, 5, 4, 2, 1, 3, 1, 3, 3, 4, 5, 3, 1, 1, 2, 1, 2, 2, 1, 2, 2, 4, 3, 3, 1, 2, 1, 2, 5, 2, 0, 1, 1, 1, 1, 2, 4, 2, 2, 3, 3, 1, 3, 2, 1, 3, 2, 2, 3, 1,
    2, 4, 4, 6, 2, 4, 2, 3, 4, 3, 5, 3, 3, 3, 1, 2, 0, 1, 2, 0, 1, 1, 3, 0, 1, 1, 4, 0, 1, 4, 1, 4, 4, 2, 4, 1, 5, 1, 2, 2, 4, 6, 2, 2, 3, 2, 1, 1, 1, 2,
    5, 1, 0, 1, 3, 4, 5, 2, 2, 2, 2, 1, 1, 3, 1, 4, 2, 4, 4, 0, 2, 4, 0, 1, 1, 7, 3, 4, 1, 1, 3, 0, 1, 1, 1, 2, 1, 4, 2, 3, 4, 1, 3, 3, 6, 4, 1, 2, 3, 5,
    4, 2, 1, 3, 2, 2, 2, 2, 5, 4, 4, 2, 0, 2, 1, 2, 2, 6, 1, 1, 5, 2, 1, 5, 3, 2, 2, 3, 4, 1, 1, 6, 1, 5, 3, 2, 6, 0, 1, 1, 1, 1, 1, 2, 4, 3, 0, 1, 1, 2,
    2, 3, 3, 3, 3, 1, 2, 0, 2, 3, 3, 4, 2, 3, 6, 1, 2, 2, 3, 3, 2, 0, 1, 2, 1, 0, 1, 2, 4, 3, 3, 0, 1, 5, 2, 2, 2, 6, 1, 1, 1, 0, 1, 3, 1, 4, 3, 0, 1, 4,
    5, 2, 3, 5, 2, 1, 3, 5, 3, 3, 2, 1, 3, 3, 4, 2, 3, 1, 6, 1, 4, 4, 5, 2, 2, 1, 1, 2, 3, 2, 0, 1, 4, 2, 0, 1, 3, 2, 0, 1, 1, 4, 2, 3, 1, 3, 0, 2, 1, 2,
    1, 4, 2, 3, 0, 1, 1, 2, 4, 3, 1, 1, 0, 1, 2, 1, 1, 4, 1, 1, 2, 2, 3, 2, 0, 1, 2, 1, 3, 2, 3, 0, 1, 3, 4, 2, 2, 3, 5, 3, 2, 2, 7, 2, 3, 3, 1, 3, 1, 4,
    3, 1, 3, 2, 1, 0, 1, 4, 2, 3, 3, 2, 4, 2, 2, 0, 1, 2, 1, 0, 1, 1, 4, 4, 1, 0, 1, 2, 3, 3, 2, 2, 3, 0, 1, 2, 2, 2, 2, 1, 1, 3, 1, 1, 0, 2, 2, 2, 6, 2,
    2, 2, 1, 1, 3, 2, 3, 2, 4, 3, 2, 2, 0, 1, 2, 3, 1, 4, 1, 2, 0, 1, 1, 3, 6, 3, 3, 3, 6, 1, 1, 0, 2, 2, 1, 0, 1, 1, 1, 1, 2, 2, 1, 2, 1, 3, 2, 4, 3, 0,
    1, 1, 0, 1, 4, 3, 4, 1, 4, 2, 3, 1, 4, 1, 1, 2, 5, 4, 2, 0, 1, 1, 4, 5, 1, 0, 1, 2, 2, 2, 5, 0, 1, 1, 2, 4, 0, 2, 3, 2, 2, 0, 1, 2, 2, 2, 0, 1, 3, 1,
    2, 3, 0, 1, 2, 0, 1, 1, 0, 1, 3, 4, 3, 1, 2, 1, 4, 1, 3, 2, 4, 2, 2, 2, 3, 1, 2, 2, 4, 1, 3, 0, 1, 2, 3, 1, 1, 4, 2, 1, 1, 4, 0, 1, 5, 1, 3, 2, 8, 2,
    2, 1, 1, 1, 2, 2, 1, 0, 1, 2, 2, 3, 3, 4, 0, 1, 3, 0, 1, 2, 0, 1, 1, 1, 1, 1, 4, 2, 0, 2, 3, 3, 3, 3, 1, 1, 3, 2, 2, 1, 1, 3, 0, 1, 1, 3, 2, 5, 2, 0,
    1, 2, 2, 4, 3, 1, 0, 1, 3, 1, 0, 2, 2, 1, 2, 1, 0, 1, 2, 1, 1, 2, 2, 1, 4, 1, 1, 0, 1, 2, 3, 1, 1, 3, 1, 2, 1, 1, 0, 1, 2, 2, 3, 1, 0, 1, 3, 2, 2, 0,
    1, 5, 2, 5, 4, 3, 4, 2, 1, 4, 3, 0, 1, 3, 1, 1, 0, 1, 2, 0, 1, 2, 1, 3, 4, 4, 0, 1, 2, 0, 2, 2, 2, 1, 4, 4, 1, 3, 2, 0, 2, 3, 2, 1, 0, 2, 1, 6, 0, 2,
    4, 0, 2, 2, 1, 1, 1, 0, 1, 2, 0, 2, 2, 0, 1, 3, 1, 1, 0, 1, 2, 2, 2, 1, 0, 1, 2, 3, 0, 2, 1, 1, 1, 1, 0, 1, 4, 0, 1, 2, 0, 1, 4, 0, 1, 2, 2, 4, 4, 2,
    4, 3, 3, 0, 1, 2, 0, 1, 1, 0, 1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 0, 2, 2, 1, 1, 1, 3, 1, 0, 1, 2, 1, 0, 1, 2, 4, 1, 1, 3, 2, 0, 1, 2, 0, 1, 3, 3, 1, 2, 1,
    2, 1, 4, 1, 2, 1, 3, 0, 1, 1, 3, 1, 3, 4, 3, 2, 1, 1, 4, 2, 4, 3, 3, 2, 2, 0, 1, 1, 0, 1, 1, 5, 0, 1, 2, 2, 2, 1, 2, 0, 2, 1, 5, 1, 2, 2, 0, 1, 2, 1,
    2, 2, 0, 1, 3, 2, 1, 1, 2, 0, 1, 3, 1, 2, 3, 1, 0, 1, 2, 0, 1, 2, 1, 0, 2, 1, 1, 2, 1, 2, 1, 0, 1, 4, 2, 0, 1, 3, 1, 0, 1, 1, 1, 0, 1, 1, 2, 2, 0, 1,
    3, 1, 0, 1, 4, 3, 3, 3, 10, 8, 9, 4, 9, 4, 3, 1, 2, 1, 1, 1, 2, 1, 0, 1, 2, 4, 1, 0, 1, 3, 3, 1, 0, 1, 2, 3, 1, 0, 1, 1, 1, 1, 1, 2, 3, 1, 2, 1, 2, 1,
    2, 0, 1, 2, 0, 1, 3, 3, 1, 1, 4, 0, 1, 2, 2, 1, 5, 0, 1, 2, 1, 0, 4, 2, 1, 1, 4, 0, 1, 4, 3, 3, 3, 1, 2, 1, 1, 1, 1, 1, 2, 2, 2, 1, 3, 4, 1, 0, 1, 1,
    2, 4, 1, 1, 1, 3, 0, 1, 1, 0, 1, 3, 1, 1, 5, 2, 2, 2, 0, 1, 1, 0, 2, 1, 3, 0, 1, 1, 1, 1, 2, 3, 2, 1, 2, 0, 2, 1, 0, 1, 1, 1, 0, 1, 2, 2, 1, 1, 1, 0,
    1, 1, 0, 1, 1, 2, 1, 1, 4, 1, 0, 1, 1, 1, 0, 1, 1, 2, 1, 2, 1, 2, 1, 3, 3, 4, 7, 1, 2, 0, 2, 1, 1, 3, 4, 7, 3, 11, 8, 4, 2, 5, 4, 1, 1, 0, 2, 2, 2, 1,
    0, 1, 2, 0, 1, 1, 2, 1, 3, 0, 1, 2, 2, 0, 3, 1, 1, 1, 1, 0, 1, 2, 1, 1, 0, 1, 3, 1, 0, 1, 2, 0, 1, 1, 0, 1, 1, 3, 1, 0, 1, 3, 4, 3, 1, 0, 2, 2, 1, 2,
    1, 1, 0, 1, 4, 2, 1, 1, 0, 2, 1, 2, 0, 3, 2, 1, 5, 1, 1, 0, 1, 1, 3, 0, 2, 1, 0, 1, 2, 1, 2, 1, 4, 3, 3, 3, 4, 2, 1, 0, 3, 2, 3, 2, 0, 2, 1, 0, 2, 2,
    0, 1, 1, 0, 1, 1, 3, 1, 2, 2, 0, 4, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 0, 2, 1, 1, 0, 1, 3, 2, 1, 3, 2, 1, 3, 1, 1, 0, 1, 2, 1, 2, 3, 0, 2, 1, 2, 2, 1, 0,
    1, 1, 1, 0, 1, 1, 2, 4, 2, 0, 1, 1, 0, 2, 1, 1, 1, 0, 2, 1, 1, 0, 1, 1, 1, 1, 2, 1, 1, 1, 0, 1, 4, 2, 1, 3, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0,
    1, 1, 0, 3, 1, 1, 0, 1, 2, 0, 1, 2, 1, 2, 3, 2, 2, 1, 2, 4, 0, 1, 1, 2, 0, 1, 1, 3, 2, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 2, 1, 3, 1, 1, 0, 2, 1, 0,
    2, 1, 1, 2, 1, 4, 0, 2, 2, 5, 4, 2, 0, 1, 2, 0, 1, 2, 4, 4, 2, 0, 1, 2, 0, 2, 1, 1, 2, 1, 1, 1, 1, 2, 3, 1, 1, 2, 3, 1, 0, 1, 3, 0, 1, 2, 1, 0, 2, 3,
    0, 1, 1, 1, 2, 0, 1, 2, 1, 0, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 1, 2, 1, 2, 2, 0, 1, 2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 3, 1, 0, 1, 2, 1, 0, 1, 2, 2, 1, 2, 1,
    3, 2, 1, 0, 1, 2, 0, 1, 1, 0, 1, 1, 3, 2, 0, 1, 1, 1, 0, 2, 1, 2, 1, 3, 0, 2, 1, 1, 1, 1, 1, 0, 1, 2, 1, 1, 0, 3, 1, 2, 1, 0, 1, 2, 0, 1, 1, 0, 1, 1,
    3, 2, 2, 0, 1, 1, 0, 1, 1, 0, 3, 2, 4, 1, 1, 2, 0, 2, 1, 2, 0, 3, 1, 3, 1, 3, 2, 2, 1, 1, 3, 2, 2, 3, 1, 2, 0, 1, 1, 0, 1, 2, 0, 1, 1, 2, 2, 7, 8, 7,
    6, 2, 8, 2, 0, 1, 1, 2, 2, 1, 0, 1, 1, 2, 0, 5, 2, 1, 2, 2, 0, 1, 3, 1, 1, 2, 0, 1, 3, 1, 2, 0, 1, 2, 0, 2, 2, 0, 1, 1, 1, 0, 1, 3, 2, 2, 1, 0, 2, 1,
    3, 1, 1, 0, 2, 2, 2, 1, 1, 0, 1, 3, 3, 0, 1, 1, 2, 1, 1, 1, 0, 1, 2, 1, 2, 1, 1, 3, 1, 1, 3, 0, 3, 2, 0, 1, 3, 1, 0, 5, 2, 0, 1, 1, 1, 4, 1, 0, 1, 1,
    2, 3, 2, 2, 0, 1, 2, 0, 1, 3, 2, 0, 1, 1, 3, 0, 1, 2, 0, 1, 3, 1, 2, 1, 3, 2, 0, 2, 1, 0, 2, 3, 0, 1, 1, 1, 2, 1, 4, 1, 1, 0, 1, 2, 2, 1, 3, 0, 1, 2,
    2, 1, 2, 0, 1, 1, 3, 1, 3, 2, 1, 3, 0, 2, 4, 2, 3, 1, 2, 0, 2, 2, 2, 1, 2, 1, 1, 1, 1, 0, 1, 2, 0, 1, 1, 2, 2, 1, 2, 1, 4, 0, 1, 2, 0, 1, 2, 0, 1, 2,
    3, 2, 0, 1, 4, 1, 0, 1, 1, 0, 3, 2, 0, 1, 1, 1, 2, 1, 1, 3, 3, 1, 2, 0, 1, 2, 0, 1, 1, 1, 2, 3, 0, 1, 1, 1, 3, 1, 0, 1, 3, 0, 1, 1, 0, 1, 1, 3, 0, 1,
    2, 0, 1, 1, 1, 1, 0, 3, 1, 2, 1, 1, 1, 1, 1, 2, 1, 4, 3, 0, 1, 2, 1, 2, 0, 1, 2, 3, 1, 1, 2, 4, 0, 1, 4, 7, 1, 1, 1, 0, 1, 1, 0, 1, 4, 1, 0, 2, 3, 3,
    1, 3, 1, 0, 1, 1, 0, 2, 2, 1, 1, 1, 2, 1, 3, 0, 1, 1, 1, 0, 1, 1, 2, 3, 1, 3, 0, 1, 2, 2, 2, 1, 1, 0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 1, 1, 3, 1, 1, 0, 1,
    2, 1, 3, 2, 2, 2, 2, 0, 1, 2, 2, 1, 2, 4, 4, 4, 2, 0, 1, 1, 3, 2, 3, 3, 4, 1, 2, 1, 2, 0, 2, 1, 2, 3, 0, 1, 2, 0, 1, 1, 1, 2, 0, 1, 2, 1, 1, 1, 0, 1,
    2, 2, 0, 1, 1, 0, 1, 2, 1, 1, 1, 1, 0, 1, 1, 1, 2, 0, 1, 1, 0, 2, 2, 2, 2, 0, 1, 2, 1, 1, 3, 3, 0, 7, 2, 2, 0, 2, 1, 4, 1, 2, 3, 2, 1, 0, 2, 2, 2, 1,
    0, 1, 1, 0, 6, 1, 1, 1, 2, 2, 2, 1, 0, 3, 1, 1, 0, 1, 1, 0, 3, 1, 1, 2, 0, 2, 1, 2, 1, 0, 1, 1, 0, 1, 3, 1, 2, 1, 1, 1, 1, 2, 2, 0, 4, 1, 1, 1, 2, 3,
    1, 0, 2, 2, 4, 1, 4, 0, 1, 1, 1, 1, 0, 2, 1, 2, 1, 1, 1, 0, 1, 1, 0, 4, 2, 0, 3, 1, 1, 2, 1, 1, 2, 0, 1, 2, 0, 2, 1, 0, 4, 2, 1, 1, 1, 0, 1, 1, 2, 2,
    1, 1, 2, 1, 0, 4, 1, 2, 0, 2, 2, 2, 1, 1, 1, 3, 1, 0, 1, 2, 1, 0, 3, 1, 1, 2, 2, 2, 0, 1, 1, 0, 3, 1, 0, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 2, 2, 0, 3, 4,
    1, 1, 2, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 2, 2, 2, 1, 1, 1, 0, 1, 3, 0, 3, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 3, 1, 1, 1, 3, 0,
    3, 2, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 8, 1, 2, 3, 0, 3, 1, 1, 0, 5, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 2, 1, 0, 3, 2, 0, 2, 1, 0, 4, 2, 0, 2, 2, 0, 2, 1,
    1, 1, 1, 2, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 4, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 3, 1, 0, 6, 1, 1, 1, 1, 2, 2, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1,
    0, 3, 3, 1, 1, 1, 0, 1, 1, 2, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 5, 1, 0, 4, 1, 1, 0, 2, 1, 0, 2, 1, 1, 1, 0, 2, 1, 0, 3, 1, 0, 2, 1, 1, 1, 2, 0, 1, 2, 0,
    1, 1, 2, 1, 2, 0, 2, 2, 0, 1, 1, 0, 2, 1, 0, 2, 3, 0, 2, 1, 2, 0, 1, 1, 0, 2, 2, 1, 0, 3, 1, 0, 1, 1, 1, 1, 0, 1, 1, 2, 0, 1, 1, 0, 1, 2, 0, 1, 1, 1,
    1, 1, 1, 0, 4, 2, 0, 3, 1, 2, 0, 2, 1, 1, 0, 3, 1, 1, 1, 1, 0, 3, 1, 3, 0, 1, 1, 1, 1, 1, 1, 0, 4, 1, 1, 0, 2, 1, 1, 0, 3, 1, 2, 1, 0, 4, 1, 0, 3, 1,
    0, 1, 1, 0, 1, 1, 0, 3, 2, 2, 0, 4, 2, 0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 9, 2, 1, 2, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 2, 2, 1, 5, 4, 15, 21, 31, 54,
    39, 49, 37, 23, 13, 3, 1, 0, 1, 1, 1, 0, 4, 1, 1, 0, 15, 1, 0, 1, 1, 0, 15, 1, 1, 0, 3, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 0, 2, 1, 0, 3, 1, 0,
    4, 1, 1, 0, 3, 1, 0, 7, 1, 0, 7, 1, 0, 3, 1, 2, 1, 0, 6, 1, 0, 2, 1, 0, 2, 1, 1, 0, 7, 1, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 7, 2, 0, 4, 1, 0, 6, 1, 0,
    4, 1, 1, 1, 0, 7, 2, 0, 4, 1, 0, 16, 1, 0, 2, 1, 0, 2, 1, 2, 0, 7, 1, 0, 8, 1, 0, 1, 1, 0, 3, 1, 1, 0, 1, 1, 0, 5, 1, 0, 1, 2, 0, 2, 1, 0, 10, 2, 0, 1,
    1, 0, 5, 1, 0, 2, 1, 0, 1, 1, 0, 8, 1, 0, 10, 1, 0, 7, 1, 0, 9, 1, 0, 3, 1, 0, 12, 1, 0, 3, 1, 0, 2, 2, 0, 2, 1, 0, 3, 1, 0, 4, 1, 0, 10, 1, 1, 0, 1, 1,
    0, 5, 2, 3, 0, 3, 1, 0, 1, 1, 1, 1, 0, 4, 1, 0, 5, 1, 0, 2, 1, 0, 7, 1, 1, 0, 10, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 0, 1, 1, 1, 0, 1, 1,
    0, 4, 1, 0, 5, 1, 0, 2, 1, 2, 0, 1, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 5, 1, 0, 3, 1, 1, 0, 1, 2, 0, 1, 1, 0, 1, 1, 0, 14, 1, 1, 0, 2, 1, 0, 3, 1, 0, 6,
    1, 0, 8, 1, 0, 1, 1, 0, 6, 1, 2, 0, 2, 1, 1, 0, 12, 1, 0, 3, 1, 0, 7, 1, 0, 2, 2, 0, 1, 2, 0, 1, 1, 0, 6, 1, 1, 0, 10, 1, 1, 0, 11, 1, 0, 1, 1, 0, 4, 1,
    1, 0, 1, 1, 1, 0, 12, 2, 0, 5, 1, 2, 0, 10, 1, 1, 0, 3, 1, 0, 19, 1, 0, 8, 1, 0, 6, 1, 0, 1, 1, 0, 1, 1, 0, 6, 1, 1, 0, 16, 1, 0, 1, 2, 0, 13, 1, 0, 8, 1,
    0, 5, 1, 1, 0, 1, 2, 0, 1, 1, 0, 2, 1, 1, 0, 2, 2, 0, 5, 3, 0, 2, 1, 0, 2, 1, 1, 0, 1, 1, 0, 9, 1, 0, 9, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 0, 4, 1, 0, 4,
    1, 0, 1, 1, 0, 10, 1, 0, 4, 1, 0, 7, 3, 3, 3, 4, 7, 2, 5, 4, 2, 1, 1, 0, 1, 1, 0, 4, 1, 0, 11, 1, 0, 1, 1, 0, 5, 1, 0, 6, 1, 0, 4, 1, 0, 15, 1, 0, 2, 2,
    0, 1, 1, 0, 7, 1, 0, 8, 1, 0, 8, 1, 0, 4, 1, 0, 7, 1, 0, 16, 2, 0, 6, 1, 0, 14, 1, 0, 12, 1, 0, 1, 1, 1, 0, 22, 1, 0, 8, 1, 0, 10, 1, 0, 1, 1, 0, 1, 1, 0,
    5, 1, 0, 4, 1, 0, 4, 1, 0, 2, 1, 1, 1, 0, 13, 1, 1, 0, 3, 1, 0, 7, 1, 0, 32, 1, 0, 17, 1, 0, 4, 1, 0, 1, 1, 0, 8, 1, 0, 2, 1, 0, 7, 1, 0, 4, 1, 0, 4, 1,
    0, 1, 2, 0, 3, 1, 0, 6, 1, 0, 6, 2, 0, 5, 1, 1, 0, 9, 1, 0, 1, 2, 0, 20, 1, 0, 6, 1, 0, 1, 2, 0, 4, 1, 0, 4, 1, 0, 2, 1, 0, 14, 1, 0, 13, 1, 0, 8, 1, 0,
    12, 1, 2, 0, 1, 1, 0, 19, 1, 0, 9, 1, 0, 1, 2, 1, 0, 20, 1, 0, 2, 2, 0, 1, 2, 0, 1, 1, 0, 9, 1, 0, 3, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 0, 10, 2, 0, 5, 2, 0,
    3, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 15, 1, 0, 5, 1, 0, 1, 1, 1, 0, 3, 1, 0, 2, 1, 0, 2, 1, 0, 26, 1, 0, 23, 1, 0, 13, 1, 0, 17, 1, 0, 3, 1, 0, 11, 1, 0, 61,
    1, 0, 5, 1, 0, 7, 1, 0, 4, 1, 0, 11, 1, 0, 2, 1, 0, 9, 1, 0, 14, 1, 0, 3, 1, 0, 25, 1, 0, 2, 2, 0, 9, 1, 0, 4, 1, 0, 5, 1, 0, 6, 1, 0, 2, 2, 0, 5, 1, 2,
    0, 2, 1, 0, 15, 1, 0, 1, 2, 0, 7, 1, 0, 7, 1, 1, 0, 5, 1, 0, 4, 1, 1, 0, 1, 1, 0, 8, 1, 0, 12, 1, 0, 33, 1, 2, 0, 4, 2, 0, 20, 1, 0, 21, 1, 0, 10, 1, 0, 4,
    1, 0, 6, 1, 0, 3, 1, 0, 5, 1, 0, 18, 1, 1, 0, 4, 1, 0, 14, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 3, 2, 0, 13, 2, 0, 1, 1, 0, 2, 1, 0, 4, 2, 2, 0, 1, 1, 2, 2,
    1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 1, 0, 26, 1, 0, 12, 1, 0, 2, 2, 0, 21, 1, 0, 1, 1, 0, 4, 1, 0, 7, 1, 0, 1, 1, 3, 1, 0, 1, 1, 0, 1, 1, 0, 7, 1, 0, 2, 1, 0,
    15, 1, 0, 16, 1, 0, 4, 1, 0, 4, 1, 0, 4, 1, 0, 24, 1, 0, 1, 1, 0, 9, 1, 0, 1, 2, 0, 14, 1, 0, 4, 1, 0, 7, 1, 0, 3, 1, 0, 7, 1, 0, 6, 1, 0, 2, 1, 0, 6, 1,
    0, 15, 1, 0, 1, 1, 1, 0, 4, 1, 0, 6, 1, 0, 8, 1, 0, 1, 1, 0, 6, 1, 1, 1, 0, 17, 1, 0, 11, 1, 0, 3, 1, 0, 12, 1, 0, 4, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 0, 2,
    1, 0, 19, 1, 0, 3, 1, 0, 10, 1, 0, 4, 1, 0, 1, 3, 0, 5, 1, 1, 0, 6, 1, 0, 11, 1, 2, 0, 9, 1, 0, 3, 1, 0, 18, 1, 0, 15, 1, 0, 12, 1, 0, 7, 1, 1, 0, 2, 1, 0,
    12, 1, 0, 7, 1, 0, 15, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 23, 1, 0, 2, 1, 0, 12, 1, 0, 7, 1, 0, 3, 1, 0, 12, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 71, 1, 0, 6, 1,
    0, 6, 1, 0, 4, 1, 0, 8, 1, 0, 7, 1, 0, 7, 1, 1, 0, 16, 1, 0, 23, 1, 1, 0, 44, 1, 0, 30, 1, 0, 8, 1, 0, 40, 1, 0, 8, 1, 0, 4, 1, 0, 30, 1, 0, 22, 1, 0, 66, 1,
    0, 4, 1, 0, 54, 1, 0, 1, 1, 2, 2, 2, 6, 6, 8, 3, 4, 4, 5, 2, 0, 53, 1, 0, 251, 1, 0, 55, 1, 0, 15, 1, 0, 20, 1, 0, 24, 1, 0, 13, 1, 0, 126, 1, 0, 44, 1, 0, 123, 1,
    0, 10, 1, 0, 43, 1, 0, 16, 1, 0, 188, 1, 0, 20, 1, 0, 1, 1, 0, 30
  };
  assert( test_9_packed.size() == 6770 );
  const vector<uint8_t> test_9_encoded = QRSpecDev::encode_stream_vbyte( test_9_chan_cnts );
  assert( test_9_encoded == test_9_packed );
  vector<uint32_t> test_9_dec;
  const size_t test_9_nbytedec = QRSpecDev::decode_stream_vbyte(test_9_encoded,test_9_dec);
  assert( test_9_nbytedec == test_9_packed.size() );
  assert( test_9_dec == test_9_chan_cnts );
  
  
  
  
  // Test case 10
  const vector<uint32_t> test_10_chan_cnts{
    0, 40, 24, 50, 50, 56, 56, 52, 48, 54, 57, 46, 40, 30, 36, 37, 48, 35, 42, 39,
    42, 35, 38, 32, 44, 37, 43, 48, 43, 38, 36, 33, 38, 42, 40, 28, 37, 33, 30, 37,
    29, 53, 29, 26, 29, 35, 32, 27, 38, 33, 34, 33, 34, 38, 33, 42, 21, 36, 32, 37,
    28, 29, 39, 31, 24, 41, 42, 24, 34, 31, 35, 28, 36, 40, 40, 24, 25, 24, 41, 32,
    30, 39, 31, 28, 27, 25, 30, 29, 31, 24, 32, 41, 29, 32, 23, 25, 31, 33, 34, 32,
    32, 28, 32, 22, 34, 37, 29, 37, 38, 27, 23, 38, 34, 35, 35, 36, 28, 38, 36, 42,
    36, 40, 49, 39, 32, 45, 38, 42, 26, 35, 49, 53, 35, 39, 47, 55, 56, 54, 44, 43,
    40, 47, 61, 40, 61, 63, 67, 58, 66, 70, 68, 64, 55, 49, 47, 43, 35, 66, 63, 53,
    66, 46, 47, 61, 61, 57, 63, 59, 67, 59, 69, 67, 82, 61, 66, 68, 65, 75, 62, 62,
    55, 54, 51, 51, 68, 65, 60, 55, 54, 50, 71, 75, 54, 64, 60, 78, 64, 65, 70, 59,
    77, 69, 59, 59, 77, 73, 56, 60, 73, 71, 84, 67, 73, 67, 84, 82, 73, 63, 58, 66,
    68, 70, 75, 79, 74, 63, 81, 67, 60, 65, 82, 79, 84, 84, 79, 61, 67, 58, 67, 64,
    90, 72, 70, 83, 77, 78, 85, 91, 84, 61, 95, 58, 98, 78, 84, 67, 78, 79, 81, 91,
    80, 90, 89, 81, 73, 72, 82, 75, 87, 79, 104, 82, 86, 69, 77, 83, 97, 76, 80, 79,
    68, 80, 84, 76, 77, 65, 73, 88, 65, 76, 69, 77, 79, 76, 72, 80, 88, 52, 81, 83,
    72, 62, 74, 67, 72, 68, 78, 79, 70, 73, 80, 72, 61, 71, 93, 70, 70, 82, 67, 74,
    77, 70, 72, 84, 85, 78, 79, 80, 75, 58, 66, 75, 75, 72, 63, 61, 73, 67, 75, 87,
    73, 72, 73, 72, 63, 64, 64, 66, 63, 63, 58, 69, 77, 86, 119, 98, 92, 66, 65, 63,
    66, 73, 74, 54, 70, 62, 73, 69, 74, 65, 56, 74, 69, 67, 65, 54, 67, 67, 58, 69,
    70, 62, 58, 65, 51, 62, 68, 58, 54, 56, 60, 68, 57, 65, 49, 81, 72, 57, 70, 69,
    67, 69, 62, 67, 71, 69, 80, 83, 83, 96, 101, 75, 63, 57, 62, 68, 71, 68, 68, 61,
    70, 56, 71, 57, 46, 69, 71, 70, 64, 68, 58, 79, 66, 76, 66, 58, 52, 60, 62, 56,
    61, 56, 60, 70, 71, 73, 60, 68, 56, 52, 65, 86, 61, 72, 55, 50, 65, 64, 57, 67,
    75, 48, 75, 66, 69, 72, 104, 199, 431, 575, 601, 328, 157, 55, 36, 37, 36, 27, 27, 25,
    41, 37, 30, 26, 33, 25, 40, 29, 31, 29, 41, 36, 24, 22, 37, 41, 34, 17, 32, 31,
    28, 23, 25, 24, 20, 43, 28, 14, 21, 29, 22, 32, 26, 49, 33, 38, 36, 23, 27, 27,
    43, 51, 65, 96, 84, 55, 37, 27, 20, 23, 27, 24, 30, 20, 17, 31, 22, 23, 20, 27,
    24, 14, 24, 21, 27, 24, 17, 24, 17, 24, 29, 23, 19, 22, 28, 14, 31, 29, 22, 21,
    19, 23, 20, 14, 22, 25, 26, 31, 19, 21, 26, 18, 19, 23, 22, 26, 28, 19, 18, 16,
    20, 23, 25, 29, 22, 24, 22, 21, 21, 14, 22, 20, 16, 22, 17, 15, 14, 21, 18, 16,
    21, 25, 24, 21, 10, 21, 16, 19, 21, 12, 20, 30, 28, 53, 36, 42, 29, 25, 21, 19,
    25, 17, 27, 24, 18, 17, 18, 15, 12, 21, 21, 14, 11, 13, 14, 11, 11, 22, 21, 17,
    13, 15, 14, 20, 18, 18, 12, 9, 18, 6, 17, 18, 26, 19, 14, 13, 16, 18, 16, 24,
    20, 12, 8, 6, 18, 22, 16, 14, 13, 11, 16, 14, 3, 12, 17, 10, 10, 16, 12, 10,
    10, 18, 19, 15, 13, 12, 12, 16, 10, 8, 14, 17, 13, 7, 13, 19, 13, 13, 17, 11,
    18, 14, 9, 14, 13, 12, 13, 11, 10, 10, 9, 8, 13, 13, 16, 13, 15, 10, 20, 15,
    14, 14, 11, 16, 6, 16, 9, 17, 14, 12, 10, 13, 4, 10, 8, 11, 13, 7, 13, 14,
    13, 12, 13, 14, 14, 19, 15, 12, 11, 13, 10, 6, 13, 21, 8, 9, 8, 10, 13, 10,
    16, 7, 12, 8, 9, 5, 15, 17, 21, 21, 19, 14, 14, 10, 15, 24, 14, 16, 10, 14,
    10, 10, 13, 6, 9, 14, 13, 9, 9, 7, 8, 12, 7, 10, 5, 9, 8, 12, 11, 9,
    6, 9, 11, 8, 8, 8, 10, 7, 5, 3, 4, 12, 8, 10, 9, 10, 8, 12, 13, 9,
    13, 7, 11, 10, 8, 7, 8, 6, 8, 5, 6, 11, 5, 12, 11, 13, 16, 6, 7, 7,
    7, 4, 9, 7, 10, 6, 13, 11, 13, 11, 12, 10, 3, 12, 10, 4, 9, 11, 9, 9,
    10, 11, 7, 9, 8, 10, 8, 11, 12, 5, 6, 13, 9, 12, 11, 6, 9, 10, 9, 8,
    12, 3, 5, 7, 5, 10, 19, 11, 10, 7, 14, 8, 8, 5, 10, 4, 6, 8, 7, 11,
    6, 6, 7, 8, 4, 5, 7, 12, 5, 6, 9, 13, 9, 9, 6, 13, 7, 6, 12, 6,
    10, 11, 25, 27, 21, 21, 15, 10, 10, 11, 6, 10, 4, 6, 8, 6, 4, 1, 7, 11,
    6, 3, 5, 9, 7, 11, 9, 3, 9, 8, 8, 10, 9, 8, 10, 9, 7, 10, 4, 11,
    1, 9, 12, 10, 10, 9, 4, 8, 8, 8, 7, 4, 8, 12, 5, 9, 4, 7, 7, 9,
    7, 5, 9, 5, 9, 9, 9, 10, 6, 9, 10, 8, 11, 5, 7, 4, 4, 9, 3, 6,
    2, 5, 8, 4, 4, 9, 8, 7, 4, 6, 6, 5, 3, 4, 5, 6, 3, 8, 8, 11,
    5, 9, 5, 3, 8, 13, 5, 7, 4, 7, 5, 9, 5, 4, 9, 5, 3, 5, 6, 6,
    6, 5, 7, 6, 2, 5, 6, 4, 6, 2, 5, 6, 9, 5, 6, 7, 8, 7, 6, 6,
    6, 9, 7, 6, 4, 13, 10, 7, 5, 5, 8, 7, 3, 4, 8, 4, 5, 4, 8, 8,
    6, 5, 9, 6, 9, 1, 9, 7, 3, 4, 3, 9, 11, 7, 7, 5, 7, 5, 7, 12,
    6, 1, 7, 7, 7, 6, 4, 7, 3, 8, 3, 6, 6, 6, 7, 7, 5, 5, 5, 6,
    6, 4, 2, 2, 14, 7, 6, 6, 2, 2, 6, 4, 4, 7, 10, 5, 3, 5, 7, 8,
    6, 5, 5, 7, 6, 5, 6, 2, 6, 4, 10, 3, 5, 3, 5, 2, 3, 5, 6, 6,
    4, 5, 2, 5, 1, 5, 7, 7, 6, 7, 3, 2, 2, 3, 6, 6, 4, 6, 6, 2,
    2, 2, 11, 3, 2, 3, 3, 7, 9, 4, 8, 3, 3, 11, 5, 3, 5, 6, 3, 5,
    6, 4, 2, 2, 4, 3, 4, 3, 5, 2, 3, 6, 5, 9, 4, 3, 3, 6, 9, 2,
    2, 2, 3, 6, 3, 4, 7, 7, 2, 4, 2, 5, 3, 8, 2, 2, 7, 2, 8, 2,
    3, 6, 4, 4, 3, 8, 6, 1, 2, 3, 3, 3, 7, 3, 5, 2, 4, 4, 2, 4,
    4, 5, 4, 3, 8, 7, 5, 2, 4, 6, 3, 5, 6, 2, 3, 2, 5, 3, 3, 8,
    4, 6, 3, 4, 1, 4, 4, 4, 5, 3, 5, 1, 3, 4, 1, 7, 4, 3, 1, 5,
    4, 8, 4, 2, 3, 5, 5, 2, 5, 5, 2, 6, 4, 5, 4, 6, 2, 3, 1, 4,
    4, 4, 5, 4, 1, 4, 7, 5, 3, 1, 7, 4, 1, 6, 2, 1, 4, 3, 1, 2,
    3, 2, 2, 4, 3, 1, 1, 1, 8, 8, 5, 7, 4, 8, 8, 5, 12, 15, 6, 4,
    15, 5, 4, 2, 1, 3, 5, 3, 4, 1, 3, 7, 4, 8, 4, 2, 2, 2, 5, 3,
    3, 5, 6, 4, 4, 2, 7, 1, 3, 2, 2, 4, 3, 4, 5, 3, 3, 3, 2, 2,
    3, 2, 3, 1, 3, 3, 1, 4, 4, 3, 3, 3, 1, 6, 5, 5, 4, 3, 1, 3,
    4, 4, 0, 1, 3, 1, 1, 2, 1, 4, 1, 0, 1, 5, 2, 3, 7, 1, 3, 5,
    1, 3, 0, 1, 3, 3, 3, 6, 3, 1, 3, 2, 6, 6, 2, 1, 4, 2, 3, 5,
    3, 1, 3, 1, 2, 2, 1, 1, 5, 3, 4, 2, 5, 5, 5, 3, 5, 3, 2, 2,
    4, 3, 2, 4, 2, 4, 1, 2, 3, 3, 6, 1, 3, 6, 0, 1, 6, 3, 3, 5,
    2, 2, 4, 3, 4, 6, 9, 3, 0, 1, 3, 1, 3, 2, 1, 7, 5, 5, 4, 3,
    4, 3, 2, 5, 2, 2, 2, 5, 3, 1, 2, 0, 1, 3, 3, 2, 5, 1, 5, 3,
    2, 2, 4, 5, 4, 2, 2, 1, 3, 0, 1, 3, 2, 2, 2, 5, 2, 1, 2, 4,
    11, 15, 12, 19, 13, 4, 5, 5, 0, 1, 5, 1, 0, 1, 3, 1, 2, 4, 5, 0,
    2, 2, 1, 2, 3, 1, 5, 2, 3, 2, 5, 2, 6, 5, 2, 0, 1, 4, 2, 3,
    3, 3, 3, 1, 2, 1, 3, 2, 1, 3, 3, 1, 2, 2, 4, 1, 2, 3, 2, 0,
    1, 1, 2, 0, 1, 3, 5, 4, 4, 1, 2, 3, 1, 4, 6, 2, 6, 17, 15, 39,
    12, 14, 7, 4, 4, 2, 3, 3, 3, 1, 4, 3, 3, 2, 4, 3, 4, 3, 3, 0,
    1, 7, 3, 1, 2, 4, 4, 2, 1, 2, 1, 8, 2, 2, 3, 3, 2, 3, 3, 3,
    3, 4, 3, 2, 0, 1, 1, 2, 4, 3, 4, 1, 1, 1, 2, 5, 3, 2, 4, 3,
    2, 4, 3, 4, 2, 0, 3, 2, 4, 5, 3, 4, 1, 2, 2, 2, 3, 3, 2, 2,
    4, 4, 2, 0, 1, 3, 2, 3, 4, 0, 1, 4, 4, 2, 3, 4, 0, 1, 4, 2,
    1, 1, 2, 1, 5, 1, 0, 1, 1, 7, 1, 2, 0, 1, 1, 2, 1, 2, 4, 5,
    1, 3, 2, 2, 5, 4, 5, 3, 1, 2, 4, 1, 4, 2, 2, 3, 3, 5, 3, 2,
    1, 2, 1, 1, 3, 0, 2, 1, 3, 2, 2, 1, 2, 3, 5, 5, 1, 6, 7, 1,
    4, 2, 3, 4, 4, 10, 3, 5, 1, 1, 0, 1, 2, 1, 3, 2, 3, 2, 1, 4,
    4, 0, 1, 1, 0, 1, 1, 1, 1, 4, 1, 4, 2, 1, 1, 3, 0, 1, 2, 3,
    1, 2, 3, 0, 1, 3, 0, 1, 2, 4, 3, 1, 1, 1, 3, 2, 0, 2, 3, 2,
    0, 1, 3, 3, 2, 2, 2, 1, 1, 3, 1, 2, 1, 2, 1, 1, 3, 4, 3, 1,
    0, 2, 5, 4, 2, 2, 1, 6, 1, 0, 1, 4, 0, 1, 3, 1, 2, 0, 1, 2,
    1, 1, 3, 3, 2, 6, 4, 3, 4, 1, 0, 1, 3, 1, 1, 3, 5, 4, 1, 0,
    1, 1, 1, 1, 2, 1, 1, 0, 1, 4, 1, 5, 1, 3, 3, 3, 4, 0, 1, 3,
    3, 1, 5, 1, 5, 3, 0, 1, 2, 1, 2, 0, 1, 1, 2, 1, 1, 1, 2, 1,
    1, 1, 2, 1, 2, 1, 2, 5, 3, 1, 4, 1, 1, 4, 1, 3, 2, 1, 2, 2,
    1, 4, 5, 2, 5, 4, 2, 3, 2, 5, 2, 1, 2, 3, 4, 3, 2, 3, 1, 2,
    0, 1, 3, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 1, 5, 5, 1, 3, 2,
    2, 2, 2, 1, 4, 1, 3, 1, 2, 1, 0, 2, 1, 3, 3, 0, 1, 2, 1, 3,
    2, 3, 0, 1, 1, 1, 0, 1, 1, 0, 1, 5, 2, 2, 2, 2, 0, 1, 1, 1,
    0, 1, 1, 1, 1, 3, 1, 4, 2, 2, 2, 3, 5, 3, 0, 1, 1, 2, 3, 3,
    2, 3, 3, 3, 1, 0, 1, 1, 2, 0, 1, 1, 2, 1, 1, 4, 2, 0, 1, 2,
    0, 1, 3, 2, 3, 1, 3, 6, 1, 1, 5, 2, 3, 2, 3, 2, 3, 4, 4, 1,
    1, 4, 3, 0, 1, 5, 2, 2, 1, 1, 1, 3, 1, 4, 0, 1, 1, 3, 7, 2,
    1, 2, 1, 0, 1, 2, 4, 0, 1, 1, 2, 0, 1, 1, 3, 1, 1, 4, 2, 2,
    1, 3, 9, 3, 2, 1, 3, 1, 1, 5, 0, 2, 1, 0, 1, 1, 1, 4, 2, 0,
    1, 1, 0, 1, 1, 1, 1, 1, 2, 1, 3, 3, 2, 1, 5, 0, 1, 2, 1, 0,
    1, 5, 4, 1, 0, 1, 3, 2, 0, 1, 3, 2, 1, 1, 0, 4, 2, 3, 1, 3,
    1, 5, 2, 5, 1, 2, 0, 1, 1, 3, 0, 1, 1, 2, 1, 1, 1, 0, 1, 1,
    1, 2, 1, 1, 2, 2, 3, 3, 1, 3, 0, 1, 3, 0, 1, 2, 5, 1, 0, 1,
    1, 2, 0, 1, 4, 2, 0, 1, 2, 3, 0, 1, 3, 0, 1, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 0, 2, 1, 1, 1, 3, 4, 1, 0, 1, 2, 0, 2, 1, 5, 0,
    1, 1, 2, 2, 0, 1, 3, 0, 1, 3, 0, 1, 1, 0, 1, 2, 1, 3, 2, 1,
    0, 1, 1, 1, 1, 5, 0, 1, 1, 1, 1, 0, 1, 2, 3, 2, 1, 2, 0, 1,
    1, 3, 1, 2, 0, 1, 1, 2, 0, 1, 1, 0, 1, 1, 0, 2, 1, 2, 0, 1,
    1, 1, 3, 3, 5, 3, 1, 1, 1, 1, 2, 1, 2, 3, 0, 1, 1, 0, 1, 3,
    0, 2, 3, 1, 1, 1, 2, 2, 4, 3, 5, 0, 1, 2, 2, 3, 3, 1, 1, 5,
    4, 2, 0, 1, 3, 3, 3, 2, 3, 0, 3, 2, 0, 2, 1, 2, 2, 1, 0, 1,
    1, 2, 2, 2, 2, 0, 1, 3, 1, 1, 1, 1, 2, 1, 0, 5, 1, 1, 0, 2,
    1, 2, 1, 0, 4, 1, 3, 2, 3, 1, 1, 1, 3, 3, 0, 1, 2, 1, 1, 1,
    4, 0, 1, 4, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 5, 0, 1, 1, 5,
    4, 1, 2, 2, 0, 1, 2, 3, 2, 1, 2, 1, 2, 0, 1, 4, 0, 1, 2, 3,
    1, 0, 1, 2, 4, 0, 1, 2, 1, 1, 0, 1, 3, 1, 1, 3, 0, 1, 3, 1,
    0, 1, 2, 0, 1, 2, 2, 1, 2, 2, 5, 0, 2, 2, 2, 1, 3, 4, 0, 1,
    6, 0, 2, 5, 1, 3, 4, 1, 14, 8, 7, 4, 5, 5, 0, 1, 1, 3, 0, 1,
    2, 1, 3, 1, 2, 0, 2, 2, 2, 7, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    1, 0, 1, 2, 2, 0, 1, 4, 1, 2, 1, 1, 0, 2, 3, 1, 0, 1, 2, 1,
    0, 2, 2, 1, 0, 1, 1, 2, 0, 1, 1, 1, 3, 2, 0, 1, 4, 0, 1, 1,
    5, 3, 1, 2, 4, 4, 1, 2, 1, 1, 0, 1, 3, 2, 0, 2, 1, 4, 1, 0,
    1, 1, 2, 0, 1, 2, 0, 1, 2, 2, 2, 3, 1, 1, 3, 1, 1, 2, 1, 1,
    0, 1, 3, 1, 2, 1, 2, 4, 2, 2, 0, 1, 2, 4, 1, 0, 3, 2, 3, 1,
    1, 2, 2, 0, 3, 2, 1, 0, 1, 1, 0, 3, 1, 1, 1, 2, 1, 1, 3, 3,
    3, 0, 1, 3, 3, 1, 6, 1, 4, 3, 1, 3, 4, 3, 2, 1, 1, 2, 1, 3,
    4, 0, 1, 7, 10, 8, 4, 5, 1, 1, 1, 0, 3, 2, 0, 1, 1, 1, 0, 3,
    1, 1, 3, 3, 0, 5, 1, 1, 0, 1, 1, 1, 1, 1, 2, 2, 1, 3, 1, 0,
    1, 3, 2, 4, 3, 3, 1, 1, 2, 1, 0, 3, 1, 5, 2, 2, 0, 2, 2, 1,
    1, 3, 0, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 3, 1, 1, 2, 2, 2, 1,
    0, 1, 2, 1, 1, 1, 2, 2, 4, 1, 2, 2, 2, 3, 4, 2, 3, 1, 1, 1,
    0, 1, 1, 1, 4, 3, 2, 0, 1, 1, 5, 4, 0, 1, 1, 1, 4, 2, 0, 2,
    1, 3, 1, 0, 1, 1, 3, 1, 4, 0, 1, 1, 0, 2, 2, 3, 0, 1, 4, 1,
    0, 1, 1, 0, 1, 2, 3, 2, 4, 3, 2, 1, 1, 2, 2, 4, 2, 0, 1, 3,
    1, 1, 0, 1, 3, 2, 0, 2, 3, 0, 2, 1, 2, 2, 1, 2, 1, 0, 1, 2,
    0, 1, 1, 2, 3, 2, 2, 1, 1, 0, 2, 4, 3, 2, 3, 1, 1, 1, 1, 2,
    2, 2, 1, 0, 1, 1, 1, 0, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 2, 0,
    2, 2, 1, 0, 2, 1, 1, 1, 1, 1, 3, 1, 2, 0, 1, 1, 0, 2, 1, 2,
    3, 1, 1, 2, 1, 2, 1, 3, 2, 1, 2, 0, 1, 3, 0, 1, 2, 0, 1, 1,
    1, 4, 1, 0, 1, 2, 0, 1, 3, 1, 2, 0, 1, 4, 4, 0, 1, 1, 1, 1,
    0, 3, 2, 2, 0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 2, 4, 0, 3, 2, 1,
    1, 0, 1, 2, 0, 2, 2, 0, 2, 3, 0, 1, 2, 0, 1, 2, 1, 0, 1, 1,
    2, 1, 0, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 2, 2, 0, 1,
    1, 2, 2, 4, 3, 0, 2, 1, 1, 0, 1, 3, 0, 1, 2, 2, 1, 0, 1, 2,
    0, 1, 2, 3, 2, 4, 1, 4, 1, 2, 0, 1, 2, 0, 2, 1, 1, 0, 2, 3,
    0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 5, 1, 1, 0, 2, 4, 0, 1,
    3, 1, 0, 1, 2, 1, 2, 1, 0, 1, 1, 1, 0, 2, 1, 2, 3, 3, 1, 0,
    1, 3, 1, 0, 1, 4, 0, 1, 3, 2, 0, 1, 3, 3, 0, 2, 3, 0, 2, 2,
    0, 1, 2, 2, 1, 1, 0, 1, 1, 4, 0, 1, 1, 2, 3, 2, 1, 0, 1, 2,
    0, 1, 2, 1, 3, 9, 10, 7, 6, 1, 2, 1, 2, 1, 0, 1, 1, 1, 1, 2,
    0, 2, 1, 0, 3, 2, 2, 0, 4, 2, 1, 0, 1, 2, 1, 0, 3, 1, 2, 0,
    1, 2, 0, 2, 1, 4, 0, 2, 1, 0, 2, 3, 1, 1, 3, 3, 1, 3, 0, 1,
    5, 1, 1, 0, 1, 2, 1, 2, 1, 0, 1, 1, 2, 3, 0, 2, 1, 2, 2, 2,
    0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 3, 1, 3, 1, 0, 2, 1, 2, 1, 0,
    1, 1, 0, 1, 1, 3, 3, 2, 1, 0, 1, 4, 2, 2, 1, 4, 1, 2, 1, 1,
    1, 0, 1, 1, 1, 2, 2, 2, 1, 0, 2, 1, 1, 2, 0, 2, 3, 2, 0, 1,
    1, 1, 1, 1, 0, 1, 2, 1, 3, 2, 2, 2, 0, 1, 4, 2, 0, 1, 2, 1,
    3, 1, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 1, 0, 3, 1, 2, 1, 2, 2,
    1, 0, 1, 2, 2, 1, 1, 0, 2, 2, 4, 2, 0, 1, 2, 1, 2, 1, 1, 0,
    1, 2, 1, 1, 0, 1, 1, 3, 0, 1, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0,
    1, 2, 0, 1, 3, 1, 2, 3, 2, 1, 2, 0, 2, 1, 1, 1, 0, 3, 1, 3,
    0, 1, 2, 1, 1, 0, 2, 3, 1, 2, 2, 2, 2, 0, 1, 2, 1, 1, 2, 1,
    2, 1, 0, 1, 3, 2, 1, 1, 1, 2, 0, 2, 4, 1, 1, 2, 2, 0, 1, 1,
    2, 0, 3, 1, 3, 1, 1, 0, 1, 1, 0, 2, 2, 4, 2, 0, 2, 1, 3, 3,
    3, 2, 0, 2, 1, 1, 2, 2, 3, 1, 1, 1, 2, 0, 1, 1, 0, 1, 1, 2,
    0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 2, 3, 7, 0,
    1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 3, 2,
    1, 2, 2, 5, 0, 2, 1, 1, 1, 2, 3, 1, 0, 2, 2, 3, 0, 1, 1, 0,
    1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 2, 1, 2, 3, 2, 0, 2, 1,
    2, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 5, 1, 1, 1, 0, 2, 1, 0,
    4, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 1, 3, 3, 1, 0, 1, 2, 1,
    0, 2, 1, 0, 3, 1, 1, 1, 1, 1, 3, 0, 1, 1, 1, 1, 0, 3, 1, 0,
    8, 3, 0, 2, 1, 0, 1, 1, 0, 1, 2, 1, 1, 1, 1, 2, 1, 3, 1, 1,
    2, 1, 0, 1, 1, 1, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1,
    1, 0, 5, 1, 0, 2, 4, 0, 1, 1, 1, 2, 0, 1, 1, 0, 2, 1, 0, 1,
    2, 1, 0, 2, 2, 1, 1, 0, 1, 2, 0, 1, 2, 0, 4, 2, 0, 2, 1, 1,
    1, 2, 1, 4, 2, 0, 1, 1, 1, 1, 1, 1, 0, 4, 1, 1, 0, 3, 2, 0,
    1, 1, 1, 0, 3, 1, 0, 2, 3, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2,
    0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 0, 1, 2, 1, 1, 0, 1, 2, 3, 1,
    1, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 3, 0, 1, 3, 0, 3, 2, 1,
    0, 1, 2, 0, 1, 1, 0, 4, 2, 0, 1, 2, 0, 1, 1, 1, 0, 1, 3, 2,
    2, 3, 0, 4, 1, 1, 1, 3, 0, 6, 1, 0, 2, 2, 1, 1, 0, 3, 1, 0,
    3, 1, 0, 2, 1, 0, 3, 1, 0, 2, 1, 2, 1, 2, 0, 1, 1, 1, 1, 0,
    2, 1, 1, 2, 0, 2, 1, 0, 5, 2, 1, 0, 1, 1, 1, 0, 2, 2, 0, 1,
    1, 0, 1, 1, 0, 2, 1, 0, 4, 2, 0, 6, 1, 0, 2, 1, 1, 0, 1, 3,
    0, 1, 1, 1, 0, 1, 2, 0, 5, 1, 0, 1, 1, 2, 0, 2, 1, 0, 1, 1,
    1, 2, 2, 1, 3, 1, 3, 0, 1, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 1,
    1, 0, 4, 1, 1, 0, 1, 1, 1, 2, 1, 0, 1, 1, 0, 5, 1, 0, 1, 1,
    0, 3, 2, 0, 2, 1, 0, 3, 1, 0, 2, 1, 0, 4, 2, 0, 3, 1, 0, 3,
    1, 1, 1, 2, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 0, 3,
    1, 0, 2, 2, 2, 0, 1, 1, 2, 0, 5, 1, 0, 3, 1, 0, 1, 1, 1, 0,
    2, 1, 1, 2, 1, 0, 3, 1, 1, 0, 2, 1, 0, 3, 1, 0, 2, 3, 0, 1,
    1, 1, 0, 1, 1, 1, 0, 4, 1, 0, 2, 2, 0, 1, 2, 0, 2, 1, 0, 3,
    1, 1, 0, 2, 1, 0, 1, 1, 1, 2, 0, 6, 1, 1, 1, 0, 9, 1, 1, 0,
    1, 2, 0, 2, 1, 2, 1, 0, 1, 1, 2, 0, 2, 1, 0, 2, 1, 0, 3, 1,
    2, 0, 4, 2, 0, 2, 2, 0, 1, 1, 0, 1, 3, 1, 0, 1, 1, 0, 2, 1,
    1, 0, 1, 1, 3, 0, 2, 2, 0, 1, 1, 2, 11, 18, 26, 35, 54, 53, 69, 44,
    27, 12, 12, 3, 0, 5, 1, 0, 4, 1, 1, 0, 1, 1, 0, 4, 1, 0, 2, 1,
    0, 1, 1, 0, 6, 1, 1, 0, 2, 2, 1, 0, 5, 1, 0, 4, 1, 0, 8, 1,
    0, 2, 1, 0, 8, 2, 0, 1, 1, 2, 0, 2, 1, 1, 0, 4, 1, 0, 2, 2,
    0, 4, 1, 0, 10, 1, 0, 8, 1, 0, 6, 1, 0, 5, 1, 2, 0, 3, 1, 2,
    2, 2, 0, 4, 1, 2, 0, 1, 2, 1, 0, 2, 1, 1, 2, 0, 4, 1, 1, 0,
    7, 1, 0, 4, 1, 0, 1, 1, 0, 9, 1, 0, 1, 1, 1, 0, 4, 1, 2, 0,
    1, 2, 0, 7, 1, 0, 2, 1, 0, 3, 1, 1, 0, 4, 1, 1, 0, 8, 1, 1,
    0, 11, 2, 0, 6, 1, 0, 2, 1, 0, 6, 1, 0, 1, 2, 1, 0, 3, 2, 0,
    6, 2, 1, 0, 1, 1, 0, 14, 2, 0, 12, 1, 0, 6, 1, 0, 2, 1, 0, 3,
    1, 0, 2, 2, 0, 3, 1, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 1, 0,
    5, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 3, 1, 1, 2, 2, 0, 1,
    1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 2, 1, 0, 2, 2, 0, 1, 1, 0, 2,
    1, 1, 0, 6, 2, 1, 0, 3, 1, 0, 2, 1, 1, 0, 1, 1, 0, 8, 1, 0,
    1, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 1, 0, 4, 1, 0, 7,
    1, 0, 7, 1, 0, 3, 1, 0, 4, 1, 0, 4, 1, 0, 5, 1, 1, 0, 1, 1,
    1, 0, 1, 1, 1, 1, 1, 0, 15, 1, 0, 3, 2, 2, 0, 1, 1, 0, 4, 1,
    0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 4, 1,
    0, 1, 1, 0, 10, 1, 1, 1, 1, 0, 3, 1, 1, 0, 7, 1, 0, 1, 1, 0,
    4, 1, 0, 7, 1, 0, 7, 1, 0, 3, 1, 1, 0, 2, 2, 0, 1, 2, 0, 21,
    1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 10, 1, 0, 9, 1, 0, 2, 1,
    0, 11, 1, 0, 2, 2, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 9, 1, 0, 5,
    1, 0, 11, 1, 0, 9, 1, 0, 22, 2, 0, 1, 1, 0, 8, 1, 1, 2, 1, 0,
    4, 1, 1, 2, 0, 1, 1, 0, 19, 1, 1, 0, 26, 1, 1, 1, 0, 10, 1, 1,
    0, 8, 1, 0, 1, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 1, 2, 5, 6, 7,
    8, 2, 5, 3, 0, 1, 1, 1, 0, 2, 1, 0, 2, 1, 0, 14, 1, 0, 3, 1,
    0, 12, 1, 0, 1, 1, 0, 1, 1, 0, 16, 1, 0, 15, 1, 0, 7, 1, 0, 11,
    1, 0, 1, 1, 1, 1, 0, 17, 2, 0, 18, 1, 0, 1, 1, 0, 16, 1, 0, 3,
    1, 1, 0, 2, 1, 0, 3, 1, 0, 12, 1, 0, 22, 1, 0, 8, 1, 0, 4, 2,
    1, 0, 8, 1, 0, 13, 1, 0, 4, 1, 0, 15, 1, 0, 1, 1, 0, 2, 1, 1,
    0, 1, 1, 0, 12, 1, 0, 15, 1, 0, 15, 1, 0, 2, 1, 0, 19, 1, 0, 2,
    1, 0, 2, 1, 0, 3, 2, 1, 0, 1, 1, 0, 4, 1, 0, 9, 1, 0, 2, 1,
    0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 10, 1, 0, 36, 1, 0, 2, 1, 0, 3,
    1, 0, 1, 1, 0, 13, 1, 0, 11, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0,
    2, 1, 0, 10, 1, 0, 2, 1, 0, 13, 1, 0, 5, 2, 1, 1, 0, 4, 1, 0,
    6, 2, 0, 6, 1, 0, 17, 1, 0, 14, 1, 0, 6, 1, 0, 6, 1, 0, 4, 1,
    0, 21, 1, 0, 38, 1, 0, 3, 1, 0, 2, 2, 1, 0, 9, 1, 0, 2, 1, 0,
    1, 1, 0, 1, 1, 0, 3, 1, 0, 5, 1, 0, 8, 1, 0, 2, 1, 0, 16, 1,
    0, 5, 1, 0, 5, 1, 0, 1, 1, 0, 11, 1, 0, 2, 1, 0, 6, 1, 0, 3,
    1, 0, 2, 1, 0, 4, 1, 0, 7, 1, 0, 9, 1, 0, 9, 1, 0, 3, 1, 1,
    0, 8, 1, 0, 3, 1, 0, 7, 1, 0, 9, 2, 0, 20, 1, 0, 1, 1, 0, 1,
    1, 0, 3, 1, 0, 3, 1, 0, 24, 1, 0, 7, 1, 0, 4, 1, 1, 1, 0, 10,
    1, 0, 6, 1, 0, 3, 1, 2, 2, 2, 1, 2, 0, 1, 2, 0, 8, 1, 0, 9,
    2, 0, 6, 1, 0, 8, 1, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 0, 11, 2,
    0, 5, 1, 0, 6, 2, 0, 23, 1, 0, 5, 1, 1, 0, 3, 1, 0, 8, 1, 1,
    0, 7, 1, 0, 1, 1, 1, 0, 1, 1, 0, 18, 2, 1, 0, 3, 1, 0, 5, 1,
    0, 2, 1, 2, 0, 2, 1, 0, 5, 1, 0, 1, 1, 1, 1, 0, 7, 1, 1, 2,
    1, 0, 5, 1, 0, 6, 1, 0, 6, 1, 0, 2, 1, 0, 15, 1, 0, 11, 1, 0,
    7, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 1,
    2, 0, 3, 3, 2, 2, 2, 1, 1, 0, 1, 1, 0, 1, 1, 0, 10, 1, 0, 17,
    1, 0, 9, 1, 0, 4, 2, 0, 7, 1, 0, 4, 1, 0, 4, 1, 0, 26, 1, 0,
    16, 1, 0, 4, 1, 0, 10, 1, 0, 34, 2, 0, 24, 2, 1, 0, 5, 1, 0, 17,
    1, 0, 4, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 6, 2, 0, 3, 1, 1, 0,
    5, 1, 0, 14, 1, 1, 0, 3, 1, 0, 7, 1, 0, 5, 2, 0, 8, 1, 1, 0,
    10, 1, 1, 0, 8, 1, 0, 16, 1, 1, 0, 8, 1, 0, 2, 1, 0, 29, 1, 0,
    7, 2, 0, 9, 1, 0, 5, 1, 0, 11, 1, 0, 9, 1, 0, 4, 1, 0, 15, 1,
    0, 10, 1, 0, 3, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 0, 5, 2, 0, 10,
    1, 0, 17, 1, 0, 9, 1, 0, 4, 2, 0, 1, 1, 0, 6, 1, 0, 11, 1, 0,
    3, 1, 0, 18, 1, 0, 3, 1, 0, 27, 1, 0, 6, 2, 0, 34, 1, 0, 2, 1,
    0, 9, 1, 0, 13, 1, 0, 6, 3, 0, 1, 1, 0, 13, 1, 0, 7, 1, 0, 3,
    1, 0, 16, 1, 0, 18, 1, 0, 2, 1, 1, 0, 24, 1, 0, 6, 1, 0, 6, 1,
    0, 12, 1, 0, 46, 1, 0, 1, 1, 0, 12, 1, 0, 37, 1, 0, 9, 1, 0, 4,
    1, 1, 0, 26, 1, 0, 27, 1, 0, 23, 1, 0, 19, 1, 0, 28, 1, 0, 7, 1,
    0, 73, 1, 0, 2, 1, 0, 1, 2, 2, 10, 4, 3, 6, 9, 5, 0, 1, 1, 2,
    1, 0, 40, 1, 0, 17, 1, 0, 7, 1, 0, 1, 1, 0, 72, 1, 0, 49, 1, 0,
    10, 1, 0, 11, 1, 0, 95, 1, 0, 87, 1, 0, 1, 1, 0, 71, 1, 0, 60, 1,
    0, 6, 1, 0, 123, 1, 0, 11, 1, 0, 121, 1, 0, 247  };
  assert( test_10_chan_cnts.size() == 5454 );
  const vector<uint8_t> test_10_packed{
    78, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 40, 24, 50, 50, 56, 56, 52, 48, 54, 57, 46, 40, 30, 36, 37, 48, 35, 42, 39, 42, 35, 38, 32, 44, 37, 43, 48, 43, 38, 36, 33, 38, 42,
    40, 28, 37, 33, 30, 37, 29, 53, 29, 26, 29, 35, 32, 27, 38, 33, 34, 33, 34, 38, 33, 42, 21, 36, 32, 37, 28, 29, 39, 31, 24, 41, 42, 24, 34, 31, 35, 28, 36, 40, 40, 24, 25, 24, 41, 32, 30, 39, 31, 28,
    27, 25, 30, 29, 31, 24, 32, 41, 29, 32, 23, 25, 31, 33, 34, 32, 32, 28, 32, 22, 34, 37, 29, 37, 38, 27, 23, 38, 34, 35, 35, 36, 28, 38, 36, 42, 36, 40, 49, 39, 32, 45, 38, 42, 26, 35, 49, 53, 35, 39,
    47, 55, 56, 54, 44, 43, 40, 47, 61, 40, 61, 63, 67, 58, 66, 70, 68, 64, 55, 49, 47, 43, 35, 66, 63, 53, 66, 46, 47, 61, 61, 57, 63, 59, 67, 59, 69, 67, 82, 61, 66, 68, 65, 75, 62, 62, 55, 54, 51, 51,
    68, 65, 60, 55, 54, 50, 71, 75, 54, 64, 60, 78, 64, 65, 70, 59, 77, 69, 59, 59, 77, 73, 56, 60, 73, 71, 84, 67, 73, 67, 84, 82, 73, 63, 58, 66, 68, 70, 75, 79, 74, 63, 81, 67, 60, 65, 82, 79, 84, 84,
    79, 61, 67, 58, 67, 64, 90, 72, 70, 83, 77, 78, 85, 91, 84, 61, 95, 58, 98, 78, 84, 67, 78, 79, 81, 91, 80, 90, 89, 81, 73, 72, 82, 75, 87, 79, 104, 82, 86, 69, 77, 83, 97, 76, 80, 79, 68, 80, 84, 76,
    77, 65, 73, 88, 65, 76, 69, 77, 79, 76, 72, 80, 88, 52, 81, 83, 72, 62, 74, 67, 72, 68, 78, 79, 70, 73, 80, 72, 61, 71, 93, 70, 70, 82, 67, 74, 77, 70, 72, 84, 85, 78, 79, 80, 75, 58, 66, 75, 75, 72,
    63, 61, 73, 67, 75, 87, 73, 72, 73, 72, 63, 64, 64, 66, 63, 63, 58, 69, 77, 86, 119, 98, 92, 66, 65, 63, 66, 73, 74, 54, 70, 62, 73, 69, 74, 65, 56, 74, 69, 67, 65, 54, 67, 67, 58, 69, 70, 62, 58, 65,
    51, 62, 68, 58, 54, 56, 60, 68, 57, 65, 49, 81, 72, 57, 70, 69, 67, 69, 62, 67, 71, 69, 80, 83, 83, 96, 101, 75, 63, 57, 62, 68, 71, 68, 68, 61, 70, 56, 71, 57, 46, 69, 71, 70, 64, 68, 58, 79, 66, 76,
    66, 58, 52, 60, 62, 56, 61, 56, 60, 70, 71, 73, 60, 68, 56, 52, 65, 86, 61, 72, 55, 50, 65, 64, 57, 67, 75, 48, 75, 66, 69, 72, 104, 199, 175, 1, 63, 2, 89, 2, 72, 1, 157, 55, 36, 37, 36, 27, 27, 25,
    41, 37, 30, 26, 33, 25, 40, 29, 31, 29, 41, 36, 24, 22, 37, 41, 34, 17, 32, 31, 28, 23, 25, 24, 20, 43, 28, 14, 21, 29, 22, 32, 26, 49, 33, 38, 36, 23, 27, 27, 43, 51, 65, 96, 84, 55, 37, 27, 20, 23,
    27, 24, 30, 20, 17, 31, 22, 23, 20, 27, 24, 14, 24, 21, 27, 24, 17, 24, 17, 24, 29, 23, 19, 22, 28, 14, 31, 29, 22, 21, 19, 23, 20, 14, 22, 25, 26, 31, 19, 21, 26, 18, 19, 23, 22, 26, 28, 19, 18, 16,
    20, 23, 25, 29, 22, 24, 22, 21, 21, 14, 22, 20, 16, 22, 17, 15, 14, 21, 18, 16, 21, 25, 24, 21, 10, 21, 16, 19, 21, 12, 20, 30, 28, 53, 36, 42, 29, 25, 21, 19, 25, 17, 27, 24, 18, 17, 18, 15, 12, 21,
    21, 14, 11, 13, 14, 11, 11, 22, 21, 17, 13, 15, 14, 20, 18, 18, 12, 9, 18, 6, 17, 18, 26, 19, 14, 13, 16, 18, 16, 24, 20, 12, 8, 6, 18, 22, 16, 14, 13, 11, 16, 14, 3, 12, 17, 10, 10, 16, 12, 10,
    10, 18, 19, 15, 13, 12, 12, 16, 10, 8, 14, 17, 13, 7, 13, 19, 13, 13, 17, 11, 18, 14, 9, 14, 13, 12, 13, 11, 10, 10, 9, 8, 13, 13, 16, 13, 15, 10, 20, 15, 14, 14, 11, 16, 6, 16, 9, 17, 14, 12,
    10, 13, 4, 10, 8, 11, 13, 7, 13, 14, 13, 12, 13, 14, 14, 19, 15, 12, 11, 13, 10, 6, 13, 21, 8, 9, 8, 10, 13, 10, 16, 7, 12, 8, 9, 5, 15, 17, 21, 21, 19, 14, 14, 10, 15, 24, 14, 16, 10, 14,
    10, 10, 13, 6, 9, 14, 13, 9, 9, 7, 8, 12, 7, 10, 5, 9, 8, 12, 11, 9, 6, 9, 11, 8, 8, 8, 10, 7, 5, 3, 4, 12, 8, 10, 9, 10, 8, 12, 13, 9, 13, 7, 11, 10, 8, 7, 8, 6, 8, 5,
    6, 11, 5, 12, 11, 13, 16, 6, 7, 7, 7, 4, 9, 7, 10, 6, 13, 11, 13, 11, 12, 10, 3, 12, 10, 4, 9, 11, 9, 9, 10, 11, 7, 9, 8, 10, 8, 11, 12, 5, 6, 13, 9, 12, 11, 6, 9, 10, 9, 8,
    12, 3, 5, 7, 5, 10, 19, 11, 10, 7, 14, 8, 8, 5, 10, 4, 6, 8, 7, 11, 6, 6, 7, 8, 4, 5, 7, 12, 5, 6, 9, 13, 9, 9, 6, 13, 7, 6, 12, 6, 10, 11, 25, 27, 21, 21, 15, 10, 10, 11,
    6, 10, 4, 6, 8, 6, 4, 1, 7, 11, 6, 3, 5, 9, 7, 11, 9, 3, 9, 8, 8, 10, 9, 8, 10, 9, 7, 10, 4, 11, 1, 9, 12, 10, 10, 9, 4, 8, 8, 8, 7, 4, 8, 12, 5, 9, 4, 7, 7, 9,
    7, 5, 9, 5, 9, 9, 9, 10, 6, 9, 10, 8, 11, 5, 7, 4, 4, 9, 3, 6, 2, 5, 8, 4, 4, 9, 8, 7, 4, 6, 6, 5, 3, 4, 5, 6, 3, 8, 8, 11, 5, 9, 5, 3, 8, 13, 5, 7, 4, 7,
    5, 9, 5, 4, 9, 5, 3, 5, 6, 6, 6, 5, 7, 6, 2, 5, 6, 4, 6, 2, 5, 6, 9, 5, 6, 7, 8, 7, 6, 6, 6, 9, 7, 6, 4, 13, 10, 7, 5, 5, 8, 7, 3, 4, 8, 4, 5, 4, 8, 8,
    6, 5, 9, 6, 9, 1, 9, 7, 3, 4, 3, 9, 11, 7, 7, 5, 7, 5, 7, 12, 6, 1, 7, 7, 7, 6, 4, 7, 3, 8, 3, 6, 6, 6, 7, 7, 5, 5, 5, 6, 6, 4, 2, 2, 14, 7, 6, 6, 2, 2,
    6, 4, 4, 7, 10, 5, 3, 5, 7, 8, 6, 5, 5, 7, 6, 5, 6, 2, 6, 4, 10, 3, 5, 3, 5, 2, 3, 5, 6, 6, 4, 5, 2, 5, 1, 5, 7, 7, 6, 7, 3, 2, 2, 3, 6, 6, 4, 6, 6, 2,
    2, 2, 11, 3, 2, 3, 3, 7, 9, 4, 8, 3, 3, 11, 5, 3, 5, 6, 3, 5, 6, 4, 2, 2, 4, 3, 4, 3, 5, 2, 3, 6, 5, 9, 4, 3, 3, 6, 9, 2, 2, 2, 3, 6, 3, 4, 7, 7, 2, 4,
    2, 5, 3, 8, 2, 2, 7, 2, 8, 2, 3, 6, 4, 4, 3, 8, 6, 1, 2, 3, 3, 3, 7, 3, 5, 2, 4, 4, 2, 4, 4, 5, 4, 3, 8, 7, 5, 2, 4, 6, 3, 5, 6, 2, 3, 2, 5, 3, 3, 8,
    4, 6, 3, 4, 1, 4, 4, 4, 5, 3, 5, 1, 3, 4, 1, 7, 4, 3, 1, 5, 4, 8, 4, 2, 3, 5, 5, 2, 5, 5, 2, 6, 4, 5, 4, 6, 2, 3, 1, 4, 4, 4, 5, 4, 1, 4, 7, 5, 3, 1,
    7, 4, 1, 6, 2, 1, 4, 3, 1, 2, 3, 2, 2, 4, 3, 1, 1, 1, 8, 8, 5, 7, 4, 8, 8, 5, 12, 15, 6, 4, 15, 5, 4, 2, 1, 3, 5, 3, 4, 1, 3, 7, 4, 8, 4, 2, 2, 2, 5, 3,
    3, 5, 6, 4, 4, 2, 7, 1, 3, 2, 2, 4, 3, 4, 5, 3, 3, 3, 2, 2, 3, 2, 3, 1, 3, 3, 1, 4, 4, 3, 3, 3, 1, 6, 5, 5, 4, 3, 1, 3, 4, 4, 0, 1, 3, 1, 1, 2, 1, 4,
    1, 0, 1, 5, 2, 3, 7, 1, 3, 5, 1, 3, 0, 1, 3, 3, 3, 6, 3, 1, 3, 2, 6, 6, 2, 1, 4, 2, 3, 5, 3, 1, 3, 1, 2, 2, 1, 1, 5, 3, 4, 2, 5, 5, 5, 3, 5, 3, 2, 2,
    4, 3, 2, 4, 2, 4, 1, 2, 3, 3, 6, 1, 3, 6, 0, 1, 6, 3, 3, 5, 2, 2, 4, 3, 4, 6, 9, 3, 0, 1, 3, 1, 3, 2, 1, 7, 5, 5, 4, 3, 4, 3, 2, 5, 2, 2, 2, 5, 3, 1,
    2, 0, 1, 3, 3, 2, 5, 1, 5, 3, 2, 2, 4, 5, 4, 2, 2, 1, 3, 0, 1, 3, 2, 2, 2, 5, 2, 1, 2, 4, 11, 15, 12, 19, 13, 4, 5, 5, 0, 1, 5, 1, 0, 1, 3, 1, 2, 4, 5, 0,
    2, 2, 1, 2, 3, 1, 5, 2, 3, 2, 5, 2, 6, 5, 2, 0, 1, 4, 2, 3, 3, 3, 3, 1, 2, 1, 3, 2, 1, 3, 3, 1, 2, 2, 4, 1, 2, 3, 2, 0, 1, 1, 2, 0, 1, 3, 5, 4, 4, 1,
    2, 3, 1, 4, 6, 2, 6, 17, 15, 39, 12, 14, 7, 4, 4, 2, 3, 3, 3, 1, 4, 3, 3, 2, 4, 3, 4, 3, 3, 0, 1, 7, 3, 1, 2, 4, 4, 2, 1, 2, 1, 8, 2, 2, 3, 3, 2, 3, 3, 3,
    3, 4, 3, 2, 0, 1, 1, 2, 4, 3, 4, 1, 1, 1, 2, 5, 3, 2, 4, 3, 2, 4, 3, 4, 2, 0, 3, 2, 4, 5, 3, 4, 1, 2, 2, 2, 3, 3, 2, 2, 4, 4, 2, 0, 1, 3, 2, 3, 4, 0,
    1, 4, 4, 2, 3, 4, 0, 1, 4, 2, 1, 1, 2, 1, 5, 1, 0, 1, 1, 7, 1, 2, 0, 1, 1, 2, 1, 2, 4, 5, 1, 3, 2, 2, 5, 4, 5, 3, 1, 2, 4, 1, 4, 2, 2, 3, 3, 5, 3, 2,
    1, 2, 1, 1, 3, 0, 2, 1, 3, 2, 2, 1, 2, 3, 5, 5, 1, 6, 7, 1, 4, 2, 3, 4, 4, 10, 3, 5, 1, 1, 0, 1, 2, 1, 3, 2, 3, 2, 1, 4, 4, 0, 1, 1, 0, 1, 1, 1, 1, 4,
    1, 4, 2, 1, 1, 3, 0, 1, 2, 3, 1, 2, 3, 0, 1, 3, 0, 1, 2, 4, 3, 1, 1, 1, 3, 2, 0, 2, 3, 2, 0, 1, 3, 3, 2, 2, 2, 1, 1, 3, 1, 2, 1, 2, 1, 1, 3, 4, 3, 1,
    0, 2, 5, 4, 2, 2, 1, 6, 1, 0, 1, 4, 0, 1, 3, 1, 2, 0, 1, 2, 1, 1, 3, 3, 2, 6, 4, 3, 4, 1, 0, 1, 3, 1, 1, 3, 5, 4, 1, 0, 1, 1, 1, 1, 2, 1, 1, 0, 1, 4,
    1, 5, 1, 3, 3, 3, 4, 0, 1, 3, 3, 1, 5, 1, 5, 3, 0, 1, 2, 1, 2, 0, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 2, 1, 2, 1, 2, 5, 3, 1, 4, 1, 1, 4, 1, 3, 2, 1, 2, 2,
    1, 4, 5, 2, 5, 4, 2, 3, 2, 5, 2, 1, 2, 3, 4, 3, 2, 3, 1, 2, 0, 1, 3, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 1, 5, 5, 1, 3, 2, 2, 2, 2, 1, 4, 1, 3, 1, 2, 1,
    0, 2, 1, 3, 3, 0, 1, 2, 1, 3, 2, 3, 0, 1, 1, 1, 0, 1, 1, 0, 1, 5, 2, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 1, 3, 1, 4, 2, 2, 2, 3, 5, 3, 0, 1, 1, 2, 3, 3,
    2, 3, 3, 3, 1, 0, 1, 1, 2, 0, 1, 1, 2, 1, 1, 4, 2, 0, 1, 2, 0, 1, 3, 2, 3, 1, 3, 6, 1, 1, 5, 2, 3, 2, 3, 2, 3, 4, 4, 1, 1, 4, 3, 0, 1, 5, 2, 2, 1, 1,
    1, 3, 1, 4, 0, 1, 1, 3, 7, 2, 1, 2, 1, 0, 1, 2, 4, 0, 1, 1, 2, 0, 1, 1, 3, 1, 1, 4, 2, 2, 1, 3, 9, 3, 2, 1, 3, 1, 1, 5, 0, 2, 1, 0, 1, 1, 1, 4, 2, 0,
    1, 1, 0, 1, 1, 1, 1, 1, 2, 1, 3, 3, 2, 1, 5, 0, 1, 2, 1, 0, 1, 5, 4, 1, 0, 1, 3, 2, 0, 1, 3, 2, 1, 1, 0, 4, 2, 3, 1, 3, 1, 5, 2, 5, 1, 2, 0, 1, 1, 3,
    0, 1, 1, 2, 1, 1, 1, 0, 1, 1, 1, 2, 1, 1, 2, 2, 3, 3, 1, 3, 0, 1, 3, 0, 1, 2, 5, 1, 0, 1, 1, 2, 0, 1, 4, 2, 0, 1, 2, 3, 0, 1, 3, 0, 1, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 0, 2, 1, 1, 1, 3, 4, 1, 0, 1, 2, 0, 2, 1, 5, 0, 1, 1, 2, 2, 0, 1, 3, 0, 1, 3, 0, 1, 1, 0, 1, 2, 1, 3, 2, 1, 0, 1, 1, 1, 1, 5, 0, 1, 1, 1,
    1, 0, 1, 2, 3, 2, 1, 2, 0, 1, 1, 3, 1, 2, 0, 1, 1, 2, 0, 1, 1, 0, 1, 1, 0, 2, 1, 2, 0, 1, 1, 1, 3, 3, 5, 3, 1, 1, 1, 1, 2, 1, 2, 3, 0, 1, 1, 0, 1, 3,
    0, 2, 3, 1, 1, 1, 2, 2, 4, 3, 5, 0, 1, 2, 2, 3, 3, 1, 1, 5, 4, 2, 0, 1, 3, 3, 3, 2, 3, 0, 3, 2, 0, 2, 1, 2, 2, 1, 0, 1, 1, 2, 2, 2, 2, 0, 1, 3, 1, 1,
    1, 1, 2, 1, 0, 5, 1, 1, 0, 2, 1, 2, 1, 0, 4, 1, 3, 2, 3, 1, 1, 1, 3, 3, 0, 1, 2, 1, 1, 1, 4, 0, 1, 4, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 5, 0, 1, 1, 5,
    4, 1, 2, 2, 0, 1, 2, 3, 2, 1, 2, 1, 2, 0, 1, 4, 0, 1, 2, 3, 1, 0, 1, 2, 4, 0, 1, 2, 1, 1, 0, 1, 3, 1, 1, 3, 0, 1, 3, 1, 0, 1, 2, 0, 1, 2, 2, 1, 2, 2,
    5, 0, 2, 2, 2, 1, 3, 4, 0, 1, 6, 0, 2, 5, 1, 3, 4, 1, 14, 8, 7, 4, 5, 5, 0, 1, 1, 3, 0, 1, 2, 1, 3, 1, 2, 0, 2, 2, 2, 7, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    1, 0, 1, 2, 2, 0, 1, 4, 1, 2, 1, 1, 0, 2, 3, 1, 0, 1, 2, 1, 0, 2, 2, 1, 0, 1, 1, 2, 0, 1, 1, 1, 3, 2, 0, 1, 4, 0, 1, 1, 5, 3, 1, 2, 4, 4, 1, 2, 1, 1,
    0, 1, 3, 2, 0, 2, 1, 4, 1, 0, 1, 1, 2, 0, 1, 2, 0, 1, 2, 2, 2, 3, 1, 1, 3, 1, 1, 2, 1, 1, 0, 1, 3, 1, 2, 1, 2, 4, 2, 2, 0, 1, 2, 4, 1, 0, 3, 2, 3, 1,
    1, 2, 2, 0, 3, 2, 1, 0, 1, 1, 0, 3, 1, 1, 1, 2, 1, 1, 3, 3, 3, 0, 1, 3, 3, 1, 6, 1, 4, 3, 1, 3, 4, 3, 2, 1, 1, 2, 1, 3, 4, 0, 1, 7, 10, 8, 4, 5, 1, 1,
    1, 0, 3, 2, 0, 1, 1, 1, 0, 3, 1, 1, 3, 3, 0, 5, 1, 1, 0, 1, 1, 1, 1, 1, 2, 2, 1, 3, 1, 0, 1, 3, 2, 4, 3, 3, 1, 1, 2, 1, 0, 3, 1, 5, 2, 2, 0, 2, 2, 1,
    1, 3, 0, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 3, 1, 1, 2, 2, 2, 1, 0, 1, 2, 1, 1, 1, 2, 2, 4, 1, 2, 2, 2, 3, 4, 2, 3, 1, 1, 1, 0, 1, 1, 1, 4, 3, 2, 0, 1, 1,
    5, 4, 0, 1, 1, 1, 4, 2, 0, 2, 1, 3, 1, 0, 1, 1, 3, 1, 4, 0, 1, 1, 0, 2, 2, 3, 0, 1, 4, 1, 0, 1, 1, 0, 1, 2, 3, 2, 4, 3, 2, 1, 1, 2, 2, 4, 2, 0, 1, 3,
    1, 1, 0, 1, 3, 2, 0, 2, 3, 0, 2, 1, 2, 2, 1, 2, 1, 0, 1, 2, 0, 1, 1, 2, 3, 2, 2, 1, 1, 0, 2, 4, 3, 2, 3, 1, 1, 1, 1, 2, 2, 2, 1, 0, 1, 1, 1, 0, 1, 2,
    1, 2, 1, 1, 1, 1, 1, 1, 2, 0, 2, 2, 1, 0, 2, 1, 1, 1, 1, 1, 3, 1, 2, 0, 1, 1, 0, 2, 1, 2, 3, 1, 1, 2, 1, 2, 1, 3, 2, 1, 2, 0, 1, 3, 0, 1, 2, 0, 1, 1,
    1, 4, 1, 0, 1, 2, 0, 1, 3, 1, 2, 0, 1, 4, 4, 0, 1, 1, 1, 1, 0, 3, 2, 2, 0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 2, 4, 0, 3, 2, 1, 1, 0, 1, 2, 0, 2, 2, 0, 2, 3,
    0, 1, 2, 0, 1, 2, 1, 0, 1, 1, 2, 1, 0, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 2, 2, 0, 1, 1, 2, 2, 4, 3, 0, 2, 1, 1, 0, 1, 3, 0, 1, 2, 2, 1, 0, 1, 2,
    0, 1, 2, 3, 2, 4, 1, 4, 1, 2, 0, 1, 2, 0, 2, 1, 1, 0, 2, 3, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 5, 1, 1, 0, 2, 4, 0, 1, 3, 1, 0, 1, 2, 1, 2, 1, 0, 1,
    1, 1, 0, 2, 1, 2, 3, 3, 1, 0, 1, 3, 1, 0, 1, 4, 0, 1, 3, 2, 0, 1, 3, 3, 0, 2, 3, 0, 2, 2, 0, 1, 2, 2, 1, 1, 0, 1, 1, 4, 0, 1, 1, 2, 3, 2, 1, 0, 1, 2,
    0, 1, 2, 1, 3, 9, 10, 7, 6, 1, 2, 1, 2, 1, 0, 1, 1, 1, 1, 2, 0, 2, 1, 0, 3, 2, 2, 0, 4, 2, 1, 0, 1, 2, 1, 0, 3, 1, 2, 0, 1, 2, 0, 2, 1, 4, 0, 2, 1, 0,
    2, 3, 1, 1, 3, 3, 1, 3, 0, 1, 5, 1, 1, 0, 1, 2, 1, 2, 1, 0, 1, 1, 2, 3, 0, 2, 1, 2, 2, 2, 0, 1, 2, 2, 1, 1, 2, 0, 1, 1, 3, 1, 3, 1, 0, 2, 1, 2, 1, 0,
    1, 1, 0, 1, 1, 3, 3, 2, 1, 0, 1, 4, 2, 2, 1, 4, 1, 2, 1, 1, 1, 0, 1, 1, 1, 2, 2, 2, 1, 0, 2, 1, 1, 2, 0, 2, 3, 2, 0, 1, 1, 1, 1, 1, 0, 1, 2, 1, 3, 2,
    2, 2, 0, 1, 4, 2, 0, 1, 2, 1, 3, 1, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 1, 0, 3, 1, 2, 1, 2, 2, 1, 0, 1, 2, 2, 1, 1, 0, 2, 2, 4, 2, 0, 1, 2, 1, 2, 1, 1, 0,
    1, 2, 1, 1, 0, 1, 1, 3, 0, 1, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0, 1, 2, 0, 1, 3, 1, 2, 3, 2, 1, 2, 0, 2, 1, 1, 1, 0, 3, 1, 3, 0, 1, 2, 1, 1, 0, 2, 3, 1, 2,
    2, 2, 2, 0, 1, 2, 1, 1, 2, 1, 2, 1, 0, 1, 3, 2, 1, 1, 1, 2, 0, 2, 4, 1, 1, 2, 2, 0, 1, 1, 2, 0, 3, 1, 3, 1, 1, 0, 1, 1, 0, 2, 2, 4, 2, 0, 2, 1, 3, 3,
    3, 2, 0, 2, 1, 1, 2, 2, 3, 1, 1, 1, 2, 0, 1, 1, 0, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 2, 3, 7, 0, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1,
    0, 1, 1, 0, 1, 2, 1, 2, 3, 2, 1, 2, 2, 5, 0, 2, 1, 1, 1, 2, 3, 1, 0, 2, 2, 3, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 2, 1, 2, 3, 2, 0, 2, 1,
    2, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 5, 1, 1, 1, 0, 2, 1, 0, 4, 1, 0, 1, 1, 1, 1, 2, 0, 1, 1, 2, 1, 3, 3, 1, 0, 1, 2, 1, 0, 2, 1, 0, 3, 1, 1, 1, 1, 1,
    3, 0, 1, 1, 1, 1, 0, 3, 1, 0, 8, 3, 0, 2, 1, 0, 1, 1, 0, 1, 2, 1, 1, 1, 1, 2, 1, 3, 1, 1, 2, 1, 0, 1, 1, 1, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1,
    1, 0, 5, 1, 0, 2, 4, 0, 1, 1, 1, 2, 0, 1, 1, 0, 2, 1, 0, 1, 2, 1, 0, 2, 2, 1, 1, 0, 1, 2, 0, 1, 2, 0, 4, 2, 0, 2, 1, 1, 1, 2, 1, 4, 2, 0, 1, 1, 1, 1,
    1, 1, 0, 4, 1, 1, 0, 3, 2, 0, 1, 1, 1, 0, 3, 1, 0, 2, 3, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 0, 1, 2, 1, 1, 0, 1, 2, 3, 1,
    1, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 3, 0, 1, 3, 0, 3, 2, 1, 0, 1, 2, 0, 1, 1, 0, 4, 2, 0, 1, 2, 0, 1, 1, 1, 0, 1, 3, 2, 2, 3, 0, 4, 1, 1, 1, 3, 0, 6,
    1, 0, 2, 2, 1, 1, 0, 3, 1, 0, 3, 1, 0, 2, 1, 0, 3, 1, 0, 2, 1, 2, 1, 2, 0, 1, 1, 1, 1, 0, 2, 1, 1, 2, 0, 2, 1, 0, 5, 2, 1, 0, 1, 1, 1, 0, 2, 2, 0, 1,
    1, 0, 1, 1, 0, 2, 1, 0, 4, 2, 0, 6, 1, 0, 2, 1, 1, 0, 1, 3, 0, 1, 1, 1, 0, 1, 2, 0, 5, 1, 0, 1, 1, 2, 0, 2, 1, 0, 1, 1, 1, 2, 2, 1, 3, 1, 3, 0, 1, 1,
    1, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 0, 4, 1, 1, 0, 1, 1, 1, 2, 1, 0, 1, 1, 0, 5, 1, 0, 1, 1, 0, 3, 2, 0, 2, 1, 0, 3, 1, 0, 2, 1, 0, 4, 2, 0, 3, 1, 0, 3,
    1, 1, 1, 2, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 2, 2, 2, 0, 1, 1, 2, 0, 5, 1, 0, 3, 1, 0, 1, 1, 1, 0, 2, 1, 1, 2, 1, 0, 3, 1, 1, 0,
    2, 1, 0, 3, 1, 0, 2, 3, 0, 1, 1, 1, 0, 1, 1, 1, 0, 4, 1, 0, 2, 2, 0, 1, 2, 0, 2, 1, 0, 3, 1, 1, 0, 2, 1, 0, 1, 1, 1, 2, 0, 6, 1, 1, 1, 0, 9, 1, 1, 0,
    1, 2, 0, 2, 1, 2, 1, 0, 1, 1, 2, 0, 2, 1, 0, 2, 1, 0, 3, 1, 2, 0, 4, 2, 0, 2, 2, 0, 1, 1, 0, 1, 3, 1, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 3, 0, 2, 2, 0, 1,
    1, 2, 11, 18, 26, 35, 54, 53, 69, 44, 27, 12, 12, 3, 0, 5, 1, 0, 4, 1, 1, 0, 1, 1, 0, 4, 1, 0, 2, 1, 0, 1, 1, 0, 6, 1, 1, 0, 2, 2, 1, 0, 5, 1, 0, 4, 1, 0, 8, 1,
    0, 2, 1, 0, 8, 2, 0, 1, 1, 2, 0, 2, 1, 1, 0, 4, 1, 0, 2, 2, 0, 4, 1, 0, 10, 1, 0, 8, 1, 0, 6, 1, 0, 5, 1, 2, 0, 3, 1, 2, 2, 2, 0, 4, 1, 2, 0, 1, 2, 1,
    0, 2, 1, 1, 2, 0, 4, 1, 1, 0, 7, 1, 0, 4, 1, 0, 1, 1, 0, 9, 1, 0, 1, 1, 1, 0, 4, 1, 2, 0, 1, 2, 0, 7, 1, 0, 2, 1, 0, 3, 1, 1, 0, 4, 1, 1, 0, 8, 1, 1,
    0, 11, 2, 0, 6, 1, 0, 2, 1, 0, 6, 1, 0, 1, 2, 1, 0, 3, 2, 0, 6, 2, 1, 0, 1, 1, 0, 14, 2, 0, 12, 1, 0, 6, 1, 0, 2, 1, 0, 3, 1, 0, 2, 2, 0, 3, 1, 1, 1, 0,
    2, 1, 0, 2, 1, 0, 1, 1, 1, 0, 5, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 3, 1, 1, 2, 2, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 2, 1, 0, 2, 2, 0, 1, 1, 0, 2,
    1, 1, 0, 6, 2, 1, 0, 3, 1, 0, 2, 1, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 1, 0, 4, 1, 0, 7, 1, 0, 7, 1, 0, 3, 1, 0, 4, 1,
    0, 4, 1, 0, 5, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 15, 1, 0, 3, 2, 2, 0, 1, 1, 0, 4, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 4, 1,
    0, 1, 1, 0, 10, 1, 1, 1, 1, 0, 3, 1, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 0, 7, 1, 0, 7, 1, 0, 3, 1, 1, 0, 2, 2, 0, 1, 2, 0, 21, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2,
    1, 0, 10, 1, 0, 9, 1, 0, 2, 1, 0, 11, 1, 0, 2, 2, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 9, 1, 0, 5, 1, 0, 11, 1, 0, 9, 1, 0, 22, 2, 0, 1, 1, 0, 8, 1, 1, 2, 1, 0,
    4, 1, 1, 2, 0, 1, 1, 0, 19, 1, 1, 0, 26, 1, 1, 1, 0, 10, 1, 1, 0, 8, 1, 0, 1, 1, 0, 7, 1, 0, 1, 1, 0, 4, 1, 1, 2, 5, 6, 7, 8, 2, 5, 3, 0, 1, 1, 1, 0, 2,
    1, 0, 2, 1, 0, 14, 1, 0, 3, 1, 0, 12, 1, 0, 1, 1, 0, 1, 1, 0, 16, 1, 0, 15, 1, 0, 7, 1, 0, 11, 1, 0, 1, 1, 1, 1, 0, 17, 2, 0, 18, 1, 0, 1, 1, 0, 16, 1, 0, 3,
    1, 1, 0, 2, 1, 0, 3, 1, 0, 12, 1, 0, 22, 1, 0, 8, 1, 0, 4, 2, 1, 0, 8, 1, 0, 13, 1, 0, 4, 1, 0, 15, 1, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 0, 12, 1, 0, 15, 1, 0,
    15, 1, 0, 2, 1, 0, 19, 1, 0, 2, 1, 0, 2, 1, 0, 3, 2, 1, 0, 1, 1, 0, 4, 1, 0, 9, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 10, 1, 0, 36, 1, 0, 2, 1, 0, 3,
    1, 0, 1, 1, 0, 13, 1, 0, 11, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 2, 1, 0, 10, 1, 0, 2, 1, 0, 13, 1, 0, 5, 2, 1, 1, 0, 4, 1, 0, 6, 2, 0, 6, 1, 0, 17, 1, 0, 14,
    1, 0, 6, 1, 0, 6, 1, 0, 4, 1, 0, 21, 1, 0, 38, 1, 0, 3, 1, 0, 2, 2, 1, 0, 9, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 5, 1, 0, 8, 1, 0, 2, 1, 0, 16, 1,
    0, 5, 1, 0, 5, 1, 0, 1, 1, 0, 11, 1, 0, 2, 1, 0, 6, 1, 0, 3, 1, 0, 2, 1, 0, 4, 1, 0, 7, 1, 0, 9, 1, 0, 9, 1, 0, 3, 1, 1, 0, 8, 1, 0, 3, 1, 0, 7, 1, 0,
    9, 2, 0, 20, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 3, 1, 0, 24, 1, 0, 7, 1, 0, 4, 1, 1, 1, 0, 10, 1, 0, 6, 1, 0, 3, 1, 2, 2, 2, 1, 2, 0, 1, 2, 0, 8, 1, 0, 9,
    2, 0, 6, 1, 0, 8, 1, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 0, 11, 2, 0, 5, 1, 0, 6, 2, 0, 23, 1, 0, 5, 1, 1, 0, 3, 1, 0, 8, 1, 1, 0, 7, 1, 0, 1, 1, 1, 0, 1, 1,
    0, 18, 2, 1, 0, 3, 1, 0, 5, 1, 0, 2, 1, 2, 0, 2, 1, 0, 5, 1, 0, 1, 1, 1, 1, 0, 7, 1, 1, 2, 1, 0, 5, 1, 0, 6, 1, 0, 6, 1, 0, 2, 1, 0, 15, 1, 0, 11, 1, 0,
    7, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 1, 2, 0, 3, 3, 2, 2, 2, 1, 1, 0, 1, 1, 0, 1, 1, 0, 10, 1, 0, 17, 1, 0, 9, 1, 0, 4, 2, 0, 7, 1,
    0, 4, 1, 0, 4, 1, 0, 26, 1, 0, 16, 1, 0, 4, 1, 0, 10, 1, 0, 34, 2, 0, 24, 2, 1, 0, 5, 1, 0, 17, 1, 0, 4, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 6, 2, 0, 3, 1, 1, 0,
    5, 1, 0, 14, 1, 1, 0, 3, 1, 0, 7, 1, 0, 5, 2, 0, 8, 1, 1, 0, 10, 1, 1, 0, 8, 1, 0, 16, 1, 1, 0, 8, 1, 0, 2, 1, 0, 29, 1, 0, 7, 2, 0, 9, 1, 0, 5, 1, 0, 11,
    1, 0, 9, 1, 0, 4, 1, 0, 15, 1, 0, 10, 1, 0, 3, 1, 0, 8, 1, 0, 1, 1, 0, 1, 1, 0, 5, 2, 0, 10, 1, 0, 17, 1, 0, 9, 1, 0, 4, 2, 0, 1, 1, 0, 6, 1, 0, 11, 1, 0,
    3, 1, 0, 18, 1, 0, 3, 1, 0, 27, 1, 0, 6, 2, 0, 34, 1, 0, 2, 1, 0, 9, 1, 0, 13, 1, 0, 6, 3, 0, 1, 1, 0, 13, 1, 0, 7, 1, 0, 3, 1, 0, 16, 1, 0, 18, 1, 0, 2, 1,
    1, 0, 24, 1, 0, 6, 1, 0, 6, 1, 0, 12, 1, 0, 46, 1, 0, 1, 1, 0, 12, 1, 0, 37, 1, 0, 9, 1, 0, 4, 1, 1, 0, 26, 1, 0, 27, 1, 0, 23, 1, 0, 19, 1, 0, 28, 1, 0, 7, 1,
    0, 73, 1, 0, 2, 1, 0, 1, 2, 2, 10, 4, 3, 6, 9, 5, 0, 1, 1, 2, 1, 0, 40, 1, 0, 17, 1, 0, 7, 1, 0, 1, 1, 0, 72, 1, 0, 49, 1, 0, 10, 1, 0, 11, 1, 0, 95, 1, 0, 87,
    1, 0, 1, 1, 0, 71, 1, 0, 60, 1, 0, 6, 1, 0, 123, 1, 0, 11, 1, 0, 121, 1, 0, 247
  };
  assert( test_10_packed.size() == 6824 );
  const vector<uint8_t> test_10_encoded = QRSpecDev::encode_stream_vbyte( test_10_chan_cnts );
  assert( test_10_encoded == test_10_packed );
  vector<uint32_t> test_10_dec;
  const size_t test_10_nbytedec = QRSpecDev::decode_stream_vbyte(test_10_encoded,test_10_dec);
  assert( test_10_nbytedec == test_10_packed.size() );
  assert( test_10_dec == test_10_chan_cnts );
  
  
  
  
  // Test case 11
  const vector<uint32_t> test_11_chan_cnts{
    0, 4, 1, 39, 44, 48, 74, 156, 232, 261, 170, 96, 81, 68, 85, 104, 84, 99, 124, 124,
    133, 131, 126, 136, 126, 138, 138, 140, 127, 122, 103, 122, 112, 118, 113, 108, 141, 107, 106, 96,
    100, 101, 116, 92, 82, 77, 87, 77, 82, 88, 87, 71, 60, 72, 70, 98, 80, 81, 68, 85,
    80, 78, 72, 71, 87, 78, 91, 56, 80, 66, 74, 71, 71, 58, 57, 64, 65, 51, 63, 53,
    58, 41, 44, 49, 53, 38, 47, 38, 53, 52, 55, 44, 41, 37, 33, 40, 41, 31, 37, 43,
    43, 39, 40, 34, 39, 38, 35, 29, 44, 35, 35, 28, 37, 38, 31, 26, 31, 37, 26, 27,
    27, 24, 27, 29, 34, 47, 27, 32, 41, 32, 35, 30, 33, 35, 32, 25, 30, 34, 32, 33,
    30, 23, 26, 33, 25, 32, 32, 29, 27, 25, 22, 26, 22, 29, 21, 16, 24, 21, 14, 16,
    19, 18, 7, 16, 13, 14, 12, 16, 7, 10, 7, 6, 8, 10, 12, 8, 7, 12, 6, 7,
    6, 4, 4, 9, 7, 2, 5, 7, 8, 8, 2, 10, 4, 11, 5, 5, 6, 8, 5, 8,
    9, 14, 18, 15, 21, 41, 29, 38, 57, 58, 76, 81, 92, 78, 90, 102, 113, 121, 133, 152,
    140, 125, 129, 109, 105, 81, 73, 72, 59, 61, 51, 26, 27, 25, 19, 16, 9, 5, 6, 6,
    6, 5, 2, 1, 1, 1, 0, 1, 1, 2, 1, 1, 0, 1, 1, 2, 5, 1, 1, 1,
    1, 2, 1, 0, 1, 2, 0, 1, 2, 1, 2, 3, 1, 4, 0, 1, 2, 1, 1, 0,
    1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 5,
    2, 0, 1, 1, 1, 2, 0, 4, 1, 1, 2, 1, 2, 1, 1, 0, 1, 1, 3, 1,
    1, 2, 0, 5, 1, 0, 1, 1, 2, 1, 1, 3, 1, 0, 1, 1, 1, 1, 2, 0,
    2, 2, 0, 1, 1, 2, 0, 4, 3, 1, 0, 1, 1, 0, 1, 2, 1, 1, 4, 0,
    1, 2, 1, 0, 1, 2, 0, 3, 1, 0, 4, 1, 0, 3, 2, 1, 4, 1, 1, 1,
    2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 2, 0, 1, 1, 2, 2, 1,
    1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0, 1, 1, 0, 5, 2, 0,
    2, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 3, 1, 1, 1, 0, 3, 1, 0,
    1, 2, 1, 0, 1, 1, 0, 5, 1, 1, 0, 1, 2, 0, 3, 1, 0, 3, 2, 1,
    0, 3, 1, 1, 0, 1, 1, 1, 2, 1, 2, 1, 0, 5, 1, 0, 2, 1, 0, 3,
    1, 0, 13, 1, 0, 3, 1, 0, 9, 1, 1, 2, 0, 1, 1, 0, 10, 1, 1, 1,
    1, 0, 4, 1, 0, 1, 1, 0, 11, 1, 0, 4, 1, 0, 4, 1, 0, 4, 1, 0,
    5, 1, 0, 1, 2, 1, 0, 3, 1, 0, 7, 1, 0, 2, 1, 0, 10, 1, 0, 10,
    1, 0, 3, 2, 1, 1, 1, 1, 0, 19, 1, 1, 0, 1, 1, 0, 12, 1, 0, 4,
    1, 0, 2, 1, 0, 2, 1, 0, 8, 1, 0, 1, 2, 0, 2, 1, 0, 7, 1, 0,
    2, 1, 0, 6, 1, 0, 2, 1, 0, 2, 1, 0, 8, 1, 0, 10, 1, 0, 1, 1,
    0, 3, 1, 0, 3, 1, 1, 0, 11, 1, 1, 0, 1, 1, 0, 6, 1, 0, 2, 1,
    0, 9, 1, 0, 4, 1, 1, 0, 21, 1, 0, 3, 1, 0, 20, 1, 1, 0, 13, 1,
    0, 1, 1, 0, 1, 1, 0, 15, 1, 0, 9, 1, 0, 4, 1, 1, 0, 1, 1, 0,
    4, 1, 2, 0, 2, 1, 0, 3, 1, 0, 2, 1, 0, 15, 2, 0, 102, 1, 0, 1,
    1, 0, 15  };
  assert( test_11_chan_cnts.size() == 683 );
  const vector<uint8_t> test_11_packed{
    171, 2, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 1, 39, 44, 48, 74, 156, 232, 5, 1, 170, 96, 81, 68, 85, 104, 84, 99, 124, 124, 133, 131, 126, 136, 126, 138,
    138, 140, 127, 122, 103, 122, 112, 118, 113, 108, 141, 107, 106, 96, 100, 101, 116, 92, 82, 77, 87, 77, 82, 88, 87, 71, 60, 72, 70, 98, 80, 81, 68, 85, 80, 78, 72, 71, 87, 78, 91, 56, 80, 66, 74, 71, 71, 58, 57, 64,
    65, 51, 63, 53, 58, 41, 44, 49, 53, 38, 47, 38, 53, 52, 55, 44, 41, 37, 33, 40, 41, 31, 37, 43, 43, 39, 40, 34, 39, 38, 35, 29, 44, 35, 35, 28, 37, 38, 31, 26, 31, 37, 26, 27, 27, 24, 27, 29, 34, 47,
    27, 32, 41, 32, 35, 30, 33, 35, 32, 25, 30, 34, 32, 33, 30, 23, 26, 33, 25, 32, 32, 29, 27, 25, 22, 26, 22, 29, 21, 16, 24, 21, 14, 16, 19, 18, 7, 16, 13, 14, 12, 16, 7, 10, 7, 6, 8, 10, 12, 8,
    7, 12, 6, 7, 6, 4, 4, 9, 7, 2, 5, 7, 8, 8, 2, 10, 4, 11, 5, 5, 6, 8, 5, 8, 9, 14, 18, 15, 21, 41, 29, 38, 57, 58, 76, 81, 92, 78, 90, 102, 113, 121, 133, 152, 140, 125, 129, 109, 105, 81,
    73, 72, 59, 61, 51, 26, 27, 25, 19, 16, 9, 5, 6, 6, 6, 5, 2, 1, 1, 1, 0, 1, 1, 2, 1, 1, 0, 1, 1, 2, 5, 1, 1, 1, 1, 2, 1, 0, 1, 2, 0, 1, 2, 1, 2, 3, 1, 4, 0, 1,
    2, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 5, 2, 0, 1, 1, 1, 2, 0, 4, 1, 1, 2, 1, 2, 1, 1, 0, 1, 1, 3, 1, 1, 2, 0, 5, 1, 0,
    1, 1, 2, 1, 1, 3, 1, 0, 1, 1, 1, 1, 2, 0, 2, 2, 0, 1, 1, 2, 0, 4, 3, 1, 0, 1, 1, 0, 1, 2, 1, 1, 4, 0, 1, 2, 1, 0, 1, 2, 0, 3, 1, 0, 4, 1, 0, 3, 2, 1,
    4, 1, 1, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 2, 0, 1, 1, 2, 2, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0, 1, 1, 0, 5, 2, 0, 2, 1, 0, 2, 1, 0,
    1, 1, 0, 2, 1, 0, 3, 1, 1, 1, 0, 3, 1, 0, 1, 2, 1, 0, 1, 1, 0, 5, 1, 1, 0, 1, 2, 0, 3, 1, 0, 3, 2, 1, 0, 3, 1, 1, 0, 1, 1, 1, 2, 1, 2, 1, 0, 5, 1, 0,
    2, 1, 0, 3, 1, 0, 13, 1, 0, 3, 1, 0, 9, 1, 1, 2, 0, 1, 1, 0, 10, 1, 1, 1, 1, 0, 4, 1, 0, 1, 1, 0, 11, 1, 0, 4, 1, 0, 4, 1, 0, 4, 1, 0, 5, 1, 0, 1, 2, 1,
    0, 3, 1, 0, 7, 1, 0, 2, 1, 0, 10, 1, 0, 10, 1, 0, 3, 2, 1, 1, 1, 1, 0, 19, 1, 1, 0, 1, 1, 0, 12, 1, 0, 4, 1, 0, 2, 1, 0, 2, 1, 0, 8, 1, 0, 1, 2, 0, 2, 1,
    0, 7, 1, 0, 2, 1, 0, 6, 1, 0, 2, 1, 0, 2, 1, 0, 8, 1, 0, 10, 1, 0, 1, 1, 0, 3, 1, 0, 3, 1, 1, 0, 11, 1, 1, 0, 1, 1, 0, 6, 1, 0, 2, 1, 0, 9, 1, 0, 4, 1,
    1, 0, 21, 1, 0, 3, 1, 0, 20, 1, 1, 0, 13, 1, 0, 1, 1, 0, 1, 1, 0, 15, 1, 0, 9, 1, 0, 4, 1, 1, 0, 1, 1, 0, 4, 1, 2, 0, 2, 1, 0, 3, 1, 0, 2, 1, 0, 15, 2, 0,
    102, 1, 0, 1, 1, 0, 15
  };
  assert( test_11_packed.size() == 857 );
  const vector<uint8_t> test_11_encoded = QRSpecDev::encode_stream_vbyte( test_11_chan_cnts );
  assert( test_11_encoded == test_11_packed );
  vector<uint32_t> test_11_dec;
  const size_t test_11_nbytedec = QRSpecDev::decode_stream_vbyte(test_11_encoded,test_11_dec);
  assert( test_11_nbytedec == test_11_packed.size() );
  assert( test_11_dec == test_11_chan_cnts );
  
  
  
  
  // Test case 12
  const vector<uint32_t> test_12_chan_cnts{
    0, 4, 640, 27842, 60400, 39922, 15632, 11826, 13035, 12539, 9901, 7714, 6782, 6442, 6581, 6847, 7186, 7227, 7470, 7722,
    8033, 8488, 8674, 9146, 9502, 9503, 9826, 10027, 10057, 10230, 10543, 10874, 11204, 11790, 11948, 12323, 12563, 12740, 13217, 13032,
    12966, 13086, 13121, 13052, 12972, 12952, 13120, 12943, 12943, 13232, 13325, 13306, 13499, 13874, 14352, 14673, 15365, 16150, 16896, 17379,
    17728, 18065, 18123, 17605, 17536, 17185, 16681, 15970, 15539, 15053, 14410, 13865, 13383, 12840, 12246, 11871, 11550, 11346, 11052, 10529,
    10242, 10059, 9758, 9339, 9350, 9035, 8862, 8738, 8428, 8343, 8324, 7957, 8117, 7853, 7656, 7579, 7583, 7459, 7305, 7300,
    7257, 7083, 7030, 6928, 6795, 6847, 6755, 6690, 6673, 6689, 6621, 6618, 6366, 6434, 6468, 6393, 6348, 6442, 6280, 6268,
    6279, 6287, 6367, 6298, 6232, 6283, 6163, 6096, 6072, 6003, 6131, 6126, 6176, 6102, 6061, 6068, 6059, 5961, 6152, 6110,
    5860, 5934, 5879, 5925, 5843, 5724, 5507, 5367, 5164, 4956, 4657, 4551, 4290, 4055, 3822, 3655, 3365, 3231, 3066, 2808,
    2685, 2558, 2347, 2372, 2232, 2199, 2053, 2034, 1979, 1870, 1833, 1739, 1780, 1668, 1660, 1602, 1584, 1534, 1470, 1571,
    1507, 1405, 1364, 1381, 1370, 1394, 1391, 1370, 1303, 1294, 1363, 1326, 1249, 1318, 1338, 1374, 1420, 1545, 1609, 1762,
    2047, 2301, 2724, 3310, 4074, 4765, 5678, 6847, 8136, 9744, 11348, 13015, 14915, 16406, 17811, 19404, 20561, 21269, 22006, 22277,
    22197, 21604, 20646, 19318, 17805, 16026, 14529, 12735, 10941, 9256, 7717, 6394, 5175, 4085, 3196, 2528, 1930, 1497, 1177, 925,
    697, 512, 385, 312, 246, 216, 157, 138, 101, 99, 78, 62, 57, 52, 39, 26, 33, 33, 33, 30,
    32, 22, 25, 24, 17, 23, 28, 26, 14, 24, 17, 23, 23, 29, 16, 15, 21, 17, 22, 17,
    22, 22, 20, 14, 12, 19, 23, 12, 18, 13, 18, 16, 12, 14, 19, 17, 14, 11, 23, 7,
    11, 19, 15, 13, 16, 12, 8, 8, 10, 13, 17, 9, 14, 13, 8, 11, 13, 5, 10, 14,
    9, 8, 9, 13, 7, 14, 11, 10, 5, 11, 13, 12, 9, 10, 9, 10, 10, 9, 3, 8,
    8, 5, 5, 4, 7, 11, 4, 3, 4, 12, 5, 9, 9, 10, 7, 9, 7, 8, 6, 5,
    7, 6, 7, 6, 10, 8, 8, 4, 4, 6, 3, 10, 5, 8, 7, 2, 4, 6, 4, 5,
    9, 2, 1, 3, 5, 7, 1, 5, 3, 4, 5, 6, 4, 2, 5, 6, 2, 3, 6, 3,
    1, 4, 6, 2, 1, 1, 3, 2, 0, 1, 3, 4, 1, 2, 2, 2, 2, 4, 3, 3,
    3, 1, 1, 3, 1, 3, 4, 8, 2, 1, 2, 3, 4, 2, 6, 6, 11, 3, 4, 6,
    8, 9, 5, 5, 6, 7, 8, 7, 7, 2, 8, 5, 6, 5, 3, 1, 4, 7, 5, 3,
    3, 2, 4, 4, 3, 2, 2, 4, 0, 1, 3, 5, 3, 3, 3, 4, 3, 3, 6, 1,
    0, 1, 1, 1, 1, 0, 3, 2, 2, 3, 1, 1, 1, 1, 2, 4, 2, 1, 3, 1,
    1, 0, 1, 3, 1, 2, 0, 2, 2, 1, 0, 1, 4, 0, 1, 1, 0, 1, 2, 1,
    1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 4, 1, 1, 1, 1,
    0, 1, 1, 2, 0, 1, 3, 1, 1, 2, 1, 2, 2, 2, 2, 0, 2, 1, 3, 1,
    0, 1, 2, 2, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 3, 3, 1, 1, 1, 0,
    1, 2, 2, 2, 1, 3, 0, 1, 2, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2,
    0, 1, 3, 0, 2, 1, 4, 0, 3, 1, 2, 1, 0, 1, 2, 2, 1, 0, 3, 1,
    0, 5, 1, 0, 4, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1,
    1, 0, 2, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 0, 10, 1, 0, 10, 1, 0,
    5, 1, 0, 25, 1, 0, 5, 1, 0, 6, 1, 0, 11, 1, 1, 1, 0, 3, 1, 1,
    0, 2, 2, 0, 5, 2, 0, 15, 1, 0, 6, 1, 0, 4, 1, 0, 22, 2, 0, 10,
    1, 0, 13, 1, 0, 35, 1, 0, 8, 1, 1, 0, 16, 1, 0, 7, 1, 0, 10, 1,
    1, 0, 13, 1, 0, 23, 1, 0, 3, 1, 0, 4, 1, 0, 1, 1, 0, 4, 1, 0,
    27, 1, 0, 6, 1, 0, 42  };
  assert( test_12_chan_cnts.size() == 747 );
  const vector<uint8_t> test_12_packed{
    235, 2, 80, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 128, 2, 194, 108, 240, 235, 242, 155, 16,
    61, 50, 46, 235, 50, 251, 48, 173, 38, 34, 30, 126, 26, 42, 25, 181, 25, 191, 26, 18, 28, 59, 28, 46, 29, 42, 30, 97, 31, 40, 33, 226, 33, 186, 35, 30, 37, 31, 37, 98, 38, 43, 39, 73, 39, 246, 39, 47, 41, 122,
    42, 196, 43, 14, 46, 172, 46, 35, 48, 19, 49, 196, 49, 161, 51, 232, 50, 166, 50, 30, 51, 65, 51, 252, 50, 172, 50, 152, 50, 64, 51, 143, 50, 143, 50, 176, 51, 13, 52, 250, 51, 187, 52, 50, 54, 16, 56, 81, 57, 5,
    60, 22, 63, 0, 66, 227, 67, 64, 69, 145, 70, 203, 70, 197, 68, 128, 68, 33, 67, 41, 65, 98, 62, 179, 60, 205, 58, 74, 56, 41, 54, 71, 52, 40, 50, 214, 47, 95, 46, 30, 45, 82, 44, 44, 43, 33, 41, 2, 40, 75,
    39, 30, 38, 123, 36, 134, 36, 75, 35, 158, 34, 34, 34, 236, 32, 151, 32, 132, 32, 21, 31, 181, 31, 173, 30, 232, 29, 155, 29, 159, 29, 35, 29, 137, 28, 132, 28, 89, 28, 171, 27, 118, 27, 16, 27, 139, 26, 191, 26, 99,
    26, 34, 26, 17, 26, 33, 26, 221, 25, 218, 25, 222, 24, 34, 25, 68, 25, 249, 24, 204, 24, 42, 25, 136, 24, 124, 24, 135, 24, 143, 24, 223, 24, 154, 24, 88, 24, 139, 24, 19, 24, 208, 23, 184, 23, 115, 23, 243, 23, 238,
    23, 32, 24, 214, 23, 173, 23, 180, 23, 171, 23, 73, 23, 8, 24, 222, 23, 228, 22, 46, 23, 247, 22, 37, 23, 211, 22, 92, 22, 131, 21, 247, 20, 44, 20, 92, 19, 49, 18, 199, 17, 194, 16, 215, 15, 238, 14, 71, 14, 37,
    13, 159, 12, 250, 11, 248, 10, 125, 10, 254, 9, 43, 9, 68, 9, 184, 8, 151, 8, 5, 8, 242, 7, 187, 7, 78, 7, 41, 7, 203, 6, 244, 6, 132, 6, 124, 6, 66, 6, 48, 6, 254, 5, 190, 5, 35, 6, 227, 5, 125,
    5, 84, 5, 101, 5, 90, 5, 114, 5, 111, 5, 90, 5, 23, 5, 14, 5, 83, 5, 46, 5, 225, 4, 38, 5, 58, 5, 94, 5, 140, 5, 9, 6, 73, 6, 226, 6, 255, 7, 253, 8, 164, 10, 238, 12, 234, 15, 157, 18, 46,
    22, 191, 26, 200, 31, 16, 38, 84, 44, 215, 50, 67, 58, 22, 64, 147, 69, 204, 75, 81, 80, 21, 83, 246, 85, 5, 87, 181, 86, 100, 84, 166, 80, 118, 75, 141, 69, 154, 62, 193, 56, 191, 49, 189, 42, 40, 36, 37, 30, 250,
    24, 55, 20, 245, 15, 124, 12, 224, 9, 138, 7, 217, 5, 153, 4, 157, 3, 185, 2, 0, 2, 129, 1, 56, 1, 246, 216, 157, 138, 101, 99, 78, 62, 57, 52, 39, 26, 33, 33, 33, 30, 32, 22, 25, 24, 17, 23, 28, 26, 14,
    24, 17, 23, 23, 29, 16, 15, 21, 17, 22, 17, 22, 22, 20, 14, 12, 19, 23, 12, 18, 13, 18, 16, 12, 14, 19, 17, 14, 11, 23, 7, 11, 19, 15, 13, 16, 12, 8, 8, 10, 13, 17, 9, 14, 13, 8, 11, 13, 5, 10,
    14, 9, 8, 9, 13, 7, 14, 11, 10, 5, 11, 13, 12, 9, 10, 9, 10, 10, 9, 3, 8, 8, 5, 5, 4, 7, 11, 4, 3, 4, 12, 5, 9, 9, 10, 7, 9, 7, 8, 6, 5, 7, 6, 7, 6, 10, 8, 8, 4, 4,
    6, 3, 10, 5, 8, 7, 2, 4, 6, 4, 5, 9, 2, 1, 3, 5, 7, 1, 5, 3, 4, 5, 6, 4, 2, 5, 6, 2, 3, 6, 3, 1, 4, 6, 2, 1, 1, 3, 2, 0, 1, 3, 4, 1, 2, 2, 2, 2, 4, 3,
    3, 3, 1, 1, 3, 1, 3, 4, 8, 2, 1, 2, 3, 4, 2, 6, 6, 11, 3, 4, 6, 8, 9, 5, 5, 6, 7, 8, 7, 7, 2, 8, 5, 6, 5, 3, 1, 4, 7, 5, 3, 3, 2, 4, 4, 3, 2, 2, 4, 0,
    1, 3, 5, 3, 3, 3, 4, 3, 3, 6, 1, 0, 1, 1, 1, 1, 0, 3, 2, 2, 3, 1, 1, 1, 1, 2, 4, 2, 1, 3, 1, 1, 0, 1, 3, 1, 2, 0, 2, 2, 1, 0, 1, 4, 0, 1, 1, 0, 1, 2,
    1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 4, 1, 1, 1, 1, 0, 1, 1, 2, 0, 1, 3, 1, 1, 2, 1, 2, 2, 2, 2, 0, 2, 1, 3, 1, 0, 1, 2, 2, 1, 1, 0, 1, 1,
    1, 1, 0, 1, 1, 3, 3, 1, 1, 1, 0, 1, 2, 2, 2, 1, 3, 0, 1, 2, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 1, 3, 0, 2, 1, 4, 0, 3, 1, 2, 1, 0, 1, 2, 2, 1, 0, 3,
    1, 0, 5, 1, 0, 4, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 0, 10, 1, 0, 10, 1, 0, 5, 1, 0, 25, 1, 0, 5, 1, 0,
    6, 1, 0, 11, 1, 1, 1, 0, 3, 1, 1, 0, 2, 2, 0, 5, 2, 0, 15, 1, 0, 6, 1, 0, 4, 1, 0, 22, 2, 0, 10, 1, 0, 13, 1, 0, 35, 1, 0, 8, 1, 1, 0, 16, 1, 0, 7, 1, 0, 10,
    1, 1, 0, 13, 1, 0, 23, 1, 0, 3, 1, 0, 4, 1, 0, 1, 1, 0, 4, 1, 0, 27, 1, 0, 6, 1, 0, 42
  };
  assert( test_12_packed.size() == 1178 );
  const vector<uint8_t> test_12_encoded = QRSpecDev::encode_stream_vbyte( test_12_chan_cnts );
  assert( test_12_encoded == test_12_packed );
  vector<uint32_t> test_12_dec;
  const size_t test_12_nbytedec = QRSpecDev::decode_stream_vbyte(test_12_encoded,test_12_dec);
  assert( test_12_nbytedec == test_12_packed.size() );
  assert( test_12_dec == test_12_chan_cnts );
  
  
  
  
  // Test case 13
  const vector<uint32_t> test_13_chan_cnts{
    0, 4, 110, 1800, 2756, 5283, 13578, 30946, 42207, 32407, 15645, 7101, 4979, 5595, 7156, 8478, 8280, 8446, 8960, 9891,
    10613, 11289, 13682, 19197, 26244, 29672, 26630, 18785, 10985, 6370, 4592, 4137, 4152, 4260, 4319, 4457, 4502, 4542, 4739, 4826,
    4757, 4956, 4997, 5495, 5787, 5965, 6285, 6479, 6605, 6672, 6403, 6342, 6076, 5566, 5036, 4583, 4260, 3969, 3716, 3594,
    3330, 3128, 2922, 2626, 2630, 2441, 2245, 2146, 2078, 2003, 1964, 1904, 1775, 1773, 1677, 1591, 1566, 1465, 1497, 1474,
    1515, 1532, 1713, 1828, 2074, 2292, 2469, 2770, 2991, 3041, 3272, 3478, 3633, 3847, 4174, 4475, 4557, 4719, 4619, 4306,
    4135, 3744, 3381, 3101, 2835, 2666, 2715, 3118, 3796, 4509, 5316, 6538, 7454, 8361, 8979, 9691, 9564, 9544, 9159, 8326,
    7645, 6649, 5667, 4739, 3961, 3193, 2721, 2245, 1843, 1506, 1305, 1050, 890, 699, 548, 459, 362, 320, 296, 223,
    240, 229, 223, 223, 235, 265, 264, 227, 229, 230, 176, 174, 138, 140, 90, 85, 71, 60, 50, 47,
    30, 24, 24, 24, 17, 13, 21, 13, 12, 18, 10, 7, 13, 7, 13, 9, 5, 10, 4, 7,
    2, 7, 5, 9, 5, 3, 4, 4, 4, 8, 9, 8, 7, 8, 8, 1, 9, 7, 8, 13,
    13, 11, 11, 16, 17, 23, 36, 45, 50, 65, 73, 79, 72, 88, 124, 134, 130, 132, 132, 140,
    147, 113, 132, 121, 129, 79, 85, 65, 64, 42, 33, 31, 26, 19, 27, 10, 13, 4, 10, 5,
    1, 1, 4, 2, 1, 5, 2, 0, 1, 3, 4, 1, 3, 1, 2, 3, 3, 2, 1, 1,
    0, 1, 1, 0, 1, 1, 1, 1, 1, 3, 1, 0, 1, 1, 0, 1, 2, 2, 1, 0,
    2, 1, 1, 2, 0, 2, 1, 2, 1, 2, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 1, 3, 3, 1, 5, 2, 2, 0, 1, 1, 1, 1, 2, 2, 2, 2, 1, 0,
    4, 2, 1, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 5, 1, 1, 3, 1, 0, 7,
    1, 1, 1, 0, 3, 1, 0, 2, 2, 1, 0, 1, 2, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 1, 1, 1, 1, 1, 0, 3, 1, 0, 1, 2, 0, 2, 1, 0, 1, 1, 0,
    1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 6, 1, 1, 0, 1, 1, 0, 1, 1,
    0, 1, 1, 1, 1, 0, 3, 1, 1, 0, 1, 1, 0, 2, 2, 2, 1, 0, 1, 1,
    0, 5, 2, 0, 2, 1, 0, 9, 1, 1, 1, 0, 4, 1, 0, 4, 1, 1, 0, 7,
    1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 2, 0, 1, 1, 1, 0, 2, 2, 1, 0,
    3, 1, 1, 0, 7, 1, 1, 0, 3, 1, 0, 4, 1, 1, 0, 4, 1, 0, 1, 1,
    0, 1, 1, 0, 5, 1, 0, 5, 1, 0, 2, 1, 1, 0, 2, 1, 0, 1, 1, 1,
    0, 10, 2, 0, 2, 1, 1, 0, 4, 1, 0, 1, 1, 1, 0, 1, 1, 0, 14, 1,
    0, 4, 1, 0, 5, 1, 1, 0, 2, 1, 0, 3, 1, 0, 24, 1, 1, 0, 3, 1,
    0, 3, 3, 0, 16, 2, 0, 9, 1, 0, 15, 1, 0, 12, 1, 0, 3, 1, 0, 3,
    1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1, 0, 4, 1, 1, 0, 2, 1, 0, 5,
    2, 0, 11, 1, 0, 1, 1, 0, 9, 1, 1, 0, 6, 1, 0, 6, 1, 0, 16, 1,
    0, 13, 1, 0, 1, 1, 0, 3, 1, 0, 6, 1, 0, 34, 1, 0, 26, 1, 0, 2,
    1, 0, 8, 1, 0, 18, 1, 0, 14, 1, 0, 6, 1, 0, 14, 1, 0, 79, 1, 1,
    0, 21  };
  assert( test_13_chan_cnts.size() == 642 );
  const vector<uint8_t> test_13_packed{
    130, 2, 64, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 21, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 110, 8, 7, 196, 10, 163, 20, 10, 53, 226, 120, 223, 164, 151, 126, 29, 61, 189, 27, 115, 19, 219, 21, 244, 27, 30, 33, 88, 32, 254, 32, 0, 35, 163, 38,
    117, 41, 25, 44, 114, 53, 253, 74, 132, 102, 232, 115, 6, 104, 97, 73, 233, 42, 226, 24, 240, 17, 41, 16, 56, 16, 164, 16, 223, 16, 105, 17, 150, 17, 190, 17, 131, 18, 218, 18, 149, 18, 92, 19, 133, 19, 119, 21, 155, 22,
    77, 23, 141, 24, 79, 25, 205, 25, 16, 26, 3, 25, 198, 24, 188, 23, 190, 21, 172, 19, 231, 17, 164, 16, 129, 15, 132, 14, 10, 14, 2, 13, 56, 12, 106, 11, 66, 10, 70, 10, 137, 9, 197, 8, 98, 8, 30, 8, 211, 7,
    172, 7, 112, 7, 239, 6, 237, 6, 141, 6, 55, 6, 30, 6, 185, 5, 217, 5, 194, 5, 235, 5, 252, 5, 177, 6, 36, 7, 26, 8, 244, 8, 165, 9, 210, 10, 175, 11, 225, 11, 200, 12, 150, 13, 49, 14, 7, 15, 78, 16,
    123, 17, 205, 17, 111, 18, 11, 18, 210, 16, 39, 16, 160, 14, 53, 13, 29, 12, 19, 11, 106, 10, 155, 10, 46, 12, 212, 14, 157, 17, 196, 20, 138, 25, 30, 29, 169, 32, 19, 35, 219, 37, 92, 37, 72, 37, 199, 35, 134, 32,
    221, 29, 249, 25, 35, 22, 131, 18, 121, 15, 121, 12, 161, 10, 197, 8, 51, 7, 226, 5, 25, 5, 26, 4, 122, 3, 187, 2, 36, 2, 203, 1, 106, 1, 64, 1, 40, 1, 223, 240, 229, 223, 223, 235, 9, 1, 8, 1, 227, 229,
    230, 176, 174, 138, 140, 90, 85, 71, 60, 50, 47, 30, 24, 24, 24, 17, 13, 21, 13, 12, 18, 10, 7, 13, 7, 13, 9, 5, 10, 4, 7, 2, 7, 5, 9, 5, 3, 4, 4, 4, 8, 9, 8, 7, 8, 8, 1, 9, 7, 8,
    13, 13, 11, 11, 16, 17, 23, 36, 45, 50, 65, 73, 79, 72, 88, 124, 134, 130, 132, 132, 140, 147, 113, 132, 121, 129, 79, 85, 65, 64, 42, 33, 31, 26, 19, 27, 10, 13, 4, 10, 5, 1, 1, 4, 2, 1, 5, 2, 0, 1,
    3, 4, 1, 3, 1, 2, 3, 3, 2, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 3, 1, 0, 1, 1, 0, 1, 2, 2, 1, 0, 2, 1, 1, 2, 0, 2, 1, 2, 1, 2, 0, 1, 1, 1, 1, 0, 1, 1, 0,
    1, 1, 0, 1, 3, 3, 1, 5, 2, 2, 0, 1, 1, 1, 1, 2, 2, 2, 2, 1, 0, 4, 2, 1, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 5, 1, 1, 3, 1, 0, 7, 1, 1, 1, 0, 3, 1, 0, 2, 2,
    1, 0, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 3, 1, 0, 1, 2, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 6, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 1, 1, 1, 1, 0, 3, 1, 1, 0, 1, 1, 0, 2, 2, 2, 1, 0, 1, 1, 0, 5, 2, 0, 2, 1, 0, 9, 1, 1, 1, 0, 4, 1, 0, 4, 1, 1, 0, 7, 1, 0, 2, 1, 0, 2, 1, 0, 2,
    1, 2, 0, 1, 1, 1, 0, 2, 2, 1, 0, 3, 1, 1, 0, 7, 1, 1, 0, 3, 1, 0, 4, 1, 1, 0, 4, 1, 0, 1, 1, 0, 1, 1, 0, 5, 1, 0, 5, 1, 0, 2, 1, 1, 0, 2, 1, 0, 1, 1,
    1, 0, 10, 2, 0, 2, 1, 1, 0, 4, 1, 0, 1, 1, 1, 0, 1, 1, 0, 14, 1, 0, 4, 1, 0, 5, 1, 1, 0, 2, 1, 0, 3, 1, 0, 24, 1, 1, 0, 3, 1, 0, 3, 3, 0, 16, 2, 0, 9, 1,
    0, 15, 1, 0, 12, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1, 0, 4, 1, 1, 0, 2, 1, 0, 5, 2, 0, 11, 1, 0, 1, 1, 0, 9, 1, 1, 0, 6, 1, 0, 6, 1, 0, 16,
    1, 0, 13, 1, 0, 1, 1, 0, 3, 1, 0, 6, 1, 0, 34, 1, 0, 26, 1, 0, 2, 1, 0, 8, 1, 0, 18, 1, 0, 14, 1, 0, 6, 1, 0, 14, 1, 0, 79, 1, 1, 0, 21
  };
  assert( test_13_packed.size() == 943 );
  const vector<uint8_t> test_13_encoded = QRSpecDev::encode_stream_vbyte( test_13_chan_cnts );
  assert( test_13_encoded == test_13_packed );
  vector<uint32_t> test_13_dec;
  const size_t test_13_nbytedec = QRSpecDev::decode_stream_vbyte(test_13_encoded,test_13_dec);
  assert( test_13_nbytedec == test_13_packed.size() );
  assert( test_13_dec == test_13_chan_cnts );
  
  
  
  
  // Test case 14
  const vector<uint32_t> test_14_chan_cnts{
    0, 4, 26, 64, 63, 65, 104, 189, 340, 308, 197, 121, 97, 97, 100, 118, 120, 127, 136, 152,
    149, 165, 201, 171, 179, 195, 179, 201, 200, 204, 196, 191, 205, 205, 177, 185, 165, 172, 173, 164,
    161, 148, 144, 126, 128, 135, 145, 125, 128, 116, 129, 138, 94, 108, 103, 113, 106, 115, 107, 124,
    122, 106, 105, 101, 97, 109, 105, 105, 94, 90, 78, 100, 79, 84, 75, 73, 73, 78, 62, 89,
    73, 77, 68, 77, 65, 73, 66, 77, 68, 63, 60, 69, 55, 55, 41, 43, 44, 47, 50, 45,
    50, 50, 48, 40, 50, 51, 39, 38, 41, 38, 37, 44, 44, 38, 42, 47, 49, 43, 46, 36,
    41, 48, 54, 43, 39, 31, 41, 33, 26, 41, 45, 38, 40, 43, 39, 36, 37, 41, 34, 36,
    35, 40, 39, 37, 40, 40, 38, 29, 34, 38, 33, 33, 31, 27, 23, 24, 28, 33, 27, 19,
    16, 21, 20, 20, 15, 15, 17, 15, 7, 13, 16, 17, 11, 12, 6, 13, 8, 5, 6, 11,
    7, 8, 6, 5, 7, 7, 7, 13, 8, 5, 11, 10, 6, 11, 11, 8, 5, 5, 13, 12,
    15, 18, 21, 23, 27, 40, 37, 38, 58, 72, 68, 94, 90, 107, 126, 133, 149, 155, 154, 129,
    128, 150, 157, 137, 125, 120, 110, 87, 78, 81, 58, 52, 51, 35, 24, 22, 17, 13, 7, 11,
    5, 6, 3, 1, 3, 1, 3, 7, 2, 6, 3, 2, 0, 1, 4, 5, 4, 2, 0, 1,
    3, 3, 1, 3, 2, 3, 4, 2, 2, 2, 0, 1, 4, 2, 3, 3, 3, 4, 0, 1,
    4, 2, 2, 1, 1, 4, 5, 4, 2, 1, 2, 1, 3, 1, 5, 1, 2, 6, 1, 4,
    4, 1, 0, 2, 2, 1, 0, 2, 4, 1, 2, 4, 1, 2, 2, 1, 1, 1, 4, 3,
    1, 2, 1, 1, 3, 7, 4, 1, 3, 0, 3, 2, 1, 3, 1, 0, 1, 2, 3, 3,
    2, 1, 2, 0, 2, 3, 2, 1, 2, 2, 1, 2, 3, 1, 1, 1, 2, 4, 2, 1,
    0, 1, 1, 2, 0, 1, 2, 1, 1, 2, 3, 1, 1, 1, 0, 1, 3, 1, 1, 4,
    2, 2, 2, 2, 1, 2, 2, 0, 2, 2, 4, 2, 2, 0, 1, 5, 1, 0, 1, 2,
    4, 1, 0, 2, 3, 1, 0, 1, 1, 2, 0, 2, 1, 3, 1, 1, 0, 1, 2, 1,
    0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 2, 0, 1, 1, 1, 3, 1, 0, 3, 1,
    0, 1, 2, 0, 4, 1, 1, 1, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 3, 1,
    1, 1, 2, 0, 2, 1, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 1, 1, 3, 2,
    1, 1, 1, 1, 0, 1, 2, 0, 3, 1, 2, 0, 1, 2, 5, 2, 1, 1, 2, 0,
    1, 2, 3, 0, 1, 1, 3, 2, 0, 1, 1, 2, 0, 1, 1, 1, 1, 1, 1, 0,
    3, 1, 0, 1, 2, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 2, 2, 1, 0,
    6, 1, 0, 2, 1, 1, 0, 2, 1, 0, 1, 2, 1, 0, 2, 1, 1, 0, 7, 1,
    1, 1, 0, 1, 1, 0, 6, 1, 1, 0, 2, 1, 1, 0, 1, 1, 0, 9, 1, 0,
    7, 3, 1, 0, 1, 1, 0, 3, 2, 1, 0, 1, 1, 0, 7, 1, 0, 7, 1, 0,
    11, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1, 0, 5, 3, 0, 3, 2, 0, 5, 1,
    0, 9, 2, 1, 0, 9, 1, 1, 2, 0, 4, 1, 0, 10, 1, 0, 19, 2, 0, 2,
    1, 0, 2, 1, 0, 4, 1, 0, 3, 1, 0, 2, 1, 0, 13, 1, 0, 7, 1, 0,
    4, 1, 0, 3, 1, 0, 3, 1, 0, 13, 1, 0, 3, 1, 1, 0, 20, 1, 0, 50,
    1, 0, 1, 1, 0, 2, 1, 0, 12, 1, 0, 1, 1, 0, 7, 1, 0, 5, 1, 0,
    10, 1, 0, 1, 1, 0, 4, 1, 0, 21, 1, 0, 1, 1, 0, 9, 1, 0, 84  };
  assert( test_14_chan_cnts.size() == 719 );
  const vector<uint8_t> test_14_packed{
    207, 2, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 26, 64, 63, 65, 104, 189, 84, 1, 52, 1, 197, 121, 97, 97, 100, 118,
    120, 127, 136, 152, 149, 165, 201, 171, 179, 195, 179, 201, 200, 204, 196, 191, 205, 205, 177, 185, 165, 172, 173, 164, 161, 148, 144, 126, 128, 135, 145, 125, 128, 116, 129, 138, 94, 108, 103, 113, 106, 115, 107, 124, 122, 106, 105, 101, 97, 109,
    105, 105, 94, 90, 78, 100, 79, 84, 75, 73, 73, 78, 62, 89, 73, 77, 68, 77, 65, 73, 66, 77, 68, 63, 60, 69, 55, 55, 41, 43, 44, 47, 50, 45, 50, 50, 48, 40, 50, 51, 39, 38, 41, 38, 37, 44, 44, 38, 42, 47,
    49, 43, 46, 36, 41, 48, 54, 43, 39, 31, 41, 33, 26, 41, 45, 38, 40, 43, 39, 36, 37, 41, 34, 36, 35, 40, 39, 37, 40, 40, 38, 29, 34, 38, 33, 33, 31, 27, 23, 24, 28, 33, 27, 19, 16, 21, 20, 20, 15, 15,
    17, 15, 7, 13, 16, 17, 11, 12, 6, 13, 8, 5, 6, 11, 7, 8, 6, 5, 7, 7, 7, 13, 8, 5, 11, 10, 6, 11, 11, 8, 5, 5, 13, 12, 15, 18, 21, 23, 27, 40, 37, 38, 58, 72, 68, 94, 90, 107, 126, 133,
    149, 155, 154, 129, 128, 150, 157, 137, 125, 120, 110, 87, 78, 81, 58, 52, 51, 35, 24, 22, 17, 13, 7, 11, 5, 6, 3, 1, 3, 1, 3, 7, 2, 6, 3, 2, 0, 1, 4, 5, 4, 2, 0, 1, 3, 3, 1, 3, 2, 3,
    4, 2, 2, 2, 0, 1, 4, 2, 3, 3, 3, 4, 0, 1, 4, 2, 2, 1, 1, 4, 5, 4, 2, 1, 2, 1, 3, 1, 5, 1, 2, 6, 1, 4, 4, 1, 0, 2, 2, 1, 0, 2, 4, 1, 2, 4, 1, 2, 2, 1,
    1, 1, 4, 3, 1, 2, 1, 1, 3, 7, 4, 1, 3, 0, 3, 2, 1, 3, 1, 0, 1, 2, 3, 3, 2, 1, 2, 0, 2, 3, 2, 1, 2, 2, 1, 2, 3, 1, 1, 1, 2, 4, 2, 1, 0, 1, 1, 2, 0, 1,
    2, 1, 1, 2, 3, 1, 1, 1, 0, 1, 3, 1, 1, 4, 2, 2, 2, 2, 1, 2, 2, 0, 2, 2, 4, 2, 2, 0, 1, 5, 1, 0, 1, 2, 4, 1, 0, 2, 3, 1, 0, 1, 1, 2, 0, 2, 1, 3, 1, 1,
    0, 1, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 2, 0, 1, 1, 1, 3, 1, 0, 3, 1, 0, 1, 2, 0, 4, 1, 1, 1, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 3, 1, 1, 1, 2, 0, 2, 1,
    1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 0, 1, 2, 0, 3, 1, 2, 0, 1, 2, 5, 2, 1, 1, 2, 0, 1, 2, 3, 0, 1, 1, 3, 2, 0, 1, 1, 2, 0, 1, 1, 1,
    1, 1, 1, 0, 3, 1, 0, 1, 2, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 2, 2, 1, 0, 6, 1, 0, 2, 1, 1, 0, 2, 1, 0, 1, 2, 1, 0, 2, 1, 1, 0, 7, 1, 1, 1, 0, 1, 1, 0,
    6, 1, 1, 0, 2, 1, 1, 0, 1, 1, 0, 9, 1, 0, 7, 3, 1, 0, 1, 1, 0, 3, 2, 1, 0, 1, 1, 0, 7, 1, 0, 7, 1, 0, 11, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1, 0, 5, 3, 0, 3,
    2, 0, 5, 1, 0, 9, 2, 1, 0, 9, 1, 1, 2, 0, 4, 1, 0, 10, 1, 0, 19, 2, 0, 2, 1, 0, 2, 1, 0, 4, 1, 0, 3, 1, 0, 2, 1, 0, 13, 1, 0, 7, 1, 0, 4, 1, 0, 3, 1, 0,
    3, 1, 0, 13, 1, 0, 3, 1, 1, 0, 20, 1, 0, 50, 1, 0, 1, 1, 0, 2, 1, 0, 12, 1, 0, 1, 1, 0, 7, 1, 0, 5, 1, 0, 10, 1, 0, 1, 1, 0, 4, 1, 0, 21, 1, 0, 1, 1, 0, 9,
    1, 0, 84
  };
  assert( test_14_packed.size() == 903 );
  const vector<uint8_t> test_14_encoded = QRSpecDev::encode_stream_vbyte( test_14_chan_cnts );
  assert( test_14_encoded == test_14_packed );
  vector<uint32_t> test_14_dec;
  const size_t test_14_nbytedec = QRSpecDev::decode_stream_vbyte(test_14_encoded,test_14_dec);
  assert( test_14_nbytedec == test_14_packed.size() );
  assert( test_14_dec == test_14_chan_cnts );
  
  
  
  
  // Test case 15
  const vector<uint32_t> test_15_chan_cnts{
    0, 4, 1345, 2990, 3559, 4236, 5032, 5280, 5864, 6045, 5821, 6295, 6679, 7701, 8667, 9323, 10084, 11344, 12787, 14139,
    15543, 16788, 18529, 20930, 23059, 23220, 20112, 16446, 13782, 12905, 12809, 13236, 13583, 13926, 14253, 14302, 14528, 14557, 14713, 14867,
    15255, 15667, 15726, 16193, 16508, 17174, 17585, 17816, 18025, 17829, 17455, 16666, 15919, 15344, 14446, 13510, 12998, 12240, 11734, 11116,
    10540, 10077, 9702, 9402, 9015, 8464, 8181, 7876, 7720, 7562, 7222, 7128, 7006, 6664, 6594, 6372, 6314, 6057, 5965, 5993,
    5791, 6030, 6142, 6347, 6732, 6827, 7217, 7376, 7547, 7801, 7889, 8202, 8233, 8664, 8836, 8950, 8964, 8844, 8517, 8002,
    7721, 7048, 6322, 6110, 5734, 6094, 6290, 6961, 8123, 9331, 10550, 11823, 13124, 13983, 14493, 14681, 14706, 14130, 12904, 11765,
    10404, 8984, 7564, 6258, 5176, 4173, 3475, 2818, 2169, 1850, 1547, 1223, 994, 724, 576, 411, 310, 243, 187, 142,
    119, 84, 99, 71, 64, 53, 55, 55, 48, 43, 45, 46, 35, 45, 31, 32, 30, 35, 27, 19,
    23, 30, 26, 29, 17, 24, 26, 13, 18, 13, 14, 15, 12, 11, 16, 20, 18, 14, 7, 12,
    15, 13, 14, 12, 10, 10, 7, 12, 6, 11, 11, 16, 11, 13, 14, 9, 9, 10, 21, 16,
    21, 23, 26, 29, 27, 37, 52, 44, 71, 79, 81, 116, 107, 106, 142, 137, 138, 151, 160, 163,
    133, 135, 125, 134, 106, 107, 82, 75, 61, 71, 45, 28, 30, 20, 17, 20, 11, 10, 9, 6,
    2, 6, 3, 5, 3, 3, 5, 4, 4, 1, 3, 2, 4, 1, 2, 2, 0, 2, 3, 2,
    4, 1, 1, 3, 0, 1, 1, 1, 0, 1, 1, 3, 3, 0, 1, 1, 1, 3, 1, 1,
    1, 1, 1, 2, 2, 2, 1, 2, 2, 1, 2, 3, 0, 1, 1, 1, 4, 1, 1, 2,
    1, 0, 1, 2, 3, 2, 1, 0, 1, 2, 2, 0, 1, 4, 1, 4, 2, 0, 1, 1,
    0, 1, 1, 3, 0, 2, 3, 5, 0, 1, 3, 3, 0, 1, 2, 0, 2, 1, 1, 0,
    2, 2, 3, 1, 0, 2, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 3, 1, 2, 1,
    1, 0, 1, 1, 0, 1, 2, 1, 1, 1, 3, 2, 0, 1, 3, 0, 1, 1, 0, 1,
    1, 1, 1, 1, 1, 5, 2, 3, 0, 1, 1, 2, 1, 1, 0, 4, 1, 1, 0, 1,
    1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 3, 1, 0, 2, 1, 0, 1, 1, 0,
    1, 1, 1, 0, 2, 1, 0, 1, 1, 2, 2, 1, 0, 1, 1, 2, 1, 0, 1, 1,
    1, 0, 1, 1, 1, 0, 2, 2, 1, 0, 4, 1, 0, 4, 1, 1, 1, 0, 1, 1,
    0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 5, 1, 0, 2, 1, 1, 1, 0, 1,
    1, 0, 1, 1, 2, 0, 2, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 6, 1, 1,
    2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 2, 0, 2, 2, 1, 1, 1, 2, 0,
    1, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 0, 4, 1,
    0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 2, 0, 2, 1, 0, 1, 1, 0,
    5, 1, 0, 2, 1, 0, 1, 1, 0, 14, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1,
    0, 4, 1, 0, 4, 1, 0, 2, 1, 0, 2, 1, 0, 17, 1, 0, 6, 1, 0, 1,
    1, 1, 0, 5, 1, 0, 2, 1, 0, 4, 1, 1, 0, 11, 1, 1, 0, 4, 1, 0,
    9, 1, 0, 5, 1, 0, 1, 1, 0, 27, 1, 0, 1, 1, 0, 5, 1, 1, 0, 2,
    1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0, 15, 1, 0, 14,
    1, 0, 2, 1, 0, 7, 1, 0, 5, 1, 0, 6, 1, 0, 14, 1, 0, 47, 1, 1,
    0, 6, 1, 0, 8, 1, 0, 9, 1, 0, 9, 1, 1, 0, 69, 1, 0, 25, 1, 0,
    10, 1, 0, 1, 1, 0, 14, 1, 0, 23  };
  assert( test_15_chan_cnts.size() == 710 );
  const vector<uint8_t> test_15_packed{
    198, 2, 80, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 65, 5, 174, 11, 231, 13, 140, 16, 168, 19, 160, 20, 232, 22, 157, 23, 189, 22,
    151, 24, 23, 26, 21, 30, 219, 33, 107, 36, 100, 39, 80, 44, 243, 49, 59, 55, 183, 60, 148, 65, 97, 72, 194, 81, 19, 90, 180, 90, 144, 78, 62, 64, 214, 53, 105, 50, 9, 50, 180, 51, 15, 53, 102, 54, 173, 55, 222, 55,
    192, 56, 221, 56, 121, 57, 19, 58, 151, 59, 51, 61, 110, 61, 65, 63, 124, 64, 22, 67, 177, 68, 152, 69, 105, 70, 165, 69, 47, 68, 26, 65, 47, 62, 240, 59, 110, 56, 198, 52, 198, 50, 208, 47, 214, 45, 108, 43, 44, 41,
    93, 39, 230, 37, 186, 36, 55, 35, 16, 33, 245, 31, 196, 30, 40, 30, 138, 29, 54, 28, 216, 27, 94, 27, 8, 26, 194, 25, 228, 24, 170, 24, 169, 23, 77, 23, 105, 23, 159, 22, 142, 23, 254, 23, 203, 24, 76, 26, 171, 26,
    49, 28, 208, 28, 123, 29, 121, 30, 209, 30, 10, 32, 41, 32, 216, 33, 132, 34, 246, 34, 4, 35, 140, 34, 69, 33, 66, 31, 41, 30, 136, 27, 178, 24, 222, 23, 102, 22, 206, 23, 146, 24, 49, 27, 187, 31, 115, 36, 54, 41,
    47, 46, 68, 51, 159, 54, 157, 56, 89, 57, 114, 57, 50, 55, 104, 50, 245, 45, 164, 40, 24, 35, 140, 29, 114, 24, 56, 20, 77, 16, 147, 13, 2, 11, 121, 8, 58, 7, 11, 6, 199, 4, 226, 3, 212, 2, 64, 2, 155, 1,
    54, 1, 243, 187, 142, 119, 84, 99, 71, 64, 53, 55, 55, 48, 43, 45, 46, 35, 45, 31, 32, 30, 35, 27, 19, 23, 30, 26, 29, 17, 24, 26, 13, 18, 13, 14, 15, 12, 11, 16, 20, 18, 14, 7, 12, 15, 13, 14, 12, 10,
    10, 7, 12, 6, 11, 11, 16, 11, 13, 14, 9, 9, 10, 21, 16, 21, 23, 26, 29, 27, 37, 52, 44, 71, 79, 81, 116, 107, 106, 142, 137, 138, 151, 160, 163, 133, 135, 125, 134, 106, 107, 82, 75, 61, 71, 45, 28, 30, 20, 17,
    20, 11, 10, 9, 6, 2, 6, 3, 5, 3, 3, 5, 4, 4, 1, 3, 2, 4, 1, 2, 2, 0, 2, 3, 2, 4, 1, 1, 3, 0, 1, 1, 1, 0, 1, 1, 3, 3, 0, 1, 1, 1, 3, 1, 1, 1, 1, 1, 2, 2,
    2, 1, 2, 2, 1, 2, 3, 0, 1, 1, 1, 4, 1, 1, 2, 1, 0, 1, 2, 3, 2, 1, 0, 1, 2, 2, 0, 1, 4, 1, 4, 2, 0, 1, 1, 0, 1, 1, 3, 0, 2, 3, 5, 0, 1, 3, 3, 0, 1, 2,
    0, 2, 1, 1, 0, 2, 2, 3, 1, 0, 2, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 3, 1, 2, 1, 1, 0, 1, 1, 0, 1, 2, 1, 1, 1, 3, 2, 0, 1, 3, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1,
    5, 2, 3, 0, 1, 1, 2, 1, 1, 0, 4, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 3, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 1, 0, 2, 1, 0, 1, 1, 2, 2, 1, 0, 1, 1,
    2, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 1, 0, 4, 1, 0, 4, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 5, 1, 0, 2, 1, 1, 1, 0, 1, 1, 0, 1, 1, 2,
    0, 2, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 6, 1, 1, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 2, 0, 2, 2, 1, 1, 1, 2, 0, 1, 1, 0, 1, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0,
    2, 1, 0, 4, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 2, 0, 2, 1, 0, 1, 1, 0, 5, 1, 0, 2, 1, 0, 1, 1, 0, 14, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 4, 1, 0, 4,
    1, 0, 2, 1, 0, 2, 1, 0, 17, 1, 0, 6, 1, 0, 1, 1, 1, 0, 5, 1, 0, 2, 1, 0, 4, 1, 1, 0, 11, 1, 1, 0, 4, 1, 0, 9, 1, 0, 5, 1, 0, 1, 1, 0, 27, 1, 0, 1, 1, 0,
    5, 1, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0, 15, 1, 0, 14, 1, 0, 2, 1, 0, 7, 1, 0, 5, 1, 0, 6, 1, 0, 14, 1, 0, 47, 1, 1, 0, 6, 1, 0, 8,
    1, 0, 9, 1, 0, 9, 1, 1, 0, 69, 1, 0, 25, 1, 0, 10, 1, 0, 1, 1, 0, 14, 1, 0, 23
  };
  assert( test_15_packed.size() == 1025 );
  const vector<uint8_t> test_15_encoded = QRSpecDev::encode_stream_vbyte( test_15_chan_cnts );
  assert( test_15_encoded == test_15_packed );
  vector<uint32_t> test_15_dec;
  const size_t test_15_nbytedec = QRSpecDev::decode_stream_vbyte(test_15_encoded,test_15_dec);
  assert( test_15_nbytedec == test_15_packed.size() );
  assert( test_15_dec == test_15_chan_cnts );
  
  
  
  
  // Test case 16
  const vector<uint32_t> test_16_chan_cnts{
    0, 39, 1, 84, 178, 201, 187, 182, 195, 172, 224, 165, 149, 157, 155, 183, 181, 165, 139, 168,
    154, 149, 162, 141, 132, 168, 139, 130, 139, 155, 148, 136, 132, 141, 131, 145, 133, 138, 146, 146,
    149, 131, 145, 162, 133, 128, 153, 134, 107, 103, 119, 116, 132, 110, 95, 106, 86, 107, 111, 91,
    114, 98, 90, 104, 94, 102, 88, 105, 102, 107, 91, 101, 89, 103, 87, 94, 105, 97, 95, 76,
    104, 96, 104, 86, 122, 102, 98, 103, 96, 101, 115, 98, 111, 97, 97, 93, 113, 119, 97, 105,
    114, 122, 115, 125, 116, 117, 103, 111, 117, 98, 94, 127, 116, 128, 112, 109, 130, 121, 124, 94,
    128, 128, 119, 107, 121, 119, 138, 132, 131, 124, 126, 131, 112, 133, 126, 115, 130, 128, 118, 126,
    123, 119, 126, 131, 146, 134, 120, 117, 125, 114, 132, 134, 120, 157, 141, 134, 124, 117, 132, 143,
    116, 140, 128, 139, 152, 135, 138, 145, 140, 131, 155, 152, 143, 149, 140, 147, 149, 145, 130, 136,
    122, 132, 140, 134, 124, 144, 135, 108, 141, 124, 140, 127, 151, 142, 140, 150, 150, 127, 142, 131,
    136, 129, 134, 141, 115, 138, 131, 123, 136, 130, 146, 140, 141, 108, 131, 121, 118, 132, 118, 124,
    141, 124, 131, 109, 142, 142, 127, 122, 110, 126, 131, 132, 121, 124, 137, 128, 119, 134, 147, 116,
    127, 124, 114, 137, 125, 115, 114, 111, 109, 125, 128, 123, 105, 124, 120, 116, 113, 114, 106, 131,
    119, 104, 129, 90, 112, 112, 108, 103, 108, 133, 119, 107, 128, 122, 116, 94, 118, 112, 98, 119,
    109, 121, 134, 100, 107, 115, 115, 113, 121, 91, 103, 92, 88, 116, 104, 89, 78, 88, 85, 102,
    105, 99, 114, 92, 92, 84, 101, 103, 86, 105, 91, 91, 93, 91, 104, 95, 87, 94, 97, 96,
    91, 89, 106, 86, 82, 84, 114, 90, 88, 87, 90, 87, 94, 84, 85, 77, 76, 77, 83, 82,
    77, 78, 86, 87, 93, 77, 91, 89, 76, 63, 71, 71, 85, 73, 99, 74, 80, 74, 81, 72,
    76, 83, 75, 87, 75, 94, 76, 67, 77, 75, 83, 67, 79, 77, 72, 64, 62, 68, 89, 62,
    77, 69, 63, 74, 68, 76, 70, 68, 86, 80, 64, 69, 66, 79, 82, 71, 70, 65, 74, 66,
    76, 71, 63, 68, 62, 52, 60, 74, 55, 55, 71, 62, 51, 49, 70, 61, 56, 55, 70, 66,
    66, 66, 64, 54, 69, 63, 69, 69, 43, 58, 59, 63, 63, 54, 57, 47, 47, 56, 71, 42,
    58, 74, 58, 57, 72, 57, 71, 59, 57, 67, 87, 84, 87, 76, 71, 52, 62, 54, 53, 48,
    61, 54, 48, 49, 51, 53, 56, 53, 39, 30, 43, 42, 38, 47, 45, 45, 41, 43, 53, 38,
    52, 40, 28, 60, 44, 56, 51, 55, 40, 39, 53, 60, 35, 52, 50, 42, 44, 39, 46, 40,
    38, 45, 42, 40, 44, 47, 39, 35, 41, 33, 40, 32, 41, 44, 40, 45, 52, 44, 38, 45,
    43, 28, 35, 27, 36, 49, 40, 30, 36, 36, 34, 53, 45, 49, 47, 46, 45, 31, 30, 41,
    33, 39, 31, 38, 37, 34, 31, 38, 46, 37, 37, 35, 37, 45, 37, 32, 45, 23, 33, 31,
    29, 28, 25, 33, 37, 30, 47, 46, 60, 42, 47, 25, 41, 38, 39, 32, 31, 31, 21, 45,
    29, 32, 37, 33, 35, 28, 29, 35, 28, 39, 28, 36, 28, 27, 21, 30, 31, 34, 37, 23,
    20, 21, 29, 33, 40, 36, 33, 24, 43, 27, 32, 31, 23, 28, 28, 21, 30, 25, 26, 19,
    33, 31, 17, 23, 22, 28, 31, 29, 35, 17, 22, 22, 23, 28, 28, 28, 25, 25, 26, 29,
    22, 25, 30, 22, 22, 25, 22, 28, 27, 25, 30, 28, 31, 29, 29, 38, 28, 32, 26, 33,
    29, 34, 31, 27, 37, 29, 32, 18, 19, 29, 22, 31, 19, 29, 29, 25, 28, 22, 25, 24,
    27, 40, 42, 60, 63, 47, 36, 16, 25, 22, 21, 26, 23, 26, 24, 20, 17, 26, 20, 19,
    26, 18, 20, 16, 30, 22, 23, 27, 21, 26, 24, 28, 22, 23, 15, 27, 21, 17, 23, 18,
    21, 15, 22, 13, 26, 22, 19, 15, 22, 25, 19, 27, 23, 16, 27, 19, 15, 25, 22, 19,
    19, 23, 18, 20, 18, 30, 19, 15, 17, 17, 17, 20, 20, 19, 22, 20, 20, 22, 18, 26,
    17, 22, 16, 20, 26, 16, 19, 18, 22, 16, 23, 19, 24, 15, 14, 21, 20, 18, 16, 14,
    19, 16, 13, 18, 12, 17, 16, 17, 11, 11, 19, 14, 19, 21, 20, 12, 14, 18, 24, 18,
    18, 21, 16, 20, 15, 15, 19, 11, 17, 20, 18, 10, 19, 17, 22, 15, 12, 16, 12, 13,
    13, 16, 12, 15, 23, 15, 13, 19, 12, 15, 28, 16, 15, 20, 15, 16, 15, 19, 19, 16,
    17, 7, 13, 15, 18, 13, 16, 12, 21, 21, 12, 11, 19, 11, 16, 19, 13, 19, 17, 19,
    13, 18, 19, 10, 17, 16, 11, 11, 17, 20, 14, 18, 10, 14, 17, 18, 5, 10, 14, 21,
    13, 16, 10, 11, 12, 15, 15, 24, 16, 12, 11, 16, 12, 16, 14, 18, 10, 13, 20, 13,
    9, 14, 7, 14, 14, 6, 12, 15, 16, 20, 19, 22, 21, 12, 10, 15, 13, 16, 14, 14,
    8, 14, 11, 10, 9, 8, 9, 13, 11, 18, 13, 11, 13, 16, 8, 14, 11, 13, 8, 20,
    15, 18, 14, 12, 6, 8, 12, 11, 19, 16, 8, 11, 20, 8, 13, 18, 12, 16, 16, 13,
    13, 17, 12, 17, 13, 16, 15, 12, 11, 11, 9, 10, 11, 14, 14, 9, 11, 8, 9, 13,
    14, 10, 9, 14, 8, 17, 12, 10, 7, 19, 12, 12, 12, 14, 17, 8, 12, 11, 9, 12,
    14, 15, 13, 24, 20, 24, 16, 21, 25, 19, 21, 11, 9, 17, 19, 14, 13, 12, 7, 11,
    11, 8, 8, 12, 9, 8, 12, 13, 7, 9, 9, 8, 14, 20, 7, 11, 10, 9, 13, 12,
    11, 10, 11, 13, 9, 13, 13, 12, 9, 10, 10, 16, 9, 9, 11, 5, 17, 7, 10, 8,
    9, 7, 9, 11, 12, 16, 8, 14, 4, 15, 15, 13, 11, 11, 9, 9, 13, 11, 7, 12,
    9, 7, 11, 8, 12, 12, 12, 14, 7, 11, 7, 17, 7, 10, 14, 11, 14, 9, 12, 10,
    1, 9, 8, 12, 6, 8, 9, 13, 10, 4, 15, 15, 9, 7, 9, 6, 9, 15, 10, 9,
    11, 6, 12, 11, 8, 10, 7, 13, 10, 9, 4, 8, 9, 10, 12, 8, 14, 12, 16, 13,
    11, 10, 10, 9, 10, 7, 8, 8, 7, 12, 10, 15, 17, 23, 30, 38, 23, 29, 15, 8,
    6, 9, 11, 13, 8, 6, 11, 18, 11, 5, 10, 11, 14, 7, 12, 9, 13, 15, 6, 6,
    9, 15, 8, 6, 8, 4, 13, 11, 8, 8, 10, 3, 11, 8, 9, 9, 10, 9, 8, 5,
    8, 6, 7, 5, 14, 14, 13, 40, 38, 36, 34, 24, 9, 7, 7, 10, 8, 8, 5, 6,
    10, 8, 4, 9, 10, 4, 5, 9, 4, 9, 4, 9, 6, 8, 7, 5, 7, 2, 5, 8,
    8, 9, 7, 14, 6, 11, 8, 7, 9, 13, 9, 11, 12, 10, 5, 6, 10, 5, 6, 15,
    10, 8, 7, 10, 4, 9, 11, 6, 3, 5, 7, 9, 11, 5, 7, 7, 5, 5, 4, 6,
    4, 7, 10, 12, 13, 10, 9, 19, 9, 7, 9, 6, 6, 9, 8, 6, 5, 6, 6, 9,
    7, 6, 4, 7, 10, 5, 8, 11, 13, 6, 10, 6, 7, 6, 4, 10, 4, 9, 5, 7,
    6, 9, 8, 8, 7, 8, 6, 8, 8, 10, 8, 6, 9, 5, 10, 3, 4, 11, 12, 7,
    5, 9, 7, 6, 6, 10, 8, 7, 3, 9, 7, 5, 10, 7, 11, 7, 9, 8, 8, 14,
    8, 7, 8, 3, 6, 7, 7, 13, 10, 9, 4, 4, 8, 4, 11, 9, 7, 6, 11, 4,
    9, 8, 10, 9, 9, 9, 6, 10, 9, 9, 4, 12, 13, 8, 9, 6, 2, 11, 8, 10,
    9, 9, 7, 9, 9, 8, 3, 4, 9, 8, 9, 8, 7, 7, 7, 4, 6, 6, 8, 8,
    10, 8, 9, 5, 7, 6, 3, 12, 10, 7, 4, 6, 8, 7, 6, 5, 7, 7, 7, 5,
    7, 5, 6, 4, 7, 4, 9, 13, 10, 7, 7, 13, 13, 7, 6, 4, 5, 5, 6, 5,
    6, 7, 5, 8, 3, 5, 7, 7, 9, 5, 5, 12, 7, 7, 10, 4, 5, 9, 12, 7,
    8, 5, 7, 4, 5, 8, 5, 4, 5, 5, 3, 6, 11, 5, 3, 10, 4, 8, 8, 3,
    5, 6, 3, 8, 3, 6, 7, 9, 5, 8, 5, 7, 4, 2, 8, 9, 12, 11, 8, 4,
    5, 6, 5, 7, 8, 7, 4, 6, 10, 7, 9, 7, 8, 8, 11, 13, 13, 5, 5, 1,
    8, 4, 4, 6, 7, 7, 6, 7, 5, 4, 6, 7, 5, 3, 1, 4, 5, 6, 1, 7,
    5, 8, 2, 7, 6, 3, 5, 4, 9, 9, 7, 6, 7, 7, 3, 12, 4, 7, 9, 4,
    5, 7, 4, 8, 4, 9, 9, 9, 12, 7, 5, 6, 8, 7, 4, 8, 9, 1, 2, 6,
    4, 5, 7, 6, 7, 7, 3, 7, 5, 9, 5, 8, 3, 7, 4, 6, 7, 5, 4, 7,
    9, 4, 8, 6, 9, 8, 6, 11, 2, 3, 3, 5, 5, 3, 4, 6, 5, 3, 7, 5,
    10, 6, 4, 5, 1, 9, 5, 10, 9, 5, 7, 6, 2, 4, 4, 3, 5, 7, 8, 7,
    5, 3, 3, 5, 3, 3, 5, 7, 5, 4, 3, 6, 6, 3, 7, 7, 6, 8, 6, 5,
    4, 3, 2, 5, 5, 5, 10, 6, 5, 5, 2, 5, 2, 2, 7, 5, 8, 6, 3, 5,
    4, 2, 7, 6, 7, 7, 7, 4, 3, 7, 0, 1, 6, 7, 2, 3, 7, 5, 4, 5,
    6, 8, 6, 10, 12, 9, 5, 5, 3, 8, 4, 6, 9, 7, 8, 3, 7, 6, 6, 9,
    3, 2, 5, 4, 8, 5, 6, 2, 6, 5, 5, 5, 3, 6, 5, 5, 2, 4, 4, 7,
    8, 6, 7, 5, 1, 6, 2, 4, 8, 9, 6, 3, 3, 4, 6, 2, 7, 2, 5, 5,
    5, 4, 4, 4, 2, 7, 4, 5, 4, 5, 4, 7, 3, 5, 4, 7, 4, 5, 2, 6,
    6, 4, 8, 4, 3, 9, 2, 2, 2, 1, 3, 6, 8, 5, 2, 4, 3, 6, 8, 4,
    4, 2, 8, 10, 13, 22, 26, 28, 24, 13, 6, 4, 3, 4, 7, 4, 3, 6, 1, 3,
    1, 2, 3, 7, 3, 3, 7, 7, 3, 10, 6, 5, 4, 8, 3, 5, 4, 4, 9, 4,
    4, 4, 5, 2, 2, 6, 2, 4, 2, 3, 3, 4, 6, 11, 9, 4, 6, 2, 2, 7,
    4, 6, 8, 7, 4, 7, 5, 8, 3, 2, 6, 5, 6, 3, 2, 5, 4, 6, 5, 2,
    5, 2, 5, 3, 5, 12, 6, 11, 8, 5, 4, 5, 5, 4, 2, 7, 5, 2, 7, 2,
    5, 9, 2, 6, 4, 3, 4, 6, 7, 1, 2, 2, 8, 3, 8, 6, 11, 7, 6, 6,
    6, 8, 12, 21, 16, 23, 13, 10, 9, 4, 3, 4, 3, 6, 3, 5, 5, 7, 3, 3,
    3, 5, 3, 3, 5, 0, 1, 5, 4, 8, 4, 2, 6, 5, 7, 4, 5, 5, 2, 7,
    4, 5, 3, 4, 4, 3, 3, 3, 3, 7, 5, 4, 5, 4, 5, 4, 3, 4, 5, 5,
    5, 2, 3, 8, 5, 2, 6, 3, 6, 6, 4, 5, 6, 3, 2, 7, 3, 5, 1, 5,
    4, 1, 5, 5, 7, 3, 4, 1, 6, 4, 4, 3, 0, 1, 3, 3, 1, 4, 4, 5,
    3, 6, 4, 4, 8, 4, 4, 1, 3, 1, 5, 9, 3, 5, 3, 5, 5, 1, 5, 3,
    4, 3, 5, 4, 5, 3, 0, 1, 6, 4, 5, 8, 6, 4, 3, 3, 6, 5, 3, 3,
    2, 4, 3, 3, 5, 4, 6, 4, 4, 3, 6, 5, 3, 3, 6, 1, 10, 3, 3, 5,
    4, 3, 7, 5, 4, 9, 5, 5, 3, 5, 2, 6, 4, 5, 4, 3, 7, 1, 6, 8,
    3, 9, 7, 3, 8, 8, 6, 2, 4, 3, 5, 5, 4, 5, 6, 5, 1, 2, 6, 4,
    3, 2, 5, 6, 4, 6, 6, 3, 4, 6, 9, 3, 4, 8, 6, 2, 3, 5, 7, 6,
    1, 5, 8, 1, 3, 5, 5, 3, 3, 5, 6, 3, 10, 6, 0, 1, 6, 5, 3, 1,
    5, 4, 4, 8, 4, 4, 2, 9, 5, 3, 3, 5, 3, 7, 5, 2, 2, 5, 6, 3,
    4, 8, 6, 5, 3, 5, 5, 5, 1, 6, 7, 3, 4, 8, 1, 6, 4, 2, 5, 3,
    2, 1, 4, 3, 4, 3, 5, 5, 5, 3, 2, 4, 5, 2, 3, 3, 5, 4, 3, 3,
    2, 3, 2, 5, 6, 6, 5, 6, 4, 5, 7, 4, 5, 6, 3, 12, 8, 10, 13, 15,
    7, 4, 11, 5, 8, 5, 5, 4, 2, 2, 3, 2, 3, 3, 6, 3, 6, 8, 5, 6,
    4, 2, 6, 7, 3, 5, 6, 5, 1, 2, 6, 2, 5, 7, 4, 4, 5, 4, 1, 7,
    3, 5, 3, 0, 1, 2, 1, 5, 3, 4, 1, 4, 7, 1, 6, 3, 5, 1, 3, 7,
    4, 3, 4, 8, 5, 6, 4, 4, 2, 3, 7, 5, 0, 1, 7, 5, 11, 3, 5, 3,
    3, 8, 4, 5, 3, 6, 7, 7, 6, 5, 4, 7, 7, 9, 4, 1, 5, 7, 5, 5,
    3, 5, 5, 3, 3, 4, 5, 3, 4, 7, 6, 2, 3, 5, 2, 5, 5, 4, 5, 6,
    3, 2, 2, 4, 4, 4, 5, 4, 4, 2, 3, 2, 1, 3, 3, 4, 7, 5, 1, 2,
    7, 5, 2, 6, 3, 3, 3, 4, 7, 2, 7, 4, 4, 5, 5, 9, 1, 5, 3, 1,
    7, 3, 4, 4, 2, 2, 6, 5, 5, 6, 2, 3, 3, 7, 1, 4, 5, 5, 6, 2,
    6, 3, 5, 4, 5, 4, 4, 1, 4, 4, 5, 3, 6, 7, 8, 6, 2, 5, 7, 5,
    3, 4, 4, 4, 6, 4, 4, 11, 4, 4, 3, 2, 3, 6, 6, 3, 5, 5, 3, 5,
    5, 5, 1, 7, 6, 3, 6, 7, 7, 3, 6, 1, 3, 2, 5, 10, 6, 6, 10, 6,
    9, 3, 3, 6, 6, 4, 9, 2, 9, 3, 3, 3, 6, 3, 7, 4, 3, 4, 4, 3,
    2, 3, 1, 4, 2, 3, 6, 5, 5, 1, 2, 4, 3, 5, 2, 7, 1, 1, 2, 1,
    2, 0, 1, 7, 4, 1, 5, 4, 4, 0, 1, 2, 2, 4, 0, 1, 2, 5, 3, 3,
    7, 4, 5, 1, 1, 1, 3, 5, 1, 5, 4, 1, 7, 6, 0, 1, 4, 1, 1, 3,
    5, 1, 2, 4, 1, 4, 0, 1, 1, 4, 6, 7, 5, 1, 3, 3, 7, 1, 2, 3,
    9, 2, 0, 1, 2, 2, 1, 4, 2, 4, 2, 2, 5, 5, 4, 4, 4, 4, 1, 1,
    3, 1, 4, 2, 2, 3, 2, 4, 0, 1, 1, 4, 2, 1, 3, 3, 3, 3, 3, 5,
    1, 1, 2, 0, 1, 4, 3, 5, 2, 1, 0, 1, 4, 2, 7, 0, 1, 1, 1, 7,
    5, 0, 2, 2, 3, 4, 2, 2, 1, 2, 3, 2, 3, 2, 2, 2, 5, 1, 2, 3,
    2, 1, 2, 4, 4, 2, 1, 1, 2, 3, 1, 0, 1, 1, 1, 1, 2, 2, 2, 4,
    3, 5, 3, 3, 3, 2, 0, 1, 2, 4, 2, 3, 1, 6, 0, 1, 2, 2, 3, 1,
    1, 1, 1, 4, 2, 3, 1, 0, 1, 1, 2, 3, 1, 6, 2, 2, 0, 1, 2, 1,
    2, 4, 2, 3, 1, 2, 4, 1, 1, 5, 3, 0, 1, 1, 2, 2, 4, 4, 1, 0,
    2, 2, 2, 1, 3, 3, 1, 1, 2, 1, 3, 3, 4, 0, 1, 2, 1, 1, 2, 3,
    1, 3, 2, 2, 2, 3, 1, 0, 1, 1, 3, 1, 0, 1, 2, 2, 1, 3, 0, 2,
    2, 3, 2, 5, 7, 3, 4, 3, 1, 3, 2, 2, 1, 1, 1, 0, 1, 1, 3, 1,
    4, 3, 2, 4, 0, 1, 3, 1, 1, 1, 3, 1, 1, 0, 1, 1, 1, 2, 2, 1,
    1, 1, 3, 2, 3, 2, 2, 2, 3, 0, 1, 1, 0, 1, 2, 0, 1, 3, 0, 1,
    3, 2, 2, 3, 3, 3, 6, 5, 2, 3, 2, 2, 2, 3, 6, 2, 2, 1, 1, 4,
    2, 1, 1, 0, 1, 2, 1, 0, 1, 2, 2, 2, 0, 1, 1, 0, 1, 2, 0, 1,
    1, 4, 2, 1, 1, 2, 1, 3, 4, 2, 0, 2, 1, 1, 2, 1, 2, 0, 2, 1,
    2, 1, 0, 1, 1, 1, 1, 2, 0, 1, 4, 0, 1, 2, 1, 1, 1, 4, 1, 1,
    4, 2, 1, 0, 1, 1, 2, 2, 5, 2, 1, 2, 3, 0, 3, 2, 0, 1, 3, 0,
    1, 2, 0, 1, 1, 3, 2, 1, 2, 1, 0, 1, 2, 1, 1, 2, 3, 2, 3, 3,
    1, 2, 0, 1, 1, 2, 3, 3, 5, 7, 31, 41, 66, 117, 104, 143, 107, 75, 42, 22,
    3, 2, 0, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 1, 0, 1, 1, 2,
    0, 1, 1, 0, 1, 1, 2, 1, 0, 3, 1, 0, 1, 1, 1, 1, 1, 1, 1, 2,
    0, 1, 2, 1, 3, 0, 2, 2, 1, 0, 2, 3, 3, 0, 1, 2, 2, 1, 1, 1,
    0, 1, 1, 0, 1, 4, 1, 1, 1, 1, 0, 1, 2, 1, 0, 2, 3, 2, 0, 1,
    2, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 1, 1, 0,
    1, 1, 1, 1, 1, 1, 2, 1, 4, 0, 4, 2, 0, 2, 2, 1, 1, 0, 2, 1,
    0, 1, 1, 1, 0, 1, 2, 1, 1, 0, 4, 1, 1, 0, 2, 2, 0, 1, 1, 0,
    2, 1, 1, 1, 0, 2, 1, 0, 1, 2, 0, 1, 2, 1, 0, 1, 2, 0, 3, 1,
    0, 1, 1, 1, 0, 1, 2, 1, 3, 3, 1, 1, 2, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 1, 1, 0, 2, 1, 1, 0, 2, 3, 1, 0, 4, 1, 1, 1, 0, 4, 1,
    0, 3, 1, 1, 0, 2, 1, 0, 1, 2, 3, 1, 2, 2, 0, 2, 2, 1, 3, 1,
    0, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 1, 1, 0, 2, 2, 0, 3, 1, 1,
    1, 2, 1, 0, 1, 1, 1, 0, 2, 1, 0, 4, 1, 1, 0, 4, 1, 4, 0, 4,
    1, 1, 0, 1, 2, 1, 2, 3, 1, 2, 2, 1, 4, 2, 3, 1, 2, 3, 3, 3,
    1, 4, 0, 2, 1, 0, 2, 1, 1, 1, 0, 3, 2, 5, 1, 0, 4, 1, 0, 4,
    1, 3, 2, 0, 3, 1, 1, 3, 2, 0, 1, 1, 0, 2, 2, 0, 5, 1, 1, 1,
    0, 2, 1, 0, 2, 1, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 2, 0, 1,
    1, 0, 1, 3, 0, 1, 1, 2, 2, 0, 2, 1, 0, 1, 1, 1, 3, 1, 2, 1,
    0, 2, 1, 0, 2, 1, 1, 0, 3, 1, 2, 2, 0, 2, 3, 1, 0, 5, 2, 0,
    5, 3, 1, 0, 2, 1, 0, 1, 1, 2, 0, 1, 1, 0, 3, 1, 1, 1, 0, 3,
    2, 1, 1, 1, 1, 0, 5, 2, 1, 1, 0, 1, 1, 1, 1, 1, 0, 4, 2, 0,
    3, 2, 0, 5, 1, 1, 1, 0, 1, 1, 0, 4, 1, 0, 1, 1, 0, 3, 1, 0,
    1, 1, 0, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 4, 1, 0, 3,
    1, 0, 4, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 1, 0, 2, 3,
    0, 1, 1, 0, 1, 1, 0, 3, 2, 0, 6, 2, 1, 1, 1, 0, 2, 1, 0, 4,
    1, 0, 1, 1, 1, 1, 0, 2, 2, 0, 3, 2, 0, 1, 1, 0, 2, 2, 2, 0,
    10, 1, 0, 4, 1, 1, 3, 2, 1, 3, 1, 1, 1, 0, 1, 1, 0, 6, 3, 1,
    0, 4, 1, 0, 2, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 0, 4, 1, 1, 0,
    3, 3, 0, 4, 3, 0, 3, 1, 0, 5, 2, 1, 0, 3, 2, 0, 2, 3, 2, 4,
    7, 8, 7, 9, 12, 8, 4, 1, 2, 0, 1, 1, 0, 3, 1, 0, 6, 2, 0, 9,
    1, 0, 3, 1, 0, 1, 2, 0, 1, 1, 1, 1, 1, 0, 1, 1, 0, 2, 1, 0,
    3, 1, 1, 0, 6, 1, 0, 1, 2, 1, 0, 4, 2, 0, 2, 1, 0, 4, 1, 1,
    1, 0, 7, 1, 0, 2, 1, 0, 1, 1, 0, 4, 1, 0, 2, 2, 2, 0, 6, 1,
    0, 4, 1, 0, 1, 1, 0, 1, 1, 0, 8, 1, 0, 3, 1, 1, 1, 1, 0, 2,
    1, 0, 8, 1, 0, 19, 1, 0, 4, 1, 0, 1, 2, 1, 2, 3, 0, 2, 2, 1,
    1, 0, 3, 1, 1, 0, 8, 1, 0, 3, 1, 0, 10, 1, 0, 2, 1, 2, 0, 4,
    1, 0, 3, 1, 0, 2, 1, 1, 1, 0, 1, 1, 0, 8, 1, 0, 3, 3, 0, 3,
    1, 1, 0, 5, 1, 1, 1, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1,
    1, 0, 4, 3, 1, 0, 4, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1,
    1, 0, 1, 1, 1, 0, 2, 1, 0, 7, 1, 0, 5, 1, 0, 1, 2, 0, 2, 1,
    0, 13, 1, 1, 0, 2, 2, 1, 0, 3, 1, 0, 1, 1, 2, 0, 5, 1, 1, 2,
    2, 0, 1, 3, 0, 3, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0, 6, 1, 0, 5,
    2, 0, 2, 1, 0, 3, 2, 0, 3, 1, 1, 1, 0, 1, 2, 0, 4, 2, 0, 2,
    1, 2, 1, 1, 0, 4, 1, 0, 6, 2, 0, 4, 1, 0, 1, 1, 0, 4, 1, 0,
    1, 1, 1, 1, 0, 5, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 7, 1,
    0, 8, 1, 0, 1, 1, 1, 1, 0, 7, 1, 0, 1, 2, 0, 1, 1, 2, 0, 1,
    1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 0, 16, 1, 1, 0, 3, 1, 1, 0, 1,
    1, 0, 3, 1, 1, 0, 4, 1, 0, 1, 1, 0, 4, 1, 0, 4, 2, 0, 1, 1,
    0, 2, 1, 0, 9, 1, 0, 2, 1, 0, 2, 1, 1, 1, 1, 0, 4, 1, 1, 0,
    2, 1, 0, 1, 1, 0, 5, 1, 0, 3, 1, 0, 5, 1, 3, 0, 7, 1, 1, 0,
    5, 1, 0, 1, 1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 1,
    1, 0, 1, 1, 0, 3, 1, 0, 6, 2, 2, 0, 1, 2, 0, 4, 1, 0, 2, 1,
    1, 0, 1, 1, 0, 1, 1, 2, 1, 0, 2, 1, 0, 1, 1, 0, 9, 2, 0, 2,
    1, 1, 0, 2, 1, 0, 1, 1, 0, 5, 1, 1, 3, 0, 1, 1, 0, 1, 3, 3,
    2, 0, 1, 3, 1, 3, 0, 1, 1, 0, 1, 1, 0, 1, 2, 0, 11, 2, 0, 1,
    1, 0, 2, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0, 1, 2, 0, 3,
    1, 0, 1, 1, 0, 3, 1, 0, 6, 2, 0, 2, 1, 0, 2, 1, 1, 0, 5, 1,
    0, 2, 1, 0, 3, 1, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 6, 1, 1, 0,
    1, 1, 0, 5, 1, 1, 2, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 1, 0, 1,
    1, 0, 2, 2, 0, 3, 2, 1, 0, 2, 1, 0, 2, 1, 0, 9, 1, 0, 5, 1,
    0, 2, 1, 0, 1, 2, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 4, 1, 0,
    1, 2, 2, 0, 2, 2, 0, 5, 1, 0, 8, 1, 1, 1, 0, 1, 1, 0, 1, 1,
    0, 1, 1, 0, 2, 1, 1, 1, 0, 4, 1, 3, 1, 0, 1, 1, 2, 1, 2, 1,
    2, 1, 1, 1, 0, 1, 1, 1, 1, 0, 10, 1, 0, 4, 1, 0, 7, 1, 0, 2,
    1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 1, 0, 1, 2,
    1, 1, 1, 1, 0, 3, 1, 2, 0, 2, 1, 1, 1, 0, 7, 1, 1, 0, 5, 1,
    0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 5, 1, 0, 4, 2, 1, 0, 4, 1, 0,
    4, 1, 1, 0, 4, 1, 0, 1, 1, 0, 1, 2, 0, 4, 1, 0, 1, 1, 0, 8,
    1, 0, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2,
    2, 0, 3, 1, 0, 3, 1, 0, 2, 2, 0, 2, 1, 1, 1, 0, 3, 1, 0, 2,
    1, 1, 1, 0, 3, 2, 2, 2, 0, 2, 2, 0, 9, 1, 0, 1, 1, 0, 5, 1,
    0, 5, 2, 0, 9, 1, 1, 0, 3, 1, 0, 5, 2, 1, 0, 1, 2, 1, 0, 2,
    1, 0, 2, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 6, 1, 2, 0, 2, 2, 0,
    3, 1, 0, 4, 2, 2, 1, 0, 2, 1, 0, 3, 1, 1, 0, 9, 1, 1, 1, 1,
    1, 2, 0, 2, 1, 0, 6, 1, 0, 2, 1, 0, 4, 2, 0, 4, 1, 1, 1, 0,
    1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 0, 2, 1, 1, 0,
    2, 1, 0, 2, 1, 0, 5, 1, 0, 8, 1, 0, 5, 2, 0, 1, 1, 0, 1, 1,
    1, 2, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 8, 2, 0, 1, 1, 0, 3, 1,
    0, 12, 2, 0, 4, 2, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 3, 1, 1, 0,
    1, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 2, 2, 1, 0, 8, 1, 0, 1,
    1, 0, 3, 1, 0, 14, 1, 0, 4, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 1,
    2, 1, 2, 0, 2, 1, 0, 5, 1, 0, 2, 1, 0, 8, 1, 0, 22, 1, 0, 2,
    1, 0, 2, 1, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 4, 1, 0, 9, 1,
    0, 1, 1, 0, 4, 1, 0, 1, 1, 0, 8, 1, 2, 0, 6, 1, 0, 8, 1, 0,
    2, 1, 1, 0, 23, 1, 0, 2, 1, 0, 2, 1, 1, 0, 19, 1, 0, 4, 1, 1,
    0, 6, 1, 0, 15, 1, 0, 5, 2, 0, 2, 1, 0, 38, 1, 0, 14, 1, 0, 19,
    1, 1, 0, 7, 1, 0, 8, 1, 0, 16, 1, 0, 2, 1, 0, 8, 1, 0, 5, 3,
    2, 2, 2, 3, 7, 13, 15, 19, 17, 9, 10, 7, 5, 0, 39, 1, 0, 5, 1, 1,
    0, 14, 1, 0, 78, 1, 0, 28, 1, 0, 130, 1, 0, 10, 1, 0, 125, 1, 0, 30,
    1, 0, 1, 1, 0, 45, 1, 0, 7, 1, 0, 1, 1, 0, 5, 1, 0, 36, 1, 0,
    17, 1, 0, 63, 1, 0, 24, 1, 0, 49, 1, 0, 21, 1, 0, 2, 1, 0, 58, 1,
    0, 20, 1, 0, 6, 1, 0, 1, 1, 0, 5, 1, 0, 3, 1, 0, 28, 1, 0, 43,
    1, 0, 61, 1, 0, 13, 1, 0, 103, 1, 0, 83, 1, 0, 76, 1, 0, 49, 1, 0,
    15, 1, 0, 52, 1, 0, 129, 1, 0, 2, 1, 0, 77, 1, 0, 22, 1, 0, 378, 1,
    0, 1, 1, 0, 15, 1, 0, 43, 1, 0, 57, 1, 0, 30, 1, 0, 22, 1, 0, 30,
    1, 0, 17, 1, 0, 159, 1, 0, 154, 1, 0, 156, 1, 0, 114, 1, 1, 0, 102, 1,
    0, 269, 1, 0, 78, 1, 0, 66, 2, 0, 230, 1, 0, 35, 1, 0, 77, 1, 0, 4,
    1, 0, 7, 1, 0, 49, 1, 0, 7, 1, 0, 94, 1, 0, 20, 1, 0, 238, 1, 0,
    283, 1, 0, 283, 1, 0, 176, 1, 0, 149, 1, 0, 81, 1, 0, 133, 1, 0, 12, 1,
    0, 56, 1, 0, 194, 1, 0, 61, 1, 0, 313, 1, 0, 151, 1, 0, 108, 1, 0, 197,
    1, 0, 123, 1, 0, 15, 1, 0, 129, 1, 0, 71, 1, 0, 34, 1, 0, 6, 1, 1,
    0, 94, 1, 0, 29, 1, 0, 291, 1, 0, 30, 1, 0, 169, 1, 0, 77, 1, 0, 257,
    1, 0, 41, 1, 0, 10, 1, 0, 17, 1, 0, 134, 1, 0, 111, 1, 0, 34, 1, 0,
    61, 1, 0, 2, 1, 0, 87, 1, 0, 96, 1, 0, 18, 1, 0, 203, 1, 0, 119, 1,
    0, 86, 1, 0, 38, 1, 0, 408, 1, 0, 372, 1, 0, 21, 1, 0, 31, 1, 0, 75,
    1, 0, 38, 1, 0, 254, 1, 0, 434, 1, 0, 94, 1, 0, 102, 1, 0, 42, 1, 0,
    168, 1, 0, 61, 1, 0, 198  };
  assert( test_16_chan_cnts.size() == 5127 );
  const vector<uint8_t> test_16_packed{
    7, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 65, 0, 0, 0, 0, 0, 0, 16,
    0, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 16, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 39, 1, 84, 178, 201, 187, 182, 195, 172, 224, 165, 149, 157, 155, 183,
    181, 165, 139, 168, 154, 149, 162, 141, 132, 168, 139, 130, 139, 155, 148, 136, 132, 141, 131, 145, 133, 138, 146, 146, 149, 131, 145, 162, 133, 128, 153, 134, 107, 103, 119, 116, 132, 110, 95, 106, 86, 107, 111, 91, 114, 98, 90, 104, 94, 102,
    88, 105, 102, 107, 91, 101, 89, 103, 87, 94, 105, 97, 95, 76, 104, 96, 104, 86, 122, 102, 98, 103, 96, 101, 115, 98, 111, 97, 97, 93, 113, 119, 97, 105, 114, 122, 115, 125, 116, 117, 103, 111, 117, 98, 94, 127, 116, 128, 112, 109,
    130, 121, 124, 94, 128, 128, 119, 107, 121, 119, 138, 132, 131, 124, 126, 131, 112, 133, 126, 115, 130, 128, 118, 126, 123, 119, 126, 131, 146, 134, 120, 117, 125, 114, 132, 134, 120, 157, 141, 134, 124, 117, 132, 143, 116, 140, 128, 139, 152, 135,
    138, 145, 140, 131, 155, 152, 143, 149, 140, 147, 149, 145, 130, 136, 122, 132, 140, 134, 124, 144, 135, 108, 141, 124, 140, 127, 151, 142, 140, 150, 150, 127, 142, 131, 136, 129, 134, 141, 115, 138, 131, 123, 136, 130, 146, 140, 141, 108, 131, 121,
    118, 132, 118, 124, 141, 124, 131, 109, 142, 142, 127, 122, 110, 126, 131, 132, 121, 124, 137, 128, 119, 134, 147, 116, 127, 124, 114, 137, 125, 115, 114, 111, 109, 125, 128, 123, 105, 124, 120, 116, 113, 114, 106, 131, 119, 104, 129, 90, 112, 112,
    108, 103, 108, 133, 119, 107, 128, 122, 116, 94, 118, 112, 98, 119, 109, 121, 134, 100, 107, 115, 115, 113, 121, 91, 103, 92, 88, 116, 104, 89, 78, 88, 85, 102, 105, 99, 114, 92, 92, 84, 101, 103, 86, 105, 91, 91, 93, 91, 104, 95,
    87, 94, 97, 96, 91, 89, 106, 86, 82, 84, 114, 90, 88, 87, 90, 87, 94, 84, 85, 77, 76, 77, 83, 82, 77, 78, 86, 87, 93, 77, 91, 89, 76, 63, 71, 71, 85, 73, 99, 74, 80, 74, 81, 72, 76, 83, 75, 87, 75, 94,
    76, 67, 77, 75, 83, 67, 79, 77, 72, 64, 62, 68, 89, 62, 77, 69, 63, 74, 68, 76, 70, 68, 86, 80, 64, 69, 66, 79, 82, 71, 70, 65, 74, 66, 76, 71, 63, 68, 62, 52, 60, 74, 55, 55, 71, 62, 51, 49, 70, 61,
    56, 55, 70, 66, 66, 66, 64, 54, 69, 63, 69, 69, 43, 58, 59, 63, 63, 54, 57, 47, 47, 56, 71, 42, 58, 74, 58, 57, 72, 57, 71, 59, 57, 67, 87, 84, 87, 76, 71, 52, 62, 54, 53, 48, 61, 54, 48, 49, 51, 53,
    56, 53, 39, 30, 43, 42, 38, 47, 45, 45, 41, 43, 53, 38, 52, 40, 28, 60, 44, 56, 51, 55, 40, 39, 53, 60, 35, 52, 50, 42, 44, 39, 46, 40, 38, 45, 42, 40, 44, 47, 39, 35, 41, 33, 40, 32, 41, 44, 40, 45,
    52, 44, 38, 45, 43, 28, 35, 27, 36, 49, 40, 30, 36, 36, 34, 53, 45, 49, 47, 46, 45, 31, 30, 41, 33, 39, 31, 38, 37, 34, 31, 38, 46, 37, 37, 35, 37, 45, 37, 32, 45, 23, 33, 31, 29, 28, 25, 33, 37, 30,
    47, 46, 60, 42, 47, 25, 41, 38, 39, 32, 31, 31, 21, 45, 29, 32, 37, 33, 35, 28, 29, 35, 28, 39, 28, 36, 28, 27, 21, 30, 31, 34, 37, 23, 20, 21, 29, 33, 40, 36, 33, 24, 43, 27, 32, 31, 23, 28, 28, 21,
    30, 25, 26, 19, 33, 31, 17, 23, 22, 28, 31, 29, 35, 17, 22, 22, 23, 28, 28, 28, 25, 25, 26, 29, 22, 25, 30, 22, 22, 25, 22, 28, 27, 25, 30, 28, 31, 29, 29, 38, 28, 32, 26, 33, 29, 34, 31, 27, 37, 29,
    32, 18, 19, 29, 22, 31, 19, 29, 29, 25, 28, 22, 25, 24, 27, 40, 42, 60, 63, 47, 36, 16, 25, 22, 21, 26, 23, 26, 24, 20, 17, 26, 20, 19, 26, 18, 20, 16, 30, 22, 23, 27, 21, 26, 24, 28, 22, 23, 15, 27,
    21, 17, 23, 18, 21, 15, 22, 13, 26, 22, 19, 15, 22, 25, 19, 27, 23, 16, 27, 19, 15, 25, 22, 19, 19, 23, 18, 20, 18, 30, 19, 15, 17, 17, 17, 20, 20, 19, 22, 20, 20, 22, 18, 26, 17, 22, 16, 20, 26, 16,
    19, 18, 22, 16, 23, 19, 24, 15, 14, 21, 20, 18, 16, 14, 19, 16, 13, 18, 12, 17, 16, 17, 11, 11, 19, 14, 19, 21, 20, 12, 14, 18, 24, 18, 18, 21, 16, 20, 15, 15, 19, 11, 17, 20, 18, 10, 19, 17, 22, 15,
    12, 16, 12, 13, 13, 16, 12, 15, 23, 15, 13, 19, 12, 15, 28, 16, 15, 20, 15, 16, 15, 19, 19, 16, 17, 7, 13, 15, 18, 13, 16, 12, 21, 21, 12, 11, 19, 11, 16, 19, 13, 19, 17, 19, 13, 18, 19, 10, 17, 16,
    11, 11, 17, 20, 14, 18, 10, 14, 17, 18, 5, 10, 14, 21, 13, 16, 10, 11, 12, 15, 15, 24, 16, 12, 11, 16, 12, 16, 14, 18, 10, 13, 20, 13, 9, 14, 7, 14, 14, 6, 12, 15, 16, 20, 19, 22, 21, 12, 10, 15,
    13, 16, 14, 14, 8, 14, 11, 10, 9, 8, 9, 13, 11, 18, 13, 11, 13, 16, 8, 14, 11, 13, 8, 20, 15, 18, 14, 12, 6, 8, 12, 11, 19, 16, 8, 11, 20, 8, 13, 18, 12, 16, 16, 13, 13, 17, 12, 17, 13, 16,
    15, 12, 11, 11, 9, 10, 11, 14, 14, 9, 11, 8, 9, 13, 14, 10, 9, 14, 8, 17, 12, 10, 7, 19, 12, 12, 12, 14, 17, 8, 12, 11, 9, 12, 14, 15, 13, 24, 20, 24, 16, 21, 25, 19, 21, 11, 9, 17, 19, 14,
    13, 12, 7, 11, 11, 8, 8, 12, 9, 8, 12, 13, 7, 9, 9, 8, 14, 20, 7, 11, 10, 9, 13, 12, 11, 10, 11, 13, 9, 13, 13, 12, 9, 10, 10, 16, 9, 9, 11, 5, 17, 7, 10, 8, 9, 7, 9, 11, 12, 16,
    8, 14, 4, 15, 15, 13, 11, 11, 9, 9, 13, 11, 7, 12, 9, 7, 11, 8, 12, 12, 12, 14, 7, 11, 7, 17, 7, 10, 14, 11, 14, 9, 12, 10, 1, 9, 8, 12, 6, 8, 9, 13, 10, 4, 15, 15, 9, 7, 9, 6,
    9, 15, 10, 9, 11, 6, 12, 11, 8, 10, 7, 13, 10, 9, 4, 8, 9, 10, 12, 8, 14, 12, 16, 13, 11, 10, 10, 9, 10, 7, 8, 8, 7, 12, 10, 15, 17, 23, 30, 38, 23, 29, 15, 8, 6, 9, 11, 13, 8, 6,
    11, 18, 11, 5, 10, 11, 14, 7, 12, 9, 13, 15, 6, 6, 9, 15, 8, 6, 8, 4, 13, 11, 8, 8, 10, 3, 11, 8, 9, 9, 10, 9, 8, 5, 8, 6, 7, 5, 14, 14, 13, 40, 38, 36, 34, 24, 9, 7, 7, 10,
    8, 8, 5, 6, 10, 8, 4, 9, 10, 4, 5, 9, 4, 9, 4, 9, 6, 8, 7, 5, 7, 2, 5, 8, 8, 9, 7, 14, 6, 11, 8, 7, 9, 13, 9, 11, 12, 10, 5, 6, 10, 5, 6, 15, 10, 8, 7, 10, 4, 9,
    11, 6, 3, 5, 7, 9, 11, 5, 7, 7, 5, 5, 4, 6, 4, 7, 10, 12, 13, 10, 9, 19, 9, 7, 9, 6, 6, 9, 8, 6, 5, 6, 6, 9, 7, 6, 4, 7, 10, 5, 8, 11, 13, 6, 10, 6, 7, 6, 4, 10,
    4, 9, 5, 7, 6, 9, 8, 8, 7, 8, 6, 8, 8, 10, 8, 6, 9, 5, 10, 3, 4, 11, 12, 7, 5, 9, 7, 6, 6, 10, 8, 7, 3, 9, 7, 5, 10, 7, 11, 7, 9, 8, 8, 14, 8, 7, 8, 3, 6, 7,
    7, 13, 10, 9, 4, 4, 8, 4, 11, 9, 7, 6, 11, 4, 9, 8, 10, 9, 9, 9, 6, 10, 9, 9, 4, 12, 13, 8, 9, 6, 2, 11, 8, 10, 9, 9, 7, 9, 9, 8, 3, 4, 9, 8, 9, 8, 7, 7, 7, 4,
    6, 6, 8, 8, 10, 8, 9, 5, 7, 6, 3, 12, 10, 7, 4, 6, 8, 7, 6, 5, 7, 7, 7, 5, 7, 5, 6, 4, 7, 4, 9, 13, 10, 7, 7, 13, 13, 7, 6, 4, 5, 5, 6, 5, 6, 7, 5, 8, 3, 5,
    7, 7, 9, 5, 5, 12, 7, 7, 10, 4, 5, 9, 12, 7, 8, 5, 7, 4, 5, 8, 5, 4, 5, 5, 3, 6, 11, 5, 3, 10, 4, 8, 8, 3, 5, 6, 3, 8, 3, 6, 7, 9, 5, 8, 5, 7, 4, 2, 8, 9,
    12, 11, 8, 4, 5, 6, 5, 7, 8, 7, 4, 6, 10, 7, 9, 7, 8, 8, 11, 13, 13, 5, 5, 1, 8, 4, 4, 6, 7, 7, 6, 7, 5, 4, 6, 7, 5, 3, 1, 4, 5, 6, 1, 7, 5, 8, 2, 7, 6, 3,
    5, 4, 9, 9, 7, 6, 7, 7, 3, 12, 4, 7, 9, 4, 5, 7, 4, 8, 4, 9, 9, 9, 12, 7, 5, 6, 8, 7, 4, 8, 9, 1, 2, 6, 4, 5, 7, 6, 7, 7, 3, 7, 5, 9, 5, 8, 3, 7, 4, 6,
    7, 5, 4, 7, 9, 4, 8, 6, 9, 8, 6, 11, 2, 3, 3, 5, 5, 3, 4, 6, 5, 3, 7, 5, 10, 6, 4, 5, 1, 9, 5, 10, 9, 5, 7, 6, 2, 4, 4, 3, 5, 7, 8, 7, 5, 3, 3, 5, 3, 3,
    5, 7, 5, 4, 3, 6, 6, 3, 7, 7, 6, 8, 6, 5, 4, 3, 2, 5, 5, 5, 10, 6, 5, 5, 2, 5, 2, 2, 7, 5, 8, 6, 3, 5, 4, 2, 7, 6, 7, 7, 7, 4, 3, 7, 0, 1, 6, 7, 2, 3,
    7, 5, 4, 5, 6, 8, 6, 10, 12, 9, 5, 5, 3, 8, 4, 6, 9, 7, 8, 3, 7, 6, 6, 9, 3, 2, 5, 4, 8, 5, 6, 2, 6, 5, 5, 5, 3, 6, 5, 5, 2, 4, 4, 7, 8, 6, 7, 5, 1, 6,
    2, 4, 8, 9, 6, 3, 3, 4, 6, 2, 7, 2, 5, 5, 5, 4, 4, 4, 2, 7, 4, 5, 4, 5, 4, 7, 3, 5, 4, 7, 4, 5, 2, 6, 6, 4, 8, 4, 3, 9, 2, 2, 2, 1, 3, 6, 8, 5, 2, 4,
    3, 6, 8, 4, 4, 2, 8, 10, 13, 22, 26, 28, 24, 13, 6, 4, 3, 4, 7, 4, 3, 6, 1, 3, 1, 2, 3, 7, 3, 3, 7, 7, 3, 10, 6, 5, 4, 8, 3, 5, 4, 4, 9, 4, 4, 4, 5, 2, 2, 6,
    2, 4, 2, 3, 3, 4, 6, 11, 9, 4, 6, 2, 2, 7, 4, 6, 8, 7, 4, 7, 5, 8, 3, 2, 6, 5, 6, 3, 2, 5, 4, 6, 5, 2, 5, 2, 5, 3, 5, 12, 6, 11, 8, 5, 4, 5, 5, 4, 2, 7,
    5, 2, 7, 2, 5, 9, 2, 6, 4, 3, 4, 6, 7, 1, 2, 2, 8, 3, 8, 6, 11, 7, 6, 6, 6, 8, 12, 21, 16, 23, 13, 10, 9, 4, 3, 4, 3, 6, 3, 5, 5, 7, 3, 3, 3, 5, 3, 3, 5, 0,
    1, 5, 4, 8, 4, 2, 6, 5, 7, 4, 5, 5, 2, 7, 4, 5, 3, 4, 4, 3, 3, 3, 3, 7, 5, 4, 5, 4, 5, 4, 3, 4, 5, 5, 5, 2, 3, 8, 5, 2, 6, 3, 6, 6, 4, 5, 6, 3, 2, 7,
    3, 5, 1, 5, 4, 1, 5, 5, 7, 3, 4, 1, 6, 4, 4, 3, 0, 1, 3, 3, 1, 4, 4, 5, 3, 6, 4, 4, 8, 4, 4, 1, 3, 1, 5, 9, 3, 5, 3, 5, 5, 1, 5, 3, 4, 3, 5, 4, 5, 3,
    0, 1, 6, 4, 5, 8, 6, 4, 3, 3, 6, 5, 3, 3, 2, 4, 3, 3, 5, 4, 6, 4, 4, 3, 6, 5, 3, 3, 6, 1, 10, 3, 3, 5, 4, 3, 7, 5, 4, 9, 5, 5, 3, 5, 2, 6, 4, 5, 4, 3,
    7, 1, 6, 8, 3, 9, 7, 3, 8, 8, 6, 2, 4, 3, 5, 5, 4, 5, 6, 5, 1, 2, 6, 4, 3, 2, 5, 6, 4, 6, 6, 3, 4, 6, 9, 3, 4, 8, 6, 2, 3, 5, 7, 6, 1, 5, 8, 1, 3, 5,
    5, 3, 3, 5, 6, 3, 10, 6, 0, 1, 6, 5, 3, 1, 5, 4, 4, 8, 4, 4, 2, 9, 5, 3, 3, 5, 3, 7, 5, 2, 2, 5, 6, 3, 4, 8, 6, 5, 3, 5, 5, 5, 1, 6, 7, 3, 4, 8, 1, 6,
    4, 2, 5, 3, 2, 1, 4, 3, 4, 3, 5, 5, 5, 3, 2, 4, 5, 2, 3, 3, 5, 4, 3, 3, 2, 3, 2, 5, 6, 6, 5, 6, 4, 5, 7, 4, 5, 6, 3, 12, 8, 10, 13, 15, 7, 4, 11, 5, 8, 5,
    5, 4, 2, 2, 3, 2, 3, 3, 6, 3, 6, 8, 5, 6, 4, 2, 6, 7, 3, 5, 6, 5, 1, 2, 6, 2, 5, 7, 4, 4, 5, 4, 1, 7, 3, 5, 3, 0, 1, 2, 1, 5, 3, 4, 1, 4, 7, 1, 6, 3,
    5, 1, 3, 7, 4, 3, 4, 8, 5, 6, 4, 4, 2, 3, 7, 5, 0, 1, 7, 5, 11, 3, 5, 3, 3, 8, 4, 5, 3, 6, 7, 7, 6, 5, 4, 7, 7, 9, 4, 1, 5, 7, 5, 5, 3, 5, 5, 3, 3, 4,
    5, 3, 4, 7, 6, 2, 3, 5, 2, 5, 5, 4, 5, 6, 3, 2, 2, 4, 4, 4, 5, 4, 4, 2, 3, 2, 1, 3, 3, 4, 7, 5, 1, 2, 7, 5, 2, 6, 3, 3, 3, 4, 7, 2, 7, 4, 4, 5, 5, 9,
    1, 5, 3, 1, 7, 3, 4, 4, 2, 2, 6, 5, 5, 6, 2, 3, 3, 7, 1, 4, 5, 5, 6, 2, 6, 3, 5, 4, 5, 4, 4, 1, 4, 4, 5, 3, 6, 7, 8, 6, 2, 5, 7, 5, 3, 4, 4, 4, 6, 4,
    4, 11, 4, 4, 3, 2, 3, 6, 6, 3, 5, 5, 3, 5, 5, 5, 1, 7, 6, 3, 6, 7, 7, 3, 6, 1, 3, 2, 5, 10, 6, 6, 10, 6, 9, 3, 3, 6, 6, 4, 9, 2, 9, 3, 3, 3, 6, 3, 7, 4,
    3, 4, 4, 3, 2, 3, 1, 4, 2, 3, 6, 5, 5, 1, 2, 4, 3, 5, 2, 7, 1, 1, 2, 1, 2, 0, 1, 7, 4, 1, 5, 4, 4, 0, 1, 2, 2, 4, 0, 1, 2, 5, 3, 3, 7, 4, 5, 1, 1, 1,
    3, 5, 1, 5, 4, 1, 7, 6, 0, 1, 4, 1, 1, 3, 5, 1, 2, 4, 1, 4, 0, 1, 1, 4, 6, 7, 5, 1, 3, 3, 7, 1, 2, 3, 9, 2, 0, 1, 2, 2, 1, 4, 2, 4, 2, 2, 5, 5, 4, 4,
    4, 4, 1, 1, 3, 1, 4, 2, 2, 3, 2, 4, 0, 1, 1, 4, 2, 1, 3, 3, 3, 3, 3, 5, 1, 1, 2, 0, 1, 4, 3, 5, 2, 1, 0, 1, 4, 2, 7, 0, 1, 1, 1, 7, 5, 0, 2, 2, 3, 4,
    2, 2, 1, 2, 3, 2, 3, 2, 2, 2, 5, 1, 2, 3, 2, 1, 2, 4, 4, 2, 1, 1, 2, 3, 1, 0, 1, 1, 1, 1, 2, 2, 2, 4, 3, 5, 3, 3, 3, 2, 0, 1, 2, 4, 2, 3, 1, 6, 0, 1,
    2, 2, 3, 1, 1, 1, 1, 4, 2, 3, 1, 0, 1, 1, 2, 3, 1, 6, 2, 2, 0, 1, 2, 1, 2, 4, 2, 3, 1, 2, 4, 1, 1, 5, 3, 0, 1, 1, 2, 2, 4, 4, 1, 0, 2, 2, 2, 1, 3, 3,
    1, 1, 2, 1, 3, 3, 4, 0, 1, 2, 1, 1, 2, 3, 1, 3, 2, 2, 2, 3, 1, 0, 1, 1, 3, 1, 0, 1, 2, 2, 1, 3, 0, 2, 2, 3, 2, 5, 7, 3, 4, 3, 1, 3, 2, 2, 1, 1, 1, 0,
    1, 1, 3, 1, 4, 3, 2, 4, 0, 1, 3, 1, 1, 1, 3, 1, 1, 0, 1, 1, 1, 2, 2, 1, 1, 1, 3, 2, 3, 2, 2, 2, 3, 0, 1, 1, 0, 1, 2, 0, 1, 3, 0, 1, 3, 2, 2, 3, 3, 3,
    6, 5, 2, 3, 2, 2, 2, 3, 6, 2, 2, 1, 1, 4, 2, 1, 1, 0, 1, 2, 1, 0, 1, 2, 2, 2, 0, 1, 1, 0, 1, 2, 0, 1, 1, 4, 2, 1, 1, 2, 1, 3, 4, 2, 0, 2, 1, 1, 2, 1,
    2, 0, 2, 1, 2, 1, 0, 1, 1, 1, 1, 2, 0, 1, 4, 0, 1, 2, 1, 1, 1, 4, 1, 1, 4, 2, 1, 0, 1, 1, 2, 2, 5, 2, 1, 2, 3, 0, 3, 2, 0, 1, 3, 0, 1, 2, 0, 1, 1, 3,
    2, 1, 2, 1, 0, 1, 2, 1, 1, 2, 3, 2, 3, 3, 1, 2, 0, 1, 1, 2, 3, 3, 5, 7, 31, 41, 66, 117, 104, 143, 107, 75, 42, 22, 3, 2, 0, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 1,
    0, 1, 1, 2, 0, 1, 1, 0, 1, 1, 2, 1, 0, 3, 1, 0, 1, 1, 1, 1, 1, 1, 1, 2, 0, 1, 2, 1, 3, 0, 2, 2, 1, 0, 2, 3, 3, 0, 1, 2, 2, 1, 1, 1, 0, 1, 1, 0, 1, 4,
    1, 1, 1, 1, 0, 1, 2, 1, 0, 2, 3, 2, 0, 1, 2, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 2, 1, 4, 0, 4, 2, 0, 2, 2, 1,
    1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 2, 1, 1, 0, 4, 1, 1, 0, 2, 2, 0, 1, 1, 0, 2, 1, 1, 1, 0, 2, 1, 0, 1, 2, 0, 1, 2, 1, 0, 1, 2, 0, 3, 1, 0, 1, 1, 1, 0, 1,
    2, 1, 3, 3, 1, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 2, 3, 1, 0, 4, 1, 1, 1, 0, 4, 1, 0, 3, 1, 1, 0, 2, 1, 0, 1, 2, 3, 1, 2, 2, 0, 2,
    2, 1, 3, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 1, 1, 0, 2, 2, 0, 3, 1, 1, 1, 2, 1, 0, 1, 1, 1, 0, 2, 1, 0, 4, 1, 1, 0, 4, 1, 4, 0, 4, 1, 1, 0, 1, 2, 1,
    2, 3, 1, 2, 2, 1, 4, 2, 3, 1, 2, 3, 3, 3, 1, 4, 0, 2, 1, 0, 2, 1, 1, 1, 0, 3, 2, 5, 1, 0, 4, 1, 0, 4, 1, 3, 2, 0, 3, 1, 1, 3, 2, 0, 1, 1, 0, 2, 2, 0,
    5, 1, 1, 1, 0, 2, 1, 0, 2, 1, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 2, 0, 1, 1, 0, 1, 3, 0, 1, 1, 2, 2, 0, 2, 1, 0, 1, 1, 1, 3, 1, 2, 1, 0, 2, 1, 0, 2, 1,
    1, 0, 3, 1, 2, 2, 0, 2, 3, 1, 0, 5, 2, 0, 5, 3, 1, 0, 2, 1, 0, 1, 1, 2, 0, 1, 1, 0, 3, 1, 1, 1, 0, 3, 2, 1, 1, 1, 1, 0, 5, 2, 1, 1, 0, 1, 1, 1, 1, 1,
    0, 4, 2, 0, 3, 2, 0, 5, 1, 1, 1, 0, 1, 1, 0, 4, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 4, 1, 0, 3, 1, 0, 4, 1, 0, 2,
    1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 1, 0, 2, 3, 0, 1, 1, 0, 1, 1, 0, 3, 2, 0, 6, 2, 1, 1, 1, 0, 2, 1, 0, 4, 1, 0, 1, 1, 1, 1, 0, 2, 2, 0, 3, 2, 0, 1, 1, 0,
    2, 2, 2, 0, 10, 1, 0, 4, 1, 1, 3, 2, 1, 3, 1, 1, 1, 0, 1, 1, 0, 6, 3, 1, 0, 4, 1, 0, 2, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 0, 4, 1, 1, 0, 3, 3, 0, 4, 3, 0,
    3, 1, 0, 5, 2, 1, 0, 3, 2, 0, 2, 3, 2, 4, 7, 8, 7, 9, 12, 8, 4, 1, 2, 0, 1, 1, 0, 3, 1, 0, 6, 2, 0, 9, 1, 0, 3, 1, 0, 1, 2, 0, 1, 1, 1, 1, 1, 0, 1, 1,
    0, 2, 1, 0, 3, 1, 1, 0, 6, 1, 0, 1, 2, 1, 0, 4, 2, 0, 2, 1, 0, 4, 1, 1, 1, 0, 7, 1, 0, 2, 1, 0, 1, 1, 0, 4, 1, 0, 2, 2, 2, 0, 6, 1, 0, 4, 1, 0, 1, 1,
    0, 1, 1, 0, 8, 1, 0, 3, 1, 1, 1, 1, 0, 2, 1, 0, 8, 1, 0, 19, 1, 0, 4, 1, 0, 1, 2, 1, 2, 3, 0, 2, 2, 1, 1, 0, 3, 1, 1, 0, 8, 1, 0, 3, 1, 0, 10, 1, 0, 2,
    1, 2, 0, 4, 1, 0, 3, 1, 0, 2, 1, 1, 1, 0, 1, 1, 0, 8, 1, 0, 3, 3, 0, 3, 1, 1, 0, 5, 1, 1, 1, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1, 1, 0, 4, 3, 1, 0,
    4, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 1, 0, 1, 1, 1, 0, 2, 1, 0, 7, 1, 0, 5, 1, 0, 1, 2, 0, 2, 1, 0, 13, 1, 1, 0, 2, 2, 1, 0, 3, 1, 0, 1, 1, 2, 0,
    5, 1, 1, 2, 2, 0, 1, 3, 0, 3, 1, 0, 1, 1, 1, 1, 0, 2, 1, 0, 6, 1, 0, 5, 2, 0, 2, 1, 0, 3, 2, 0, 3, 1, 1, 1, 0, 1, 2, 0, 4, 2, 0, 2, 1, 2, 1, 1, 0, 4,
    1, 0, 6, 2, 0, 4, 1, 0, 1, 1, 0, 4, 1, 0, 1, 1, 1, 1, 0, 5, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 7, 1, 0, 8, 1, 0, 1, 1, 1, 1, 0, 7, 1, 0, 1, 2, 0, 1,
    1, 2, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 0, 16, 1, 1, 0, 3, 1, 1, 0, 1, 1, 0, 3, 1, 1, 0, 4, 1, 0, 1, 1, 0, 4, 1, 0, 4, 2, 0, 1, 1, 0, 2, 1, 0, 9, 1,
    0, 2, 1, 0, 2, 1, 1, 1, 1, 0, 4, 1, 1, 0, 2, 1, 0, 1, 1, 0, 5, 1, 0, 3, 1, 0, 5, 1, 3, 0, 7, 1, 1, 0, 5, 1, 0, 1, 1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 1, 1,
    0, 1, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 6, 2, 2, 0, 1, 2, 0, 4, 1, 0, 2, 1, 1, 0, 1, 1, 0, 1, 1, 2, 1, 0, 2, 1, 0, 1, 1, 0, 9, 2, 0, 2, 1, 1, 0, 2, 1, 0,
    1, 1, 0, 5, 1, 1, 3, 0, 1, 1, 0, 1, 3, 3, 2, 0, 1, 3, 1, 3, 0, 1, 1, 0, 1, 1, 0, 1, 2, 0, 11, 2, 0, 1, 1, 0, 2, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0,
    1, 2, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 6, 2, 0, 2, 1, 0, 2, 1, 1, 0, 5, 1, 0, 2, 1, 0, 3, 1, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 6, 1, 1, 0, 1, 1, 0, 5, 1, 1,
    2, 0, 1, 1, 0, 2, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 2, 2, 0, 3, 2, 1, 0, 2, 1, 0, 2, 1, 0, 9, 1, 0, 5, 1, 0, 2, 1, 0, 1, 2, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1,
    0, 4, 1, 0, 1, 2, 2, 0, 2, 2, 0, 5, 1, 0, 8, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 1, 1, 0, 4, 1, 3, 1, 0, 1, 1, 2, 1, 2, 1, 2, 1, 1, 1, 0, 1,
    1, 1, 1, 0, 10, 1, 0, 4, 1, 0, 7, 1, 0, 2, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 1, 0, 1, 2, 1, 1, 1, 1, 0, 3, 1, 2, 0, 2, 1, 1, 1, 0, 7, 1,
    1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 5, 1, 0, 4, 2, 1, 0, 4, 1, 0, 4, 1, 1, 0, 4, 1, 0, 1, 1, 0, 1, 2, 0, 4, 1, 0, 1, 1, 0, 8, 1, 0, 1, 1, 0, 2,
    1, 1, 0, 2, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2, 2, 0, 3, 1, 0, 3, 1, 0, 2, 2, 0, 2, 1, 1, 1, 0, 3, 1, 0, 2, 1, 1, 1, 0, 3, 2, 2, 2, 0, 2, 2, 0, 9, 1, 0, 1,
    1, 0, 5, 1, 0, 5, 2, 0, 9, 1, 1, 0, 3, 1, 0, 5, 2, 1, 0, 1, 2, 1, 0, 2, 1, 0, 2, 2, 1, 0, 1, 1, 1, 0, 1, 1, 0, 6, 1, 2, 0, 2, 2, 0, 3, 1, 0, 4, 2, 2,
    1, 0, 2, 1, 0, 3, 1, 1, 0, 9, 1, 1, 1, 1, 1, 2, 0, 2, 1, 0, 6, 1, 0, 2, 1, 0, 4, 2, 0, 4, 1, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 1, 1, 0, 5, 1, 0,
    2, 1, 1, 0, 2, 1, 0, 2, 1, 0, 5, 1, 0, 8, 1, 0, 5, 2, 0, 1, 1, 0, 1, 1, 1, 2, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 8, 2, 0, 1, 1, 0, 3, 1, 0, 12, 2, 0, 4, 2,
    0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 3, 1, 1, 0, 1, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 2, 2, 1, 0, 8, 1, 0, 1, 1, 0, 3, 1, 0, 14, 1, 0, 4, 1, 0, 5, 1, 0, 1, 1,
    0, 2, 1, 1, 2, 1, 2, 0, 2, 1, 0, 5, 1, 0, 2, 1, 0, 8, 1, 0, 22, 1, 0, 2, 1, 0, 2, 1, 1, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 4, 1, 0, 9, 1, 0, 1, 1, 0, 4, 1,
    0, 1, 1, 0, 8, 1, 2, 0, 6, 1, 0, 8, 1, 0, 2, 1, 1, 0, 23, 1, 0, 2, 1, 0, 2, 1, 1, 0, 19, 1, 0, 4, 1, 1, 0, 6, 1, 0, 15, 1, 0, 5, 2, 0, 2, 1, 0, 38, 1, 0,
    14, 1, 0, 19, 1, 1, 0, 7, 1, 0, 8, 1, 0, 16, 1, 0, 2, 1, 0, 8, 1, 0, 5, 3, 2, 2, 2, 3, 7, 13, 15, 19, 17, 9, 10, 7, 5, 0, 39, 1, 0, 5, 1, 1, 0, 14, 1, 0, 78, 1,
    0, 28, 1, 0, 130, 1, 0, 10, 1, 0, 125, 1, 0, 30, 1, 0, 1, 1, 0, 45, 1, 0, 7, 1, 0, 1, 1, 0, 5, 1, 0, 36, 1, 0, 17, 1, 0, 63, 1, 0, 24, 1, 0, 49, 1, 0, 21, 1, 0, 2,
    1, 0, 58, 1, 0, 20, 1, 0, 6, 1, 0, 1, 1, 0, 5, 1, 0, 3, 1, 0, 28, 1, 0, 43, 1, 0, 61, 1, 0, 13, 1, 0, 103, 1, 0, 83, 1, 0, 76, 1, 0, 49, 1, 0, 15, 1, 0, 52, 1, 0,
    129, 1, 0, 2, 1, 0, 77, 1, 0, 22, 1, 0, 122, 1, 1, 0, 1, 1, 0, 15, 1, 0, 43, 1, 0, 57, 1, 0, 30, 1, 0, 22, 1, 0, 30, 1, 0, 17, 1, 0, 159, 1, 0, 154, 1, 0, 156, 1, 0, 114,
    1, 1, 0, 102, 1, 0, 13, 1, 1, 0, 78, 1, 0, 66, 2, 0, 230, 1, 0, 35, 1, 0, 77, 1, 0, 4, 1, 0, 7, 1, 0, 49, 1, 0, 7, 1, 0, 94, 1, 0, 20, 1, 0, 238, 1, 0, 27, 1, 1, 0,
    27, 1, 1, 0, 176, 1, 0, 149, 1, 0, 81, 1, 0, 133, 1, 0, 12, 1, 0, 56, 1, 0, 194, 1, 0, 61, 1, 0, 57, 1, 1, 0, 151, 1, 0, 108, 1, 0, 197, 1, 0, 123, 1, 0, 15, 1, 0, 129, 1, 0,
    71, 1, 0, 34, 1, 0, 6, 1, 1, 0, 94, 1, 0, 29, 1, 0, 35, 1, 1, 0, 30, 1, 0, 169, 1, 0, 77, 1, 0, 1, 1, 1, 0, 41, 1, 0, 10, 1, 0, 17, 1, 0, 134, 1, 0, 111, 1, 0, 34, 1,
    0, 61, 1, 0, 2, 1, 0, 87, 1, 0, 96, 1, 0, 18, 1, 0, 203, 1, 0, 119, 1, 0, 86, 1, 0, 38, 1, 0, 152, 1, 1, 0, 116, 1, 1, 0, 21, 1, 0, 31, 1, 0, 75, 1, 0, 38, 1, 0, 254, 1,
    0, 178, 1, 1, 0, 94, 1, 0, 102, 1, 0, 42, 1, 0, 168, 1, 0, 61, 1, 0, 198
  };
  assert( test_16_packed.size() == 6421 );
  const vector<uint8_t> test_16_encoded = QRSpecDev::encode_stream_vbyte( test_16_chan_cnts );
  assert( test_16_encoded == test_16_packed );
  vector<uint32_t> test_16_dec;
  const size_t test_16_nbytedec = QRSpecDev::decode_stream_vbyte(test_16_encoded,test_16_dec);
  assert( test_16_nbytedec == test_16_packed.size() );
  assert( test_16_dec == test_16_chan_cnts );
  
  
  
  
  // Test case 17
  const vector<uint32_t> test_17_chan_cnts{
    0, 29, 1, 0, 1, 1, 2, 0, 1, 2, 3, 1, 3, 8, 23, 410, 930, 975, 965, 932,
    910, 931, 913, 943, 885, 869, 858, 872, 874, 895, 872, 833, 904, 868, 800, 768, 851, 831, 826, 753,
    831, 836, 767, 815, 832, 786, 777, 797, 784, 783, 771, 783, 777, 721, 789, 723, 694, 780, 729, 762,
    741, 732, 734, 679, 724, 712, 751, 714, 706, 694, 750, 716, 652, 701, 707, 687, 719, 643, 712, 713,
    695, 750, 678, 726, 706, 683, 703, 744, 745, 763, 778, 891, 870, 1088, 1190, 1281, 1277, 1276, 975, 836,
    696, 673, 658, 670, 674, 672, 675, 682, 668, 705, 850, 982, 1048, 1092, 982, 808, 834, 801, 706, 661,
    641, 624, 594, 624, 638, 661, 643, 655, 702, 723, 690, 708, 741, 775, 736, 680, 644, 621, 654, 603,
    616, 636, 616, 621, 627, 625, 631, 662, 635, 655, 635, 637, 711, 745, 741, 726, 671, 675, 713, 688,
    665, 656, 670, 688, 676, 704, 731, 685, 714, 632, 682, 618, 619, 630, 631, 619, 612, 657, 689, 678,
    686, 619, 665, 657, 655, 642, 654, 638, 658, 641, 685, 711, 665, 682, 648, 673, 726, 679, 697, 715,
    693, 632, 644, 666, 697, 698, 643, 640, 672, 628, 668, 715, 659, 638, 616, 670, 687, 659, 646, 632,
    600, 652, 650, 628, 626, 614, 608, 661, 626, 596, 642, 649, 691, 692, 654, 615, 706, 644, 714, 745,
    764, 723, 662, 642, 636, 672, 638, 660, 631, 645, 629, 645, 604, 658, 642, 603, 660, 649, 660, 687,
    653, 636, 640, 658, 652, 599, 698, 673, 598, 658, 663, 668, 583, 649, 642, 622, 626, 633, 652, 657,
    638, 677, 641, 655, 650, 658, 617, 668, 638, 668, 752, 703, 655, 622, 657, 647, 647, 664, 634, 681,
    621, 650, 664, 649, 640, 636, 624, 641, 622, 588, 649, 622, 613, 642, 586, 689, 682, 644, 606, 623,
    654, 636, 647, 633, 598, 680, 581, 651, 665, 609, 687, 650, 634, 672, 644, 608, 670, 672, 621, 634,
    654, 634, 605, 652, 629, 640, 634, 654, 656, 629, 600, 647, 641, 607, 676, 633, 669, 639, 644, 649,
    598, 654, 682, 656, 600, 687, 604, 675, 641, 644, 659, 708, 668, 638, 634, 588, 640, 620, 639, 629,
    687, 640, 647, 650, 745, 662, 656, 614, 693, 625, 624, 625, 704, 667, 679, 665, 607, 604, 633, 618,
    694, 783, 946, 1129, 1178, 1033, 854, 656, 617, 635, 618, 632, 614, 637, 642, 649, 649, 680, 723, 646,
    603, 626, 605, 610, 644, 678, 615, 600, 589, 625, 630, 634, 645, 632, 626, 658, 631, 629, 609, 629,
    645, 611, 625, 632, 638, 648, 641, 635, 618, 686, 619, 648, 644, 651, 652, 661, 664, 651, 641, 779,
    1175, 2329, 4328, 6836, 7481, 5855, 3547, 1912, 1399, 1184, 1000, 832, 592, 552, 535, 517, 530, 524, 503, 555,
    530, 517, 512, 559, 535, 548, 540, 473, 485, 517, 546, 546, 560, 548, 570, 543, 541, 494, 545, 482,
    522, 467, 538, 530, 535, 515, 533, 463, 514, 507, 524, 523, 519, 562, 534, 486, 554, 523, 544, 510,
    513, 529, 506, 533, 508, 531, 704, 871, 1032, 1060, 847, 715, 583, 515, 493, 478, 491, 471, 475, 485,
    564, 617, 745, 787, 838, 693, 567, 545, 470, 504, 479, 478, 480, 507, 475, 461, 455, 503, 467, 464,
    475, 454, 506, 499, 541, 515, 538, 557, 503, 460, 454, 489, 479, 477, 496, 495, 480, 472, 497, 479,
    501, 505, 465, 496, 465, 489, 519, 689, 845, 979, 1125, 908, 678, 589, 496, 433, 452, 435, 474, 417,
    482, 456, 484, 466, 444, 438, 457, 465, 446, 466, 456, 460, 441, 462, 460, 483, 454, 429, 416, 470,
    448, 448, 444, 466, 463, 433, 465, 452, 491, 471, 461, 477, 448, 485, 459, 470, 433, 513, 493, 420,
    453, 446, 452, 504, 611, 812, 1061, 1024, 883, 660, 537, 458, 495, 496, 486, 518, 548, 428, 510, 439,
    464, 433, 437, 501, 639, 1043, 1808, 2592, 2870, 2380, 1559, 881, 582, 494, 517, 456, 379, 411, 380, 393,
    388, 409, 393, 381, 418, 424, 401, 385, 371, 398, 380, 381, 405, 387, 403, 411, 388, 375, 401, 437,
    371, 376, 367, 374, 370, 415, 378, 392, 364, 404, 389, 413, 369, 358, 377, 378, 360, 375, 410, 368,
    394, 393, 388, 380, 372, 361, 365, 374, 367, 371, 411, 366, 387, 356, 387, 411, 365, 376, 408, 386,
    369, 354, 372, 391, 399, 390, 344, 395, 398, 401, 385, 403, 367, 368, 365, 366, 367, 384, 373, 383,
    364, 359, 380, 356, 357, 380, 358, 368, 369, 348, 363, 347, 344, 349, 371, 354, 372, 357, 352, 344,
    367, 390, 351, 362, 341, 365, 354, 352, 340, 353, 323, 347, 374, 366, 378, 353, 398, 367, 342, 372,
    371, 329, 362, 325, 373, 365, 356, 349, 331, 356, 435, 577, 631, 709, 628, 550, 410, 356, 314, 325,
    332, 313, 267, 355, 318, 313, 314, 304, 300, 300, 297, 304, 329, 282, 291, 324, 300, 302, 314, 354,
    314, 299, 301, 285, 279, 301, 326, 299, 327, 284, 294, 285, 318, 290, 289, 290, 287, 279, 285, 299,
    300, 292, 327, 281, 289, 294, 280, 285, 291, 296, 262, 284, 271, 270, 324, 301, 278, 309, 292, 269,
    279, 277, 282, 288, 321, 287, 281, 258, 276, 284, 260, 288, 290, 299, 297, 290, 268, 286, 262, 270,
    355, 363, 348, 319, 312, 246, 258, 252, 284, 284, 269, 279, 272, 245, 288, 300, 263, 311, 351, 502,
    747, 1032, 1163, 1080, 758, 473, 315, 270, 271, 258, 245, 287, 247, 258, 253, 278, 254, 270, 249, 265,
    257, 278, 260, 258, 274, 263, 276, 283, 280, 273, 248, 260, 310, 282, 288, 297, 267, 269, 252, 261,
    283, 262, 273, 251, 265, 263, 247, 244, 261, 258, 266, 237, 286, 254, 255, 288, 268, 267, 253, 246,
    255, 260, 254, 255, 262, 233, 270, 226, 246, 270, 237, 246, 271, 228, 255, 249, 257, 264, 283, 226,
    272, 244, 264, 282, 293, 301, 287, 281, 283, 279, 234, 281, 329, 347, 403, 580, 821, 1168, 1689, 1918,
    1945, 1543, 1016, 702, 497, 357, 321, 280, 267, 251, 242, 255, 209, 233, 238, 229, 266, 241, 242, 238,
    263, 231, 217, 235, 260, 272, 257, 270, 258, 232, 244, 244, 218, 251, 234, 266, 256, 262, 224, 257,
    235, 261, 231, 239, 229, 230, 226, 224, 243, 224, 236, 259, 236, 253, 224, 196, 249, 239, 212, 253,
    243, 259, 232, 246, 247, 223, 235, 221, 227, 216, 245, 251, 305, 254, 266, 247, 218, 241, 265, 264,
    248, 247, 238, 235, 231, 224, 220, 261, 248, 240, 218, 262, 257, 225, 239, 226, 223, 210, 243, 215,
    251, 248, 276, 305, 380, 407, 388, 336, 261, 225, 220, 250, 243, 233, 190, 209, 252, 242, 238, 259,
    236, 259, 252, 240, 273, 276, 256, 238, 227, 227, 219, 206, 245, 260, 205, 241, 242, 222, 239, 255,
    242, 304, 391, 680, 1348, 2524, 4114, 4909, 4650, 3209, 1650, 810, 324, 252, 167, 224, 204, 217, 213, 185,
    220, 197, 215, 219, 187, 198, 195, 205, 203, 194, 205, 190, 210, 189, 213, 182, 209, 189, 184, 200,
    201, 191, 205, 190, 191, 207, 206, 204, 186, 202, 200, 183, 199, 185, 210, 193, 231, 228, 223, 221,
    232, 248, 236, 229, 189, 174, 185, 191, 190, 184, 183, 202, 195, 234, 217, 208, 208, 208, 186, 181,
    202, 187, 207, 216, 213, 204, 233, 188, 222, 178, 180, 184, 193, 198, 175, 196, 205, 197, 186, 195,
    200, 220, 195, 211, 199, 220, 180, 183, 200, 180, 179, 184, 174, 186, 184, 191, 190, 196, 188, 190,
    189, 197, 204, 210, 203, 211, 194, 192, 207, 204, 186, 218, 193, 199, 190, 168, 201, 184, 221, 163,
    186, 196, 210, 205, 196, 198, 186, 189, 206, 202, 177, 171, 171, 193, 185, 186, 203, 206, 199, 185,
    206, 203, 170, 194, 189, 198, 185, 196, 181, 208, 175, 194, 201, 211, 178, 187, 190, 222, 189, 184,
    202, 207, 189, 192, 186, 220, 198, 214, 173, 212, 194, 178, 228, 229, 219, 183, 200, 199, 220, 184,
    188, 191, 187, 181, 199, 179, 186, 204, 213, 197, 188, 188, 203, 185, 186, 202, 181, 204, 160, 218,
    222, 219, 179, 201, 222, 185, 206, 209, 175, 194, 206, 187, 192, 206, 217, 196, 204, 184, 196, 176,
    183, 216, 189, 218, 193, 224, 215, 198, 191, 232, 213, 206, 220, 200, 214, 219, 217, 195, 213, 209,
    209, 232, 195, 227, 207, 195, 204, 177, 193, 201, 179, 183, 191, 206, 181, 198, 185, 158, 195, 177,
    176, 173, 167, 176, 173, 162, 174, 170, 171, 207, 184, 195, 201, 184, 173, 161, 197, 256, 365, 587,
    899, 1259, 1254, 1099, 702, 390, 264, 182, 160, 154, 161, 160, 167, 163, 172, 159, 167, 169, 168, 150,
    143, 163, 173, 163, 147, 137, 154, 152, 154, 147, 133, 160, 155, 158, 187, 176, 161, 166, 142, 156,
    128, 148, 164, 162, 143, 145, 157, 162, 154, 148, 165, 154, 138, 145, 167, 166, 199, 243, 280, 302,
    297, 252, 207, 178, 144, 147, 147, 128, 180, 147, 157, 158, 167, 191, 208, 215, 184, 213, 167, 143,
    122, 132, 152, 117, 132, 143, 145, 133, 124, 140, 168, 226, 303, 350, 368, 313, 256, 186, 142, 133,
    126, 124, 99, 114, 131, 134, 125, 101, 118, 115, 122, 122, 174, 180, 190, 201, 169, 159, 177, 205,
    253, 252, 202, 185, 144, 110, 122, 115, 97, 108, 93, 122, 112, 112, 109, 141, 189, 309, 467, 643,
    760, 757, 500, 314, 208, 122, 103, 95, 82, 90, 87, 100, 89, 96, 94, 84, 78, 94, 92, 104,
    93, 99, 89, 93, 87, 90, 85, 99, 95, 90, 82, 113, 76, 84, 81, 83, 89, 89, 83, 91,
    83, 90, 77, 84, 96, 94, 81, 99, 105, 92, 89, 81, 108, 86, 93, 83, 84, 85, 87, 92,
    76, 76, 88, 91, 82, 83, 74, 73, 84, 109, 102, 128, 144, 197, 162, 119, 101, 80, 99, 111,
    137, 186, 274, 316, 313, 274, 213, 125, 112, 105, 121, 156, 185, 213, 204, 154, 126, 97, 84, 71,
    65, 72, 92, 73, 82, 72, 68, 57, 78, 71, 67, 68, 81, 62, 76, 74, 62, 71, 67, 72,
    69, 82, 85, 65, 79, 70, 76, 72, 71, 107, 121, 181, 332, 519, 591, 663, 503, 328, 178, 92,
    73, 84, 78, 72, 53, 73, 58, 66, 71, 63, 51, 79, 59, 56, 58, 64, 61, 60, 44, 76,
    62, 80, 80, 79, 69, 83, 70, 54, 64, 67, 73, 73, 50, 68, 75, 65, 58, 71, 61, 70,
    72, 68, 66, 67, 49, 63, 64, 86, 57, 65, 63, 66, 62, 60, 69, 77, 61, 69, 73, 77,
    106, 94, 120, 115, 87, 86, 68, 54, 70, 65, 74, 56, 58, 53, 78, 57, 59, 61, 90, 76,
    94, 84, 153, 147, 179, 173, 124, 106, 87, 106, 97, 139, 180, 344, 714, 1373, 2393, 3447, 3931, 3501,
    2406, 1235, 542, 200, 76, 62, 54, 50, 52, 52, 53, 55, 62, 53, 57, 44, 49, 47, 52, 54,
    41, 47, 52, 51, 44, 40, 42, 52, 47, 52, 44, 56, 51, 48, 48, 49, 43, 43, 46, 44,
    55, 47, 33, 36, 53, 46, 46, 48, 38, 50, 50, 44, 52, 41, 38, 58, 50, 58, 42, 39,
    44, 43, 61, 54, 53, 71, 50, 65, 42, 59, 61, 45, 55, 65, 61, 58, 50, 48, 55, 53,
    55, 64, 61, 62, 65, 56, 44, 59, 70, 56, 71, 60, 61, 94, 92, 106, 81, 73, 64, 62,
    60, 69, 104, 135, 266, 465, 617, 751, 676, 567, 421, 347, 529, 915, 1501, 2029, 2270, 1939, 1274, 683,
    289, 115, 63, 44, 34, 44, 37, 33, 37, 50, 39, 39, 35, 44, 31, 50, 41, 30, 33, 40,
    45, 36, 46, 46, 50, 62, 45, 47, 53, 39, 31, 40, 42, 53, 50, 58, 57, 69, 52, 29,
    31, 44, 36, 37, 41, 39, 34, 31, 30, 37, 37, 42, 36, 35, 34, 27, 33, 43, 32, 23,
    41, 36, 38, 39, 39, 40, 38, 36, 23, 34, 37, 30, 29, 31, 24, 32, 30, 41, 40, 33,
    34, 37, 34, 32, 34, 33, 38, 37, 28, 44, 35, 42, 48, 39, 39, 31, 43, 38, 32, 31,
    42, 43, 36, 30, 46, 32, 32, 34, 40, 35, 44, 36, 37, 47, 42, 38, 41, 41, 32, 29,
    34, 34, 33, 47, 49, 59, 59, 60, 51, 47, 49, 43, 25, 34, 37, 39, 30, 38, 35, 24,
    36, 51, 41, 54, 33, 43, 26, 19, 34, 31, 39, 27, 25, 21, 35, 32, 29, 42, 39, 32,
    36, 38, 33, 34, 41, 37, 48, 37, 33, 41, 35, 45, 34, 45, 31, 33, 33, 29, 35, 20,
    33, 38, 39, 26, 33, 41, 45, 39, 43, 55, 46, 53, 64, 65, 60, 35, 33, 35, 31, 34,
    39, 33, 34, 37, 31, 41, 32, 29, 42, 31, 38, 32, 43, 31, 41, 55, 48, 61, 86, 88,
    92, 85, 70, 39, 54, 40, 35, 39, 36, 31, 34, 28, 35, 37, 30, 32, 35, 37, 34, 31,
    45, 46, 27, 42, 34, 42, 34, 41, 57, 69, 85, 72, 75, 70, 68, 60, 44, 47, 43, 34,
    42, 38, 38, 42, 39, 32, 34, 30, 39, 38, 35, 46, 30, 36, 38, 31, 28, 33, 29, 34,
    28, 48, 54, 70, 70, 75, 68, 47, 43, 40, 44, 45, 38, 30, 24, 36, 40, 33, 27, 38,
    37, 49, 39, 34, 36, 45, 48, 52, 46, 43, 41, 29, 46, 28, 32, 32, 38, 32, 32, 33,
    24, 32, 35, 24, 30, 25, 30, 34, 34, 32, 35, 35, 36, 29, 36, 35, 27, 35, 29, 28,
    20, 27, 39, 33, 26, 26, 23, 37, 47, 43, 34, 28, 35, 35, 36, 38, 27, 24, 25, 34,
    30, 29, 28, 38, 36, 39, 40, 35, 38, 40, 41, 46, 54, 49, 30, 44, 44, 42, 34, 31,
    31, 28, 40, 45, 35, 30, 33, 38, 22, 43, 36, 40, 32, 28, 42, 50, 33, 36, 36, 28,
    28, 36, 34, 48, 41, 38, 35, 26, 28, 47, 44, 35, 23, 46, 37, 22, 26, 24, 30, 24,
    41, 30, 32, 35, 34, 32, 31, 38, 28, 32, 37, 25, 30, 33, 35, 24, 25, 26, 31, 41,
    25, 33, 42, 34, 37, 38, 23, 37, 29, 30, 36, 33, 30, 35, 31, 32, 28, 36, 37, 47,
    28, 29, 31, 25, 37, 27, 39, 39, 34, 40, 40, 31, 40, 27, 39, 30, 36, 28, 31, 32,
    35, 35, 28, 32, 34, 34, 43, 33, 40, 28, 25, 37, 34, 34, 26, 24, 40, 29, 34, 31,
    29, 30, 23, 37, 30, 31, 33, 42, 33, 43, 44, 32, 23, 50, 36, 34, 27, 23, 43, 35,
    31, 21, 37, 32, 35, 45, 24, 27, 38, 36, 29, 32, 43, 40, 34, 29, 37, 34, 35, 28,
    34, 41, 31, 33, 40, 31, 47, 42, 33, 25, 32, 34, 31, 32, 44, 26, 29, 39, 42, 57,
    79, 59, 70, 85, 83, 83, 70, 42, 52, 37, 54, 32, 40, 41, 42, 30, 33, 37, 38, 36,
    34, 36, 27, 34, 39, 39, 25, 35, 28, 28, 37, 30, 40, 33, 37, 30, 25, 33, 27, 41,
    31, 36, 33, 31, 31, 34, 35, 19, 32, 36, 34, 35, 35, 45, 36, 40, 18, 37, 32, 22,
    42, 44, 42, 27, 44, 40, 23, 29, 31, 26, 28, 38, 22, 34, 34, 36, 31, 29, 39, 34,
    35, 29, 57, 49, 52, 47, 43, 33, 58, 28, 42, 33, 42, 23, 28, 25, 34, 28, 31, 34,
    42, 37, 27, 39, 28, 30, 25, 27, 27, 31, 31, 30, 27, 22, 24, 41, 30, 32, 25, 33,
    44, 34, 35, 38, 36, 30, 33, 33, 32, 34, 30, 45, 28, 36, 36, 25, 35, 28, 23, 29,
    37, 36, 32, 44, 31, 30, 21, 30, 37, 34, 27, 39, 27, 18, 25, 31, 37, 26, 29, 32,
    28, 39, 30, 24, 23, 35, 28, 30, 25, 34, 28, 26, 33, 23, 22, 39, 22, 29, 37, 41,
    38, 33, 29, 36, 31, 38, 23, 29, 21, 41, 45, 38, 26, 34, 36, 27, 28, 31, 32, 35,
    34, 20, 32, 22, 26, 39, 34, 42, 29, 30, 33, 30, 41, 25, 25, 33, 23, 32, 28, 21,
    25, 37, 35, 38, 34, 37, 30, 35, 44, 36, 37, 26, 35, 34, 32, 30, 32, 36, 36, 37,
    32, 30, 36, 33, 28, 35, 46, 28, 28, 29, 39, 35, 33, 28, 27, 34, 30, 29, 41, 27,
    34, 33, 35, 38, 42, 34, 36, 24, 30, 33, 29, 37, 35, 30, 26, 23, 29, 37, 25, 26,
    23, 33, 19, 35, 31, 24, 32, 30, 36, 29, 28, 25, 33, 22, 23, 33, 32, 29, 33, 27,
    37, 28, 26, 27, 19, 27, 31, 30, 40, 38, 18, 34, 25, 26, 30, 31, 29, 29, 29, 31,
    29, 25, 21, 21, 32, 37, 27, 29, 27, 39, 34, 28, 38, 26, 36, 20, 30, 21, 22, 30,
    24, 25, 23, 17, 32, 25, 33, 24, 39, 20, 26, 18, 29, 24, 30, 30, 29, 22, 21, 15,
    17, 25, 21, 28, 17, 25, 23, 22, 18, 24, 26, 19, 39, 26, 33, 20, 26, 28, 28, 26,
    27, 36, 32, 28, 23, 24, 14, 27, 26, 27, 25, 33, 32, 28, 20, 18, 34, 19, 20, 25,
    30, 26, 26, 23, 21, 27, 17, 25, 23, 19, 25, 21, 21, 16, 27, 28, 19, 24, 21, 33,
    31, 32, 19, 26, 30, 28, 17, 36, 33, 32, 22, 33, 26, 52, 57, 89, 101, 126, 149, 150,
    143, 155, 132, 90, 57, 50, 31, 21, 28, 23, 26, 19, 21, 30, 20, 16, 21, 19, 27, 25,
    18, 23, 20, 19, 14, 19, 23, 25, 21, 27, 14, 22, 20, 24, 19, 18, 24, 30, 14, 19,
    21, 28, 22, 23, 16, 11, 19, 21, 23, 9, 22, 21, 18, 25, 21, 19, 26, 26, 19, 16,
    16, 19, 17, 22, 19, 19, 24, 24, 35, 52, 75, 85, 87, 88, 90, 50, 46, 29, 26, 25,
    37, 24, 42, 66, 54, 61, 38, 34, 27, 29, 18, 16, 17, 16, 16, 18, 14, 19, 12, 14,
    15, 24, 20, 22, 27, 35, 42, 48, 48, 39, 34, 31, 17, 16, 14, 22, 13, 24, 17, 12,
    20, 12, 13, 23, 17, 19, 12, 12, 19, 14, 14, 19, 20, 14, 27, 17, 18, 23, 26, 26,
    14, 20, 21, 15, 18, 28, 9, 14, 18, 15, 18, 12, 23, 26, 22, 14, 22, 20, 17, 15,
    13, 25, 17, 19, 17, 17, 21, 17, 11, 26, 11, 23, 15, 18, 11, 12, 10, 21, 15, 25,
    16, 20, 14, 17, 19, 18, 20, 22, 18, 17, 19, 21, 15, 21, 20, 22, 19, 30, 28, 30,
    30, 25, 14, 19, 18, 19, 9, 15, 18, 16, 17, 19, 18, 21, 24, 14, 17, 18, 13, 24,
    19, 22, 17, 16, 21, 16, 11, 15, 20, 22, 16, 20, 19, 15, 21, 15, 18, 16, 22, 17,
    23, 27, 24, 56, 62, 78, 61, 60, 41, 32, 28, 24, 36, 34, 37, 32, 58, 83, 138, 200,
    274, 308, 260, 250, 154, 127, 95, 98, 99, 105, 144, 110, 87, 62, 57, 23, 24, 19, 14, 18,
    12, 27, 18, 19, 24, 21, 16, 21, 26, 26, 20, 20, 20, 17, 24, 20, 18, 15, 11, 14,
    20, 23, 17, 13, 32, 28, 25, 23, 30, 21, 16, 19, 16, 21, 21, 21, 18, 29, 27, 25,
    32, 31, 31, 49, 62, 76, 115, 131, 144, 117, 110, 66, 40, 31, 40, 38, 46, 49, 36, 31,
    23, 23, 24, 41, 53, 79, 109, 150, 174, 151, 117, 80, 44, 25, 22, 21, 20, 25, 30, 23,
    33, 36, 48, 72, 48, 55, 30, 33, 27, 18, 23, 16, 21, 21, 17, 23, 18, 18, 21, 12,
    18, 23, 17, 13, 11, 18, 16, 14, 18, 17, 18, 13, 19, 18, 22, 17, 12, 15, 14, 18,
    17, 19, 17, 20, 20, 16, 17, 22, 20, 21, 16, 18, 19, 21, 15, 23, 17, 20, 29, 24,
    34, 26, 26, 24, 22, 20, 23, 14, 23, 15, 18, 13, 15, 21, 7, 17, 22, 16, 13, 14,
    23, 23, 27, 18, 30, 36, 16, 27, 27, 21, 20, 15, 24, 14, 26, 23, 17, 20, 16, 22,
    20, 21, 29, 23, 24, 21, 15, 14, 25, 19, 22, 16, 19, 22, 15, 15, 15, 18, 18, 20,
    19, 22, 14, 23, 17, 15, 17, 14, 17, 18, 20, 13, 27, 25, 22, 23, 22, 21, 13, 24,
    24, 17, 14, 23, 20, 24, 25, 31, 16, 9, 20, 15, 9, 17, 17, 19, 21, 19, 17, 14,
    17, 20, 17, 16, 14, 18, 16, 15, 16, 20, 20, 18, 24, 11, 19, 17, 16, 29, 19, 14,
    29, 24, 18, 16, 19, 16, 17, 25, 16, 16, 19, 19, 20, 17, 19, 18, 14, 22, 15, 18,
    15, 20, 14, 15, 20, 12, 22, 11, 30, 22, 17, 9, 26, 19, 27, 17, 19, 17, 17, 23,
    23, 18, 24, 20, 18, 19, 17, 14, 22, 23, 11, 19, 23, 19, 12, 17, 20, 17, 23, 15,
    16, 13, 18, 18, 14, 11, 13, 18, 19, 21, 16, 22, 24, 18, 13, 13, 23, 14, 21, 18,
    24, 36, 27, 28, 25, 14, 19, 21, 18, 16, 16, 21, 21, 21, 16, 11, 22, 16, 29, 23,
    17, 13, 20, 21, 14, 19, 17, 24, 25, 23, 23, 15, 12, 16, 14, 21, 15, 13, 12, 29,
    24, 15, 18, 16, 16, 24, 19, 22, 19, 23, 16, 28, 14, 19, 19, 12, 19, 22, 13, 21,
    17, 19, 16, 22, 26, 15, 15, 24, 19, 18, 16, 12, 18, 21, 18, 14, 10, 27, 12, 17,
    18, 17, 18, 18, 16, 28, 23, 21, 13, 29, 28, 15, 27, 20, 14, 26, 18, 19, 22, 14,
    15, 18, 13, 16, 19, 24, 30, 25, 12, 16, 27, 14, 11, 18, 13, 18, 13, 21, 24, 12,
    18, 29, 24, 27, 25, 23, 23, 14, 15, 26, 12, 20, 16, 14, 17, 10, 24, 26, 18, 19,
    15, 11, 16, 17, 21, 20, 26, 18, 24, 22, 23, 19, 22, 18, 19, 16, 17, 20, 15, 25,
    26, 24, 26, 16, 9, 20, 19, 20, 13, 17, 20, 24, 20, 14, 13, 15, 10, 15, 14, 24,
    20, 17, 16, 14, 23, 27, 21, 19, 25, 22, 24, 12, 17, 12, 18, 11, 21, 20, 19, 14,
    24, 16, 19, 19, 18, 16, 26, 22, 29, 28, 26, 21, 19, 22, 29, 19, 20, 22, 23, 17,
    22, 22, 30, 17, 20, 17, 24, 19, 19, 15, 17, 16, 22, 13, 20, 16, 23, 20, 20, 15,
    18, 32, 27, 24, 24, 16, 20, 16, 16, 23, 27, 24, 19, 24, 19, 29, 16, 21, 22, 12,
    21, 18, 21, 14, 19, 19, 14, 14, 18, 16, 23, 22, 14, 23, 26, 21, 16, 27, 19, 17,
    28, 21, 19, 24, 17, 28, 21, 14, 15, 24, 25, 20, 24, 23, 24, 23, 8, 23, 22, 20,
    22, 14, 12, 14, 20, 21, 21, 11, 24, 22, 22, 24, 17, 23, 25, 27, 16, 19, 22, 23,
    20, 21, 24, 24, 21, 16, 21, 22, 25, 18, 20, 16, 15, 21, 23, 19, 16, 10, 19, 21,
    19, 23, 13, 19, 10, 15, 22, 21, 30, 19, 15, 17, 27, 19, 14, 16, 17, 14, 18, 15,
    17, 18, 20, 26, 21, 21, 18, 22, 9, 14, 14, 19, 19, 15, 13, 19, 15, 13, 31, 19,
    20, 20, 25, 25, 22, 19, 19, 35, 22, 19, 17, 20, 22, 23, 20, 18, 17, 13, 22, 22,
    19, 20, 17, 18, 24, 14, 19, 21, 29, 16, 16, 22, 16, 12, 13, 18, 17, 25, 12, 17,
    16, 12, 15, 19, 21, 19, 9, 12, 18, 17, 22, 22, 17, 15, 16, 27, 21, 14, 21, 17,
    23, 15, 14, 22, 13, 14, 18, 25, 27, 19, 20, 19, 15, 18, 20, 21, 14, 18, 16, 18,
    20, 20, 18, 20, 20, 16, 21, 20, 27, 18, 6, 14, 14, 18, 17, 19, 19, 16, 17, 22,
    14, 12, 13, 17, 30, 21, 14, 23, 24, 20, 21, 15, 20, 15, 24, 18, 19, 21, 24, 12,
    21, 10, 19, 20, 23, 12, 21, 20, 18, 18, 18, 27, 17, 23, 19, 15, 17, 21, 18, 21,
    23, 17, 18, 16, 16, 16, 18, 13, 28, 20, 13, 20, 22, 11, 21, 22, 24, 25, 16, 14,
    15, 23, 34, 15, 20, 17, 21, 16, 19, 24, 20, 15, 24, 20, 29, 16, 14, 23, 21, 31,
    18, 21, 11, 20, 18, 21, 26, 16, 19, 27, 18, 17, 23, 18, 22, 18, 18, 25, 21, 25,
    17, 19, 19, 17, 18, 15, 13, 18, 25, 14, 16, 15, 18, 20, 22, 17, 20, 13, 17, 26,
    16, 22, 24, 24, 24, 14, 18, 16, 15, 27, 21, 21, 31, 18, 28, 22, 26, 14, 16, 18,
    22, 11, 17, 17, 21, 21, 20, 18, 25, 22, 26, 14, 18, 19, 22, 23, 24, 28, 17, 19,
    30, 16, 24, 18, 22, 19, 20, 22, 18, 21, 23, 19, 16, 19, 20, 28, 18, 19, 23, 22,
    22, 17, 23, 26, 21, 21, 16, 13, 27, 16, 15, 25, 19, 18, 21, 25, 19, 24, 19, 18,
    16, 18, 24, 26, 28, 31, 21, 36, 33, 47, 67, 57, 107, 137, 140, 168, 163, 182, 134, 130,
    108, 85, 54, 38, 37, 26, 19, 24, 29, 19, 22, 27, 27, 21, 18, 24, 14, 18, 27, 18,
    27, 26, 21, 27, 21, 22, 31, 20, 16, 21, 39, 14, 20, 23, 17, 19, 17, 25, 27, 19,
    24, 23, 18, 24, 27, 23, 23, 19, 23, 16, 20, 21, 31, 16, 18, 24, 21, 22, 22, 17,
    23, 26, 27, 20, 28, 22, 26, 17, 18, 21, 29, 29, 22, 20, 33, 25, 13, 24, 23, 18,
    20, 26, 23, 20, 20, 20, 33, 25, 27, 25, 26, 20, 25, 22, 24, 22, 19, 19, 22, 21,
    23, 29, 32, 34, 22, 23, 31, 28, 27, 27, 17, 19, 18, 25, 25, 24, 25, 28, 21, 18,
    22, 19, 28, 25, 15, 27, 19, 21, 17, 23, 20, 22, 25, 21, 28, 20, 31, 20, 26, 27,
    27, 26, 33, 35, 29, 31, 19, 22, 22, 24, 24, 21, 25, 23, 22, 22, 18, 29, 26, 22,
    23, 19, 25, 24, 15, 18, 23, 36, 26, 21, 28, 17, 19, 24, 27, 21, 28, 20, 22, 24,
    28, 27, 31, 33, 28, 15, 15, 28, 24, 36, 35, 21, 34, 23, 34, 24, 31, 19, 33, 38,
    28, 29, 27, 23, 25, 23, 29, 35, 33, 26, 25, 25, 31, 26, 30, 27, 25, 29, 23, 24,
    24, 25, 31, 31, 25, 17, 19, 19, 30, 24, 19, 29, 27, 25, 27, 26, 22, 27, 25, 30,
    34, 21, 22, 27, 26, 27, 32, 29, 22, 28, 31, 30, 25, 25, 23, 27, 23, 36, 26, 29,
    22, 18, 28, 26, 21, 20, 21, 24, 27, 40, 26, 14, 28, 35, 25, 31, 22, 31, 20, 41,
    26, 30, 32, 26, 39, 26, 23, 31, 26, 22, 24, 26, 19, 17, 19, 14, 26, 27, 26, 33,
    28, 36, 25, 24, 30, 28, 23, 24, 29, 25, 20, 31, 25, 26, 31, 31, 24, 19, 17, 37,
    27, 34, 23, 32, 41, 33, 36, 27, 31, 20, 22, 24, 36, 23, 29, 24, 24, 31, 22, 32,
    22, 30, 20, 24, 28, 21, 24, 34, 31, 35, 35, 23, 29, 42, 31, 27, 30, 29, 21, 26,
    36, 37, 29, 33, 24, 44, 36, 28, 26, 26, 30, 30, 33, 41, 21, 33, 25, 31, 29, 34,
    36, 28, 31, 27, 22, 39, 27, 44, 24, 25, 38, 29, 39, 26, 33, 22, 28, 35, 30, 35,
    31, 19, 35, 29, 42, 34, 33, 38, 38, 33, 42, 35, 26, 25, 33, 33, 32, 36, 24, 31,
    35, 28, 26, 33, 34, 36, 24, 30, 31, 30, 28, 31, 33, 24, 40, 38, 30, 45, 30, 40,
    30, 35, 41, 38, 29, 33, 35, 40, 30, 33, 42, 26, 30, 38, 27, 31, 26, 40, 44, 41,
    33, 32, 24, 33, 31, 29, 33, 37, 36, 37, 31, 32, 37, 45, 45, 40, 38, 43, 37, 39,
    39, 32, 27, 34, 34, 27, 28, 46, 33, 33, 39, 41, 37, 34, 32, 32, 31, 34, 38, 39,
    40, 38, 30, 39, 42, 39, 34, 35, 37, 34, 35, 48, 31, 32, 39, 40, 25, 50, 37, 39,
    24, 40, 49, 25, 39, 42, 41, 54, 29, 48, 36, 45, 34, 34, 40, 30, 42, 33, 42, 38,
    37, 44, 31, 38, 36, 34, 32, 36, 45, 50, 33, 45, 39, 39, 40, 37, 46, 41, 46, 37,
    34, 41, 29, 43, 37, 38, 42, 34, 30, 31, 27, 30, 27, 34, 28, 37, 13, 38, 29, 29,
    30, 26, 37, 20, 26, 19, 25, 27, 22, 29, 26, 24, 26, 26, 26, 21, 25, 26, 23, 19,
    26, 12, 31, 20, 18, 27, 17, 15, 19, 23, 24, 31, 29, 28, 23, 23, 21, 22, 36, 25,
    29, 28, 19, 28, 21, 30, 30, 30, 23, 31, 29, 35, 23, 29, 23, 21, 25, 15, 21, 22,
    28, 28, 21, 37, 26, 22, 26, 20, 25, 14, 22, 22, 31, 24, 38, 26, 21, 25, 33, 34,
    24, 20, 25, 21, 11, 22, 32, 21, 25, 35, 28, 23, 29, 14, 18, 23, 25, 18, 27, 27,
    26, 23, 23, 26, 20, 11, 22, 32, 16, 24, 22, 19, 11, 25, 28, 26, 19, 26, 16, 21,
    21, 18, 13, 23, 16, 18, 20, 20, 18, 19, 25, 16, 16, 26, 20, 15, 17, 19, 20, 20,
    10, 22, 22, 15, 20, 18, 18, 18, 26, 17, 22, 13, 17, 6, 28, 22, 19, 19, 17, 18,
    16, 14, 21, 16, 14, 15, 16, 15, 25, 11, 17, 16, 16, 13, 19, 16, 19, 12, 14, 10,
    14, 16, 14, 10, 12, 13, 13, 9, 15, 17, 16, 17, 7, 22, 17, 11, 17, 22, 8, 9,
    15, 12, 9, 15, 9, 8, 25, 9, 5, 15, 5, 12, 12, 8, 6, 12, 11, 12, 13, 11,
    15, 13, 12, 15, 12, 5, 5, 12, 4, 5, 7, 12, 9, 6, 15, 5, 3, 9, 10, 14,
    12, 10, 10, 14, 9, 10, 6, 6, 9, 6, 10, 11, 11, 13, 8, 11, 12, 6, 9, 7,
    19, 6, 6, 7, 8, 2, 9, 9, 10, 6, 9, 9, 9, 2, 5, 3, 6, 12, 5, 5,
    3, 7, 8, 9, 14, 9, 6, 10, 4, 9, 7, 8, 9, 9, 4, 8, 10, 6, 12, 9,
    7, 12, 10, 6, 5, 10, 9, 4, 6, 9, 7, 3, 9, 13, 3, 9, 6, 5, 10, 7,
    4, 12, 9, 6, 9, 12, 7, 5, 8, 14, 7, 5, 5, 10, 4, 3, 5, 9, 9, 9,
    7, 7, 7, 4, 5, 5, 12, 5, 8, 10, 7, 4, 6, 13, 6, 8, 8, 5, 4, 4,
    7, 13, 9, 10, 7, 4, 7, 4, 5, 4, 7, 6, 7, 7, 6, 6, 5, 10, 7, 7,
    8, 8, 5, 10, 4, 7, 7, 5, 12, 3, 9, 4, 5, 10, 8, 16, 4, 3, 8, 6,
    9, 6, 9, 7, 9, 6, 6, 6, 7, 5, 7, 6, 7, 6, 10, 5, 6, 5, 12, 6,
    15, 6, 11, 6, 6, 5, 6, 9, 9, 6, 7, 13, 5, 13, 8, 8, 10, 13, 9, 10,
    10, 17, 17, 14, 16, 18, 26, 18, 28, 37, 39, 26, 56, 62, 93, 106, 152, 237, 367, 592,
    741, 1045, 1273, 1307, 1192, 959, 726, 429, 221, 99, 44, 14, 4, 4, 7, 3, 6, 3, 5, 3,
    3, 9, 1, 11, 8, 7, 3, 6, 4, 1, 2, 2, 4, 3, 6, 2, 3, 3, 2, 4,
    4, 5, 8, 4, 9, 3, 5, 4, 3, 5, 4, 4, 3, 4, 2, 3, 1, 6, 2, 5,
    4, 2, 7, 2, 3, 4, 2, 1, 5, 6, 7, 4, 4, 3, 4, 4, 2, 6, 3, 11,
    2, 0, 1, 1, 3, 7, 6, 6, 4, 5, 4, 7, 3, 2, 6, 6, 2, 4, 3, 2,
    5, 3, 2, 2, 2, 3, 1, 2, 3, 4, 3, 3, 4, 2, 4, 3, 2, 1, 3, 3,
    5, 3, 6, 4, 1, 2, 2, 4, 4, 5, 6, 2, 4, 3, 6, 6, 6, 3, 3, 9,
    3, 1, 5, 5, 4, 4, 4, 9, 6, 3, 7, 4, 9, 7, 8, 1, 4, 4, 4, 4,
    3, 3, 1, 7, 0, 1, 1, 6, 2, 6, 6, 3, 3, 3, 5, 3, 1, 2, 2, 4,
    1, 4, 2, 5, 1, 4, 2, 2, 2, 4, 7, 1, 3, 2, 1, 5, 2, 2, 2, 4,
    3, 4, 4, 4, 4, 3, 3, 7, 3, 4, 4, 1, 3, 1, 0, 1, 4, 5, 4, 2,
    2, 4, 6, 5, 5, 5, 6, 3, 4, 1, 2, 4, 0, 1, 3, 3, 2, 4, 5, 1,
    0, 1, 1, 4, 4, 2, 2, 4, 1, 0, 1, 3, 3, 0, 1, 2, 1, 2, 1, 0,
    1, 1, 2, 5, 3, 3, 7, 3, 3, 4, 3, 6, 3, 3, 2, 2, 2, 2, 1, 3,
    1, 1, 6, 2, 2, 2, 4, 0, 1, 5, 5, 1, 3, 2, 5, 3, 7, 2, 2, 1,
    4, 2, 5, 2, 1, 2, 1, 2, 4, 3, 1, 3, 2, 4, 4, 6, 2, 1, 2, 7,
    4, 4, 2, 2, 1, 4, 3, 0, 1, 2, 2, 3, 1, 3, 3, 4, 2, 1, 4, 3,
    4, 3, 2, 4, 2, 2, 3, 2, 2, 4, 6, 3, 2, 4, 1, 4, 6, 3, 3, 4,
    2, 4, 3, 5, 2, 1, 0, 1, 1, 2, 5, 4, 3, 3, 1, 1, 2, 5, 2, 0,
    1, 3, 4, 2, 2, 4, 6, 3, 3, 3, 3, 6, 7, 3, 2, 2, 5, 3, 1, 0,
    1, 1, 6, 3, 1, 5, 4, 5, 3, 3, 5, 2, 1, 2, 2, 4, 3, 1, 4, 3,
    2, 4, 4, 1, 4, 2, 4, 2, 4, 2, 3, 3, 5, 5, 3, 2, 2, 2, 4, 2,
    3, 4, 2, 4, 0, 1, 1, 2, 5, 4, 4, 3, 4, 2, 1, 3, 2, 0, 1, 3,
    3, 2, 2, 3, 3, 2, 4, 2, 1, 5, 1, 3, 4, 1, 2, 2, 2, 0, 1, 1,
    6, 5, 4, 6, 3, 3, 2, 5, 4, 0, 1, 3, 2, 0, 1, 2, 1, 4, 2, 2,
    2, 0, 1, 2, 1, 1, 6, 3, 5, 3, 4, 0, 1, 1, 4, 2, 2, 3, 3, 6,
    2, 3, 2, 5, 2, 3, 3, 3, 5, 3, 2, 2, 2, 1, 2, 1, 1, 3, 2, 2,
    2, 1, 2, 4, 3, 1, 5, 2, 3, 2, 2, 1, 3, 4, 1, 4, 0, 1, 5, 3,
    2, 7, 1, 4, 3, 1, 2, 0, 1, 1, 3, 3, 5, 2, 4, 4, 2, 0, 1, 4,
    3, 4, 2, 2, 5, 4, 2, 0, 1, 3, 3, 1, 0, 1, 2, 6, 6, 6, 4, 1,
    3, 2, 4, 6, 4, 1, 3, 3, 2, 0, 1, 5, 2, 3, 1, 1, 1, 1, 1, 3,
    3, 1, 1, 4, 1, 8, 2, 2, 2, 2, 0, 2, 5, 1, 0, 1, 2, 3, 1, 4,
    3, 1, 3, 1, 1, 1, 1, 3, 3, 1, 2, 1, 2, 4, 2, 4, 2, 2, 1, 1,
    2, 2, 4, 2, 4, 4, 0, 1, 1, 1, 3, 1, 1, 0, 1, 2, 1, 3, 3, 1,
    2, 3, 1, 4, 2, 0, 1, 3, 1, 3, 4, 1, 1, 3, 5, 2, 2, 1, 1, 2,
    2, 3, 4, 4, 0, 1, 2, 2, 3, 2, 4, 3, 2, 1, 2, 5, 4, 6, 2, 3,
    4, 1, 7, 4, 1, 3, 3, 3, 1, 7, 3, 1, 2, 1, 0, 1, 2, 3, 3, 2,
    2, 0, 2, 2, 2, 0, 2, 3, 1, 3, 2, 1, 3, 1, 2, 4, 0, 1, 4, 0,
    1, 1, 2, 3, 0, 1, 1, 2, 4, 3, 3, 5, 0, 1, 1, 2, 5, 1, 3, 2,
    0, 1, 1, 0, 1, 3, 1, 3, 0, 1, 4, 2, 3, 4, 0, 2, 2, 2, 3, 1,
    4, 3, 3, 3, 2, 2, 0, 1, 3, 0, 1, 1, 6, 0, 1, 1, 1, 2, 2, 1,
    0, 1, 2, 2, 0, 1, 3, 0, 1, 1, 1, 3, 1, 2, 1, 2, 4, 2, 1, 3,
    5, 4, 0, 1, 3, 1, 4, 4, 2, 1, 3, 3, 0, 1, 3, 1, 1, 3, 2, 1,
    2, 6, 3, 1, 3, 2, 3, 1, 1, 1, 2, 2, 1, 0, 2, 3, 1, 2, 0, 4,
    3, 3, 0, 1, 2, 1, 0, 3, 2, 3, 1, 0, 1, 2, 6, 0, 1, 1, 1, 3,
    1, 1, 0, 3, 4, 1, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 2, 1,
    2, 2, 1, 3, 3, 2, 2, 1, 2, 0, 2, 4, 3, 2, 1, 1, 1, 0, 1, 2,
    2, 2, 2, 2, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 4, 1,
    1, 0, 1, 1, 1, 2, 2, 2, 1, 1, 2, 1, 2, 0, 1, 2, 2, 1, 1, 0,
    1, 3, 0, 1, 1, 1, 2, 0, 1, 1, 0, 4, 1, 0, 1, 1, 1, 0, 1, 2,
    0, 3, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 2, 0, 1, 3, 1, 0, 1,
    1, 0, 2, 1, 0, 3, 2, 0, 1, 1, 1, 0, 5, 1, 1, 1, 1, 1, 0, 2,
    3, 1, 0, 2, 1, 1, 1, 1, 0, 2, 2, 2, 0, 2, 1, 0, 1, 2, 1, 1,
    1, 0, 2, 1, 0, 1, 2, 0, 1, 1, 0, 2, 3, 0, 1, 2, 0, 4, 1, 2,
    0, 1, 1, 1, 0, 2, 1, 0, 4, 3, 0, 1, 1, 1, 0, 2, 3, 1, 1, 2,
    2, 1, 3, 5, 6, 4, 4, 6, 3, 4, 3, 8, 6, 3, 2, 0, 1, 1, 0, 7,
    2, 1, 2, 0, 2, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 3, 1, 0, 2, 2,
    0, 4, 1, 0, 2, 1, 1, 0, 3, 1, 1, 1, 0, 4, 1, 1, 1, 0, 4, 1,
    0, 9, 1, 0, 1, 3, 0, 1, 2, 0, 2, 1, 0, 2, 2, 2, 0, 2, 1, 2,
    0, 1, 1, 1, 0, 3, 4, 0, 1, 1, 1, 1, 1, 0, 2, 1, 0, 3, 1, 0,
    1, 1, 1, 0, 1, 1, 0, 2, 1, 0, 4, 1, 1, 1, 1, 0, 1, 1, 2, 1,
    4, 0, 1, 2, 1, 2, 2, 1, 2, 1, 2, 4, 4, 10, 8, 10, 18, 12, 19, 21,
    20, 14, 18, 19, 5, 2, 6, 1, 1, 1, 0, 1, 1, 0, 8, 2, 0, 1, 1, 1,
    0, 2, 1, 1, 0, 2, 1, 0, 7, 1, 0, 3, 1, 0, 3, 1, 0, 1, 1, 0,
    3, 1, 0, 1, 1, 0, 4, 1, 0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1,
    0, 2, 1, 0, 4, 1, 0, 6, 1, 1, 1, 0, 5, 2, 0, 7, 1, 0, 3, 1,
    0, 4, 1, 1, 1, 1, 0, 3, 1, 0, 5, 2, 0, 1, 1, 1, 1, 3, 0, 7,
    1, 0, 2, 1, 0, 8, 1, 1, 0, 12, 1, 1, 0, 11, 1, 0, 8, 1, 0, 16,
    1, 0, 4, 1, 0, 2, 2, 2, 0, 5, 1, 0, 7, 2, 0, 16, 1, 0, 3, 1,
    0, 6, 1, 0, 1, 2, 0, 2, 1, 0, 1, 1, 0, 13, 1, 0, 1, 1, 0, 2,
    1, 0, 10, 2, 0, 2, 1, 2, 1, 0, 8, 1, 0, 15, 1, 1, 0, 1, 1, 0,
    14, 1, 0, 5, 1, 1, 0, 2, 1, 0, 19, 1, 0, 1, 2, 0, 1, 1, 0, 2,
    1, 1, 0, 3, 2, 0, 39, 2, 0, 24, 1, 0, 8, 1, 0, 7, 1, 0, 9, 1,
    1, 0, 7, 1, 0, 11, 1, 0, 31, 1, 0, 2, 1, 0, 7, 1, 0, 6, 1, 0,
    4, 1, 1, 0, 3, 1, 0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 1, 2, 1, 1,
    1, 2, 1, 3, 0, 1, 2, 1, 2, 1, 0, 13, 1, 0, 8, 1, 0, 5, 1, 0,
    31, 1, 0, 28, 1, 0, 6, 1, 0, 2, 1, 0, 18, 1, 0, 8, 1, 0, 2, 1,
    0, 3, 1, 0, 16, 1, 0, 16, 1, 0, 6, 1, 0, 93, 1, 0, 2, 1, 0, 5,
    1, 0, 2, 1, 0, 26, 1, 0, 31, 1, 0, 92, 1, 0, 3, 1, 0, 29, 1, 1,
    0, 5, 1, 0, 36, 1, 0, 10, 1, 0, 1, 1, 0, 10, 1, 0, 51, 1, 0, 42,
    1, 0, 12, 1, 0, 36, 1, 0, 52, 1, 0, 21, 1, 0, 53, 1, 0, 67, 1, 0,
    31, 1, 0, 76, 1, 0, 115, 1, 0, 51, 1, 0, 23, 1, 0, 8, 1, 0, 22, 1,
    0, 10, 1, 0, 78, 1, 0, 174, 1, 0, 35, 1, 0, 16, 1, 0, 6, 1, 0, 104,
    1, 0, 66, 1, 0, 109, 1, 0, 19, 1, 0, 15, 1, 0, 251, 1, 0, 9, 1, 0,
    26, 1, 0, 105, 1, 0, 17, 1, 0, 32, 1, 0, 37, 1, 0, 88, 1, 0, 8, 1,
    0, 333, 1, 0, 25, 1, 0, 48, 1, 0, 111, 1, 0, 156, 1, 0, 3, 1, 0, 192,
    1, 0, 13, 1, 0, 169, 1, 0, 65, 1, 0, 42, 1, 0, 60, 1, 0, 114, 1, 0,
    127, 1, 0, 44, 1, 0, 17, 1, 0, 32, 1, 0, 35, 1, 0, 200, 1, 0, 56, 1,
    0, 61, 1, 0, 478, 1, 0, 142, 1, 0, 25, 1, 0, 536, 1, 0, 304, 1, 0, 61,
    1, 0, 16, 1, 0, 104, 1, 0, 492, 1, 0, 207, 1, 0, 114, 1, 0, 123, 1, 0,
    412, 1, 0, 7, 1, 0, 658, 1, 0, 33, 1, 0, 507, 1, 0, 238, 1, 0, 14, 1,
    0, 311, 1, 0, 109  };
  assert( test_17_chan_cnts.size() == 7145 );
  const vector<uint8_t> test_17_packed{
    233, 27, 0, 0, 0, 64, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 17, 85, 81, 85, 85, 85, 69, 68, 68, 85, 85, 69, 85, 69, 21, 5, 21, 65, 5, 4, 17, 4,
    1, 21, 81, 85, 69, 85, 85, 85, 85, 1, 0, 1, 1, 85, 1, 64, 69, 4, 0, 64, 0, 0, 4, 0, 0, 17, 80, 0, 64, 64, 1, 0, 80, 85, 1, 0, 64, 4, 21, 0, 4, 0, 84, 85, 85, 1, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 84, 85, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 1, 0, 0, 0, 0, 0, 0, 0, 85, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 84, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 84, 85, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 85, 85, 85, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 85, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 4, 1, 0, 0, 1, 0, 0, 1, 16, 0, 1, 0, 4, 0, 0, 29, 1, 0, 1, 1, 2, 0, 1, 2, 3,
    1, 3, 8, 23, 154, 1, 162, 3, 207, 3, 197, 3, 164, 3, 142, 3, 163, 3, 145, 3, 175, 3, 117, 3, 101, 3, 90, 3, 104, 3, 106, 3, 127, 3, 104, 3, 65, 3, 136, 3, 100, 3, 32, 3, 0, 3, 83, 3, 63, 3,
    58, 3, 241, 2, 63, 3, 68, 3, 255, 2, 47, 3, 64, 3, 18, 3, 9, 3, 29, 3, 16, 3, 15, 3, 3, 3, 15, 3, 9, 3, 209, 2, 21, 3, 211, 2, 182, 2, 12, 3, 217, 2, 250, 2, 229, 2, 220, 2, 222, 2,
    167, 2, 212, 2, 200, 2, 239, 2, 202, 2, 194, 2, 182, 2, 238, 2, 204, 2, 140, 2, 189, 2, 195, 2, 175, 2, 207, 2, 131, 2, 200, 2, 201, 2, 183, 2, 238, 2, 166, 2, 214, 2, 194, 2, 171, 2, 191, 2, 232, 2,
    233, 2, 251, 2, 10, 3, 123, 3, 102, 3, 64, 4, 166, 4, 1, 5, 253, 4, 252, 4, 207, 3, 68, 3, 184, 2, 161, 2, 146, 2, 158, 2, 162, 2, 160, 2, 163, 2, 170, 2, 156, 2, 193, 2, 82, 3, 214, 3, 24, 4,
    68, 4, 214, 3, 40, 3, 66, 3, 33, 3, 194, 2, 149, 2, 129, 2, 112, 2, 82, 2, 112, 2, 126, 2, 149, 2, 131, 2, 143, 2, 190, 2, 211, 2, 178, 2, 196, 2, 229, 2, 7, 3, 224, 2, 168, 2, 132, 2, 109, 2,
    142, 2, 91, 2, 104, 2, 124, 2, 104, 2, 109, 2, 115, 2, 113, 2, 119, 2, 150, 2, 123, 2, 143, 2, 123, 2, 125, 2, 199, 2, 233, 2, 229, 2, 214, 2, 159, 2, 163, 2, 201, 2, 176, 2, 153, 2, 144, 2, 158, 2,
    176, 2, 164, 2, 192, 2, 219, 2, 173, 2, 202, 2, 120, 2, 170, 2, 106, 2, 107, 2, 118, 2, 119, 2, 107, 2, 100, 2, 145, 2, 177, 2, 166, 2, 174, 2, 107, 2, 153, 2, 145, 2, 143, 2, 130, 2, 142, 2, 126, 2,
    146, 2, 129, 2, 173, 2, 199, 2, 153, 2, 170, 2, 136, 2, 161, 2, 214, 2, 167, 2, 185, 2, 203, 2, 181, 2, 120, 2, 132, 2, 154, 2, 185, 2, 186, 2, 131, 2, 128, 2, 160, 2, 116, 2, 156, 2, 203, 2, 147, 2,
    126, 2, 104, 2, 158, 2, 175, 2, 147, 2, 134, 2, 120, 2, 88, 2, 140, 2, 138, 2, 116, 2, 114, 2, 102, 2, 96, 2, 149, 2, 114, 2, 84, 2, 130, 2, 137, 2, 179, 2, 180, 2, 142, 2, 103, 2, 194, 2, 132, 2,
    202, 2, 233, 2, 252, 2, 211, 2, 150, 2, 130, 2, 124, 2, 160, 2, 126, 2, 148, 2, 119, 2, 133, 2, 117, 2, 133, 2, 92, 2, 146, 2, 130, 2, 91, 2, 148, 2, 137, 2, 148, 2, 175, 2, 141, 2, 124, 2, 128, 2,
    146, 2, 140, 2, 87, 2, 186, 2, 161, 2, 86, 2, 146, 2, 151, 2, 156, 2, 71, 2, 137, 2, 130, 2, 110, 2, 114, 2, 121, 2, 140, 2, 145, 2, 126, 2, 165, 2, 129, 2, 143, 2, 138, 2, 146, 2, 105, 2, 156, 2,
    126, 2, 156, 2, 240, 2, 191, 2, 143, 2, 110, 2, 145, 2, 135, 2, 135, 2, 152, 2, 122, 2, 169, 2, 109, 2, 138, 2, 152, 2, 137, 2, 128, 2, 124, 2, 112, 2, 129, 2, 110, 2, 76, 2, 137, 2, 110, 2, 101, 2,
    130, 2, 74, 2, 177, 2, 170, 2, 132, 2, 94, 2, 111, 2, 142, 2, 124, 2, 135, 2, 121, 2, 86, 2, 168, 2, 69, 2, 139, 2, 153, 2, 97, 2, 175, 2, 138, 2, 122, 2, 160, 2, 132, 2, 96, 2, 158, 2, 160, 2,
    109, 2, 122, 2, 142, 2, 122, 2, 93, 2, 140, 2, 117, 2, 128, 2, 122, 2, 142, 2, 144, 2, 117, 2, 88, 2, 135, 2, 129, 2, 95, 2, 164, 2, 121, 2, 157, 2, 127, 2, 132, 2, 137, 2, 86, 2, 142, 2, 170, 2,
    144, 2, 88, 2, 175, 2, 92, 2, 163, 2, 129, 2, 132, 2, 147, 2, 196, 2, 156, 2, 126, 2, 122, 2, 76, 2, 128, 2, 108, 2, 127, 2, 117, 2, 175, 2, 128, 2, 135, 2, 138, 2, 233, 2, 150, 2, 144, 2, 102, 2,
    181, 2, 113, 2, 112, 2, 113, 2, 192, 2, 155, 2, 167, 2, 153, 2, 95, 2, 92, 2, 121, 2, 106, 2, 182, 2, 15, 3, 178, 3, 105, 4, 154, 4, 9, 4, 86, 3, 144, 2, 105, 2, 123, 2, 106, 2, 120, 2, 102, 2,
    125, 2, 130, 2, 137, 2, 137, 2, 168, 2, 211, 2, 134, 2, 91, 2, 114, 2, 93, 2, 98, 2, 132, 2, 166, 2, 103, 2, 88, 2, 77, 2, 113, 2, 118, 2, 122, 2, 133, 2, 120, 2, 114, 2, 146, 2, 119, 2, 117, 2,
    97, 2, 117, 2, 133, 2, 99, 2, 113, 2, 120, 2, 126, 2, 136, 2, 129, 2, 123, 2, 106, 2, 174, 2, 107, 2, 136, 2, 132, 2, 139, 2, 140, 2, 149, 2, 152, 2, 139, 2, 129, 2, 11, 3, 151, 4, 25, 9, 232, 16,
    180, 26, 57, 29, 223, 22, 219, 13, 120, 7, 119, 5, 160, 4, 232, 3, 64, 3, 80, 2, 40, 2, 23, 2, 5, 2, 18, 2, 12, 2, 247, 1, 43, 2, 18, 2, 5, 2, 0, 2, 47, 2, 23, 2, 36, 2, 28, 2, 217, 1,
    229, 1, 5, 2, 34, 2, 34, 2, 48, 2, 36, 2, 58, 2, 31, 2, 29, 2, 238, 1, 33, 2, 226, 1, 10, 2, 211, 1, 26, 2, 18, 2, 23, 2, 3, 2, 21, 2, 207, 1, 2, 2, 251, 1, 12, 2, 11, 2, 7, 2,
    50, 2, 22, 2, 230, 1, 42, 2, 11, 2, 32, 2, 254, 1, 1, 2, 17, 2, 250, 1, 21, 2, 252, 1, 19, 2, 192, 2, 103, 3, 8, 4, 36, 4, 79, 3, 203, 2, 71, 2, 3, 2, 237, 1, 222, 1, 235, 1, 215, 1,
    219, 1, 229, 1, 52, 2, 105, 2, 233, 2, 19, 3, 70, 3, 181, 2, 55, 2, 33, 2, 214, 1, 248, 1, 223, 1, 222, 1, 224, 1, 251, 1, 219, 1, 205, 1, 199, 1, 247, 1, 211, 1, 208, 1, 219, 1, 198, 1, 250, 1,
    243, 1, 29, 2, 3, 2, 26, 2, 45, 2, 247, 1, 204, 1, 198, 1, 233, 1, 223, 1, 221, 1, 240, 1, 239, 1, 224, 1, 216, 1, 241, 1, 223, 1, 245, 1, 249, 1, 209, 1, 240, 1, 209, 1, 233, 1, 7, 2, 177, 2,
    77, 3, 211, 3, 101, 4, 140, 3, 166, 2, 77, 2, 240, 1, 177, 1, 196, 1, 179, 1, 218, 1, 161, 1, 226, 1, 200, 1, 228, 1, 210, 1, 188, 1, 182, 1, 201, 1, 209, 1, 190, 1, 210, 1, 200, 1, 204, 1, 185, 1,
    206, 1, 204, 1, 227, 1, 198, 1, 173, 1, 160, 1, 214, 1, 192, 1, 192, 1, 188, 1, 210, 1, 207, 1, 177, 1, 209, 1, 196, 1, 235, 1, 215, 1, 205, 1, 221, 1, 192, 1, 229, 1, 203, 1, 214, 1, 177, 1, 1, 2,
    237, 1, 164, 1, 197, 1, 190, 1, 196, 1, 248, 1, 99, 2, 44, 3, 37, 4, 0, 4, 115, 3, 148, 2, 25, 2, 202, 1, 239, 1, 240, 1, 230, 1, 6, 2, 36, 2, 172, 1, 254, 1, 183, 1, 208, 1, 177, 1, 181, 1,
    245, 1, 127, 2, 19, 4, 16, 7, 32, 10, 54, 11, 76, 9, 23, 6, 113, 3, 70, 2, 238, 1, 5, 2, 200, 1, 123, 1, 155, 1, 124, 1, 137, 1, 132, 1, 153, 1, 137, 1, 125, 1, 162, 1, 168, 1, 145, 1, 129, 1,
    115, 1, 142, 1, 124, 1, 125, 1, 149, 1, 131, 1, 147, 1, 155, 1, 132, 1, 119, 1, 145, 1, 181, 1, 115, 1, 120, 1, 111, 1, 118, 1, 114, 1, 159, 1, 122, 1, 136, 1, 108, 1, 148, 1, 133, 1, 157, 1, 113, 1,
    102, 1, 121, 1, 122, 1, 104, 1, 119, 1, 154, 1, 112, 1, 138, 1, 137, 1, 132, 1, 124, 1, 116, 1, 105, 1, 109, 1, 118, 1, 111, 1, 115, 1, 155, 1, 110, 1, 131, 1, 100, 1, 131, 1, 155, 1, 109, 1, 120, 1,
    152, 1, 130, 1, 113, 1, 98, 1, 116, 1, 135, 1, 143, 1, 134, 1, 88, 1, 139, 1, 142, 1, 145, 1, 129, 1, 147, 1, 111, 1, 112, 1, 109, 1, 110, 1, 111, 1, 128, 1, 117, 1, 127, 1, 108, 1, 103, 1, 124, 1,
    100, 1, 101, 1, 124, 1, 102, 1, 112, 1, 113, 1, 92, 1, 107, 1, 91, 1, 88, 1, 93, 1, 115, 1, 98, 1, 116, 1, 101, 1, 96, 1, 88, 1, 111, 1, 134, 1, 95, 1, 106, 1, 85, 1, 109, 1, 98, 1, 96, 1,
    84, 1, 97, 1, 67, 1, 91, 1, 118, 1, 110, 1, 122, 1, 97, 1, 142, 1, 111, 1, 86, 1, 116, 1, 115, 1, 73, 1, 106, 1, 69, 1, 117, 1, 109, 1, 100, 1, 93, 1, 75, 1, 100, 1, 179, 1, 65, 2, 119, 2,
    197, 2, 116, 2, 38, 2, 154, 1, 100, 1, 58, 1, 69, 1, 76, 1, 57, 1, 11, 1, 99, 1, 62, 1, 57, 1, 58, 1, 48, 1, 44, 1, 44, 1, 41, 1, 48, 1, 73, 1, 26, 1, 35, 1, 68, 1, 44, 1, 46, 1,
    58, 1, 98, 1, 58, 1, 43, 1, 45, 1, 29, 1, 23, 1, 45, 1, 70, 1, 43, 1, 71, 1, 28, 1, 38, 1, 29, 1, 62, 1, 34, 1, 33, 1, 34, 1, 31, 1, 23, 1, 29, 1, 43, 1, 44, 1, 36, 1, 71, 1,
    25, 1, 33, 1, 38, 1, 24, 1, 29, 1, 35, 1, 40, 1, 6, 1, 28, 1, 15, 1, 14, 1, 68, 1, 45, 1, 22, 1, 53, 1, 36, 1, 13, 1, 23, 1, 21, 1, 26, 1, 32, 1, 65, 1, 31, 1, 25, 1, 2, 1,
    20, 1, 28, 1, 4, 1, 32, 1, 34, 1, 43, 1, 41, 1, 34, 1, 12, 1, 30, 1, 6, 1, 14, 1, 99, 1, 107, 1, 92, 1, 63, 1, 56, 1, 246, 2, 1, 252, 28, 1, 28, 1, 13, 1, 23, 1, 16, 1, 245, 32,
    1, 44, 1, 7, 1, 55, 1, 95, 1, 246, 1, 235, 2, 8, 4, 139, 4, 56, 4, 246, 2, 217, 1, 59, 1, 14, 1, 15, 1, 2, 1, 245, 31, 1, 247, 2, 1, 253, 22, 1, 254, 14, 1, 249, 9, 1, 1, 1, 22, 1,
    4, 1, 2, 1, 18, 1, 7, 1, 20, 1, 27, 1, 24, 1, 17, 1, 248, 4, 1, 54, 1, 26, 1, 32, 1, 41, 1, 11, 1, 13, 1, 252, 5, 1, 27, 1, 6, 1, 17, 1, 251, 9, 1, 7, 1, 247, 244, 5, 1, 2,
    1, 10, 1, 237, 30, 1, 254, 255, 32, 1, 12, 1, 11, 1, 253, 246, 255, 4, 1, 254, 255, 6, 1, 233, 14, 1, 226, 246, 14, 1, 237, 246, 15, 1, 228, 255, 249, 1, 1, 8, 1, 27, 1, 226, 16, 1, 244, 8, 1, 26,
    1, 37, 1, 45, 1, 31, 1, 25, 1, 27, 1, 23, 1, 234, 25, 1, 73, 1, 91, 1, 147, 1, 68, 2, 53, 3, 144, 4, 153, 6, 126, 7, 153, 7, 7, 6, 248, 3, 190, 2, 241, 1, 101, 1, 65, 1, 24, 1, 11, 1,
    251, 242, 255, 209, 233, 238, 229, 10, 1, 241, 242, 238, 7, 1, 231, 217, 235, 4, 1, 16, 1, 1, 1, 14, 1, 2, 1, 232, 244, 244, 218, 251, 234, 10, 1, 0, 1, 6, 1, 224, 1, 1, 235, 5, 1, 231, 239, 229, 230, 226,
    224, 243, 224, 236, 3, 1, 236, 253, 224, 196, 249, 239, 212, 253, 243, 3, 1, 232, 246, 247, 223, 235, 221, 227, 216, 245, 251, 49, 1, 254, 10, 1, 247, 218, 241, 9, 1, 8, 1, 248, 247, 238, 235, 231, 224, 220, 5, 1, 248, 240,
    218, 6, 1, 1, 1, 225, 239, 226, 223, 210, 243, 215, 251, 248, 20, 1, 49, 1, 124, 1, 151, 1, 132, 1, 80, 1, 5, 1, 225, 220, 250, 243, 233, 190, 209, 252, 242, 238, 3, 1, 236, 3, 1, 252, 240, 17, 1, 20, 1, 0,
    1, 238, 227, 227, 219, 206, 245, 4, 1, 205, 241, 242, 222, 239, 255, 242, 48, 1, 135, 1, 168, 2, 68, 5, 220, 9, 18, 16, 45, 19, 42, 18, 137, 12, 114, 6, 42, 3, 68, 1, 252, 167, 224, 204, 217, 213, 185, 220, 197, 215,
    219, 187, 198, 195, 205, 203, 194, 205, 190, 210, 189, 213, 182, 209, 189, 184, 200, 201, 191, 205, 190, 191, 207, 206, 204, 186, 202, 200, 183, 199, 185, 210, 193, 231, 228, 223, 221, 232, 248, 236, 229, 189, 174, 185, 191, 190, 184, 183, 202, 195,
    234, 217, 208, 208, 208, 186, 181, 202, 187, 207, 216, 213, 204, 233, 188, 222, 178, 180, 184, 193, 198, 175, 196, 205, 197, 186, 195, 200, 220, 195, 211, 199, 220, 180, 183, 200, 180, 179, 184, 174, 186, 184, 191, 190, 196, 188, 190, 189, 197, 204,
    210, 203, 211, 194, 192, 207, 204, 186, 218, 193, 199, 190, 168, 201, 184, 221, 163, 186, 196, 210, 205, 196, 198, 186, 189, 206, 202, 177, 171, 171, 193, 185, 186, 203, 206, 199, 185, 206, 203, 170, 194, 189, 198, 185, 196, 181, 208, 175, 194, 201,
    211, 178, 187, 190, 222, 189, 184, 202, 207, 189, 192, 186, 220, 198, 214, 173, 212, 194, 178, 228, 229, 219, 183, 200, 199, 220, 184, 188, 191, 187, 181, 199, 179, 186, 204, 213, 197, 188, 188, 203, 185, 186, 202, 181, 204, 160, 218, 222, 219, 179,
    201, 222, 185, 206, 209, 175, 194, 206, 187, 192, 206, 217, 196, 204, 184, 196, 176, 183, 216, 189, 218, 193, 224, 215, 198, 191, 232, 213, 206, 220, 200, 214, 219, 217, 195, 213, 209, 209, 232, 195, 227, 207, 195, 204, 177, 193, 201, 179, 183, 191,
    206, 181, 198, 185, 158, 195, 177, 176, 173, 167, 176, 173, 162, 174, 170, 171, 207, 184, 195, 201, 184, 173, 161, 197, 0, 1, 109, 1, 75, 2, 131, 3, 235, 4, 230, 4, 75, 4, 190, 2, 134, 1, 8, 1, 182, 160, 154, 161, 160, 167,
    163, 172, 159, 167, 169, 168, 150, 143, 163, 173, 163, 147, 137, 154, 152, 154, 147, 133, 160, 155, 158, 187, 176, 161, 166, 142, 156, 128, 148, 164, 162, 143, 145, 157, 162, 154, 148, 165, 154, 138, 145, 167, 166, 199, 243, 24, 1, 46, 1, 41,
    1, 252, 207, 178, 144, 147, 147, 128, 180, 147, 157, 158, 167, 191, 208, 215, 184, 213, 167, 143, 122, 132, 152, 117, 132, 143, 145, 133, 124, 140, 168, 226, 47, 1, 94, 1, 112, 1, 57, 1, 0, 1, 186, 142, 133, 126, 124, 99, 114, 131,
    134, 125, 101, 118, 115, 122, 122, 174, 180, 190, 201, 169, 159, 177, 205, 253, 252, 202, 185, 144, 110, 122, 115, 97, 108, 93, 122, 112, 112, 109, 141, 189, 53, 1, 211, 1, 131, 2, 248, 2, 245, 2, 244, 1, 58, 1, 208, 122, 103, 95,
    82, 90, 87, 100, 89, 96, 94, 84, 78, 94, 92, 104, 93, 99, 89, 93, 87, 90, 85, 99, 95, 90, 82, 113, 76, 84, 81, 83, 89, 89, 83, 91, 83, 90, 77, 84, 96, 94, 81, 99, 105, 92, 89, 81, 108, 86, 93, 83, 84, 85,
    87, 92, 76, 76, 88, 91, 82, 83, 74, 73, 84, 109, 102, 128, 144, 197, 162, 119, 101, 80, 99, 111, 137, 186, 18, 1, 60, 1, 57, 1, 18, 1, 213, 125, 112, 105, 121, 156, 185, 213, 204, 154, 126, 97, 84, 71, 65, 72, 92, 73,
    82, 72, 68, 57, 78, 71, 67, 68, 81, 62, 76, 74, 62, 71, 67, 72, 69, 82, 85, 65, 79, 70, 76, 72, 71, 107, 121, 181, 76, 1, 7, 2, 79, 2, 151, 2, 247, 1, 72, 1, 178, 92, 73, 84, 78, 72, 53, 73, 58, 66,
    71, 63, 51, 79, 59, 56, 58, 64, 61, 60, 44, 76, 62, 80, 80, 79, 69, 83, 70, 54, 64, 67, 73, 73, 50, 68, 75, 65, 58, 71, 61, 70, 72, 68, 66, 67, 49, 63, 64, 86, 57, 65, 63, 66, 62, 60, 69, 77, 61, 69,
    73, 77, 106, 94, 120, 115, 87, 86, 68, 54, 70, 65, 74, 56, 58, 53, 78, 57, 59, 61, 90, 76, 94, 84, 153, 147, 179, 173, 124, 106, 87, 106, 97, 139, 180, 88, 1, 202, 2, 93, 5, 89, 9, 119, 13, 91, 15, 173, 13, 102,
    9, 211, 4, 30, 2, 200, 76, 62, 54, 50, 52, 52, 53, 55, 62, 53, 57, 44, 49, 47, 52, 54, 41, 47, 52, 51, 44, 40, 42, 52, 47, 52, 44, 56, 51, 48, 48, 49, 43, 43, 46, 44, 55, 47, 33, 36, 53, 46, 46, 48,
    38, 50, 50, 44, 52, 41, 38, 58, 50, 58, 42, 39, 44, 43, 61, 54, 53, 71, 50, 65, 42, 59, 61, 45, 55, 65, 61, 58, 50, 48, 55, 53, 55, 64, 61, 62, 65, 56, 44, 59, 70, 56, 71, 60, 61, 94, 92, 106, 81, 73,
    64, 62, 60, 69, 104, 135, 10, 1, 209, 1, 105, 2, 239, 2, 164, 2, 55, 2, 165, 1, 91, 1, 17, 2, 147, 3, 221, 5, 237, 7, 222, 8, 147, 7, 250, 4, 171, 2, 33, 1, 115, 63, 44, 34, 44, 37, 33, 37, 50, 39,
    39, 35, 44, 31, 50, 41, 30, 33, 40, 45, 36, 46, 46, 50, 62, 45, 47, 53, 39, 31, 40, 42, 53, 50, 58, 57, 69, 52, 29, 31, 44, 36, 37, 41, 39, 34, 31, 30, 37, 37, 42, 36, 35, 34, 27, 33, 43, 32, 23, 41,
    36, 38, 39, 39, 40, 38, 36, 23, 34, 37, 30, 29, 31, 24, 32, 30, 41, 40, 33, 34, 37, 34, 32, 34, 33, 38, 37, 28, 44, 35, 42, 48, 39, 39, 31, 43, 38, 32, 31, 42, 43, 36, 30, 46, 32, 32, 34, 40, 35, 44,
    36, 37, 47, 42, 38, 41, 41, 32, 29, 34, 34, 33, 47, 49, 59, 59, 60, 51, 47, 49, 43, 25, 34, 37, 39, 30, 38, 35, 24, 36, 51, 41, 54, 33, 43, 26, 19, 34, 31, 39, 27, 25, 21, 35, 32, 29, 42, 39, 32, 36,
    38, 33, 34, 41, 37, 48, 37, 33, 41, 35, 45, 34, 45, 31, 33, 33, 29, 35, 20, 33, 38, 39, 26, 33, 41, 45, 39, 43, 55, 46, 53, 64, 65, 60, 35, 33, 35, 31, 34, 39, 33, 34, 37, 31, 41, 32, 29, 42, 31, 38,
    32, 43, 31, 41, 55, 48, 61, 86, 88, 92, 85, 70, 39, 54, 40, 35, 39, 36, 31, 34, 28, 35, 37, 30, 32, 35, 37, 34, 31, 45, 46, 27, 42, 34, 42, 34, 41, 57, 69, 85, 72, 75, 70, 68, 60, 44, 47, 43, 34, 42,
    38, 38, 42, 39, 32, 34, 30, 39, 38, 35, 46, 30, 36, 38, 31, 28, 33, 29, 34, 28, 48, 54, 70, 70, 75, 68, 47, 43, 40, 44, 45, 38, 30, 24, 36, 40, 33, 27, 38, 37, 49, 39, 34, 36, 45, 48, 52, 46, 43, 41,
    29, 46, 28, 32, 32, 38, 32, 32, 33, 24, 32, 35, 24, 30, 25, 30, 34, 34, 32, 35, 35, 36, 29, 36, 35, 27, 35, 29, 28, 20, 27, 39, 33, 26, 26, 23, 37, 47, 43, 34, 28, 35, 35, 36, 38, 27, 24, 25, 34, 30,
    29, 28, 38, 36, 39, 40, 35, 38, 40, 41, 46, 54, 49, 30, 44, 44, 42, 34, 31, 31, 28, 40, 45, 35, 30, 33, 38, 22, 43, 36, 40, 32, 28, 42, 50, 33, 36, 36, 28, 28, 36, 34, 48, 41, 38, 35, 26, 28, 47, 44,
    35, 23, 46, 37, 22, 26, 24, 30, 24, 41, 30, 32, 35, 34, 32, 31, 38, 28, 32, 37, 25, 30, 33, 35, 24, 25, 26, 31, 41, 25, 33, 42, 34, 37, 38, 23, 37, 29, 30, 36, 33, 30, 35, 31, 32, 28, 36, 37, 47, 28,
    29, 31, 25, 37, 27, 39, 39, 34, 40, 40, 31, 40, 27, 39, 30, 36, 28, 31, 32, 35, 35, 28, 32, 34, 34, 43, 33, 40, 28, 25, 37, 34, 34, 26, 24, 40, 29, 34, 31, 29, 30, 23, 37, 30, 31, 33, 42, 33, 43, 44,
    32, 23, 50, 36, 34, 27, 23, 43, 35, 31, 21, 37, 32, 35, 45, 24, 27, 38, 36, 29, 32, 43, 40, 34, 29, 37, 34, 35, 28, 34, 41, 31, 33, 40, 31, 47, 42, 33, 25, 32, 34, 31, 32, 44, 26, 29, 39, 42, 57, 79,
    59, 70, 85, 83, 83, 70, 42, 52, 37, 54, 32, 40, 41, 42, 30, 33, 37, 38, 36, 34, 36, 27, 34, 39, 39, 25, 35, 28, 28, 37, 30, 40, 33, 37, 30, 25, 33, 27, 41, 31, 36, 33, 31, 31, 34, 35, 19, 32, 36, 34,
    35, 35, 45, 36, 40, 18, 37, 32, 22, 42, 44, 42, 27, 44, 40, 23, 29, 31, 26, 28, 38, 22, 34, 34, 36, 31, 29, 39, 34, 35, 29, 57, 49, 52, 47, 43, 33, 58, 28, 42, 33, 42, 23, 28, 25, 34, 28, 31, 34, 42,
    37, 27, 39, 28, 30, 25, 27, 27, 31, 31, 30, 27, 22, 24, 41, 30, 32, 25, 33, 44, 34, 35, 38, 36, 30, 33, 33, 32, 34, 30, 45, 28, 36, 36, 25, 35, 28, 23, 29, 37, 36, 32, 44, 31, 30, 21, 30, 37, 34, 27,
    39, 27, 18, 25, 31, 37, 26, 29, 32, 28, 39, 30, 24, 23, 35, 28, 30, 25, 34, 28, 26, 33, 23, 22, 39, 22, 29, 37, 41, 38, 33, 29, 36, 31, 38, 23, 29, 21, 41, 45, 38, 26, 34, 36, 27, 28, 31, 32, 35, 34,
    20, 32, 22, 26, 39, 34, 42, 29, 30, 33, 30, 41, 25, 25, 33, 23, 32, 28, 21, 25, 37, 35, 38, 34, 37, 30, 35, 44, 36, 37, 26, 35, 34, 32, 30, 32, 36, 36, 37, 32, 30, 36, 33, 28, 35, 46, 28, 28, 29, 39,
    35, 33, 28, 27, 34, 30, 29, 41, 27, 34, 33, 35, 38, 42, 34, 36, 24, 30, 33, 29, 37, 35, 30, 26, 23, 29, 37, 25, 26, 23, 33, 19, 35, 31, 24, 32, 30, 36, 29, 28, 25, 33, 22, 23, 33, 32, 29, 33, 27, 37,
    28, 26, 27, 19, 27, 31, 30, 40, 38, 18, 34, 25, 26, 30, 31, 29, 29, 29, 31, 29, 25, 21, 21, 32, 37, 27, 29, 27, 39, 34, 28, 38, 26, 36, 20, 30, 21, 22, 30, 24, 25, 23, 17, 32, 25, 33, 24, 39, 20, 26,
    18, 29, 24, 30, 30, 29, 22, 21, 15, 17, 25, 21, 28, 17, 25, 23, 22, 18, 24, 26, 19, 39, 26, 33, 20, 26, 28, 28, 26, 27, 36, 32, 28, 23, 24, 14, 27, 26, 27, 25, 33, 32, 28, 20, 18, 34, 19, 20, 25, 30,
    26, 26, 23, 21, 27, 17, 25, 23, 19, 25, 21, 21, 16, 27, 28, 19, 24, 21, 33, 31, 32, 19, 26, 30, 28, 17, 36, 33, 32, 22, 33, 26, 52, 57, 89, 101, 126, 149, 150, 143, 155, 132, 90, 57, 50, 31, 21, 28, 23, 26,
    19, 21, 30, 20, 16, 21, 19, 27, 25, 18, 23, 20, 19, 14, 19, 23, 25, 21, 27, 14, 22, 20, 24, 19, 18, 24, 30, 14, 19, 21, 28, 22, 23, 16, 11, 19, 21, 23, 9, 22, 21, 18, 25, 21, 19, 26, 26, 19, 16, 16,
    19, 17, 22, 19, 19, 24, 24, 35, 52, 75, 85, 87, 88, 90, 50, 46, 29, 26, 25, 37, 24, 42, 66, 54, 61, 38, 34, 27, 29, 18, 16, 17, 16, 16, 18, 14, 19, 12, 14, 15, 24, 20, 22, 27, 35, 42, 48, 48, 39, 34,
    31, 17, 16, 14, 22, 13, 24, 17, 12, 20, 12, 13, 23, 17, 19, 12, 12, 19, 14, 14, 19, 20, 14, 27, 17, 18, 23, 26, 26, 14, 20, 21, 15, 18, 28, 9, 14, 18, 15, 18, 12, 23, 26, 22, 14, 22, 20, 17, 15, 13,
    25, 17, 19, 17, 17, 21, 17, 11, 26, 11, 23, 15, 18, 11, 12, 10, 21, 15, 25, 16, 20, 14, 17, 19, 18, 20, 22, 18, 17, 19, 21, 15, 21, 20, 22, 19, 30, 28, 30, 30, 25, 14, 19, 18, 19, 9, 15, 18, 16, 17,
    19, 18, 21, 24, 14, 17, 18, 13, 24, 19, 22, 17, 16, 21, 16, 11, 15, 20, 22, 16, 20, 19, 15, 21, 15, 18, 16, 22, 17, 23, 27, 24, 56, 62, 78, 61, 60, 41, 32, 28, 24, 36, 34, 37, 32, 58, 83, 138, 200, 18,
    1, 52, 1, 4, 1, 250, 154, 127, 95, 98, 99, 105, 144, 110, 87, 62, 57, 23, 24, 19, 14, 18, 12, 27, 18, 19, 24, 21, 16, 21, 26, 26, 20, 20, 20, 17, 24, 20, 18, 15, 11, 14, 20, 23, 17, 13, 32, 28, 25, 23,
    30, 21, 16, 19, 16, 21, 21, 21, 18, 29, 27, 25, 32, 31, 31, 49, 62, 76, 115, 131, 144, 117, 110, 66, 40, 31, 40, 38, 46, 49, 36, 31, 23, 23, 24, 41, 53, 79, 109, 150, 174, 151, 117, 80, 44, 25, 22, 21, 20, 25,
    30, 23, 33, 36, 48, 72, 48, 55, 30, 33, 27, 18, 23, 16, 21, 21, 17, 23, 18, 18, 21, 12, 18, 23, 17, 13, 11, 18, 16, 14, 18, 17, 18, 13, 19, 18, 22, 17, 12, 15, 14, 18, 17, 19, 17, 20, 20, 16, 17, 22,
    20, 21, 16, 18, 19, 21, 15, 23, 17, 20, 29, 24, 34, 26, 26, 24, 22, 20, 23, 14, 23, 15, 18, 13, 15, 21, 7, 17, 22, 16, 13, 14, 23, 23, 27, 18, 30, 36, 16, 27, 27, 21, 20, 15, 24, 14, 26, 23, 17, 20,
    16, 22, 20, 21, 29, 23, 24, 21, 15, 14, 25, 19, 22, 16, 19, 22, 15, 15, 15, 18, 18, 20, 19, 22, 14, 23, 17, 15, 17, 14, 17, 18, 20, 13, 27, 25, 22, 23, 22, 21, 13, 24, 24, 17, 14, 23, 20, 24, 25, 31,
    16, 9, 20, 15, 9, 17, 17, 19, 21, 19, 17, 14, 17, 20, 17, 16, 14, 18, 16, 15, 16, 20, 20, 18, 24, 11, 19, 17, 16, 29, 19, 14, 29, 24, 18, 16, 19, 16, 17, 25, 16, 16, 19, 19, 20, 17, 19, 18, 14, 22,
    15, 18, 15, 20, 14, 15, 20, 12, 22, 11, 30, 22, 17, 9, 26, 19, 27, 17, 19, 17, 17, 23, 23, 18, 24, 20, 18, 19, 17, 14, 22, 23, 11, 19, 23, 19, 12, 17, 20, 17, 23, 15, 16, 13, 18, 18, 14, 11, 13, 18,
    19, 21, 16, 22, 24, 18, 13, 13, 23, 14, 21, 18, 24, 36, 27, 28, 25, 14, 19, 21, 18, 16, 16, 21, 21, 21, 16, 11, 22, 16, 29, 23, 17, 13, 20, 21, 14, 19, 17, 24, 25, 23, 23, 15, 12, 16, 14, 21, 15, 13,
    12, 29, 24, 15, 18, 16, 16, 24, 19, 22, 19, 23, 16, 28, 14, 19, 19, 12, 19, 22, 13, 21, 17, 19, 16, 22, 26, 15, 15, 24, 19, 18, 16, 12, 18, 21, 18, 14, 10, 27, 12, 17, 18, 17, 18, 18, 16, 28, 23, 21,
    13, 29, 28, 15, 27, 20, 14, 26, 18, 19, 22, 14, 15, 18, 13, 16, 19, 24, 30, 25, 12, 16, 27, 14, 11, 18, 13, 18, 13, 21, 24, 12, 18, 29, 24, 27, 25, 23, 23, 14, 15, 26, 12, 20, 16, 14, 17, 10, 24, 26,
    18, 19, 15, 11, 16, 17, 21, 20, 26, 18, 24, 22, 23, 19, 22, 18, 19, 16, 17, 20, 15, 25, 26, 24, 26, 16, 9, 20, 19, 20, 13, 17, 20, 24, 20, 14, 13, 15, 10, 15, 14, 24, 20, 17, 16, 14, 23, 27, 21, 19,
    25, 22, 24, 12, 17, 12, 18, 11, 21, 20, 19, 14, 24, 16, 19, 19, 18, 16, 26, 22, 29, 28, 26, 21, 19, 22, 29, 19, 20, 22, 23, 17, 22, 22, 30, 17, 20, 17, 24, 19, 19, 15, 17, 16, 22, 13, 20, 16, 23, 20,
    20, 15, 18, 32, 27, 24, 24, 16, 20, 16, 16, 23, 27, 24, 19, 24, 19, 29, 16, 21, 22, 12, 21, 18, 21, 14, 19, 19, 14, 14, 18, 16, 23, 22, 14, 23, 26, 21, 16, 27, 19, 17, 28, 21, 19, 24, 17, 28, 21, 14,
    15, 24, 25, 20, 24, 23, 24, 23, 8, 23, 22, 20, 22, 14, 12, 14, 20, 21, 21, 11, 24, 22, 22, 24, 17, 23, 25, 27, 16, 19, 22, 23, 20, 21, 24, 24, 21, 16, 21, 22, 25, 18, 20, 16, 15, 21, 23, 19, 16, 10,
    19, 21, 19, 23, 13, 19, 10, 15, 22, 21, 30, 19, 15, 17, 27, 19, 14, 16, 17, 14, 18, 15, 17, 18, 20, 26, 21, 21, 18, 22, 9, 14, 14, 19, 19, 15, 13, 19, 15, 13, 31, 19, 20, 20, 25, 25, 22, 19, 19, 35,
    22, 19, 17, 20, 22, 23, 20, 18, 17, 13, 22, 22, 19, 20, 17, 18, 24, 14, 19, 21, 29, 16, 16, 22, 16, 12, 13, 18, 17, 25, 12, 17, 16, 12, 15, 19, 21, 19, 9, 12, 18, 17, 22, 22, 17, 15, 16, 27, 21, 14,
    21, 17, 23, 15, 14, 22, 13, 14, 18, 25, 27, 19, 20, 19, 15, 18, 20, 21, 14, 18, 16, 18, 20, 20, 18, 20, 20, 16, 21, 20, 27, 18, 6, 14, 14, 18, 17, 19, 19, 16, 17, 22, 14, 12, 13, 17, 30, 21, 14, 23,
    24, 20, 21, 15, 20, 15, 24, 18, 19, 21, 24, 12, 21, 10, 19, 20, 23, 12, 21, 20, 18, 18, 18, 27, 17, 23, 19, 15, 17, 21, 18, 21, 23, 17, 18, 16, 16, 16, 18, 13, 28, 20, 13, 20, 22, 11, 21, 22, 24, 25,
    16, 14, 15, 23, 34, 15, 20, 17, 21, 16, 19, 24, 20, 15, 24, 20, 29, 16, 14, 23, 21, 31, 18, 21, 11, 20, 18, 21, 26, 16, 19, 27, 18, 17, 23, 18, 22, 18, 18, 25, 21, 25, 17, 19, 19, 17, 18, 15, 13, 18,
    25, 14, 16, 15, 18, 20, 22, 17, 20, 13, 17, 26, 16, 22, 24, 24, 24, 14, 18, 16, 15, 27, 21, 21, 31, 18, 28, 22, 26, 14, 16, 18, 22, 11, 17, 17, 21, 21, 20, 18, 25, 22, 26, 14, 18, 19, 22, 23, 24, 28,
    17, 19, 30, 16, 24, 18, 22, 19, 20, 22, 18, 21, 23, 19, 16, 19, 20, 28, 18, 19, 23, 22, 22, 17, 23, 26, 21, 21, 16, 13, 27, 16, 15, 25, 19, 18, 21, 25, 19, 24, 19, 18, 16, 18, 24, 26, 28, 31, 21, 36,
    33, 47, 67, 57, 107, 137, 140, 168, 163, 182, 134, 130, 108, 85, 54, 38, 37, 26, 19, 24, 29, 19, 22, 27, 27, 21, 18, 24, 14, 18, 27, 18, 27, 26, 21, 27, 21, 22, 31, 20, 16, 21, 39, 14, 20, 23, 17, 19, 17, 25,
    27, 19, 24, 23, 18, 24, 27, 23, 23, 19, 23, 16, 20, 21, 31, 16, 18, 24, 21, 22, 22, 17, 23, 26, 27, 20, 28, 22, 26, 17, 18, 21, 29, 29, 22, 20, 33, 25, 13, 24, 23, 18, 20, 26, 23, 20, 20, 20, 33, 25,
    27, 25, 26, 20, 25, 22, 24, 22, 19, 19, 22, 21, 23, 29, 32, 34, 22, 23, 31, 28, 27, 27, 17, 19, 18, 25, 25, 24, 25, 28, 21, 18, 22, 19, 28, 25, 15, 27, 19, 21, 17, 23, 20, 22, 25, 21, 28, 20, 31, 20,
    26, 27, 27, 26, 33, 35, 29, 31, 19, 22, 22, 24, 24, 21, 25, 23, 22, 22, 18, 29, 26, 22, 23, 19, 25, 24, 15, 18, 23, 36, 26, 21, 28, 17, 19, 24, 27, 21, 28, 20, 22, 24, 28, 27, 31, 33, 28, 15, 15, 28,
    24, 36, 35, 21, 34, 23, 34, 24, 31, 19, 33, 38, 28, 29, 27, 23, 25, 23, 29, 35, 33, 26, 25, 25, 31, 26, 30, 27, 25, 29, 23, 24, 24, 25, 31, 31, 25, 17, 19, 19, 30, 24, 19, 29, 27, 25, 27, 26, 22, 27,
    25, 30, 34, 21, 22, 27, 26, 27, 32, 29, 22, 28, 31, 30, 25, 25, 23, 27, 23, 36, 26, 29, 22, 18, 28, 26, 21, 20, 21, 24, 27, 40, 26, 14, 28, 35, 25, 31, 22, 31, 20, 41, 26, 30, 32, 26, 39, 26, 23, 31,
    26, 22, 24, 26, 19, 17, 19, 14, 26, 27, 26, 33, 28, 36, 25, 24, 30, 28, 23, 24, 29, 25, 20, 31, 25, 26, 31, 31, 24, 19, 17, 37, 27, 34, 23, 32, 41, 33, 36, 27, 31, 20, 22, 24, 36, 23, 29, 24, 24, 31,
    22, 32, 22, 30, 20, 24, 28, 21, 24, 34, 31, 35, 35, 23, 29, 42, 31, 27, 30, 29, 21, 26, 36, 37, 29, 33, 24, 44, 36, 28, 26, 26, 30, 30, 33, 41, 21, 33, 25, 31, 29, 34, 36, 28, 31, 27, 22, 39, 27, 44,
    24, 25, 38, 29, 39, 26, 33, 22, 28, 35, 30, 35, 31, 19, 35, 29, 42, 34, 33, 38, 38, 33, 42, 35, 26, 25, 33, 33, 32, 36, 24, 31, 35, 28, 26, 33, 34, 36, 24, 30, 31, 30, 28, 31, 33, 24, 40, 38, 30, 45,
    30, 40, 30, 35, 41, 38, 29, 33, 35, 40, 30, 33, 42, 26, 30, 38, 27, 31, 26, 40, 44, 41, 33, 32, 24, 33, 31, 29, 33, 37, 36, 37, 31, 32, 37, 45, 45, 40, 38, 43, 37, 39, 39, 32, 27, 34, 34, 27, 28, 46,
    33, 33, 39, 41, 37, 34, 32, 32, 31, 34, 38, 39, 40, 38, 30, 39, 42, 39, 34, 35, 37, 34, 35, 48, 31, 32, 39, 40, 25, 50, 37, 39, 24, 40, 49, 25, 39, 42, 41, 54, 29, 48, 36, 45, 34, 34, 40, 30, 42, 33,
    42, 38, 37, 44, 31, 38, 36, 34, 32, 36, 45, 50, 33, 45, 39, 39, 40, 37, 46, 41, 46, 37, 34, 41, 29, 43, 37, 38, 42, 34, 30, 31, 27, 30, 27, 34, 28, 37, 13, 38, 29, 29, 30, 26, 37, 20, 26, 19, 25, 27,
    22, 29, 26, 24, 26, 26, 26, 21, 25, 26, 23, 19, 26, 12, 31, 20, 18, 27, 17, 15, 19, 23, 24, 31, 29, 28, 23, 23, 21, 22, 36, 25, 29, 28, 19, 28, 21, 30, 30, 30, 23, 31, 29, 35, 23, 29, 23, 21, 25, 15,
    21, 22, 28, 28, 21, 37, 26, 22, 26, 20, 25, 14, 22, 22, 31, 24, 38, 26, 21, 25, 33, 34, 24, 20, 25, 21, 11, 22, 32, 21, 25, 35, 28, 23, 29, 14, 18, 23, 25, 18, 27, 27, 26, 23, 23, 26, 20, 11, 22, 32,
    16, 24, 22, 19, 11, 25, 28, 26, 19, 26, 16, 21, 21, 18, 13, 23, 16, 18, 20, 20, 18, 19, 25, 16, 16, 26, 20, 15, 17, 19, 20, 20, 10, 22, 22, 15, 20, 18, 18, 18, 26, 17, 22, 13, 17, 6, 28, 22, 19, 19,
    17, 18, 16, 14, 21, 16, 14, 15, 16, 15, 25, 11, 17, 16, 16, 13, 19, 16, 19, 12, 14, 10, 14, 16, 14, 10, 12, 13, 13, 9, 15, 17, 16, 17, 7, 22, 17, 11, 17, 22, 8, 9, 15, 12, 9, 15, 9, 8, 25, 9,
    5, 15, 5, 12, 12, 8, 6, 12, 11, 12, 13, 11, 15, 13, 12, 15, 12, 5, 5, 12, 4, 5, 7, 12, 9, 6, 15, 5, 3, 9, 10, 14, 12, 10, 10, 14, 9, 10, 6, 6, 9, 6, 10, 11, 11, 13, 8, 11, 12, 6,
    9, 7, 19, 6, 6, 7, 8, 2, 9, 9, 10, 6, 9, 9, 9, 2, 5, 3, 6, 12, 5, 5, 3, 7, 8, 9, 14, 9, 6, 10, 4, 9, 7, 8, 9, 9, 4, 8, 10, 6, 12, 9, 7, 12, 10, 6, 5, 10, 9, 4,
    6, 9, 7, 3, 9, 13, 3, 9, 6, 5, 10, 7, 4, 12, 9, 6, 9, 12, 7, 5, 8, 14, 7, 5, 5, 10, 4, 3, 5, 9, 9, 9, 7, 7, 7, 4, 5, 5, 12, 5, 8, 10, 7, 4, 6, 13, 6, 8, 8, 5,
    4, 4, 7, 13, 9, 10, 7, 4, 7, 4, 5, 4, 7, 6, 7, 7, 6, 6, 5, 10, 7, 7, 8, 8, 5, 10, 4, 7, 7, 5, 12, 3, 9, 4, 5, 10, 8, 16, 4, 3, 8, 6, 9, 6, 9, 7, 9, 6, 6, 6,
    7, 5, 7, 6, 7, 6, 10, 5, 6, 5, 12, 6, 15, 6, 11, 6, 6, 5, 6, 9, 9, 6, 7, 13, 5, 13, 8, 8, 10, 13, 9, 10, 10, 17, 17, 14, 16, 18, 26, 18, 28, 37, 39, 26, 56, 62, 93, 106, 152, 237,
    111, 1, 80, 2, 229, 2, 21, 4, 249, 4, 27, 5, 168, 4, 191, 3, 214, 2, 173, 1, 221, 99, 44, 14, 4, 4, 7, 3, 6, 3, 5, 3, 3, 9, 1, 11, 8, 7, 3, 6, 4, 1, 2, 2, 4, 3, 6, 2, 3, 3,
    2, 4, 4, 5, 8, 4, 9, 3, 5, 4, 3, 5, 4, 4, 3, 4, 2, 3, 1, 6, 2, 5, 4, 2, 7, 2, 3, 4, 2, 1, 5, 6, 7, 4, 4, 3, 4, 4, 2, 6, 3, 11, 2, 0, 1, 1, 3, 7, 6, 6,
    4, 5, 4, 7, 3, 2, 6, 6, 2, 4, 3, 2, 5, 3, 2, 2, 2, 3, 1, 2, 3, 4, 3, 3, 4, 2, 4, 3, 2, 1, 3, 3, 5, 3, 6, 4, 1, 2, 2, 4, 4, 5, 6, 2, 4, 3, 6, 6, 6, 3,
    3, 9, 3, 1, 5, 5, 4, 4, 4, 9, 6, 3, 7, 4, 9, 7, 8, 1, 4, 4, 4, 4, 3, 3, 1, 7, 0, 1, 1, 6, 2, 6, 6, 3, 3, 3, 5, 3, 1, 2, 2, 4, 1, 4, 2, 5, 1, 4, 2, 2,
    2, 4, 7, 1, 3, 2, 1, 5, 2, 2, 2, 4, 3, 4, 4, 4, 4, 3, 3, 7, 3, 4, 4, 1, 3, 1, 0, 1, 4, 5, 4, 2, 2, 4, 6, 5, 5, 5, 6, 3, 4, 1, 2, 4, 0, 1, 3, 3, 2, 4,
    5, 1, 0, 1, 1, 4, 4, 2, 2, 4, 1, 0, 1, 3, 3, 0, 1, 2, 1, 2, 1, 0, 1, 1, 2, 5, 3, 3, 7, 3, 3, 4, 3, 6, 3, 3, 2, 2, 2, 2, 1, 3, 1, 1, 6, 2, 2, 2, 4, 0,
    1, 5, 5, 1, 3, 2, 5, 3, 7, 2, 2, 1, 4, 2, 5, 2, 1, 2, 1, 2, 4, 3, 1, 3, 2, 4, 4, 6, 2, 1, 2, 7, 4, 4, 2, 2, 1, 4, 3, 0, 1, 2, 2, 3, 1, 3, 3, 4, 2, 1,
    4, 3, 4, 3, 2, 4, 2, 2, 3, 2, 2, 4, 6, 3, 2, 4, 1, 4, 6, 3, 3, 4, 2, 4, 3, 5, 2, 1, 0, 1, 1, 2, 5, 4, 3, 3, 1, 1, 2, 5, 2, 0, 1, 3, 4, 2, 2, 4, 6, 3,
    3, 3, 3, 6, 7, 3, 2, 2, 5, 3, 1, 0, 1, 1, 6, 3, 1, 5, 4, 5, 3, 3, 5, 2, 1, 2, 2, 4, 3, 1, 4, 3, 2, 4, 4, 1, 4, 2, 4, 2, 4, 2, 3, 3, 5, 5, 3, 2, 2, 2,
    4, 2, 3, 4, 2, 4, 0, 1, 1, 2, 5, 4, 4, 3, 4, 2, 1, 3, 2, 0, 1, 3, 3, 2, 2, 3, 3, 2, 4, 2, 1, 5, 1, 3, 4, 1, 2, 2, 2, 0, 1, 1, 6, 5, 4, 6, 3, 3, 2, 5,
    4, 0, 1, 3, 2, 0, 1, 2, 1, 4, 2, 2, 2, 0, 1, 2, 1, 1, 6, 3, 5, 3, 4, 0, 1, 1, 4, 2, 2, 3, 3, 6, 2, 3, 2, 5, 2, 3, 3, 3, 5, 3, 2, 2, 2, 1, 2, 1, 1, 3,
    2, 2, 2, 1, 2, 4, 3, 1, 5, 2, 3, 2, 2, 1, 3, 4, 1, 4, 0, 1, 5, 3, 2, 7, 1, 4, 3, 1, 2, 0, 1, 1, 3, 3, 5, 2, 4, 4, 2, 0, 1, 4, 3, 4, 2, 2, 5, 4, 2, 0,
    1, 3, 3, 1, 0, 1, 2, 6, 6, 6, 4, 1, 3, 2, 4, 6, 4, 1, 3, 3, 2, 0, 1, 5, 2, 3, 1, 1, 1, 1, 1, 3, 3, 1, 1, 4, 1, 8, 2, 2, 2, 2, 0, 2, 5, 1, 0, 1, 2, 3,
    1, 4, 3, 1, 3, 1, 1, 1, 1, 3, 3, 1, 2, 1, 2, 4, 2, 4, 2, 2, 1, 1, 2, 2, 4, 2, 4, 4, 0, 1, 1, 1, 3, 1, 1, 0, 1, 2, 1, 3, 3, 1, 2, 3, 1, 4, 2, 0, 1, 3,
    1, 3, 4, 1, 1, 3, 5, 2, 2, 1, 1, 2, 2, 3, 4, 4, 0, 1, 2, 2, 3, 2, 4, 3, 2, 1, 2, 5, 4, 6, 2, 3, 4, 1, 7, 4, 1, 3, 3, 3, 1, 7, 3, 1, 2, 1, 0, 1, 2, 3,
    3, 2, 2, 0, 2, 2, 2, 0, 2, 3, 1, 3, 2, 1, 3, 1, 2, 4, 0, 1, 4, 0, 1, 1, 2, 3, 0, 1, 1, 2, 4, 3, 3, 5, 0, 1, 1, 2, 5, 1, 3, 2, 0, 1, 1, 0, 1, 3, 1, 3,
    0, 1, 4, 2, 3, 4, 0, 2, 2, 2, 3, 1, 4, 3, 3, 3, 2, 2, 0, 1, 3, 0, 1, 1, 6, 0, 1, 1, 1, 2, 2, 1, 0, 1, 2, 2, 0, 1, 3, 0, 1, 1, 1, 3, 1, 2, 1, 2, 4, 2,
    1, 3, 5, 4, 0, 1, 3, 1, 4, 4, 2, 1, 3, 3, 0, 1, 3, 1, 1, 3, 2, 1, 2, 6, 3, 1, 3, 2, 3, 1, 1, 1, 2, 2, 1, 0, 2, 3, 1, 2, 0, 4, 3, 3, 0, 1, 2, 1, 0, 3,
    2, 3, 1, 0, 1, 2, 6, 0, 1, 1, 1, 3, 1, 1, 0, 3, 4, 1, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 2, 1, 3, 3, 2, 2, 1, 2, 0, 2, 4, 3, 2, 1, 1, 1, 0,
    1, 2, 2, 2, 2, 2, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 4, 1, 1, 0, 1, 1, 1, 2, 2, 2, 1, 1, 2, 1, 2, 0, 1, 2, 2, 1, 1, 0, 1, 3, 0, 1, 1, 1, 2, 0,
    1, 1, 0, 4, 1, 0, 1, 1, 1, 0, 1, 2, 0, 3, 1, 1, 0, 2, 1, 0, 1, 1, 1, 0, 1, 2, 0, 1, 3, 1, 0, 1, 1, 0, 2, 1, 0, 3, 2, 0, 1, 1, 1, 0, 5, 1, 1, 1, 1, 1,
    0, 2, 3, 1, 0, 2, 1, 1, 1, 1, 0, 2, 2, 2, 0, 2, 1, 0, 1, 2, 1, 1, 1, 0, 2, 1, 0, 1, 2, 0, 1, 1, 0, 2, 3, 0, 1, 2, 0, 4, 1, 2, 0, 1, 1, 1, 0, 2, 1, 0,
    4, 3, 0, 1, 1, 1, 0, 2, 3, 1, 1, 2, 2, 1, 3, 5, 6, 4, 4, 6, 3, 4, 3, 8, 6, 3, 2, 0, 1, 1, 0, 7, 2, 1, 2, 0, 2, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 3, 1, 0,
    2, 2, 0, 4, 1, 0, 2, 1, 1, 0, 3, 1, 1, 1, 0, 4, 1, 1, 1, 0, 4, 1, 0, 9, 1, 0, 1, 3, 0, 1, 2, 0, 2, 1, 0, 2, 2, 2, 0, 2, 1, 2, 0, 1, 1, 1, 0, 3, 4, 0,
    1, 1, 1, 1, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 1, 0, 1, 1, 0, 2, 1, 0, 4, 1, 1, 1, 1, 0, 1, 1, 2, 1, 4, 0, 1, 2, 1, 2, 2, 1, 2, 1, 2, 4, 4, 10, 8, 10, 18, 12,
    19, 21, 20, 14, 18, 19, 5, 2, 6, 1, 1, 1, 0, 1, 1, 0, 8, 2, 0, 1, 1, 1, 0, 2, 1, 1, 0, 2, 1, 0, 7, 1, 0, 3, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1,
    0, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 2, 1, 0, 4, 1, 0, 6, 1, 1, 1, 0, 5, 2, 0, 7, 1, 0, 3, 1, 0, 4, 1, 1, 1, 1, 0, 3, 1, 0, 5, 2, 0, 1, 1, 1, 1, 3,
    0, 7, 1, 0, 2, 1, 0, 8, 1, 1, 0, 12, 1, 1, 0, 11, 1, 0, 8, 1, 0, 16, 1, 0, 4, 1, 0, 2, 2, 2, 0, 5, 1, 0, 7, 2, 0, 16, 1, 0, 3, 1, 0, 6, 1, 0, 1, 2, 0, 2,
    1, 0, 1, 1, 0, 13, 1, 0, 1, 1, 0, 2, 1, 0, 10, 2, 0, 2, 1, 2, 1, 0, 8, 1, 0, 15, 1, 1, 0, 1, 1, 0, 14, 1, 0, 5, 1, 1, 0, 2, 1, 0, 19, 1, 0, 1, 2, 0, 1, 1,
    0, 2, 1, 1, 0, 3, 2, 0, 39, 2, 0, 24, 1, 0, 8, 1, 0, 7, 1, 0, 9, 1, 1, 0, 7, 1, 0, 11, 1, 0, 31, 1, 0, 2, 1, 0, 7, 1, 0, 6, 1, 0, 4, 1, 1, 0, 3, 1, 0, 3,
    1, 0, 1, 1, 0, 1, 1, 0, 1, 2, 1, 1, 1, 2, 1, 3, 0, 1, 2, 1, 2, 1, 0, 13, 1, 0, 8, 1, 0, 5, 1, 0, 31, 1, 0, 28, 1, 0, 6, 1, 0, 2, 1, 0, 18, 1, 0, 8, 1, 0,
    2, 1, 0, 3, 1, 0, 16, 1, 0, 16, 1, 0, 6, 1, 0, 93, 1, 0, 2, 1, 0, 5, 1, 0, 2, 1, 0, 26, 1, 0, 31, 1, 0, 92, 1, 0, 3, 1, 0, 29, 1, 1, 0, 5, 1, 0, 36, 1, 0, 10,
    1, 0, 1, 1, 0, 10, 1, 0, 51, 1, 0, 42, 1, 0, 12, 1, 0, 36, 1, 0, 52, 1, 0, 21, 1, 0, 53, 1, 0, 67, 1, 0, 31, 1, 0, 76, 1, 0, 115, 1, 0, 51, 1, 0, 23, 1, 0, 8, 1, 0,
    22, 1, 0, 10, 1, 0, 78, 1, 0, 174, 1, 0, 35, 1, 0, 16, 1, 0, 6, 1, 0, 104, 1, 0, 66, 1, 0, 109, 1, 0, 19, 1, 0, 15, 1, 0, 251, 1, 0, 9, 1, 0, 26, 1, 0, 105, 1, 0, 17, 1,
    0, 32, 1, 0, 37, 1, 0, 88, 1, 0, 8, 1, 0, 77, 1, 1, 0, 25, 1, 0, 48, 1, 0, 111, 1, 0, 156, 1, 0, 3, 1, 0, 192, 1, 0, 13, 1, 0, 169, 1, 0, 65, 1, 0, 42, 1, 0, 60, 1, 0,
    114, 1, 0, 127, 1, 0, 44, 1, 0, 17, 1, 0, 32, 1, 0, 35, 1, 0, 200, 1, 0, 56, 1, 0, 61, 1, 0, 222, 1, 1, 0, 142, 1, 0, 25, 1, 0, 24, 2, 1, 0, 48, 1, 1, 0, 61, 1, 0, 16, 1,
    0, 104, 1, 0, 236, 1, 1, 0, 207, 1, 0, 114, 1, 0, 123, 1, 0, 156, 1, 1, 0, 7, 1, 0, 146, 2, 1, 0, 33, 1, 0, 251, 1, 1, 0, 238, 1, 0, 14, 1, 0, 55, 1, 1, 0, 109
  };
  assert( test_17_packed.size() == 10046 );
  const vector<uint8_t> test_17_encoded = QRSpecDev::encode_stream_vbyte( test_17_chan_cnts );
  assert( test_17_encoded == test_17_packed );
  vector<uint32_t> test_17_dec;
  const size_t test_17_nbytedec = QRSpecDev::decode_stream_vbyte(test_17_encoded,test_17_dec);
  assert( test_17_nbytedec == test_17_packed.size() );
  assert( test_17_dec == test_17_chan_cnts );
  
  
  
  
  // Test case 18
  const vector<uint32_t> test_18_chan_cnts{
    0, 28, 1, 0, 3, 2, 1, 0, 2, 1, 2, 6, 16, 460, 950, 933, 961, 807, 877, 914,
    942, 900, 860, 913, 913, 840, 865, 782, 835, 877, 836, 797, 788, 780, 817, 785, 737, 766, 770, 741,
    702, 748, 749, 738, 728, 799, 733, 743, 779, 686, 730, 724, 743, 714, 743, 701, 724, 699, 697, 647,
    626, 636, 636, 630, 627, 622, 683, 667, 620, 632, 600, 601, 574, 578, 594, 600, 613, 569, 568, 603,
    614, 636, 650, 593, 567, 547, 612, 579, 561, 592, 590, 575, 568, 584, 563, 596, 599, 630, 596, 633,
    706, 669, 712, 706, 669, 691, 640, 645, 666, 655, 662, 667, 717, 704, 730, 731, 704, 694, 670, 639,
    672, 693, 719, 703, 649, 742, 740, 765, 776, 734, 864, 773, 851, 890, 1067, 1307, 1933, 3175, 4469, 4607,
    3522, 2001, 1013, 679, 595, 587, 539, 610, 529, 591, 586, 598, 633, 626, 616, 591, 554, 593, 597, 588,
    569, 568, 554, 582, 584, 599, 598, 572, 585, 566, 586, 569, 569, 535, 561, 580, 577, 580, 610, 578,
    588, 592, 581, 593, 588, 613, 601, 599, 571, 605, 619, 584, 589, 601, 623, 614, 625, 643, 654, 581,
    630, 630, 637, 610, 633, 640, 676, 637, 673, 675, 654, 714, 682, 686, 662, 659, 663, 665, 751, 1002,
    1775, 3074, 3916, 3918, 2790, 1491, 869, 613, 548, 554, 588, 541, 525, 525, 550, 582, 586, 579, 566, 576,
    562, 571, 539, 520, 537, 544, 587, 563, 537, 536, 608, 588, 623, 652, 632, 564, 529, 519, 558, 588,
    552, 546, 587, 599, 541, 559, 564, 575, 540, 593, 533, 616, 597, 575, 638, 575, 612, 624, 649, 616,
    604, 606, 654, 642, 635, 579, 623, 596, 590, 632, 598, 643, 594, 652, 628, 651, 664, 590, 586, 634,
    635, 719, 727, 684, 665, 652, 610, 611, 573, 608, 634, 528, 595, 546, 582, 563, 516, 551, 576, 594,
    537, 605, 640, 624, 596, 617, 609, 589, 570, 598, 555, 629, 584, 558, 565, 614, 536, 561, 591, 580,
    587, 507, 628, 581, 586, 563, 580, 527, 549, 572, 626, 551, 550, 560, 584, 541, 529, 552, 542, 561,
    561, 550, 579, 509, 508, 548, 543, 528, 550, 563, 529, 550, 574, 521, 531, 555, 521, 549, 526, 506,
    485, 472, 515, 517, 504, 487, 540, 504, 536, 499, 526, 470, 521, 524, 461, 490, 474, 487, 482, 426,
    432, 447, 465, 450, 459, 437, 444, 446, 495, 486, 461, 476, 509, 441, 416, 446, 432, 431, 442, 463,
    455, 433, 392, 460, 432, 408, 412, 466, 417, 488, 448, 450, 444, 450, 442, 414, 422, 395, 383, 399,
    416, 392, 415, 396, 404, 408, 394, 418, 398, 369, 422, 371, 419, 371, 363, 397, 386, 383, 409, 364,
    427, 417, 418, 419, 417, 377, 371, 374, 374, 383, 376, 474, 672, 890, 998, 1011, 700, 455, 428, 403,
    347, 376, 374, 368, 329, 317, 357, 320, 385, 328, 342, 375, 303, 321, 360, 344, 380, 331, 333, 357,
    336, 324, 359, 366, 333, 335, 332, 350, 321, 299, 329, 323, 315, 279, 341, 313, 294, 313, 361, 304,
    334, 318, 302, 325, 292, 345, 343, 302, 339, 322, 329, 332, 339, 305, 337, 355, 474, 642, 989, 1164,
    1125, 769, 519, 378, 334, 329, 325, 295, 336, 332, 336, 285, 314, 313, 309, 315, 330, 321, 297, 351,
    342, 308, 353, 311, 306, 324, 301, 296, 332, 298, 296, 318, 317, 312, 279, 288, 327, 345, 366, 347,
    353, 333, 334, 296, 294, 307, 338, 337, 317, 397, 611, 1084, 1751, 2296, 2235, 1630, 913, 522, 336, 340,
    316, 320, 284, 302, 299, 333, 332, 274, 299, 284, 313, 315, 301, 290, 289, 308, 280, 301, 308, 296,
    274, 301, 294, 277, 287, 281, 299, 329, 332, 296, 269, 344, 288, 324, 291, 308, 344, 310, 291, 295,
    300, 277, 316, 297, 312, 290, 297, 315, 288, 332, 319, 326, 308, 291, 305, 298, 295, 299, 302, 269,
    313, 306, 299, 271, 338, 315, 330, 270, 305, 282, 303, 306, 294, 296, 408, 700, 1201, 1912, 2207, 1873,
    1209, 592, 353, 293, 213, 236, 257, 232, 242, 222, 241, 260, 235, 249, 258, 262, 316, 414, 684, 1558,
    3409, 5412, 6283, 5091, 3099, 1345, 496, 247, 181, 160, 155, 178, 173, 176, 171, 169, 181, 185, 177, 187,
    198, 156, 165, 185, 185, 232, 221, 202, 207, 180, 205, 161, 188, 165, 165, 165, 134, 149, 146, 145,
    151, 167, 151, 159, 158, 162, 144, 171, 172, 165, 149, 127, 171, 168, 190, 239, 412, 687, 908, 933,
    742, 468, 277, 179, 136, 152, 136, 155, 156, 147, 142, 135, 138, 139, 143, 152, 140, 143, 162, 137,
    115, 146, 144, 138, 142, 138, 140, 129, 126, 126, 140, 152, 140, 135, 137, 137, 133, 141, 141, 156,
    136, 142, 140, 145, 140, 137, 135, 122, 129, 144, 143, 133, 173, 242, 253, 234, 225, 182, 165, 130,
    119, 136, 130, 125, 132, 143, 125, 109, 121, 126, 144, 134, 136, 112, 108, 121, 130, 131, 94, 115,
    124, 103, 147, 121, 127, 125, 130, 134, 125, 132, 141, 119, 129, 127, 141, 131, 127, 117, 136, 117,
    122, 135, 135, 128, 153, 143, 149, 141, 130, 128, 123, 140, 149, 127, 127, 101, 125, 139, 165, 207,
    263, 294, 272, 206, 168, 141, 128, 113, 137, 133, 126, 133, 123, 112, 133, 135, 126, 127, 117, 125,
    151, 120, 101, 121, 137, 128, 121, 123, 129, 147, 138, 121, 123, 143, 124, 121, 111, 138, 142, 125,
    137, 118, 115, 117, 115, 130, 145, 121, 137, 126, 128, 135, 135, 124, 149, 135, 120, 112, 142, 119,
    140, 113, 120, 121, 130, 119, 114, 104, 131, 108, 123, 128, 127, 105, 99, 119, 127, 105, 110, 126,
    138, 93, 105, 107, 115, 111, 108, 105, 107, 114, 122, 133, 112, 125, 130, 124, 106, 119, 117, 110,
    134, 96, 93, 98, 94, 101, 122, 111, 119, 108, 107, 108, 89, 109, 98, 115, 115, 120, 116, 112,
    123, 111, 129, 145, 133, 137, 131, 145, 163, 224, 237, 420, 726, 1203, 1972, 3121, 4306, 5087, 5528, 4893,
    3733, 2554, 1639, 839, 449, 225, 155, 92, 98, 77, 73, 75, 81, 69, 74, 70, 77, 76, 62, 84,
    72, 69, 73, 58, 80, 59, 72, 66, 75, 66, 78, 68, 76, 68, 71, 56, 72, 70, 68, 62,
    71, 70, 66, 72, 62, 81, 57, 69, 88, 61, 73, 76, 60, 68, 70, 72, 68, 70, 77, 71,
    63, 61, 68, 46, 70, 55, 64, 63, 72, 63, 54, 62, 71, 61, 67, 60, 74, 55, 61, 75,
    66, 70, 77, 74, 70, 65, 55, 75, 64, 75, 59, 76, 73, 74, 61, 71, 61, 67, 70, 67,
    72, 56, 76, 57, 63, 76, 80, 87, 69, 63, 56, 83, 84, 67, 71, 77, 67, 69, 78, 55,
    64, 50, 63, 72, 61, 66, 72, 62, 58, 79, 66, 69, 77, 60, 74, 63, 65, 72, 77, 69,
    64, 70, 75, 83, 71, 87, 77, 73, 62, 83, 74, 81, 101, 84, 75, 76, 63, 48, 68, 44,
    69, 56, 61, 72, 70, 57, 73, 74, 45, 71, 54, 75, 60, 48, 57, 61, 67, 54, 61, 59,
    61, 65, 61, 48, 76, 61, 65, 58, 61, 63, 61, 47, 61, 43, 67, 75, 68, 84, 88, 91,
    72, 69, 57, 74, 59, 59, 69, 64, 56, 65, 71, 52, 53, 62, 59, 60, 67, 58, 52, 52,
    70, 57, 71, 54, 65, 60, 74, 50, 68, 51, 67, 62, 58, 63, 48, 61, 70, 77, 66, 57,
    54, 69, 63, 68, 56, 50, 60, 65, 54, 68, 65, 65, 55, 67, 54, 51, 61, 56, 71, 59,
    61, 72, 62, 53, 51, 56, 53, 60, 64, 69, 61, 61, 53, 64, 50, 81, 51, 61, 61, 47,
    64, 68, 63, 58, 55, 58, 74, 70, 51, 68, 56, 69, 84, 49, 77, 65, 66, 79, 91, 101,
    132, 247, 456, 1041, 1692, 2252, 2117, 1638, 938, 401, 144, 77, 55, 51, 62, 41, 48, 57, 43, 47,
    51, 49, 40, 38, 41, 38, 34, 51, 47, 52, 49, 48, 50, 49, 51, 52, 43, 47, 44, 57,
    58, 59, 79, 57, 50, 42, 52, 45, 45, 50, 45, 44, 48, 53, 38, 49, 48, 44, 62, 78,
    76, 80, 65, 62, 67, 49, 51, 42, 46, 54, 52, 52, 45, 49, 49, 44, 44, 41, 58, 44,
    46, 41, 30, 37, 45, 45, 43, 38, 47, 55, 39, 35, 41, 54, 46, 45, 48, 54, 48, 41,
    38, 40, 58, 44, 53, 32, 46, 46, 53, 58, 41, 54, 42, 41, 38, 40, 41, 50, 42, 45,
    41, 52, 48, 50, 64, 55, 58, 48, 49, 46, 44, 46, 50, 38, 50, 54, 44, 61, 45, 59,
    50, 34, 48, 56, 47, 37, 52, 46, 56, 51, 37, 42, 46, 43, 42, 41, 40, 56, 46, 43,
    43, 44, 36, 39, 58, 57, 48, 49, 39, 52, 47, 49, 44, 57, 44, 53, 40, 37, 53, 56,
    40, 40, 51, 48, 44, 45, 49, 42, 48, 43, 30, 50, 58, 47, 38, 51, 45, 48, 57, 55,
    43, 46, 38, 58, 46, 33, 46, 51, 53, 52, 52, 59, 51, 48, 67, 59, 55, 61, 51, 54,
    44, 47, 47, 42, 51, 53, 50, 55, 39, 41, 39, 54, 37, 53, 40, 56, 44, 46, 41, 57,
    57, 88, 139, 293, 424, 429, 434, 315, 225, 113, 65, 38, 38, 47, 40, 37, 33, 40, 39, 38,
    38, 61, 41, 45, 54, 48, 52, 35, 41, 25, 51, 43, 36, 44, 45, 42, 46, 57, 44, 47,
    48, 49, 39, 40, 33, 33, 40, 47, 40, 31, 53, 24, 43, 42, 48, 41, 54, 28, 41, 51,
    43, 44, 39, 23, 41, 30, 40, 44, 52, 50, 67, 45, 40, 50, 32, 39, 47, 39, 43, 51,
    47, 46, 34, 37, 36, 47, 34, 43, 51, 34, 37, 46, 44, 50, 41, 41, 43, 49, 38, 37,
    39, 39, 39, 48, 47, 40, 45, 39, 56, 49, 38, 32, 41, 47, 49, 72, 93, 155, 207, 381,
    370, 327, 222, 127, 83, 57, 48, 47, 50, 47, 34, 44, 54, 40, 34, 39, 44, 39, 44, 38,
    44, 35, 37, 60, 43, 42, 27, 57, 47, 45, 47, 60, 43, 40, 43, 46, 31, 33, 47, 35,
    40, 39, 42, 53, 32, 46, 48, 51, 51, 39, 44, 44, 43, 51, 46, 49, 50, 36, 42, 39,
    43, 54, 68, 91, 114, 142, 179, 148, 126, 85, 66, 53, 51, 44, 37, 37, 41, 47, 45, 44,
    42, 36, 55, 48, 51, 44, 38, 33, 46, 29, 40, 39, 41, 42, 38, 35, 32, 46, 34, 43,
    47, 50, 56, 45, 48, 43, 38, 39, 40, 46, 41, 31, 40, 43, 32, 39, 43, 44, 40, 34,
    45, 30, 46, 40, 28, 43, 50, 41, 37, 39, 47, 51, 29, 45, 42, 36, 45, 40, 57, 39,
    45, 45, 40, 32, 53, 55, 41, 45, 44, 42, 25, 41, 37, 47, 43, 55, 51, 50, 48, 47,
    33, 36, 39, 34, 33, 41, 47, 48, 33, 40, 46, 49, 58, 60, 52, 50, 35, 58, 46, 37,
    43, 35, 34, 43, 33, 43, 44, 54, 57, 46, 48, 39, 32, 40, 29, 47, 44, 31, 52, 40,
    37, 43, 37, 47, 30, 42, 42, 40, 50, 35, 45, 48, 39, 39, 42, 44, 46, 35, 39, 37,
    39, 45, 42, 37, 36, 32, 46, 49, 37, 33, 40, 53, 26, 32, 57, 41, 38, 45, 44, 48,
    40, 47, 37, 42, 33, 41, 45, 36, 32, 43, 55, 33, 57, 47, 58, 52, 47, 54, 61, 87,
    121, 184, 299, 420, 459, 364, 216, 155, 73, 57, 29, 40, 54, 40, 43, 47, 47, 42, 40, 44,
    37, 40, 36, 48, 46, 43, 38, 39, 53, 32, 31, 37, 47, 42, 58, 49, 38, 45, 51, 43,
    31, 39, 38, 33, 48, 39, 37, 35, 44, 52, 41, 38, 30, 39, 43, 49, 36, 41, 38, 42,
    41, 36, 40, 38, 39, 53, 51, 39, 48, 44, 43, 39, 38, 48, 41, 46, 50, 27, 37, 51,
    46, 41, 45, 37, 44, 53, 52, 58, 56, 56, 54, 52, 42, 36, 51, 40, 42, 58, 49, 59,
    41, 50, 49, 40, 45, 37, 32, 46, 45, 44, 42, 51, 48, 49, 40, 45, 44, 36, 57, 48,
    51, 44, 53, 43, 40, 46, 45, 48, 44, 42, 38, 41, 41, 51, 51, 37, 55, 45, 54, 55,
    46, 53, 44, 57, 43, 49, 60, 42, 41, 52, 47, 38, 48, 48, 35, 49, 45, 58, 41, 54,
    43, 52, 47, 42, 33, 58, 47, 42, 50, 43, 59, 45, 65, 52, 41, 43, 53, 47, 60, 46,
    43, 52, 47, 47, 61, 56, 43, 50, 51, 30, 44, 40, 42, 42, 44, 46, 58, 49, 49, 51,
    47, 33, 43, 52, 47, 46, 41, 49, 31, 34, 45, 43, 32, 39, 39, 42, 44, 36, 31, 33,
    33, 32, 32, 32, 32, 32, 31, 35, 32, 44, 40, 40, 37, 34, 30, 54, 44, 27, 32, 41,
    41, 30, 42, 39, 39, 44, 40, 55, 65, 110, 172, 241, 274, 283, 213, 146, 69, 77, 57, 75,
    58, 72, 64, 57, 38, 33, 31, 25, 39, 31, 27, 27, 23, 32, 20, 19, 24, 27, 29, 27,
    27, 25, 24, 32, 24, 24, 26, 23, 27, 32, 29, 22, 22, 42, 35, 24, 32, 32, 32, 34,
    44, 90, 101, 144, 230, 327, 327, 291, 208, 128, 61, 25, 27, 36, 26, 27, 21, 33, 31, 21,
    27, 30, 28, 34, 36, 25, 25, 13, 25, 24, 25, 27, 23, 28, 23, 24, 22, 21, 21, 20,
    22, 30, 21, 29, 21, 18, 17, 28, 25, 38, 21, 24, 18, 16, 20, 24, 25, 19, 14, 22,
    35, 29, 24, 20, 25, 26, 23, 15, 25, 17, 27, 23, 28, 26, 20, 13, 26, 28, 15, 22,
    26, 25, 25, 20, 22, 23, 23, 22, 22, 22, 28, 24, 15, 19, 18, 25, 28, 19, 22, 21,
    18, 19, 19, 13, 20, 13, 17, 21, 27, 26, 22, 12, 26, 25, 19, 15, 13, 18, 21, 28,
    17, 16, 25, 12, 20, 16, 23, 23, 23, 18, 26, 16, 23, 19, 18, 18, 20, 22, 23, 20,
    20, 17, 15, 17, 17, 17, 15, 17, 15, 15, 29, 15, 19, 24, 15, 17, 17, 15, 13, 13,
    19, 16, 17, 17, 14, 20, 17, 16, 21, 14, 13, 25, 15, 17, 17, 7, 13, 16, 4, 14,
    13, 15, 18, 18, 18, 13, 7, 11, 10, 11, 9, 14, 12, 14, 18, 15, 11, 17, 15, 9,
    21, 12, 17, 9, 12, 14, 16, 22, 11, 24, 25, 49, 49, 41, 41, 26, 19, 12, 15, 13,
    13, 16, 12, 12, 10, 13, 15, 9, 12, 9, 16, 17, 11, 16, 22, 13, 11, 6, 16, 16,
    13, 14, 11, 13, 15, 14, 13, 12, 21, 17, 17, 12, 12, 8, 12, 17, 15, 7, 9, 13,
    16, 21, 16, 10, 21, 13, 25, 20, 15, 15, 9, 10, 16, 13, 13, 9, 12, 8, 13, 18,
    6, 15, 7, 8, 17, 11, 10, 18, 16, 18, 12, 12, 13, 10, 16, 10, 11, 14, 13, 11,
    8, 12, 5, 11, 17, 11, 9, 14, 12, 10, 14, 17, 5, 12, 11, 16, 19, 12, 16, 12,
    16, 17, 15, 25, 24, 23, 22, 26, 35, 41, 62, 78, 110, 208, 458, 805, 1211, 1667, 1694, 1482,
    998, 607, 239, 93, 32, 9, 12, 7, 7, 6, 7, 11, 8, 5, 9, 6, 9, 5, 2, 2,
    9, 2, 4, 6, 11, 6, 5, 9, 9, 6, 3, 6, 2, 10, 8, 9, 6, 8, 5, 6,
    6, 10, 16, 13, 14, 30, 32, 27, 41, 31, 25, 18, 10, 9, 7, 5, 4, 7, 6, 3,
    3, 5, 4, 5, 5, 4, 5, 8, 6, 5, 5, 6, 3, 5, 3, 4, 8, 8, 0, 1,
    4, 2, 4, 3, 7, 5, 3, 4, 5, 3, 7, 4, 12, 6, 6, 7, 5, 2, 6, 5,
    7, 3, 6, 3, 2, 4, 6, 8, 4, 6, 5, 5, 2, 9, 1, 5, 5, 3, 10, 5,
    4, 6, 1, 5, 2, 4, 7, 8, 6, 1, 5, 3, 6, 2, 6, 6, 5, 6, 3, 3,
    0, 1, 4, 5, 2, 4, 2, 6, 5, 2, 6, 6, 4, 4, 1, 2, 4, 7, 4, 5,
    0, 1, 2, 1, 2, 4, 2, 3, 4, 4, 2, 2, 2, 3, 2, 2, 6, 3, 2, 3,
    3, 3, 5, 4, 1, 4, 3, 2, 3, 3, 2, 6, 4, 4, 3, 6, 1, 2, 2, 5,
    1, 2, 4, 4, 4, 3, 7, 7, 3, 4, 8, 8, 7, 2, 5, 4, 5, 2, 1, 4,
    1, 0, 1, 6, 2, 3, 2, 5, 4, 2, 2, 6, 0, 1, 1, 3, 2, 7, 3, 5,
    2, 4, 5, 1, 7, 4, 7, 4, 6, 5, 3, 2, 4, 6, 2, 1, 3, 6, 4, 5,
    6, 6, 3, 7, 7, 10, 8, 9, 11, 35, 36, 72, 117, 215, 286, 399, 409, 352, 230, 114,
    62, 15, 7, 6, 2, 4, 1, 4, 1, 1, 2, 2, 2, 2, 4, 5, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 5, 4, 2, 0, 1, 2, 1, 4, 0, 1, 1, 6, 1, 4, 2,
    4, 1, 1, 2, 2, 3, 4, 3, 2, 0, 1, 2, 3, 0, 1, 3, 1, 4, 2, 4,
    1, 3, 2, 3, 0, 1, 2, 1, 0, 1, 2, 0, 1, 5, 3, 5, 3, 2, 1, 4,
    1, 3, 0, 1, 5, 1, 1, 2, 3, 2, 0, 1, 1, 3, 4, 3, 2, 2, 2, 4,
    4, 6, 7, 6, 6, 13, 17, 18, 19, 33, 52, 93, 97, 111, 103, 63, 29, 21, 6, 6,
    0, 1, 4, 0, 1, 1, 1, 0, 1, 2, 0, 1, 2, 0, 1, 1, 0, 1, 2, 0,
    2, 1, 3, 4, 5, 4, 2, 3, 5, 3, 3, 3, 3, 1, 0, 1, 1, 4, 0, 1,
    2, 2, 2, 2, 3, 1, 2, 1, 1, 0, 1, 1, 3, 0, 1, 4, 1, 0, 1, 1,
    1, 0, 2, 4, 2, 0, 3, 1, 1, 1, 1, 0, 1, 2, 1, 0, 1, 1, 0, 1,
    1, 1, 1, 1, 2, 1, 2, 3, 0, 1, 2, 6, 1, 2, 3, 0, 1, 3, 1, 3,
    3, 2, 1, 1, 6, 3, 2, 2, 5, 1, 1, 1, 1, 1, 2, 3, 0, 1, 2, 4,
    2, 1, 1, 5, 1, 0, 1, 1, 4, 0, 2, 1, 1, 0, 1, 1, 2, 1, 0, 1,
    1, 0, 1, 1, 0, 1, 3, 3, 2, 2, 5, 5, 11, 2, 3, 6, 2, 2, 2, 1,
    0, 1, 3, 2, 0, 2, 2, 0, 2, 1, 1, 0, 1, 1, 1, 1, 2, 2, 3, 2,
    0, 1, 4, 0, 1, 1, 2, 3, 0, 1, 1, 0, 2, 4, 1, 2, 2, 2, 1, 1,
    0, 1, 1, 1, 0, 3, 1, 0, 2, 1, 2, 2, 2, 1, 1, 1, 3, 1, 0, 1,
    1, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 4, 0, 1, 2, 1, 0, 1, 2, 0,
    1, 2, 1, 0, 1, 1, 0, 1, 3, 0, 1, 2, 3, 1, 0, 1, 1, 0, 1, 1,
    1, 2, 1, 0, 1, 3, 2, 2, 0, 1, 2, 4, 1, 1, 3, 0, 1, 2, 0, 1,
    3, 0, 1, 3, 2, 0, 2, 2, 0, 3, 2, 3, 1, 1, 2, 2, 2, 2, 3, 2,
    2, 0, 2, 2, 2, 0, 1, 1, 1, 1, 5, 0, 1, 4, 1, 2, 1, 1, 0, 1,
    1, 1, 0, 1, 2, 1, 0, 1, 2, 0, 1, 1, 2, 0, 1, 2, 0, 2, 1, 2,
    0, 1, 1, 3, 1, 0, 1, 2, 1, 0, 1, 4, 3, 0, 1, 1, 1, 0, 1, 1,
    0, 2, 3, 0, 1, 2, 1, 0, 1, 1, 2, 0, 1, 2, 0, 2, 2, 3, 0, 1,
    2, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 1, 1, 0,
    1, 4, 2, 0, 3, 1, 1, 0, 1, 1, 2, 1, 0, 1, 2, 2, 0, 1, 1, 3,
    1, 2, 0, 1, 1, 0, 1, 1, 0, 4, 1, 0, 1, 2, 0, 1, 1, 2, 1, 3,
    1, 0, 1, 2, 2, 0, 3, 3, 0, 6, 1, 0, 2, 1, 2, 0, 1, 1, 0, 1,
    2, 1, 1, 3, 1, 0, 1, 1, 1, 3, 0, 5, 1, 2, 2, 1, 0, 1, 1, 1,
    0, 2, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 8, 1, 0, 2, 1, 1, 3,
    0, 1, 1, 0, 2, 1, 0, 3, 2, 1, 2, 1, 0, 2, 1, 2, 2, 0, 4, 1,
    1, 0, 4, 1, 1, 1, 0, 1, 3, 3, 0, 4, 1, 0, 3, 2, 0, 1, 1, 1,
    0, 1, 2, 2, 1, 1, 0, 2, 2, 0, 1, 1, 0, 1, 1, 0, 1, 2, 2, 0,
    3, 2, 0, 1, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 3, 2, 0, 10, 1, 1,
    0, 1, 2, 0, 1, 1, 1, 0, 1, 3, 1, 0, 1, 1, 4, 4, 1, 2, 2, 0,
    6, 1, 0, 3, 3, 0, 1, 3, 0, 1, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0,
    1, 1, 0, 3, 1, 0, 2, 3, 0, 1, 1, 0, 2, 1, 1, 1, 0, 4, 1, 0,
    2, 1, 0, 3, 1, 3, 0, 1, 1, 1, 3, 0, 7, 1, 1, 1, 3, 5, 8, 5,
    3, 2, 4, 1, 2, 1, 1, 1, 0, 1, 2, 2, 1, 0, 1, 1, 0, 3, 1, 0,
    1, 2, 0, 9, 1, 3, 2, 3, 1, 2, 5, 5, 6, 5, 13, 12, 9, 11, 4, 6,
    2, 3, 1, 1, 0, 6, 1, 1, 1, 0, 5, 1, 1, 0, 5, 1, 0, 5, 2, 0,
    1, 2, 1, 1, 0, 1, 1, 1, 0, 1, 2, 0, 3, 1, 0, 12, 1, 1, 0, 7,
    1, 0, 4, 1, 0, 3, 1, 0, 5, 2, 0, 2, 1, 0, 7, 1, 0, 2, 1, 0,
    5, 1, 0, 1, 1, 0, 2, 1, 0, 7, 1, 0, 5, 2, 3, 1, 0, 2, 1, 0,
    1, 1, 0, 1, 1, 1, 0, 1, 2, 0, 1, 2, 1, 0, 1, 1, 0, 2, 1, 1,
    1, 0, 3, 1, 0, 7, 1, 0, 5, 1, 1, 0, 5, 1, 0, 3, 1, 0, 10, 1,
    1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 6, 1, 0, 4, 1, 1, 0, 4, 1,
    0, 2, 1, 0, 2, 1, 1, 1, 1, 0, 1, 1, 0, 2, 2, 0, 1, 2, 0, 1,
    1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 3,
    1, 0, 1, 1, 0, 4, 1, 1, 1, 0, 9, 1, 1, 0, 1, 2, 0, 4, 1, 0,
    3, 3, 0, 2, 1, 0, 1, 1, 0, 4, 1, 0, 1, 2, 0, 7, 1, 0, 1, 1,
    0, 1, 1, 0, 2, 1, 1, 1, 1, 0, 17, 1, 0, 5, 1, 1, 0, 1, 1, 0,
    1, 1, 0, 3, 1, 0, 3, 1, 0, 4, 1, 1, 0, 11, 1, 1, 0, 4, 2, 0,
    4, 2, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 8, 1, 1, 1,
    0, 3, 1, 0, 2, 1, 0, 1, 1, 1, 0, 3, 1, 1, 0, 4, 1, 1, 0, 9,
    1, 0, 2, 2, 0, 6, 1, 0, 11, 1, 1, 1, 1, 0, 1, 2, 0, 9, 1, 0,
    1, 1, 0, 10, 1, 0, 22, 1, 0, 3, 1, 0, 3, 2, 0, 3, 2, 0, 1, 1,
    0, 2, 2, 0, 4, 1, 0, 3, 1, 1, 1, 0, 4, 1, 1, 0, 1, 1, 0, 6,
    1, 0, 1, 2, 0, 1, 1, 0, 6, 1, 0, 2, 2, 0, 4, 1, 0, 1, 1, 0,
    1, 1, 0, 1, 1, 0, 1, 2, 0, 2, 1, 2, 0, 1, 1, 0, 2, 1, 0, 4,
    3, 0, 2, 2, 1, 0, 1, 1, 0, 5, 1, 0, 6, 1, 0, 10, 1, 0, 2, 1,
    0, 1, 1, 0, 1, 1, 1, 0, 11, 1, 0, 4, 1, 0, 1, 3, 0, 2, 1, 0,
    1, 3, 0, 1, 1, 2, 1, 1, 1, 0, 2, 1, 0, 7, 1, 1, 0, 2, 1, 0,
    4, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 22, 1, 1, 0, 5, 1, 0, 1,
    1, 1, 1, 1, 0, 8, 1, 0, 1, 1, 1, 0, 5, 1, 0, 10, 1, 0, 2, 1,
    0, 4, 1, 0, 2, 1, 0, 6, 1, 0, 6, 1, 1, 0, 1, 1, 0, 4, 2, 0,
    1, 1, 0, 4, 2, 0, 9, 1, 1, 0, 6, 1, 1, 0, 2, 1, 1, 0, 3, 1,
    0, 4, 1, 0, 4, 1, 0, 1, 1, 0, 5, 2, 0, 1, 1, 0, 4, 1, 1, 0,
    2, 1, 0, 4, 1, 1, 0, 1, 1, 0, 1, 1, 1, 3, 3, 4, 1, 0, 1, 3,
    0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 7, 1, 1, 0, 2, 1, 0, 4, 1, 0,
    4, 1, 0, 4, 2, 0, 2, 1, 0, 5, 1, 0, 6, 1, 0, 5, 1, 0, 15, 1,
    0, 4, 1, 0, 6, 1, 0, 4, 1, 2, 0, 7, 1, 0, 2, 1, 0, 1, 1, 0,
    1, 1, 0, 2, 1, 1, 0, 4, 1, 0, 5, 1, 0, 3, 1, 0, 1, 2, 0, 7,
    1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2, 1, 0, 5, 3,
    0, 4, 1, 2, 0, 5, 1, 0, 6, 1, 0, 1, 1, 1, 1, 0, 4, 1, 0, 4,
    2, 0, 5, 1, 0, 6, 1, 0, 2, 2, 0, 2, 1, 0, 13, 1, 0, 1, 1, 0,
    6, 1, 0, 4, 1, 0, 2, 2, 1, 0, 4, 1, 0, 3, 1, 0, 5, 1, 0, 7,
    1, 0, 10, 1, 0, 4, 1, 0, 1, 1, 0, 4, 2, 0, 3, 1, 0, 2, 1, 0,
    1, 1, 4, 0, 1, 1, 0, 8, 1, 0, 7, 1, 0, 3, 1, 0, 2, 1, 0, 1,
    1, 0, 3, 1, 0, 3, 1, 1, 0, 3, 1, 0, 1, 3, 0, 1, 1, 0, 5, 1,
    0, 1, 1, 0, 5, 1, 0, 2, 1, 0, 3, 1, 0, 4, 1, 0, 6, 1, 2, 0,
    3, 1, 0, 4, 1, 0, 2, 1, 0, 4, 1, 0, 6, 1, 1, 1, 1, 0, 1, 1,
    0, 2, 1, 0, 4, 1, 0, 3, 1, 0, 8, 1, 1, 0, 8, 1, 0, 19, 1, 0,
    4, 1, 0, 3, 1, 0, 4, 1, 0, 10, 1, 0, 3, 1, 0, 6, 1, 1, 0, 1,
    1, 0, 1, 1, 0, 1, 1, 0, 17, 1, 0, 2, 1, 0, 11, 1, 0, 1, 1, 1,
    0, 2, 2, 0, 16, 1, 0, 1, 1, 0, 6, 1, 0, 1, 1, 0, 14, 1, 0, 12,
    1, 0, 12, 1, 0, 4, 1, 0, 15, 1, 0, 23, 1, 0, 17, 2, 0, 3, 1, 0,
    13, 1, 0, 30, 1, 0, 16, 1, 0, 10, 1, 0, 7, 1, 0, 5, 1, 0, 4, 1,
    0, 2, 1, 0, 31, 1, 0, 6, 1, 0, 2, 1, 0, 4, 1, 0, 6, 1, 1, 0,
    1, 1, 0, 2, 2, 4, 4, 4, 3, 6, 6, 12, 8, 13, 7, 1, 1, 3, 0, 1,
    1, 0, 36, 1, 0, 63, 1, 0, 102, 1, 0, 41, 1, 0, 60, 1, 0, 63, 1, 0,
    67, 1, 0, 53, 1, 0, 112, 1, 0, 2, 1, 0, 36, 1, 0, 35, 1, 0, 12, 1,
    0, 92, 1, 0, 72, 1, 0, 42, 1, 0, 1, 1, 0, 30, 1, 0, 38, 1, 0, 85,
    1, 0, 14, 1, 0, 122, 1, 0, 36, 1, 0, 51, 1, 0, 321, 1, 0, 138, 1, 0,
    57, 1, 0, 2, 1, 0, 45, 1, 0, 218, 1, 0, 32, 1, 0, 13, 1, 0, 173, 1,
    0, 41, 1, 0, 130, 1, 0, 564, 1, 0, 17, 1, 0, 64, 1, 0, 236, 1, 0, 14,
    1, 0, 31, 1, 0, 72, 1, 0, 97, 1, 0, 4, 1, 0, 100, 1, 0, 231, 1, 0,
    241, 1, 0, 68, 1, 0, 131, 1, 0, 191, 1, 0, 295, 1, 0, 149, 1, 0, 142, 1,
    0, 502, 1, 0, 648, 1, 0, 222, 1, 0, 152, 1, 0, 100, 1, 0, 78, 1, 0, 189,
    1, 0, 721, 1, 0, 80, 1, 0, 20, 1, 0, 434, 1, 0, 65, 1, 0, 136, 1, 0,
    148, 1, 0, 35, 1, 0, 53, 1, 0, 387, 1, 0, 92, 1, 0, 106, 1, 0, 516, 1,
    0, 568, 1, 0, 187, 1, 0, 249, 1, 0, 59, 1, 0, 122  };
  assert( test_18_chan_cnts.size() == 4914 );
  const vector<uint8_t> test_18_packed{
    50, 19, 0, 0, 0, 84, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 16, 64, 80, 85, 85, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 21, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 64, 85, 85, 85, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 85, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 85, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 85, 5, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 64, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 4, 1, 0, 0, 0, 16, 0, 64, 0, 0, 0, 0, 4, 0, 16, 4, 0, 0, 0, 0, 28, 1, 0, 3, 2, 1, 0, 2, 1, 2, 6, 16, 204, 1, 182, 3, 165, 3,
    193, 3, 39, 3, 109, 3, 146, 3, 174, 3, 132, 3, 92, 3, 145, 3, 145, 3, 72, 3, 97, 3, 14, 3, 67, 3, 109, 3, 68, 3, 29, 3, 20, 3, 12, 3, 49, 3, 17, 3, 225, 2, 254, 2, 2, 3, 229, 2, 190, 2,
    236, 2, 237, 2, 226, 2, 216, 2, 31, 3, 221, 2, 231, 2, 11, 3, 174, 2, 218, 2, 212, 2, 231, 2, 202, 2, 231, 2, 189, 2, 212, 2, 187, 2, 185, 2, 135, 2, 114, 2, 124, 2, 124, 2, 118, 2, 115, 2, 110, 2,
    171, 2, 155, 2, 108, 2, 120, 2, 88, 2, 89, 2, 62, 2, 66, 2, 82, 2, 88, 2, 101, 2, 57, 2, 56, 2, 91, 2, 102, 2, 124, 2, 138, 2, 81, 2, 55, 2, 35, 2, 100, 2, 67, 2, 49, 2, 80, 2, 78, 2,
    63, 2, 56, 2, 72, 2, 51, 2, 84, 2, 87, 2, 118, 2, 84, 2, 121, 2, 194, 2, 157, 2, 200, 2, 194, 2, 157, 2, 179, 2, 128, 2, 133, 2, 154, 2, 143, 2, 150, 2, 155, 2, 205, 2, 192, 2, 218, 2, 219, 2,
    192, 2, 182, 2, 158, 2, 127, 2, 160, 2, 181, 2, 207, 2, 191, 2, 137, 2, 230, 2, 228, 2, 253, 2, 8, 3, 222, 2, 96, 3, 5, 3, 83, 3, 122, 3, 43, 4, 27, 5, 141, 7, 103, 12, 117, 17, 255, 17, 194, 13,
    209, 7, 245, 3, 167, 2, 83, 2, 75, 2, 27, 2, 98, 2, 17, 2, 79, 2, 74, 2, 86, 2, 121, 2, 114, 2, 104, 2, 79, 2, 42, 2, 81, 2, 85, 2, 76, 2, 57, 2, 56, 2, 42, 2, 70, 2, 72, 2, 87, 2,
    86, 2, 60, 2, 73, 2, 54, 2, 74, 2, 57, 2, 57, 2, 23, 2, 49, 2, 68, 2, 65, 2, 68, 2, 98, 2, 66, 2, 76, 2, 80, 2, 69, 2, 81, 2, 76, 2, 101, 2, 89, 2, 87, 2, 59, 2, 93, 2, 107, 2,
    72, 2, 77, 2, 89, 2, 111, 2, 102, 2, 113, 2, 131, 2, 142, 2, 69, 2, 118, 2, 118, 2, 125, 2, 98, 2, 121, 2, 128, 2, 164, 2, 125, 2, 161, 2, 163, 2, 142, 2, 202, 2, 170, 2, 174, 2, 150, 2, 147, 2,
    151, 2, 153, 2, 239, 2, 234, 3, 239, 6, 2, 12, 76, 15, 78, 15, 230, 10, 211, 5, 101, 3, 101, 2, 36, 2, 42, 2, 76, 2, 29, 2, 13, 2, 13, 2, 38, 2, 70, 2, 74, 2, 67, 2, 54, 2, 64, 2, 50, 2,
    59, 2, 27, 2, 8, 2, 25, 2, 32, 2, 75, 2, 51, 2, 25, 2, 24, 2, 96, 2, 76, 2, 111, 2, 140, 2, 120, 2, 52, 2, 17, 2, 7, 2, 46, 2, 76, 2, 40, 2, 34, 2, 75, 2, 87, 2, 29, 2, 47, 2,
    52, 2, 63, 2, 28, 2, 81, 2, 21, 2, 104, 2, 85, 2, 63, 2, 126, 2, 63, 2, 100, 2, 112, 2, 137, 2, 104, 2, 92, 2, 94, 2, 142, 2, 130, 2, 123, 2, 67, 2, 111, 2, 84, 2, 78, 2, 120, 2, 86, 2,
    131, 2, 82, 2, 140, 2, 116, 2, 139, 2, 152, 2, 78, 2, 74, 2, 122, 2, 123, 2, 207, 2, 215, 2, 172, 2, 153, 2, 140, 2, 98, 2, 99, 2, 61, 2, 96, 2, 122, 2, 16, 2, 83, 2, 34, 2, 70, 2, 51, 2,
    4, 2, 39, 2, 64, 2, 82, 2, 25, 2, 93, 2, 128, 2, 112, 2, 84, 2, 105, 2, 97, 2, 77, 2, 58, 2, 86, 2, 43, 2, 117, 2, 72, 2, 46, 2, 53, 2, 102, 2, 24, 2, 49, 2, 79, 2, 68, 2, 75, 2,
    251, 1, 116, 2, 69, 2, 74, 2, 51, 2, 68, 2, 15, 2, 37, 2, 60, 2, 114, 2, 39, 2, 38, 2, 48, 2, 72, 2, 29, 2, 17, 2, 40, 2, 30, 2, 49, 2, 49, 2, 38, 2, 67, 2, 253, 1, 252, 1, 36, 2,
    31, 2, 16, 2, 38, 2, 51, 2, 17, 2, 38, 2, 62, 2, 9, 2, 19, 2, 43, 2, 9, 2, 37, 2, 14, 2, 250, 1, 229, 1, 216, 1, 3, 2, 5, 2, 248, 1, 231, 1, 28, 2, 248, 1, 24, 2, 243, 1, 14, 2,
    214, 1, 9, 2, 12, 2, 205, 1, 234, 1, 218, 1, 231, 1, 226, 1, 170, 1, 176, 1, 191, 1, 209, 1, 194, 1, 203, 1, 181, 1, 188, 1, 190, 1, 239, 1, 230, 1, 205, 1, 220, 1, 253, 1, 185, 1, 160, 1, 190, 1,
    176, 1, 175, 1, 186, 1, 207, 1, 199, 1, 177, 1, 136, 1, 204, 1, 176, 1, 152, 1, 156, 1, 210, 1, 161, 1, 232, 1, 192, 1, 194, 1, 188, 1, 194, 1, 186, 1, 158, 1, 166, 1, 139, 1, 127, 1, 143, 1, 160, 1,
    136, 1, 159, 1, 140, 1, 148, 1, 152, 1, 138, 1, 162, 1, 142, 1, 113, 1, 166, 1, 115, 1, 163, 1, 115, 1, 107, 1, 141, 1, 130, 1, 127, 1, 153, 1, 108, 1, 171, 1, 161, 1, 162, 1, 163, 1, 161, 1, 121, 1,
    115, 1, 118, 1, 118, 1, 127, 1, 120, 1, 218, 1, 160, 2, 122, 3, 230, 3, 243, 3, 188, 2, 199, 1, 172, 1, 147, 1, 91, 1, 120, 1, 118, 1, 112, 1, 73, 1, 61, 1, 101, 1, 64, 1, 129, 1, 72, 1, 86, 1,
    119, 1, 47, 1, 65, 1, 104, 1, 88, 1, 124, 1, 75, 1, 77, 1, 101, 1, 80, 1, 68, 1, 103, 1, 110, 1, 77, 1, 79, 1, 76, 1, 94, 1, 65, 1, 43, 1, 73, 1, 67, 1, 59, 1, 23, 1, 85, 1, 57, 1,
    38, 1, 57, 1, 105, 1, 48, 1, 78, 1, 62, 1, 46, 1, 69, 1, 36, 1, 89, 1, 87, 1, 46, 1, 83, 1, 66, 1, 73, 1, 76, 1, 83, 1, 49, 1, 81, 1, 99, 1, 218, 1, 130, 2, 221, 3, 140, 4, 101, 4,
    1, 3, 7, 2, 122, 1, 78, 1, 73, 1, 69, 1, 39, 1, 80, 1, 76, 1, 80, 1, 29, 1, 58, 1, 57, 1, 53, 1, 59, 1, 74, 1, 65, 1, 41, 1, 95, 1, 86, 1, 52, 1, 97, 1, 55, 1, 50, 1, 68, 1,
    45, 1, 40, 1, 76, 1, 42, 1, 40, 1, 62, 1, 61, 1, 56, 1, 23, 1, 32, 1, 71, 1, 89, 1, 110, 1, 91, 1, 97, 1, 77, 1, 78, 1, 40, 1, 38, 1, 51, 1, 82, 1, 81, 1, 61, 1, 141, 1, 99, 2,
    60, 4, 215, 6, 248, 8, 187, 8, 94, 6, 145, 3, 10, 2, 80, 1, 84, 1, 60, 1, 64, 1, 28, 1, 46, 1, 43, 1, 77, 1, 76, 1, 18, 1, 43, 1, 28, 1, 57, 1, 59, 1, 45, 1, 34, 1, 33, 1, 52, 1,
    24, 1, 45, 1, 52, 1, 40, 1, 18, 1, 45, 1, 38, 1, 21, 1, 31, 1, 25, 1, 43, 1, 73, 1, 76, 1, 40, 1, 13, 1, 88, 1, 32, 1, 68, 1, 35, 1, 52, 1, 88, 1, 54, 1, 35, 1, 39, 1, 44, 1,
    21, 1, 60, 1, 41, 1, 56, 1, 34, 1, 41, 1, 59, 1, 32, 1, 76, 1, 63, 1, 70, 1, 52, 1, 35, 1, 49, 1, 42, 1, 39, 1, 43, 1, 46, 1, 13, 1, 57, 1, 50, 1, 43, 1, 15, 1, 82, 1, 59, 1,
    74, 1, 14, 1, 49, 1, 26, 1, 47, 1, 50, 1, 38, 1, 40, 1, 152, 1, 188, 2, 177, 4, 120, 7, 159, 8, 81, 7, 185, 4, 80, 2, 97, 1, 37, 1, 213, 236, 1, 1, 232, 242, 222, 241, 4, 1, 235, 249, 2, 1,
    6, 1, 60, 1, 158, 1, 172, 2, 22, 6, 81, 13, 36, 21, 139, 24, 227, 19, 27, 12, 65, 5, 240, 1, 247, 181, 160, 155, 178, 173, 176, 171, 169, 181, 185, 177, 187, 198, 156, 165, 185, 185, 232, 221, 202, 207, 180, 205, 161, 188,
    165, 165, 165, 134, 149, 146, 145, 151, 167, 151, 159, 158, 162, 144, 171, 172, 165, 149, 127, 171, 168, 190, 239, 156, 1, 175, 2, 140, 3, 165, 3, 230, 2, 212, 1, 21, 1, 179, 136, 152, 136, 155, 156, 147, 142, 135, 138, 139, 143, 152,
    140, 143, 162, 137, 115, 146, 144, 138, 142, 138, 140, 129, 126, 126, 140, 152, 140, 135, 137, 137, 133, 141, 141, 156, 136, 142, 140, 145, 140, 137, 135, 122, 129, 144, 143, 133, 173, 242, 253, 234, 225, 182, 165, 130, 119, 136, 130, 125, 132, 143,
    125, 109, 121, 126, 144, 134, 136, 112, 108, 121, 130, 131, 94, 115, 124, 103, 147, 121, 127, 125, 130, 134, 125, 132, 141, 119, 129, 127, 141, 131, 127, 117, 136, 117, 122, 135, 135, 128, 153, 143, 149, 141, 130, 128, 123, 140, 149, 127, 127, 101,
    125, 139, 165, 207, 7, 1, 38, 1, 16, 1, 206, 168, 141, 128, 113, 137, 133, 126, 133, 123, 112, 133, 135, 126, 127, 117, 125, 151, 120, 101, 121, 137, 128, 121, 123, 129, 147, 138, 121, 123, 143, 124, 121, 111, 138, 142, 125, 137, 118, 115,
    117, 115, 130, 145, 121, 137, 126, 128, 135, 135, 124, 149, 135, 120, 112, 142, 119, 140, 113, 120, 121, 130, 119, 114, 104, 131, 108, 123, 128, 127, 105, 99, 119, 127, 105, 110, 126, 138, 93, 105, 107, 115, 111, 108, 105, 107, 114, 122, 133, 112,
    125, 130, 124, 106, 119, 117, 110, 134, 96, 93, 98, 94, 101, 122, 111, 119, 108, 107, 108, 89, 109, 98, 115, 115, 120, 116, 112, 123, 111, 129, 145, 133, 137, 131, 145, 163, 224, 237, 164, 1, 214, 2, 179, 4, 180, 7, 49, 12, 210, 16,
    223, 19, 152, 21, 29, 19, 149, 14, 250, 9, 103, 6, 71, 3, 193, 1, 225, 155, 92, 98, 77, 73, 75, 81, 69, 74, 70, 77, 76, 62, 84, 72, 69, 73, 58, 80, 59, 72, 66, 75, 66, 78, 68, 76, 68, 71, 56, 72, 70, 68,
    62, 71, 70, 66, 72, 62, 81, 57, 69, 88, 61, 73, 76, 60, 68, 70, 72, 68, 70, 77, 71, 63, 61, 68, 46, 70, 55, 64, 63, 72, 63, 54, 62, 71, 61, 67, 60, 74, 55, 61, 75, 66, 70, 77, 74, 70, 65, 55, 75, 64,
    75, 59, 76, 73, 74, 61, 71, 61, 67, 70, 67, 72, 56, 76, 57, 63, 76, 80, 87, 69, 63, 56, 83, 84, 67, 71, 77, 67, 69, 78, 55, 64, 50, 63, 72, 61, 66, 72, 62, 58, 79, 66, 69, 77, 60, 74, 63, 65, 72, 77,
    69, 64, 70, 75, 83, 71, 87, 77, 73, 62, 83, 74, 81, 101, 84, 75, 76, 63, 48, 68, 44, 69, 56, 61, 72, 70, 57, 73, 74, 45, 71, 54, 75, 60, 48, 57, 61, 67, 54, 61, 59, 61, 65, 61, 48, 76, 61, 65, 58, 61,
    63, 61, 47, 61, 43, 67, 75, 68, 84, 88, 91, 72, 69, 57, 74, 59, 59, 69, 64, 56, 65, 71, 52, 53, 62, 59, 60, 67, 58, 52, 52, 70, 57, 71, 54, 65, 60, 74, 50, 68, 51, 67, 62, 58, 63, 48, 61, 70, 77, 66,
    57, 54, 69, 63, 68, 56, 50, 60, 65, 54, 68, 65, 65, 55, 67, 54, 51, 61, 56, 71, 59, 61, 72, 62, 53, 51, 56, 53, 60, 64, 69, 61, 61, 53, 64, 50, 81, 51, 61, 61, 47, 64, 68, 63, 58, 55, 58, 74, 70, 51,
    68, 56, 69, 84, 49, 77, 65, 66, 79, 91, 101, 132, 247, 200, 1, 17, 4, 156, 6, 204, 8, 69, 8, 102, 6, 170, 3, 145, 1, 144, 77, 55, 51, 62, 41, 48, 57, 43, 47, 51, 49, 40, 38, 41, 38, 34, 51, 47, 52, 49,
    48, 50, 49, 51, 52, 43, 47, 44, 57, 58, 59, 79, 57, 50, 42, 52, 45, 45, 50, 45, 44, 48, 53, 38, 49, 48, 44, 62, 78, 76, 80, 65, 62, 67, 49, 51, 42, 46, 54, 52, 52, 45, 49, 49, 44, 44, 41, 58, 44, 46,
    41, 30, 37, 45, 45, 43, 38, 47, 55, 39, 35, 41, 54, 46, 45, 48, 54, 48, 41, 38, 40, 58, 44, 53, 32, 46, 46, 53, 58, 41, 54, 42, 41, 38, 40, 41, 50, 42, 45, 41, 52, 48, 50, 64, 55, 58, 48, 49, 46, 44,
    46, 50, 38, 50, 54, 44, 61, 45, 59, 50, 34, 48, 56, 47, 37, 52, 46, 56, 51, 37, 42, 46, 43, 42, 41, 40, 56, 46, 43, 43, 44, 36, 39, 58, 57, 48, 49, 39, 52, 47, 49, 44, 57, 44, 53, 40, 37, 53, 56, 40,
    40, 51, 48, 44, 45, 49, 42, 48, 43, 30, 50, 58, 47, 38, 51, 45, 48, 57, 55, 43, 46, 38, 58, 46, 33, 46, 51, 53, 52, 52, 59, 51, 48, 67, 59, 55, 61, 51, 54, 44, 47, 47, 42, 51, 53, 50, 55, 39, 41, 39,
    54, 37, 53, 40, 56, 44, 46, 41, 57, 57, 88, 139, 37, 1, 168, 1, 173, 1, 178, 1, 59, 1, 225, 113, 65, 38, 38, 47, 40, 37, 33, 40, 39, 38, 38, 61, 41, 45, 54, 48, 52, 35, 41, 25, 51, 43, 36, 44, 45, 42,
    46, 57, 44, 47, 48, 49, 39, 40, 33, 33, 40, 47, 40, 31, 53, 24, 43, 42, 48, 41, 54, 28, 41, 51, 43, 44, 39, 23, 41, 30, 40, 44, 52, 50, 67, 45, 40, 50, 32, 39, 47, 39, 43, 51, 47, 46, 34, 37, 36, 47,
    34, 43, 51, 34, 37, 46, 44, 50, 41, 41, 43, 49, 38, 37, 39, 39, 39, 48, 47, 40, 45, 39, 56, 49, 38, 32, 41, 47, 49, 72, 93, 155, 207, 125, 1, 114, 1, 71, 1, 222, 127, 83, 57, 48, 47, 50, 47, 34, 44, 54,
    40, 34, 39, 44, 39, 44, 38, 44, 35, 37, 60, 43, 42, 27, 57, 47, 45, 47, 60, 43, 40, 43, 46, 31, 33, 47, 35, 40, 39, 42, 53, 32, 46, 48, 51, 51, 39, 44, 44, 43, 51, 46, 49, 50, 36, 42, 39, 43, 54, 68,
    91, 114, 142, 179, 148, 126, 85, 66, 53, 51, 44, 37, 37, 41, 47, 45, 44, 42, 36, 55, 48, 51, 44, 38, 33, 46, 29, 40, 39, 41, 42, 38, 35, 32, 46, 34, 43, 47, 50, 56, 45, 48, 43, 38, 39, 40, 46, 41, 31, 40,
    43, 32, 39, 43, 44, 40, 34, 45, 30, 46, 40, 28, 43, 50, 41, 37, 39, 47, 51, 29, 45, 42, 36, 45, 40, 57, 39, 45, 45, 40, 32, 53, 55, 41, 45, 44, 42, 25, 41, 37, 47, 43, 55, 51, 50, 48, 47, 33, 36, 39,
    34, 33, 41, 47, 48, 33, 40, 46, 49, 58, 60, 52, 50, 35, 58, 46, 37, 43, 35, 34, 43, 33, 43, 44, 54, 57, 46, 48, 39, 32, 40, 29, 47, 44, 31, 52, 40, 37, 43, 37, 47, 30, 42, 42, 40, 50, 35, 45, 48, 39,
    39, 42, 44, 46, 35, 39, 37, 39, 45, 42, 37, 36, 32, 46, 49, 37, 33, 40, 53, 26, 32, 57, 41, 38, 45, 44, 48, 40, 47, 37, 42, 33, 41, 45, 36, 32, 43, 55, 33, 57, 47, 58, 52, 47, 54, 61, 87, 121, 184, 43,
    1, 164, 1, 203, 1, 108, 1, 216, 155, 73, 57, 29, 40, 54, 40, 43, 47, 47, 42, 40, 44, 37, 40, 36, 48, 46, 43, 38, 39, 53, 32, 31, 37, 47, 42, 58, 49, 38, 45, 51, 43, 31, 39, 38, 33, 48, 39, 37, 35, 44,
    52, 41, 38, 30, 39, 43, 49, 36, 41, 38, 42, 41, 36, 40, 38, 39, 53, 51, 39, 48, 44, 43, 39, 38, 48, 41, 46, 50, 27, 37, 51, 46, 41, 45, 37, 44, 53, 52, 58, 56, 56, 54, 52, 42, 36, 51, 40, 42, 58, 49,
    59, 41, 50, 49, 40, 45, 37, 32, 46, 45, 44, 42, 51, 48, 49, 40, 45, 44, 36, 57, 48, 51, 44, 53, 43, 40, 46, 45, 48, 44, 42, 38, 41, 41, 51, 51, 37, 55, 45, 54, 55, 46, 53, 44, 57, 43, 49, 60, 42, 41,
    52, 47, 38, 48, 48, 35, 49, 45, 58, 41, 54, 43, 52, 47, 42, 33, 58, 47, 42, 50, 43, 59, 45, 65, 52, 41, 43, 53, 47, 60, 46, 43, 52, 47, 47, 61, 56, 43, 50, 51, 30, 44, 40, 42, 42, 44, 46, 58, 49, 49,
    51, 47, 33, 43, 52, 47, 46, 41, 49, 31, 34, 45, 43, 32, 39, 39, 42, 44, 36, 31, 33, 33, 32, 32, 32, 32, 32, 31, 35, 32, 44, 40, 40, 37, 34, 30, 54, 44, 27, 32, 41, 41, 30, 42, 39, 39, 44, 40, 55, 65,
    110, 172, 241, 18, 1, 27, 1, 213, 146, 69, 77, 57, 75, 58, 72, 64, 57, 38, 33, 31, 25, 39, 31, 27, 27, 23, 32, 20, 19, 24, 27, 29, 27, 27, 25, 24, 32, 24, 24, 26, 23, 27, 32, 29, 22, 22, 42, 35, 24, 32,
    32, 32, 34, 44, 90, 101, 144, 230, 71, 1, 71, 1, 35, 1, 208, 128, 61, 25, 27, 36, 26, 27, 21, 33, 31, 21, 27, 30, 28, 34, 36, 25, 25, 13, 25, 24, 25, 27, 23, 28, 23, 24, 22, 21, 21, 20, 22, 30, 21, 29,
    21, 18, 17, 28, 25, 38, 21, 24, 18, 16, 20, 24, 25, 19, 14, 22, 35, 29, 24, 20, 25, 26, 23, 15, 25, 17, 27, 23, 28, 26, 20, 13, 26, 28, 15, 22, 26, 25, 25, 20, 22, 23, 23, 22, 22, 22, 28, 24, 15, 19,
    18, 25, 28, 19, 22, 21, 18, 19, 19, 13, 20, 13, 17, 21, 27, 26, 22, 12, 26, 25, 19, 15, 13, 18, 21, 28, 17, 16, 25, 12, 20, 16, 23, 23, 23, 18, 26, 16, 23, 19, 18, 18, 20, 22, 23, 20, 20, 17, 15, 17,
    17, 17, 15, 17, 15, 15, 29, 15, 19, 24, 15, 17, 17, 15, 13, 13, 19, 16, 17, 17, 14, 20, 17, 16, 21, 14, 13, 25, 15, 17, 17, 7, 13, 16, 4, 14, 13, 15, 18, 18, 18, 13, 7, 11, 10, 11, 9, 14, 12, 14,
    18, 15, 11, 17, 15, 9, 21, 12, 17, 9, 12, 14, 16, 22, 11, 24, 25, 49, 49, 41, 41, 26, 19, 12, 15, 13, 13, 16, 12, 12, 10, 13, 15, 9, 12, 9, 16, 17, 11, 16, 22, 13, 11, 6, 16, 16, 13, 14, 11, 13,
    15, 14, 13, 12, 21, 17, 17, 12, 12, 8, 12, 17, 15, 7, 9, 13, 16, 21, 16, 10, 21, 13, 25, 20, 15, 15, 9, 10, 16, 13, 13, 9, 12, 8, 13, 18, 6, 15, 7, 8, 17, 11, 10, 18, 16, 18, 12, 12, 13, 10,
    16, 10, 11, 14, 13, 11, 8, 12, 5, 11, 17, 11, 9, 14, 12, 10, 14, 17, 5, 12, 11, 16, 19, 12, 16, 12, 16, 17, 15, 25, 24, 23, 22, 26, 35, 41, 62, 78, 110, 208, 202, 1, 37, 3, 187, 4, 131, 6, 158, 6,
    202, 5, 230, 3, 95, 2, 239, 93, 32, 9, 12, 7, 7, 6, 7, 11, 8, 5, 9, 6, 9, 5, 2, 2, 9, 2, 4, 6, 11, 6, 5, 9, 9, 6, 3, 6, 2, 10, 8, 9, 6, 8, 5, 6, 6, 10, 16, 13, 14, 30,
    32, 27, 41, 31, 25, 18, 10, 9, 7, 5, 4, 7, 6, 3, 3, 5, 4, 5, 5, 4, 5, 8, 6, 5, 5, 6, 3, 5, 3, 4, 8, 8, 0, 1, 4, 2, 4, 3, 7, 5, 3, 4, 5, 3, 7, 4, 12, 6, 6, 7,
    5, 2, 6, 5, 7, 3, 6, 3, 2, 4, 6, 8, 4, 6, 5, 5, 2, 9, 1, 5, 5, 3, 10, 5, 4, 6, 1, 5, 2, 4, 7, 8, 6, 1, 5, 3, 6, 2, 6, 6, 5, 6, 3, 3, 0, 1, 4, 5, 2, 4,
    2, 6, 5, 2, 6, 6, 4, 4, 1, 2, 4, 7, 4, 5, 0, 1, 2, 1, 2, 4, 2, 3, 4, 4, 2, 2, 2, 3, 2, 2, 6, 3, 2, 3, 3, 3, 5, 4, 1, 4, 3, 2, 3, 3, 2, 6, 4, 4, 3, 6,
    1, 2, 2, 5, 1, 2, 4, 4, 4, 3, 7, 7, 3, 4, 8, 8, 7, 2, 5, 4, 5, 2, 1, 4, 1, 0, 1, 6, 2, 3, 2, 5, 4, 2, 2, 6, 0, 1, 1, 3, 2, 7, 3, 5, 2, 4, 5, 1, 7, 4,
    7, 4, 6, 5, 3, 2, 4, 6, 2, 1, 3, 6, 4, 5, 6, 6, 3, 7, 7, 10, 8, 9, 11, 35, 36, 72, 117, 215, 30, 1, 143, 1, 153, 1, 96, 1, 230, 114, 62, 15, 7, 6, 2, 4, 1, 4, 1, 1, 2, 2,
    2, 2, 4, 5, 2, 2, 2, 2, 1, 1, 1, 1, 1, 5, 4, 2, 0, 1, 2, 1, 4, 0, 1, 1, 6, 1, 4, 2, 4, 1, 1, 2, 2, 3, 4, 3, 2, 0, 1, 2, 3, 0, 1, 3, 1, 4, 2, 4, 1, 3,
    2, 3, 0, 1, 2, 1, 0, 1, 2, 0, 1, 5, 3, 5, 3, 2, 1, 4, 1, 3, 0, 1, 5, 1, 1, 2, 3, 2, 0, 1, 1, 3, 4, 3, 2, 2, 2, 4, 4, 6, 7, 6, 6, 13, 17, 18, 19, 33, 52, 93,
    97, 111, 103, 63, 29, 21, 6, 6, 0, 1, 4, 0, 1, 1, 1, 0, 1, 2, 0, 1, 2, 0, 1, 1, 0, 1, 2, 0, 2, 1, 3, 4, 5, 4, 2, 3, 5, 3, 3, 3, 3, 1, 0, 1, 1, 4, 0, 1, 2, 2,
    2, 2, 3, 1, 2, 1, 1, 0, 1, 1, 3, 0, 1, 4, 1, 0, 1, 1, 1, 0, 2, 4, 2, 0, 3, 1, 1, 1, 1, 0, 1, 2, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 2, 1, 2, 3, 0, 1, 2, 6,
    1, 2, 3, 0, 1, 3, 1, 3, 3, 2, 1, 1, 6, 3, 2, 2, 5, 1, 1, 1, 1, 1, 2, 3, 0, 1, 2, 4, 2, 1, 1, 5, 1, 0, 1, 1, 4, 0, 2, 1, 1, 0, 1, 1, 2, 1, 0, 1, 1, 0,
    1, 1, 0, 1, 3, 3, 2, 2, 5, 5, 11, 2, 3, 6, 2, 2, 2, 1, 0, 1, 3, 2, 0, 2, 2, 0, 2, 1, 1, 0, 1, 1, 1, 1, 2, 2, 3, 2, 0, 1, 4, 0, 1, 1, 2, 3, 0, 1, 1, 0,
    2, 4, 1, 2, 2, 2, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 2, 1, 2, 2, 2, 1, 1, 1, 3, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 4, 0, 1, 2, 1, 0, 1, 2, 0, 1, 2,
    1, 0, 1, 1, 0, 1, 3, 0, 1, 2, 3, 1, 0, 1, 1, 0, 1, 1, 1, 2, 1, 0, 1, 3, 2, 2, 0, 1, 2, 4, 1, 1, 3, 0, 1, 2, 0, 1, 3, 0, 1, 3, 2, 0, 2, 2, 0, 3, 2, 3,
    1, 1, 2, 2, 2, 2, 3, 2, 2, 0, 2, 2, 2, 0, 1, 1, 1, 1, 5, 0, 1, 4, 1, 2, 1, 1, 0, 1, 1, 1, 0, 1, 2, 1, 0, 1, 2, 0, 1, 1, 2, 0, 1, 2, 0, 2, 1, 2, 0, 1,
    1, 3, 1, 0, 1, 2, 1, 0, 1, 4, 3, 0, 1, 1, 1, 0, 1, 1, 0, 2, 3, 0, 1, 2, 1, 0, 1, 1, 2, 0, 1, 2, 0, 2, 2, 3, 0, 1, 2, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0,
    1, 1, 1, 0, 2, 1, 1, 0, 1, 4, 2, 0, 3, 1, 1, 0, 1, 1, 2, 1, 0, 1, 2, 2, 0, 1, 1, 3, 1, 2, 0, 1, 1, 0, 1, 1, 0, 4, 1, 0, 1, 2, 0, 1, 1, 2, 1, 3, 1, 0,
    1, 2, 2, 0, 3, 3, 0, 6, 1, 0, 2, 1, 2, 0, 1, 1, 0, 1, 2, 1, 1, 3, 1, 0, 1, 1, 1, 3, 0, 5, 1, 2, 2, 1, 0, 1, 1, 1, 0, 2, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2,
    0, 8, 1, 0, 2, 1, 1, 3, 0, 1, 1, 0, 2, 1, 0, 3, 2, 1, 2, 1, 0, 2, 1, 2, 2, 0, 4, 1, 1, 0, 4, 1, 1, 1, 0, 1, 3, 3, 0, 4, 1, 0, 3, 2, 0, 1, 1, 1, 0, 1,
    2, 2, 1, 1, 0, 2, 2, 0, 1, 1, 0, 1, 1, 0, 1, 2, 2, 0, 3, 2, 0, 1, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 3, 2, 0, 10, 1, 1, 0, 1, 2, 0, 1, 1, 1, 0, 1, 3, 1, 0,
    1, 1, 4, 4, 1, 2, 2, 0, 6, 1, 0, 3, 3, 0, 1, 3, 0, 1, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 0, 2, 3, 0, 1, 1, 0, 2, 1, 1, 1, 0, 4, 1, 0, 2, 1,
    0, 3, 1, 3, 0, 1, 1, 1, 3, 0, 7, 1, 1, 1, 3, 5, 8, 5, 3, 2, 4, 1, 2, 1, 1, 1, 0, 1, 2, 2, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0, 9, 1, 3, 2, 3, 1, 2, 5, 5,
    6, 5, 13, 12, 9, 11, 4, 6, 2, 3, 1, 1, 0, 6, 1, 1, 1, 0, 5, 1, 1, 0, 5, 1, 0, 5, 2, 0, 1, 2, 1, 1, 0, 1, 1, 1, 0, 1, 2, 0, 3, 1, 0, 12, 1, 1, 0, 7, 1, 0,
    4, 1, 0, 3, 1, 0, 5, 2, 0, 2, 1, 0, 7, 1, 0, 2, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 0, 7, 1, 0, 5, 2, 3, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 2, 0, 1, 2,
    1, 0, 1, 1, 0, 2, 1, 1, 1, 0, 3, 1, 0, 7, 1, 0, 5, 1, 1, 0, 5, 1, 0, 3, 1, 0, 10, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 6, 1, 0, 4, 1, 1, 0, 4, 1, 0, 2,
    1, 0, 2, 1, 1, 1, 1, 0, 1, 1, 0, 2, 2, 0, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 1, 1, 0, 9, 1,
    1, 0, 1, 2, 0, 4, 1, 0, 3, 3, 0, 2, 1, 0, 1, 1, 0, 4, 1, 0, 1, 2, 0, 7, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 1, 1, 1, 0, 17, 1, 0, 5, 1, 1, 0, 1, 1, 0, 1, 1,
    0, 3, 1, 0, 3, 1, 0, 4, 1, 1, 0, 11, 1, 1, 0, 4, 2, 0, 4, 2, 1, 0, 5, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 8, 1, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 1, 0, 3,
    1, 1, 0, 4, 1, 1, 0, 9, 1, 0, 2, 2, 0, 6, 1, 0, 11, 1, 1, 1, 1, 0, 1, 2, 0, 9, 1, 0, 1, 1, 0, 10, 1, 0, 22, 1, 0, 3, 1, 0, 3, 2, 0, 3, 2, 0, 1, 1, 0, 2,
    2, 0, 4, 1, 0, 3, 1, 1, 1, 0, 4, 1, 1, 0, 1, 1, 0, 6, 1, 0, 1, 2, 0, 1, 1, 0, 6, 1, 0, 2, 2, 0, 4, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 2, 0, 2, 1, 2,
    0, 1, 1, 0, 2, 1, 0, 4, 3, 0, 2, 2, 1, 0, 1, 1, 0, 5, 1, 0, 6, 1, 0, 10, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 1, 0, 11, 1, 0, 4, 1, 0, 1, 3, 0, 2, 1, 0, 1, 3,
    0, 1, 1, 2, 1, 1, 1, 0, 2, 1, 0, 7, 1, 1, 0, 2, 1, 0, 4, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 22, 1, 1, 0, 5, 1, 0, 1, 1, 1, 1, 1, 0, 8, 1, 0, 1, 1, 1, 0,
    5, 1, 0, 10, 1, 0, 2, 1, 0, 4, 1, 0, 2, 1, 0, 6, 1, 0, 6, 1, 1, 0, 1, 1, 0, 4, 2, 0, 1, 1, 0, 4, 2, 0, 9, 1, 1, 0, 6, 1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 4,
    1, 0, 4, 1, 0, 1, 1, 0, 5, 2, 0, 1, 1, 0, 4, 1, 1, 0, 2, 1, 0, 4, 1, 1, 0, 1, 1, 0, 1, 1, 1, 3, 3, 4, 1, 0, 1, 3, 0, 3, 1, 0, 1, 1, 0, 1, 1, 0, 7, 1,
    1, 0, 2, 1, 0, 4, 1, 0, 4, 1, 0, 4, 2, 0, 2, 1, 0, 5, 1, 0, 6, 1, 0, 5, 1, 0, 15, 1, 0, 4, 1, 0, 6, 1, 0, 4, 1, 2, 0, 7, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1,
    0, 2, 1, 1, 0, 4, 1, 0, 5, 1, 0, 3, 1, 0, 1, 2, 0, 7, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2, 1, 0, 5, 3, 0, 4, 1, 2, 0, 5, 1, 0, 6, 1, 0, 1,
    1, 1, 1, 0, 4, 1, 0, 4, 2, 0, 5, 1, 0, 6, 1, 0, 2, 2, 0, 2, 1, 0, 13, 1, 0, 1, 1, 0, 6, 1, 0, 4, 1, 0, 2, 2, 1, 0, 4, 1, 0, 3, 1, 0, 5, 1, 0, 7, 1, 0,
    10, 1, 0, 4, 1, 0, 1, 1, 0, 4, 2, 0, 3, 1, 0, 2, 1, 0, 1, 1, 4, 0, 1, 1, 0, 8, 1, 0, 7, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 3, 1, 1, 0, 3, 1, 0,
    1, 3, 0, 1, 1, 0, 5, 1, 0, 1, 1, 0, 5, 1, 0, 2, 1, 0, 3, 1, 0, 4, 1, 0, 6, 1, 2, 0, 3, 1, 0, 4, 1, 0, 2, 1, 0, 4, 1, 0, 6, 1, 1, 1, 1, 0, 1, 1, 0, 2,
    1, 0, 4, 1, 0, 3, 1, 0, 8, 1, 1, 0, 8, 1, 0, 19, 1, 0, 4, 1, 0, 3, 1, 0, 4, 1, 0, 10, 1, 0, 3, 1, 0, 6, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 17, 1, 0, 2,
    1, 0, 11, 1, 0, 1, 1, 1, 0, 2, 2, 0, 16, 1, 0, 1, 1, 0, 6, 1, 0, 1, 1, 0, 14, 1, 0, 12, 1, 0, 12, 1, 0, 4, 1, 0, 15, 1, 0, 23, 1, 0, 17, 2, 0, 3, 1, 0, 13, 1,
    0, 30, 1, 0, 16, 1, 0, 10, 1, 0, 7, 1, 0, 5, 1, 0, 4, 1, 0, 2, 1, 0, 31, 1, 0, 6, 1, 0, 2, 1, 0, 4, 1, 0, 6, 1, 1, 0, 1, 1, 0, 2, 2, 4, 4, 4, 3, 6, 6, 12,
    8, 13, 7, 1, 1, 3, 0, 1, 1, 0, 36, 1, 0, 63, 1, 0, 102, 1, 0, 41, 1, 0, 60, 1, 0, 63, 1, 0, 67, 1, 0, 53, 1, 0, 112, 1, 0, 2, 1, 0, 36, 1, 0, 35, 1, 0, 12, 1, 0, 92,
    1, 0, 72, 1, 0, 42, 1, 0, 1, 1, 0, 30, 1, 0, 38, 1, 0, 85, 1, 0, 14, 1, 0, 122, 1, 0, 36, 1, 0, 51, 1, 0, 65, 1, 1, 0, 138, 1, 0, 57, 1, 0, 2, 1, 0, 45, 1, 0, 218, 1,
    0, 32, 1, 0, 13, 1, 0, 173, 1, 0, 41, 1, 0, 130, 1, 0, 52, 2, 1, 0, 17, 1, 0, 64, 1, 0, 236, 1, 0, 14, 1, 0, 31, 1, 0, 72, 1, 0, 97, 1, 0, 4, 1, 0, 100, 1, 0, 231, 1, 0,
    241, 1, 0, 68, 1, 0, 131, 1, 0, 191, 1, 0, 39, 1, 1, 0, 149, 1, 0, 142, 1, 0, 246, 1, 1, 0, 136, 2, 1, 0, 222, 1, 0, 152, 1, 0, 100, 1, 0, 78, 1, 0, 189, 1, 0, 209, 2, 1, 0, 80,
    1, 0, 20, 1, 0, 178, 1, 1, 0, 65, 1, 0, 136, 1, 0, 148, 1, 0, 35, 1, 0, 53, 1, 0, 131, 1, 1, 0, 92, 1, 0, 106, 1, 0, 4, 2, 1, 0, 56, 2, 1, 0, 187, 1, 0, 249, 1, 0, 59, 1,
    0, 122
  };
  assert( test_18_packed.size() == 6902 );
  const vector<uint8_t> test_18_encoded = QRSpecDev::encode_stream_vbyte( test_18_chan_cnts );
  assert( test_18_encoded == test_18_packed );
  vector<uint32_t> test_18_dec;
  const size_t test_18_nbytedec = QRSpecDev::decode_stream_vbyte(test_18_encoded,test_18_dec);
  assert( test_18_nbytedec == test_18_packed.size() );
  assert( test_18_dec == test_18_chan_cnts );
  
  
  
  
  // Test case 19
  const vector<uint32_t> test_19_chan_cnts{
    0, 32, 1, 3, 1, 0, 1, 3, 4, 9, 16, 357, 741, 742, 793, 742, 782, 736, 738, 724,
    709, 769, 695, 713, 755, 727, 710, 720, 746, 715, 651, 681, 712, 703, 678, 668, 704, 687, 668, 666,
    663, 641, 690, 627, 625, 652, 664, 662, 621, 632, 661, 583, 650, 581, 620, 614, 625, 624, 639, 611,
    612, 587, 573, 634, 565, 569, 524, 588, 575, 575, 562, 602, 578, 544, 585, 601, 627, 562, 554, 560,
    546, 552, 586, 577, 576, 574, 600, 649, 707, 779, 909, 1058, 1044, 965, 767, 677, 602, 571, 565, 590,
    561, 581, 603, 572, 581, 556, 660, 818, 913, 922, 780, 757, 724, 692, 614, 579, 559, 559, 516, 576,
    527, 541, 547, 572, 618, 631, 617, 594, 689, 686, 630, 615, 551, 568, 517, 584, 559, 537, 517, 557,
    524, 510, 510, 610, 556, 523, 563, 538, 582, 618, 602, 574, 569, 595, 596, 574, 570, 580, 542, 565,
    590, 541, 623, 631, 576, 563, 540, 568, 559, 549, 580, 569, 534, 532, 551, 582, 522, 543, 552, 526,
    556, 546, 567, 556, 581, 602, 598, 633, 602, 620, 620, 531, 580, 589, 564, 553, 584, 560, 597, 538,
    537, 565, 540, 554, 582, 580, 616, 551, 562, 581, 539, 550, 571, 549, 573, 576, 564, 600, 534, 533,
    567, 548, 560, 553, 572, 614, 563, 563, 599, 592, 561, 558, 575, 639, 602, 652, 701, 585, 599, 547,
    665, 532, 560, 548, 615, 614, 588, 563, 560, 556, 560, 582, 563, 572, 532, 525, 583, 583, 602, 564,
    557, 553, 594, 597, 580, 604, 608, 544, 571, 543, 537, 547, 591, 593, 580, 560, 564, 533, 550, 542,
    530, 548, 563, 545, 577, 589, 656, 591, 589, 546, 597, 545, 534, 579, 570, 530, 532, 598, 592, 569,
    555, 591, 550, 530, 526, 552, 573, 580, 585, 519, 584, 566, 547, 554, 559, 513, 527, 558, 566, 589,
    490, 569, 560, 557, 490, 500, 532, 580, 535, 570, 569, 520, 579, 577, 543, 572, 551, 589, 538, 538,
    538, 558, 580, 544, 534, 537, 545, 532, 572, 600, 541, 553, 601, 556, 564, 553, 566, 557, 534, 594,
    536, 584, 529, 539, 554, 549, 494, 517, 539, 559, 554, 537, 559, 536, 556, 562, 573, 561, 547, 532,
    535, 569, 534, 535, 549, 524, 540, 577, 581, 570, 538, 530, 528, 555, 545, 569, 630, 720, 860, 940,
    966, 676, 614, 597, 533, 574, 535, 520, 548, 548, 547, 561, 516, 540, 581, 526, 526, 548, 535, 531,
    531, 532, 535, 555, 536, 532, 557, 501, 531, 537, 516, 567, 538, 520, 543, 540, 568, 524, 564, 522,
    534, 548, 534, 526, 536, 506, 517, 542, 544, 555, 544, 520, 567, 519, 573, 720, 1172, 2596, 4446, 6151,
    5408, 3650, 1981, 1237, 1015, 905, 717, 547, 461, 458, 443, 424, 456, 470, 437, 420, 483, 417, 445, 430,
    433, 473, 428, 448, 464, 432, 429, 433, 435, 443, 445, 405, 435, 448, 430, 441, 410, 457, 397, 416,
    451, 465, 427, 395, 449, 427, 414, 445, 422, 438, 415, 432, 447, 421, 430, 430, 426, 416, 411, 447,
    445, 541, 675, 804, 929, 813, 660, 486, 425, 443, 425, 428, 398, 417, 435, 440, 522, 567, 647, 661,
    599, 527, 458, 421, 408, 418, 397, 430, 422, 432, 403, 430, 399, 384, 391, 381, 393, 389, 455, 435,
    452, 423, 401, 366, 431, 414, 426, 399, 388, 403, 401, 385, 409, 424, 415, 438, 436, 418, 374, 393,
    376, 423, 434, 613, 774, 869, 814, 672, 460, 410, 425, 374, 388, 368, 353, 407, 380, 353, 375, 405,
    379, 375, 379, 366, 383, 371, 364, 324, 362, 393, 366, 367, 371, 338, 347, 358, 343, 351, 406, 355,
    323, 362, 352, 359, 360, 350, 363, 411, 369, 416, 389, 380, 395, 361, 367, 350, 364, 375, 395, 418,
    563, 767, 906, 838, 598, 462, 386, 353, 371, 384, 418, 414, 387, 367, 394, 367, 321, 360, 413, 400,
    633, 1002, 1803, 2201, 2097, 1514, 880, 541, 434, 391, 384, 353, 335, 318, 327, 352, 324, 318, 331, 304,
    301, 293, 318, 285, 311, 317, 310, 307, 353, 349, 362, 390, 372, 354, 353, 306, 297, 346, 331, 304,
    301, 278, 299, 326, 321, 308, 282, 321, 300, 282, 299, 310, 310, 290, 300, 281, 335, 324, 319, 316,
    312, 296, 310, 319, 290, 287, 321, 292, 318, 323, 318, 293, 336, 308, 319, 302, 316, 282, 287, 304,
    328, 322, 317, 297, 302, 314, 328, 332, 312, 336, 324, 313, 318, 306, 285, 269, 312, 298, 292, 313,
    327, 308, 296, 288, 340, 297, 303, 330, 304, 286, 320, 293, 281, 290, 295, 314, 283, 266, 292, 277,
    294, 280, 291, 291, 303, 280, 295, 304, 283, 293, 287, 286, 293, 308, 303, 260, 281, 278, 265, 263,
    263, 291, 301, 260, 270, 283, 355, 430, 534, 554, 509, 445, 342, 297, 262, 250, 278, 229, 253, 256,
    258, 234, 255, 265, 270, 250, 306, 288, 241, 259, 242, 253, 253, 257, 256, 231, 223, 255, 239, 258,
    245, 243, 220, 248, 246, 244, 268, 266, 247, 232, 208, 240, 234, 258, 231, 242, 232, 244, 250, 239,
    246, 234, 228, 226, 237, 236, 233, 226, 248, 243, 239, 223, 285, 250, 241, 246, 229, 221, 236, 220,
    257, 213, 231, 213, 249, 226, 248, 230, 208, 229, 233, 228, 224, 210, 242, 217, 232, 244, 276, 277,
    270, 233, 237, 223, 226, 225, 219, 221, 222, 188, 216, 200, 234, 234, 225, 293, 388, 610, 848, 900,
    824, 562, 336, 271, 210, 215, 222, 202, 231, 222, 211, 194, 207, 218, 191, 212, 199, 201, 209, 217,
    232, 220, 221, 213, 197, 197, 199, 232, 200, 228, 245, 244, 237, 227, 214, 225, 213, 193, 212, 189,
    194, 187, 208, 207, 218, 204, 206, 212, 222, 195, 207, 201, 202, 222, 171, 184, 207, 202, 189, 200,
    196, 188, 213, 199, 181, 229, 227, 215, 218, 208, 204, 233, 203, 217, 193, 197, 201, 192, 184, 208,
    201, 216, 234, 246, 234, 216, 215, 182, 234, 238, 225, 297, 385, 536, 781, 1157, 1487, 1582, 1319, 977,
    584, 414, 340, 260, 198, 222, 201, 192, 197, 195, 183, 184, 182, 189, 216, 216, 202, 204, 212, 193,
    198, 218, 215, 196, 218, 180, 192, 225, 206, 158, 186, 200, 188, 197, 197, 188, 187, 197, 191, 173,
    171, 187, 229, 167, 213, 171, 211, 201, 190, 170, 173, 197, 197, 189, 206, 187, 194, 206, 174, 179,
    190, 179, 179, 219, 185, 179, 161, 164, 199, 228, 224, 196, 202, 182, 214, 192, 184, 179, 187, 201,
    189, 197, 198, 149, 196, 163, 166, 214, 192, 177, 178, 162, 178, 196, 173, 183, 199, 171, 187, 208,
    236, 271, 279, 301, 303, 231, 209, 177, 176, 192, 189, 174, 187, 168, 144, 189, 176, 196, 188, 197,
    200, 223, 179, 197, 185, 187, 176, 178, 197, 196, 182, 190, 179, 170, 165, 182, 176, 205, 185, 203,
    268, 431, 997, 2078, 3374, 4153, 3626, 2451, 1242, 558, 243, 195, 171, 164, 172, 152, 146, 159, 154, 160,
    148, 168, 162, 155, 154, 168, 168, 143, 155, 155, 161, 154, 161, 172, 194, 170, 170, 162, 139, 175,
    161, 148, 171, 168, 162, 158, 137, 163, 188, 177, 180, 169, 161, 174, 144, 169, 152, 201, 234, 232,
    205, 180, 169, 143, 150, 157, 138, 158, 144, 160, 149, 173, 146, 169, 178, 154, 154, 152, 162, 163,
    185, 167, 174, 160, 166, 131, 137, 171, 156, 149, 172, 135, 154, 154, 153, 198, 155, 164, 161, 161,
    169, 146, 146, 151, 166, 151, 154, 158, 168, 127, 178, 156, 156, 158, 154, 152, 150, 162, 148, 149,
    147, 138, 162, 147, 156, 168, 180, 174, 139, 150, 172, 149, 160, 148, 142, 154, 173, 148, 133, 169,
    153, 128, 158, 164, 155, 170, 165, 156, 164, 151, 180, 148, 150, 156, 173, 160, 157, 144, 165, 173,
    180, 159, 156, 158, 173, 159, 172, 159, 193, 159, 148, 160, 161, 147, 176, 154, 159, 159, 176, 135,
    152, 137, 163, 152, 128, 161, 164, 161, 169, 153, 173, 162, 154, 155, 157, 161, 177, 184, 154, 181,
    156, 155, 121, 154, 148, 174, 161, 151, 171, 151, 130, 157, 161, 176, 145, 172, 152, 172, 156, 136,
    160, 177, 156, 142, 143, 135, 165, 147, 151, 151, 139, 161, 142, 152, 153, 166, 152, 170, 192, 157,
    153, 170, 154, 147, 172, 161, 189, 189, 181, 189, 173, 156, 160, 161, 152, 152, 159, 147, 163, 163,
    183, 164, 163, 159, 142, 155, 162, 135, 151, 156, 142, 150, 126, 150, 147, 137, 152, 154, 131, 132,
    156, 155, 147, 140, 138, 124, 139, 134, 128, 134, 154, 142, 155, 153, 134, 166, 196, 339, 606, 912,
    966, 906, 673, 417, 221, 161, 136, 132, 112, 142, 124, 114, 124, 120, 133, 132, 117, 110, 119, 118,
    131, 128, 140, 130, 125, 143, 133, 126, 123, 132, 122, 126, 114, 126, 128, 120, 113, 132, 124, 141,
    110, 119, 104, 120, 119, 132, 140, 124, 123, 138, 120, 116, 138, 123, 120, 175, 229, 251, 233, 224,
    175, 139, 128, 119, 141, 108, 111, 112, 120, 114, 131, 143, 159, 152, 148, 159, 158, 112, 113, 113,
    100, 122, 107, 107, 117, 118, 109, 115, 108, 142, 195, 249, 301, 268, 239, 196, 114, 115, 108, 98,
    103, 109, 94, 81, 91, 97, 80, 95, 91, 95, 104, 128, 165, 137, 147, 131, 116, 162, 158, 205,
    215, 167, 133, 96, 123, 96, 91, 89, 103, 87, 98, 97, 90, 112, 122, 168, 278, 441, 592, 606,
    504, 326, 187, 127, 86, 84, 59, 75, 77, 70, 64, 83, 75, 56, 80, 67, 86, 73, 77, 77,
    77, 71, 84, 60, 62, 77, 54, 72, 55, 67, 79, 78, 57, 72, 60, 63, 73, 72, 78, 73,
    72, 81, 84, 69, 85, 66, 67, 79, 68, 67, 74, 68, 79, 65, 78, 83, 67, 85, 70, 67,
    82, 72, 56, 68, 82, 71, 70, 75, 97, 111, 121, 136, 153, 112, 89, 71, 72, 59, 77, 112,
    186, 245, 274, 236, 181, 114, 91, 65, 75, 106, 139, 182, 171, 149, 130, 88, 62, 66, 69, 57,
    64, 60, 53, 51, 61, 65, 55, 53, 59, 52, 56, 59, 64, 59, 54, 56, 66, 55, 65, 72,
    68, 48, 51, 63, 57, 58, 64, 58, 75, 116, 196, 312, 469, 506, 468, 333, 179, 119, 93, 59,
    55, 56, 65, 46, 49, 48, 53, 46, 53, 57, 64, 55, 59, 51, 67, 59, 63, 51, 56, 55,
    64, 67, 54, 62, 57, 47, 57, 52, 58, 41, 54, 49, 47, 45, 62, 45, 45, 50, 51, 60,
    41, 56, 64, 56, 64, 55, 58, 49, 49, 47, 52, 49, 51, 48, 59, 56, 61, 65, 61, 98,
    69, 81, 81, 56, 55, 52, 50, 52, 45, 53, 46, 52, 48, 55, 50, 60, 48, 69, 56, 62,
    106, 106, 109, 125, 100, 93, 70, 53, 51, 57, 66, 120, 284, 719, 1382, 2189, 2919, 3092, 2448, 1427,
    713, 271, 110, 46, 39, 39, 49, 38, 50, 43, 55, 41, 32, 29, 35, 43, 37, 37, 46, 37,
    51, 36, 35, 38, 44, 39, 36, 31, 42, 36, 42, 37, 44, 42, 48, 31, 39, 46, 43, 48,
    35, 39, 43, 43, 39, 47, 35, 29, 42, 33, 38, 42, 32, 27, 41, 36, 37, 28, 41, 50,
    37, 42, 32, 52, 45, 30, 43, 40, 33, 39, 44, 42, 44, 42, 59, 42, 38, 46, 54, 58,
    55, 51, 45, 48, 54, 41, 40, 41, 35, 46, 60, 49, 74, 54, 65, 48, 39, 30, 40, 44,
    38, 61, 113, 209, 318, 444, 581, 528, 375, 269, 252, 386, 773, 1255, 1643, 1790, 1433, 1004, 457, 204,
    89, 45, 33, 27, 31, 34, 34, 38, 34, 48, 34, 29, 35, 35, 30, 29, 36, 29, 23, 43,
    36, 43, 39, 32, 40, 22, 38, 39, 29, 29, 23, 35, 29, 52, 45, 52, 35, 35, 33, 33,
    39, 23, 27, 25, 17, 35, 32, 26, 29, 24, 20, 31, 45, 27, 28, 26, 21, 25, 27, 27,
    21, 27, 28, 36, 36, 25, 32, 27, 27, 34, 32, 29, 22, 26, 28, 24, 35, 26, 27, 25,
    29, 29, 25, 31, 29, 32, 30, 33, 28, 23, 41, 27, 36, 26, 30, 32, 27, 26, 34, 34,
    41, 31, 25, 40, 34, 28, 32, 23, 31, 25, 38, 29, 31, 29, 32, 31, 29, 18, 35, 21,
    26, 27, 33, 27, 33, 33, 36, 44, 48, 34, 29, 26, 39, 29, 21, 34, 23, 36, 40, 29,
    42, 33, 40, 39, 26, 23, 31, 18, 22, 27, 21, 29, 30, 22, 28, 35, 30, 31, 32, 26,
    30, 26, 31, 29, 22, 28, 30, 23, 35, 30, 28, 26, 20, 32, 30, 23, 36, 33, 19, 29,
    30, 29, 30, 35, 33, 28, 31, 17, 37, 40, 56, 34, 46, 41, 39, 23, 29, 30, 25, 41,
    30, 32, 35, 25, 23, 22, 29, 30, 16, 33, 31, 28, 26, 28, 29, 43, 37, 42, 51, 71,
    78, 64, 48, 41, 35, 23, 26, 23, 36, 26, 26, 28, 29, 25, 31, 20, 26, 36, 25, 33,
    31, 25, 30, 25, 37, 26, 38, 35, 52, 60, 69, 61, 82, 53, 52, 40, 39, 28, 33, 34,
    16, 24, 32, 35, 22, 30, 36, 25, 32, 40, 26, 34, 32, 29, 32, 22, 17, 19, 27, 17,
    27, 33, 40, 45, 51, 65, 47, 45, 32, 30, 27, 24, 31, 26, 34, 27, 23, 37, 29, 40,
    43, 47, 35, 42, 43, 39, 39, 33, 39, 38, 39, 20, 25, 35, 29, 24, 32, 21, 31, 24,
    12, 30, 24, 20, 27, 22, 21, 29, 30, 28, 28, 24, 32, 23, 24, 32, 29, 29, 27, 24,
    30, 23, 19, 27, 23, 26, 23, 24, 21, 23, 32, 21, 24, 24, 32, 30, 32, 30, 33, 21,
    23, 22, 16, 31, 33, 32, 18, 22, 23, 34, 38, 40, 43, 42, 33, 38, 40, 33, 31, 26,
    37, 29, 24, 27, 25, 20, 25, 23, 33, 19, 14, 26, 29, 37, 29, 33, 27, 28, 24, 20,
    27, 23, 36, 23, 37, 37, 33, 24, 24, 32, 20, 30, 23, 22, 19, 32, 29, 28, 27, 30,
    32, 30, 30, 25, 24, 35, 23, 31, 28, 30, 24, 16, 19, 25, 23, 28, 23, 28, 25, 36,
    32, 27, 31, 22, 27, 24, 35, 34, 27, 39, 30, 26, 25, 26, 39, 21, 32, 35, 24, 16,
    22, 29, 41, 32, 22, 18, 32, 28, 26, 28, 32, 24, 25, 27, 28, 20, 22, 26, 38, 25,
    26, 28, 25, 30, 27, 28, 32, 29, 27, 27, 20, 26, 37, 23, 28, 26, 21, 20, 20, 21,
    22, 24, 25, 37, 37, 33, 31, 23, 25, 22, 35, 19, 32, 27, 16, 24, 23, 25, 24, 39,
    23, 23, 25, 33, 35, 31, 25, 26, 42, 28, 22, 24, 30, 30, 30, 24, 36, 29, 33, 28,
    34, 21, 26, 40, 37, 41, 36, 35, 25, 31, 26, 26, 21, 34, 32, 35, 28, 40, 39, 29,
    51, 63, 57, 60, 69, 48, 47, 37, 41, 26, 43, 36, 22, 23, 29, 15, 33, 31, 25, 20,
    29, 28, 17, 21, 22, 32, 34, 35, 37, 42, 28, 31, 26, 26, 29, 25, 26, 26, 28, 29,
    16, 24, 33, 27, 33, 28, 18, 24, 25, 21, 26, 25, 27, 28, 26, 27, 33, 27, 30, 27,
    22, 25, 27, 26, 21, 22, 23, 27, 24, 24, 27, 35, 26, 31, 25, 21, 32, 33, 26, 39,
    22, 26, 31, 31, 30, 41, 35, 41, 28, 21, 33, 19, 27, 31, 23, 30, 27, 32, 19, 30,
    32, 20, 19, 24, 24, 35, 26, 26, 24, 25, 31, 31, 20, 28, 23, 15, 35, 26, 26, 17,
    24, 23, 20, 32, 27, 35, 28, 28, 25, 25, 21, 29, 28, 32, 35, 22, 24, 26, 22, 23,
    24, 30, 30, 31, 32, 27, 28, 24, 28, 33, 32, 36, 18, 24, 19, 21, 34, 33, 24, 29,
    22, 25, 23, 30, 24, 32, 27, 25, 25, 15, 27, 28, 28, 30, 22, 17, 18, 25, 33, 30,
    29, 27, 26, 17, 21, 29, 25, 27, 30, 30, 25, 20, 24, 25, 33, 20, 21, 32, 28, 30,
    25, 26, 24, 29, 28, 36, 30, 25, 26, 27, 27, 22, 18, 25, 26, 23, 26, 24, 19, 28,
    25, 23, 23, 18, 25, 22, 21, 26, 24, 25, 27, 35, 27, 29, 23, 22, 26, 32, 24, 28,
    25, 31, 26, 32, 37, 22, 26, 27, 32, 30, 29, 16, 28, 27, 32, 21, 24, 24, 21, 25,
    25, 36, 34, 29, 29, 34, 39, 35, 24, 23, 27, 29, 33, 20, 29, 31, 25, 26, 20, 17,
    26, 19, 28, 18, 29, 21, 21, 24, 23, 28, 28, 26, 21, 23, 30, 23, 27, 18, 26, 19,
    21, 29, 27, 27, 22, 25, 24, 30, 12, 23, 25, 17, 29, 20, 22, 23, 22, 28, 27, 23,
    30, 14, 25, 27, 24, 30, 24, 25, 17, 21, 22, 23, 33, 24, 18, 27, 25, 24, 21, 16,
    24, 28, 17, 23, 27, 13, 19, 21, 23, 20, 18, 17, 21, 34, 27, 15, 27, 12, 17, 14,
    29, 23, 17, 20, 15, 16, 18, 27, 14, 22, 19, 24, 33, 17, 26, 12, 18, 17, 20, 28,
    18, 28, 19, 20, 18, 23, 19, 20, 23, 19, 16, 17, 18, 22, 22, 29, 21, 24, 16, 17,
    21, 20, 30, 19, 19, 21, 21, 24, 21, 16, 17, 15, 9, 18, 19, 16, 14, 18, 17, 12,
    28, 18, 21, 25, 17, 11, 32, 13, 20, 26, 17, 14, 17, 23, 32, 49, 69, 74, 111, 125,
    136, 146, 138, 117, 67, 45, 37, 16, 15, 15, 19, 14, 16, 22, 23, 14, 20, 23, 23, 10,
    21, 16, 24, 16, 20, 15, 15, 20, 20, 21, 17, 12, 19, 20, 18, 16, 15, 18, 27, 14,
    17, 14, 15, 18, 12, 14, 12, 16, 20, 22, 22, 15, 12, 15, 22, 15, 17, 11, 14, 13,
    9, 10, 17, 17, 20, 22, 18, 16, 24, 26, 40, 51, 63, 74, 77, 58, 42, 27, 20, 21,
    30, 28, 27, 45, 39, 48, 39, 37, 36, 32, 25, 14, 16, 16, 14, 18, 12, 11, 15, 11,
    19, 22, 24, 17, 23, 26, 30, 42, 41, 30, 26, 25, 21, 12, 17, 18, 17, 6, 11, 16,
    14, 18, 17, 18, 20, 19, 14, 16, 15, 20, 17, 18, 15, 12, 16, 13, 16, 11, 10, 11,
    14, 19, 20, 18, 27, 14, 14, 13, 15, 10, 9, 21, 15, 15, 9, 13, 29, 10, 18, 15,
    19, 17, 14, 15, 13, 14, 10, 21, 15, 13, 18, 16, 15, 16, 16, 9, 16, 15, 11, 15,
    17, 18, 19, 18, 17, 13, 11, 14, 18, 19, 10, 11, 17, 8, 16, 20, 23, 23, 27, 25,
    24, 21, 15, 16, 17, 13, 10, 11, 14, 18, 15, 19, 11, 14, 9, 11, 22, 9, 14, 16,
    17, 14, 14, 18, 14, 13, 14, 13, 15, 14, 15, 22, 16, 14, 11, 11, 14, 18, 9, 12,
    14, 21, 21, 21, 39, 46, 43, 52, 46, 39, 25, 14, 16, 13, 19, 25, 23, 46, 77, 106,
    131, 201, 208, 195, 162, 149, 99, 107, 107, 111, 126, 130, 89, 66, 49, 46, 26, 20, 20, 19,
    16, 13, 16, 20, 19, 11, 12, 6, 18, 8, 14, 17, 19, 13, 19, 17, 7, 10, 12, 11,
    13, 11, 16, 13, 16, 13, 19, 11, 15, 16, 8, 18, 13, 8, 16, 12, 15, 15, 27, 13,
    18, 21, 11, 25, 29, 39, 55, 85, 95, 111, 105, 70, 52, 46, 30, 28, 32, 30, 34, 37,
    32, 22, 12, 18, 25, 34, 50, 91, 107, 138, 132, 104, 78, 46, 26, 22, 19, 13, 14, 22,
    18, 19, 20, 22, 34, 48, 45, 46, 27, 30, 22, 15, 11, 16, 14, 14, 16, 17, 5, 14,
    7, 11, 15, 15, 15, 9, 10, 13, 17, 10, 13, 12, 23, 14, 11, 19, 18, 9, 8, 13,
    16, 18, 14, 14, 15, 12, 19, 10, 15, 10, 15, 13, 10, 25, 18, 14, 15, 17, 19, 18,
    19, 25, 30, 39, 24, 23, 31, 14, 18, 13, 11, 17, 8, 21, 17, 10, 17, 10, 16, 15,
    16, 8, 13, 14, 16, 19, 16, 27, 20, 15, 20, 19, 15, 19, 22, 19, 19, 16, 16, 14,
    18, 26, 17, 17, 22, 17, 18, 16, 11, 12, 11, 18, 14, 11, 12, 16, 18, 23, 15, 23,
    11, 22, 22, 19, 15, 14, 11, 19, 12, 11, 11, 16, 19, 16, 14, 16, 17, 20, 18, 15,
    16, 17, 15, 25, 15, 10, 19, 12, 16, 14, 21, 22, 15, 15, 13, 16, 15, 12, 10, 13,
    10, 17, 15, 17, 14, 17, 16, 8, 15, 11, 13, 12, 13, 7, 14, 16, 14, 20, 9, 16,
    15, 16, 14, 16, 10, 13, 7, 20, 17, 11, 19, 14, 12, 15, 21, 17, 18, 12, 14, 19,
    11, 16, 7, 13, 8, 16, 15, 14, 11, 18, 11, 15, 11, 18, 21, 11, 16, 11, 14, 14,
    13, 17, 16, 14, 22, 11, 14, 9, 13, 13, 15, 15, 21, 20, 16, 21, 8, 10, 14, 17,
    13, 17, 10, 16, 14, 19, 15, 23, 14, 13, 11, 15, 18, 11, 13, 18, 9, 21, 19, 13,
    22, 24, 18, 27, 27, 26, 22, 20, 13, 22, 17, 10, 16, 20, 10, 16, 11, 9, 12, 17,
    17, 9, 12, 18, 22, 14, 12, 22, 16, 15, 11, 9, 13, 14, 17, 16, 23, 12, 10, 17,
    16, 10, 11, 13, 16, 10, 22, 14, 13, 13, 17, 11, 19, 5, 12, 12, 15, 8, 12, 20,
    22, 21, 20, 12, 11, 17, 21, 14, 14, 18, 10, 17, 16, 17, 8, 7, 19, 16, 12, 20,
    14, 21, 18, 14, 17, 21, 22, 20, 27, 22, 19, 14, 18, 19, 16, 9, 11, 9, 12, 13,
    16, 23, 19, 12, 16, 18, 15, 15, 16, 14, 13, 18, 16, 15, 12, 11, 9, 15, 16, 14,
    13, 13, 18, 24, 17, 13, 18, 17, 12, 15, 16, 11, 16, 20, 15, 15, 12, 16, 16, 19,
    18, 11, 19, 11, 12, 15, 13, 19, 20, 13, 9, 12, 22, 9, 21, 11, 10, 16, 17, 24,
    17, 18, 23, 23, 18, 13, 20, 17, 20, 9, 14, 15, 11, 22, 12, 12, 16, 11, 18, 18,
    15, 13, 17, 15, 11, 20, 13, 18, 16, 12, 13, 19, 8, 11, 20, 16, 16, 17, 20, 26,
    17, 20, 16, 14, 20, 10, 17, 17, 14, 17, 22, 11, 11, 19, 20, 13, 14, 18, 17, 17,
    16, 16, 17, 16, 16, 12, 18, 16, 18, 15, 23, 11, 14, 15, 24, 20, 20, 14, 17, 19,
    7, 16, 20, 18, 20, 16, 19, 14, 7, 16, 19, 14, 22, 21, 23, 28, 21, 13, 19, 15,
    16, 19, 12, 15, 16, 14, 13, 18, 16, 18, 17, 11, 20, 18, 16, 19, 9, 17, 19, 10,
    18, 17, 18, 9, 13, 15, 15, 15, 19, 20, 16, 15, 17, 12, 16, 13, 23, 16, 15, 18,
    14, 28, 18, 16, 22, 22, 12, 16, 12, 11, 12, 17, 16, 19, 14, 13, 20, 19, 6, 18,
    13, 22, 15, 10, 12, 9, 13, 21, 13, 21, 17, 18, 18, 20, 16, 16, 17, 18, 16, 18,
    17, 19, 26, 20, 21, 12, 18, 17, 16, 13, 7, 23, 20, 15, 9, 12, 21, 17, 20, 14,
    19, 15, 20, 26, 14, 19, 16, 19, 15, 11, 10, 16, 5, 21, 16, 10, 14, 23, 22, 26,
    15, 15, 15, 14, 14, 17, 18, 18, 19, 16, 18, 22, 16, 20, 13, 13, 19, 14, 16, 24,
    12, 16, 20, 17, 20, 22, 18, 18, 20, 9, 17, 18, 12, 16, 24, 24, 19, 16, 17, 6,
    11, 14, 14, 17, 21, 17, 14, 16, 16, 14, 7, 14, 13, 13, 15, 20, 17, 12, 15, 16,
    15, 13, 20, 19, 13, 10, 12, 20, 21, 16, 13, 13, 17, 16, 12, 15, 17, 12, 21, 19,
    14, 10, 21, 15, 14, 13, 16, 17, 22, 15, 18, 12, 24, 22, 20, 21, 14, 11, 14, 21,
    11, 18, 13, 14, 11, 20, 14, 13, 27, 12, 21, 11, 13, 16, 16, 16, 14, 11, 27, 23,
    22, 16, 11, 10, 20, 14, 20, 20, 14, 6, 20, 14, 19, 22, 13, 19, 13, 20, 14, 18,
    14, 17, 14, 15, 14, 16, 11, 17, 15, 17, 16, 14, 9, 13, 16, 18, 19, 16, 16, 13,
    15, 12, 16, 23, 14, 20, 19, 16, 19, 9, 15, 22, 12, 14, 15, 15, 25, 13, 24, 16,
    13, 15, 16, 16, 20, 18, 17, 13, 14, 15, 30, 24, 13, 17, 15, 22, 16, 18, 15, 15,
    19, 13, 8, 15, 14, 16, 15, 11, 14, 22, 16, 14, 14, 25, 13, 19, 15, 16, 11, 16,
    18, 18, 12, 24, 23, 18, 16, 16, 24, 15, 22, 9, 10, 14, 19, 16, 18, 17, 18, 10,
    13, 14, 9, 20, 16, 19, 10, 13, 22, 15, 16, 27, 13, 20, 21, 18, 22, 20, 10, 14,
    16, 17, 20, 15, 18, 19, 19, 15, 21, 20, 13, 17, 15, 16, 27, 10, 12, 18, 14, 15,
    15, 9, 17, 17, 16, 14, 6, 15, 13, 22, 20, 9, 14, 16, 26, 12, 16, 23, 17, 21,
    18, 16, 19, 15, 16, 23, 17, 20, 21, 30, 37, 42, 57, 74, 93, 102, 101, 142, 121, 130,
    106, 94, 69, 48, 38, 29, 12, 21, 14, 21, 25, 26, 23, 16, 14, 18, 24, 24, 21, 19,
    14, 18, 21, 22, 19, 7, 26, 19, 13, 24, 16, 19, 23, 26, 13, 13, 19, 23, 17, 18,
    24, 24, 25, 10, 19, 19, 22, 19, 10, 15, 19, 18, 14, 17, 13, 18, 20, 17, 19, 18,
    21, 11, 18, 13, 18, 18, 10, 25, 25, 16, 21, 14, 20, 31, 19, 21, 21, 11, 24, 17,
    15, 20, 17, 19, 19, 29, 25, 11, 17, 21, 19, 21, 14, 31, 22, 13, 17, 20, 17, 23,
    17, 16, 16, 15, 19, 21, 21, 15, 18, 13, 15, 14, 18, 17, 13, 30, 18, 17, 13, 19,
    22, 19, 24, 27, 28, 22, 18, 17, 16, 19, 19, 20, 18, 24, 20, 16, 18, 19, 17, 27,
    20, 28, 24, 20, 25, 22, 20, 19, 28, 20, 19, 18, 21, 22, 16, 22, 24, 24, 19, 19,
    22, 18, 16, 17, 21, 30, 19, 16, 20, 26, 23, 24, 18, 26, 24, 18, 19, 15, 20, 15,
    18, 20, 13, 19, 23, 23, 25, 22, 21, 23, 22, 21, 16, 21, 26, 20, 16, 21, 24, 20,
    21, 21, 23, 30, 19, 23, 22, 20, 24, 20, 25, 25, 24, 23, 23, 26, 33, 24, 18, 20,
    17, 12, 22, 24, 13, 11, 20, 23, 22, 18, 32, 10, 19, 25, 24, 26, 22, 29, 17, 15,
    21, 22, 28, 21, 21, 21, 13, 15, 24, 19, 21, 20, 27, 28, 24, 15, 26, 17, 22, 17,
    20, 28, 24, 23, 33, 27, 25, 21, 22, 21, 14, 23, 20, 22, 25, 22, 17, 22, 25, 26,
    19, 15, 16, 32, 17, 21, 25, 26, 22, 30, 15, 22, 22, 29, 21, 23, 20, 28, 20, 29,
    23, 24, 28, 26, 20, 16, 23, 30, 27, 26, 26, 26, 23, 30, 17, 24, 25, 20, 30, 17,
    20, 21, 27, 25, 21, 21, 22, 31, 28, 27, 17, 24, 27, 24, 21, 14, 18, 15, 22, 30,
    21, 31, 23, 22, 26, 33, 32, 24, 22, 20, 29, 15, 28, 27, 24, 20, 29, 22, 28, 27,
    25, 23, 26, 25, 22, 21, 17, 19, 17, 23, 24, 23, 34, 26, 17, 32, 23, 27, 31, 27,
    29, 25, 30, 20, 18, 18, 14, 23, 33, 21, 15, 19, 23, 26, 18, 25, 32, 22, 33, 23,
    16, 21, 30, 29, 33, 31, 31, 26, 29, 24, 24, 25, 30, 26, 27, 23, 23, 28, 23, 30,
    34, 29, 31, 28, 28, 27, 35, 35, 29, 22, 28, 27, 24, 26, 22, 31, 20, 21, 30, 24,
    27, 29, 21, 32, 25, 18, 31, 31, 28, 29, 27, 27, 28, 32, 29, 20, 22, 22, 27, 28,
    30, 26, 26, 37, 29, 29, 33, 31, 29, 36, 18, 30, 31, 31, 34, 20, 29, 28, 38, 32,
    23, 39, 39, 31, 27, 23, 32, 32, 21, 38, 34, 33, 28, 35, 23, 33, 33, 29, 26, 26,
    30, 29, 37, 32, 25, 30, 30, 26, 26, 32, 30, 30, 20, 37, 34, 30, 37, 28, 35, 29,
    22, 18, 41, 24, 32, 25, 23, 39, 38, 31, 37, 23, 28, 29, 29, 30, 29, 18, 34, 33,
    24, 35, 33, 35, 23, 33, 27, 30, 28, 35, 20, 37, 31, 38, 29, 35, 25, 34, 35, 22,
    23, 32, 32, 28, 33, 24, 18, 37, 31, 21, 19, 23, 19, 24, 22, 24, 22, 18, 28, 15,
    19, 28, 18, 23, 17, 30, 18, 33, 17, 14, 16, 19, 30, 25, 20, 16, 15, 21, 17, 18,
    19, 18, 13, 21, 20, 15, 13, 28, 14, 15, 24, 15, 21, 15, 18, 19, 9, 30, 21, 19,
    17, 16, 28, 18, 16, 20, 24, 17, 19, 23, 22, 21, 23, 18, 21, 13, 19, 17, 24, 26,
    14, 23, 20, 24, 13, 25, 22, 18, 22, 20, 16, 18, 10, 20, 12, 24, 16, 14, 26, 16,
    15, 15, 24, 18, 22, 14, 9, 23, 18, 20, 14, 23, 12, 17, 11, 17, 16, 17, 15, 16,
    19, 17, 14, 26, 18, 20, 13, 21, 18, 14, 17, 11, 12, 16, 16, 16, 18, 17, 19, 15,
    16, 25, 16, 14, 18, 20, 24, 13, 14, 18, 12, 11, 12, 21, 11, 13, 10, 11, 20, 17,
    17, 13, 11, 12, 7, 17, 13, 11, 15, 16, 12, 9, 7, 11, 10, 16, 15, 20, 13, 18,
    14, 11, 13, 14, 17, 17, 14, 14, 11, 14, 6, 17, 13, 9, 13, 13, 13, 9, 12, 6,
    10, 15, 8, 13, 16, 14, 9, 11, 11, 11, 13, 17, 10, 19, 8, 8, 15, 9, 10, 12,
    9, 9, 9, 9, 7, 10, 10, 9, 12, 18, 12, 12, 13, 8, 8, 7, 15, 7, 10, 14,
    11, 13, 5, 11, 10, 12, 13, 14, 13, 12, 8, 10, 12, 12, 2, 7, 9, 3, 7, 12,
    9, 7, 9, 4, 4, 5, 12, 3, 11, 8, 8, 7, 9, 6, 6, 11, 9, 10, 9, 9,
    4, 9, 9, 3, 10, 9, 10, 7, 8, 4, 13, 2, 8, 6, 5, 5, 6, 5, 11, 5,
    7, 6, 4, 4, 7, 7, 4, 6, 7, 6, 7, 5, 6, 5, 1, 9, 8, 4, 6, 5,
    5, 5, 4, 2, 7, 7, 7, 4, 4, 3, 10, 4, 7, 3, 5, 5, 2, 9, 4, 7,
    5, 2, 5, 9, 6, 6, 4, 7, 6, 6, 6, 10, 7, 6, 5, 5, 6, 4, 5, 4,
    5, 12, 2, 6, 3, 5, 3, 8, 5, 3, 4, 4, 4, 6, 4, 6, 3, 3, 5, 4,
    6, 6, 3, 7, 4, 6, 7, 8, 4, 9, 5, 1, 3, 4, 5, 5, 5, 3, 7, 4,
    8, 2, 9, 5, 6, 6, 4, 7, 6, 9, 7, 2, 4, 3, 5, 5, 7, 5, 8, 1,
    6, 4, 6, 3, 5, 6, 5, 7, 3, 2, 8, 8, 6, 1, 3, 2, 9, 8, 5, 4,
    9, 5, 2, 2, 7, 6, 4, 8, 4, 5, 4, 6, 4, 11, 5, 7, 5, 7, 3, 4,
    2, 5, 3, 9, 12, 7, 7, 6, 8, 5, 5, 9, 9, 14, 18, 48, 58, 107, 187, 282,
    456, 618, 811, 927, 920, 942, 843, 607, 406, 272, 131, 55, 28, 15, 5, 4, 2, 3, 3, 3,
    6, 2, 1, 2, 2, 3, 5, 2, 1, 4, 5, 2, 4, 5, 2, 1, 5, 4, 3, 2,
    1, 3, 3, 4, 3, 3, 3, 1, 4, 4, 3, 5, 3, 3, 2, 5, 1, 4, 3, 4,
    3, 3, 3, 5, 4, 6, 0, 1, 4, 4, 0, 1, 1, 2, 1, 6, 1, 2, 2, 3,
    4, 3, 4, 4, 4, 2, 4, 3, 3, 1, 1, 3, 5, 3, 4, 2, 2, 3, 0, 1,
    6, 3, 2, 2, 1, 5, 5, 1, 2, 4, 2, 5, 4, 4, 3, 2, 3, 4, 2, 2,
    3, 4, 2, 1, 3, 2, 3, 7, 4, 1, 3, 2, 3, 1, 6, 3, 0, 1, 2, 2,
    1, 0, 1, 1, 0, 1, 7, 3, 1, 3, 1, 8, 2, 2, 6, 4, 6, 1, 3, 3,
    1, 4, 2, 5, 3, 3, 4, 2, 3, 2, 1, 1, 3, 2, 4, 2, 2, 2, 4, 3,
    1, 4, 1, 4, 2, 4, 3, 1, 4, 3, 4, 1, 3, 3, 2, 2, 1, 2, 3, 4,
    3, 5, 2, 0, 1, 1, 4, 1, 3, 3, 1, 5, 2, 2, 2, 3, 0, 1, 4, 1,
    0, 1, 3, 0, 2, 3, 2, 4, 1, 0, 1, 2, 0, 1, 3, 5, 2, 1, 3, 1,
    0, 1, 4, 0, 1, 3, 4, 0, 1, 3, 3, 4, 3, 0, 1, 1, 2, 2, 1, 3,
    2, 1, 2, 3, 2, 1, 7, 3, 3, 3, 0, 3, 1, 2, 3, 2, 3, 4, 1, 1,
    1, 1, 1, 3, 1, 2, 2, 3, 2, 1, 6, 1, 4, 4, 2, 1, 3, 2, 3, 2,
    0, 1, 5, 3, 2, 1, 2, 1, 2, 3, 5, 3, 1, 0, 1, 1, 1, 6, 3, 3,
    4, 1, 2, 1, 1, 5, 5, 3, 0, 1, 1, 3, 0, 1, 2, 2, 5, 0, 2, 1,
    1, 6, 2, 1, 0, 1, 1, 1, 2, 1, 4, 0, 1, 2, 1, 1, 3, 2, 3, 2,
    1, 3, 4, 2, 4, 3, 4, 3, 3, 2, 3, 0, 1, 2, 2, 0, 1, 1, 1, 0,
    1, 1, 2, 3, 1, 3, 3, 1, 3, 2, 3, 5, 3, 0, 1, 5, 2, 0, 1, 2,
    1, 1, 0, 1, 1, 5, 2, 1, 0, 1, 1, 1, 1, 0, 1, 2, 0, 2, 3, 3,
    4, 3, 3, 2, 0, 1, 1, 2, 1, 2, 1, 1, 0, 2, 2, 2, 2, 2, 0, 2,
    1, 0, 3, 5, 1, 3, 2, 1, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 4, 2,
    4, 0, 1, 2, 5, 0, 1, 5, 4, 1, 2, 2, 0, 1, 1, 2, 2, 2, 1, 3,
    2, 1, 0, 1, 1, 4, 0, 1, 5, 0, 1, 9, 2, 1, 3, 0, 2, 3, 2, 2,
    1, 0, 1, 3, 3, 3, 1, 2, 2, 1, 3, 1, 1, 1, 1, 3, 2, 2, 3, 1,
    1, 4, 1, 1, 0, 1, 6, 1, 0, 1, 1, 1, 2, 1, 3, 0, 1, 2, 1, 2,
    4, 5, 2, 4, 2, 3, 3, 5, 1, 0, 2, 1, 1, 1, 4, 2, 3, 1, 1, 4,
    1, 2, 3, 1, 2, 2, 1, 2, 0, 1, 3, 4, 1, 2, 2, 0, 1, 2, 4, 1,
    2, 0, 1, 1, 4, 2, 0, 1, 2, 3, 2, 5, 3, 1, 2, 1, 2, 1, 2, 0,
    1, 3, 2, 2, 0, 1, 2, 0, 1, 1, 1, 1, 3, 1, 3, 5, 4, 1, 0, 1,
    2, 3, 1, 3, 1, 2, 3, 0, 1, 3, 2, 1, 4, 1, 1, 0, 1, 2, 2, 4,
    0, 1, 1, 2, 0, 1, 3, 3, 2, 1, 2, 0, 3, 2, 1, 2, 3, 2, 1, 1,
    1, 3, 1, 2, 3, 3, 1, 2, 0, 1, 2, 1, 2, 1, 1, 0, 1, 2, 0, 1,
    3, 2, 3, 1, 0, 1, 1, 0, 2, 1, 1, 2, 3, 1, 1, 3, 2, 1, 3, 1,
    1, 4, 1, 0, 1, 2, 3, 1, 4, 1, 2, 1, 3, 3, 3, 1, 1, 3, 1, 0,
    1, 1, 4, 4, 1, 1, 2, 2, 0, 1, 1, 0, 1, 1, 0, 2, 1, 3, 2, 2,
    2, 4, 3, 1, 3, 2, 1, 1, 1, 0, 2, 3, 0, 1, 1, 1, 5, 2, 0, 1,
    2, 2, 1, 4, 1, 0, 1, 3, 1, 2, 1, 1, 2, 3, 1, 1, 1, 0, 1, 2,
    1, 0, 1, 2, 3, 2, 3, 1, 3, 1, 1, 0, 1, 2, 2, 1, 2, 1, 3, 0,
    1, 3, 3, 1, 0, 1, 1, 0, 1, 2, 1, 0, 1, 3, 1, 1, 1, 1, 2, 1,
    2, 0, 1, 1, 0, 1, 3, 1, 1, 1, 2, 2, 1, 1, 0, 2, 2, 1, 1, 0,
    1, 2, 1, 2, 1, 0, 1, 1, 2, 0, 1, 4, 3, 1, 0, 3, 1, 2, 1, 0,
    1, 1, 1, 1, 0, 2, 1, 1, 3, 1, 0, 1, 1, 1, 0, 2, 3, 4, 1, 0,
    1, 3, 0, 1, 4, 1, 0, 1, 1, 2, 1, 1, 1, 3, 0, 1, 2, 1, 1, 2,
    1, 1, 4, 0, 1, 2, 0, 1, 1, 1, 4, 0, 2, 1, 1, 2, 1, 0, 2, 2,
    1, 1, 1, 1, 1, 2, 0, 1, 1, 3, 0, 1, 2, 0, 1, 1, 0, 1, 2, 4,
    0, 1, 3, 0, 1, 1, 0, 3, 2, 0, 1, 1, 0, 1, 1, 0, 3, 5, 2, 2,
    1, 2, 1, 3, 2, 0, 1, 1, 0, 1, 1, 1, 1, 0, 2, 2, 0, 4, 1, 2,
    2, 2, 2, 2, 1, 2, 0, 1, 1, 0, 2, 2, 0, 3, 1, 0, 1, 1, 2, 0,
    3, 2, 0, 1, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 0, 2, 3, 0,
    1, 1, 1, 1, 0, 2, 3, 0, 1, 1, 0, 3, 2, 0, 1, 2, 1, 0, 1, 1,
    0, 1, 1, 1, 0, 1, 1, 0, 5, 1, 0, 3, 3, 0, 1, 1, 1, 0, 4, 1,
    0, 3, 1, 1, 2, 1, 0, 1, 2, 0, 3, 1, 0, 1, 1, 1, 0, 1, 2, 0,
    3, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 4, 1, 0, 3, 3,
    2, 2, 1, 0, 3, 2, 0, 3, 2, 0, 4, 2, 0, 2, 1, 2, 0, 1, 3, 0,
    7, 1, 0, 2, 2, 2, 1, 3, 6, 7, 4, 4, 6, 1, 3, 2, 2, 1, 1, 0,
    2, 1, 1, 0, 1, 3, 1, 0, 2, 1, 0, 8, 1, 0, 1, 1, 0, 3, 1, 0,
    2, 1, 0, 3, 1, 1, 0, 3, 1, 0, 5, 1, 2, 0, 1, 1, 1, 0, 1, 1,
    0, 8, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 2, 2, 1, 1, 1, 0, 1,
    1, 1, 0, 4, 1, 2, 0, 3, 1, 0, 3, 1, 0, 5, 1, 0, 1, 1, 0, 1,
    1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 2, 0, 2, 1, 1, 0, 1, 1, 0,
    1, 1, 0, 1, 1, 0, 4, 2, 0, 3, 1, 0, 6, 2, 1, 3, 4, 7, 5, 15,
    12, 21, 18, 12, 13, 10, 9, 3, 2, 0, 4, 1, 0, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 2, 1, 1, 1, 0, 5, 1, 1, 0, 6, 1, 0, 5, 1, 0, 3, 1, 2,
    0, 8, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0, 1, 1, 0, 7,
    1, 0, 3, 2, 0, 5, 2, 0, 3, 1, 0, 4, 1, 0, 6, 1, 0, 7, 1, 1,
    0, 3, 1, 0, 3, 1, 1, 0, 3, 1, 0, 8, 1, 0, 3, 1, 0, 1, 1, 0,
    3, 1, 1, 0, 2, 1, 0, 7, 1, 0, 2, 1, 0, 6, 1, 0, 7, 1, 0, 7,
    1, 0, 9, 1, 0, 2, 1, 0, 3, 1, 0, 16, 1, 0, 6, 1, 0, 3, 2, 0,
    25, 2, 0, 1, 1, 0, 14, 1, 0, 10, 1, 0, 48, 1, 0, 5, 1, 0, 5, 1,
    0, 22, 1, 1, 0, 3, 1, 0, 8, 1, 0, 10, 1, 0, 5, 1, 0, 30, 1, 0,
    4, 1, 0, 22, 1, 0, 4, 1, 0, 7, 1, 0, 13, 1, 0, 2, 1, 0, 70, 2,
    0, 2, 1, 0, 2, 1, 2, 4, 3, 2, 4, 4, 0, 1, 1, 0, 12, 1, 0, 5,
    1, 0, 12, 1, 0, 5, 1, 0, 19, 1, 0, 1, 1, 0, 22, 1, 0, 6, 1, 0,
    8, 1, 0, 10, 1, 0, 14, 1, 0, 88, 2, 0, 28, 1, 0, 14, 1, 0, 9, 1,
    0, 5, 1, 0, 10, 1, 0, 19, 1, 0, 96, 1, 0, 27, 1, 0, 21, 1, 0, 22,
    1, 0, 65, 1, 0, 11, 1, 0, 10, 1, 0, 24, 1, 0, 50, 1, 0, 30, 1, 0,
    11, 1, 0, 79, 1, 0, 54, 1, 0, 9, 1, 0, 70, 1, 0, 52, 1, 0, 74, 1,
    0, 37, 1, 0, 5, 1, 0, 9, 1, 0, 5, 1, 0, 19, 1, 0, 54, 1, 0, 7,
    1, 0, 18, 1, 0, 9, 1, 0, 15, 1, 0, 12, 1, 0, 30, 1, 0, 22, 1, 0,
    125, 1, 0, 66, 1, 0, 100, 1, 0, 1, 1, 0, 31, 1, 0, 100, 1, 0, 52, 1,
    0, 17, 1, 0, 116, 1, 0, 128, 1, 0, 81, 1, 0, 17, 1, 0, 33, 1, 0, 57,
    1, 0, 265, 1, 0, 108, 1, 0, 82, 1, 0, 140, 1, 0, 31, 1, 0, 81, 1, 0,
    99, 1, 0, 14, 1, 0, 4, 1, 0, 114, 1, 0, 80, 1, 0, 50, 1, 0, 42, 1,
    0, 48, 1, 0, 122, 1, 0, 15, 1, 0, 16, 1, 0, 1, 1, 0, 13, 1, 0, 127,
    1, 0, 46, 1, 0, 31, 1, 0, 66, 1, 0, 125, 1, 0, 45, 1, 0, 19, 1, 0,
    50, 1, 0, 3, 1, 0, 8, 1, 0, 63, 1, 0, 30, 1, 0, 58, 1, 0, 38, 1,
    0, 9, 1, 0, 18, 1, 0, 42, 1, 0, 11, 1, 0, 77, 1, 0, 76, 1, 0, 33,
    1, 0, 29, 1, 0, 33, 1, 0, 80, 1, 0, 72, 1, 0, 49, 1, 0, 213, 1, 0,
    192, 1, 0, 9, 1, 0, 35, 1, 0, 142, 1, 0, 16, 1, 0, 36, 1, 0, 28, 1,
    0, 14, 1, 0, 227, 1, 0, 53, 1, 0, 40, 1, 0, 3, 1, 0, 20, 1, 0, 134,
    1, 0, 95, 1, 0, 106, 1, 0, 195, 1, 0, 51, 1, 0, 3, 1, 0, 20, 1, 0,
    21, 1, 0, 46, 1, 0, 21, 1, 0, 108, 1, 0, 24, 1, 0, 323, 1, 0, 380, 1,
    0, 4, 1, 0, 66, 1, 0, 34, 1, 0, 106, 1, 0, 127, 1, 0, 28, 1, 0, 17,
    1, 0, 195, 1, 0, 263, 1, 0, 2, 1, 0, 188, 1, 0, 11, 1, 0, 69, 1, 0,
    14, 1, 0, 126, 1, 0, 34, 1, 0, 100, 1, 0, 124, 1, 0, 37, 1, 0, 140, 1,
    0, 135, 1, 0, 14  };
  assert( test_19_chan_cnts.size() == 7265 );
  const vector<uint8_t> test_19_packed{
    97, 28, 0, 0, 64, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85, 85,
    85, 85, 85, 85, 85, 21, 65, 65, 81, 4, 20, 64, 0, 80, 0, 4, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 80, 1, 0, 0, 64, 85, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 64, 85, 85, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 84, 1, 0, 0, 0, 0, 0, 0, 0, 0, 85, 85, 5, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 84, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 85, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 85, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 69, 85, 21, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 85, 85, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    64, 16, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 1, 3, 1, 0, 1, 3, 4, 9, 16, 101, 1, 229, 2, 230, 2, 25, 3, 230, 2, 14, 3, 224, 2, 226, 2, 212, 2, 197, 2,
    1, 3, 183, 2, 201, 2, 243, 2, 215, 2, 198, 2, 208, 2, 234, 2, 203, 2, 139, 2, 169, 2, 200, 2, 191, 2, 166, 2, 156, 2, 192, 2, 175, 2, 156, 2, 154, 2, 151, 2, 129, 2, 178, 2, 115, 2, 113, 2, 140, 2,
    152, 2, 150, 2, 109, 2, 120, 2, 149, 2, 71, 2, 138, 2, 69, 2, 108, 2, 102, 2, 113, 2, 112, 2, 127, 2, 99, 2, 100, 2, 75, 2, 61, 2, 122, 2, 53, 2, 57, 2, 12, 2, 76, 2, 63, 2, 63, 2, 50, 2,
    90, 2, 66, 2, 32, 2, 73, 2, 89, 2, 115, 2, 50, 2, 42, 2, 48, 2, 34, 2, 40, 2, 74, 2, 65, 2, 64, 2, 62, 2, 88, 2, 137, 2, 195, 2, 11, 3, 141, 3, 34, 4, 20, 4, 197, 3, 255, 2, 165, 2,
    90, 2, 59, 2, 53, 2, 78, 2, 49, 2, 69, 2, 91, 2, 60, 2, 69, 2, 44, 2, 148, 2, 50, 3, 145, 3, 154, 3, 12, 3, 245, 2, 212, 2, 180, 2, 102, 2, 67, 2, 47, 2, 47, 2, 4, 2, 64, 2, 15, 2,
    29, 2, 35, 2, 60, 2, 106, 2, 119, 2, 105, 2, 82, 2, 177, 2, 174, 2, 118, 2, 103, 2, 39, 2, 56, 2, 5, 2, 72, 2, 47, 2, 25, 2, 5, 2, 45, 2, 12, 2, 254, 1, 254, 1, 98, 2, 44, 2, 11, 2,
    51, 2, 26, 2, 70, 2, 106, 2, 90, 2, 62, 2, 57, 2, 83, 2, 84, 2, 62, 2, 58, 2, 68, 2, 30, 2, 53, 2, 78, 2, 29, 2, 111, 2, 119, 2, 64, 2, 51, 2, 28, 2, 56, 2, 47, 2, 37, 2, 68, 2,
    57, 2, 22, 2, 20, 2, 39, 2, 70, 2, 10, 2, 31, 2, 40, 2, 14, 2, 44, 2, 34, 2, 55, 2, 44, 2, 69, 2, 90, 2, 86, 2, 121, 2, 90, 2, 108, 2, 108, 2, 19, 2, 68, 2, 77, 2, 52, 2, 41, 2,
    72, 2, 48, 2, 85, 2, 26, 2, 25, 2, 53, 2, 28, 2, 42, 2, 70, 2, 68, 2, 104, 2, 39, 2, 50, 2, 69, 2, 27, 2, 38, 2, 59, 2, 37, 2, 61, 2, 64, 2, 52, 2, 88, 2, 22, 2, 21, 2, 55, 2,
    36, 2, 48, 2, 41, 2, 60, 2, 102, 2, 51, 2, 51, 2, 87, 2, 80, 2, 49, 2, 46, 2, 63, 2, 127, 2, 90, 2, 140, 2, 189, 2, 73, 2, 87, 2, 35, 2, 153, 2, 20, 2, 48, 2, 36, 2, 103, 2, 102, 2,
    76, 2, 51, 2, 48, 2, 44, 2, 48, 2, 70, 2, 51, 2, 60, 2, 20, 2, 13, 2, 71, 2, 71, 2, 90, 2, 52, 2, 45, 2, 41, 2, 82, 2, 85, 2, 68, 2, 92, 2, 96, 2, 32, 2, 59, 2, 31, 2, 25, 2,
    35, 2, 79, 2, 81, 2, 68, 2, 48, 2, 52, 2, 21, 2, 38, 2, 30, 2, 18, 2, 36, 2, 51, 2, 33, 2, 65, 2, 77, 2, 144, 2, 79, 2, 77, 2, 34, 2, 85, 2, 33, 2, 22, 2, 67, 2, 58, 2, 18, 2,
    20, 2, 86, 2, 80, 2, 57, 2, 43, 2, 79, 2, 38, 2, 18, 2, 14, 2, 40, 2, 61, 2, 68, 2, 73, 2, 7, 2, 72, 2, 54, 2, 35, 2, 42, 2, 47, 2, 1, 2, 15, 2, 46, 2, 54, 2, 77, 2, 234, 1,
    57, 2, 48, 2, 45, 2, 234, 1, 244, 1, 20, 2, 68, 2, 23, 2, 58, 2, 57, 2, 8, 2, 67, 2, 65, 2, 31, 2, 60, 2, 39, 2, 77, 2, 26, 2, 26, 2, 26, 2, 46, 2, 68, 2, 32, 2, 22, 2, 25, 2,
    33, 2, 20, 2, 60, 2, 88, 2, 29, 2, 41, 2, 89, 2, 44, 2, 52, 2, 41, 2, 54, 2, 45, 2, 22, 2, 82, 2, 24, 2, 72, 2, 17, 2, 27, 2, 42, 2, 37, 2, 238, 1, 5, 2, 27, 2, 47, 2, 42, 2,
    25, 2, 47, 2, 24, 2, 44, 2, 50, 2, 61, 2, 49, 2, 35, 2, 20, 2, 23, 2, 57, 2, 22, 2, 23, 2, 37, 2, 12, 2, 28, 2, 65, 2, 69, 2, 58, 2, 26, 2, 18, 2, 16, 2, 43, 2, 33, 2, 57, 2,
    118, 2, 208, 2, 92, 3, 172, 3, 198, 3, 164, 2, 102, 2, 85, 2, 21, 2, 62, 2, 23, 2, 8, 2, 36, 2, 36, 2, 35, 2, 49, 2, 4, 2, 28, 2, 69, 2, 14, 2, 14, 2, 36, 2, 23, 2, 19, 2, 19, 2,
    20, 2, 23, 2, 43, 2, 24, 2, 20, 2, 45, 2, 245, 1, 19, 2, 25, 2, 4, 2, 55, 2, 26, 2, 8, 2, 31, 2, 28, 2, 56, 2, 12, 2, 52, 2, 10, 2, 22, 2, 36, 2, 22, 2, 14, 2, 24, 2, 250, 1,
    5, 2, 30, 2, 32, 2, 43, 2, 32, 2, 8, 2, 55, 2, 7, 2, 61, 2, 208, 2, 148, 4, 36, 10, 94, 17, 7, 24, 32, 21, 66, 14, 189, 7, 213, 4, 247, 3, 137, 3, 205, 2, 35, 2, 205, 1, 202, 1, 187, 1,
    168, 1, 200, 1, 214, 1, 181, 1, 164, 1, 227, 1, 161, 1, 189, 1, 174, 1, 177, 1, 217, 1, 172, 1, 192, 1, 208, 1, 176, 1, 173, 1, 177, 1, 179, 1, 187, 1, 189, 1, 149, 1, 179, 1, 192, 1, 174, 1, 185, 1,
    154, 1, 201, 1, 141, 1, 160, 1, 195, 1, 209, 1, 171, 1, 139, 1, 193, 1, 171, 1, 158, 1, 189, 1, 166, 1, 182, 1, 159, 1, 176, 1, 191, 1, 165, 1, 174, 1, 174, 1, 170, 1, 160, 1, 155, 1, 191, 1, 189, 1,
    29, 2, 163, 2, 36, 3, 161, 3, 45, 3, 148, 2, 230, 1, 169, 1, 187, 1, 169, 1, 172, 1, 142, 1, 161, 1, 179, 1, 184, 1, 10, 2, 55, 2, 135, 2, 149, 2, 87, 2, 15, 2, 202, 1, 165, 1, 152, 1, 162, 1,
    141, 1, 174, 1, 166, 1, 176, 1, 147, 1, 174, 1, 143, 1, 128, 1, 135, 1, 125, 1, 137, 1, 133, 1, 199, 1, 179, 1, 196, 1, 167, 1, 145, 1, 110, 1, 175, 1, 158, 1, 170, 1, 143, 1, 132, 1, 147, 1, 145, 1,
    129, 1, 153, 1, 168, 1, 159, 1, 182, 1, 180, 1, 162, 1, 118, 1, 137, 1, 120, 1, 167, 1, 178, 1, 101, 2, 6, 3, 101, 3, 46, 3, 160, 2, 204, 1, 154, 1, 169, 1, 118, 1, 132, 1, 112, 1, 97, 1, 151, 1,
    124, 1, 97, 1, 119, 1, 149, 1, 123, 1, 119, 1, 123, 1, 110, 1, 127, 1, 115, 1, 108, 1, 68, 1, 106, 1, 137, 1, 110, 1, 111, 1, 115, 1, 82, 1, 91, 1, 102, 1, 87, 1, 95, 1, 150, 1, 99, 1, 67, 1,
    106, 1, 96, 1, 103, 1, 104, 1, 94, 1, 107, 1, 155, 1, 113, 1, 160, 1, 133, 1, 124, 1, 139, 1, 105, 1, 111, 1, 94, 1, 108, 1, 119, 1, 139, 1, 162, 1, 51, 2, 255, 2, 138, 3, 70, 3, 86, 2, 206, 1,
    130, 1, 97, 1, 115, 1, 128, 1, 162, 1, 158, 1, 131, 1, 111, 1, 138, 1, 111, 1, 65, 1, 104, 1, 157, 1, 144, 1, 121, 2, 234, 3, 11, 7, 153, 8, 49, 8, 234, 5, 112, 3, 29, 2, 178, 1, 135, 1, 128, 1,
    97, 1, 79, 1, 62, 1, 71, 1, 96, 1, 68, 1, 62, 1, 75, 1, 48, 1, 45, 1, 37, 1, 62, 1, 29, 1, 55, 1, 61, 1, 54, 1, 51, 1, 97, 1, 93, 1, 106, 1, 134, 1, 116, 1, 98, 1, 97, 1, 50, 1,
    41, 1, 90, 1, 75, 1, 48, 1, 45, 1, 22, 1, 43, 1, 70, 1, 65, 1, 52, 1, 26, 1, 65, 1, 44, 1, 26, 1, 43, 1, 54, 1, 54, 1, 34, 1, 44, 1, 25, 1, 79, 1, 68, 1, 63, 1, 60, 1, 56, 1,
    40, 1, 54, 1, 63, 1, 34, 1, 31, 1, 65, 1, 36, 1, 62, 1, 67, 1, 62, 1, 37, 1, 80, 1, 52, 1, 63, 1, 46, 1, 60, 1, 26, 1, 31, 1, 48, 1, 72, 1, 66, 1, 61, 1, 41, 1, 46, 1, 58, 1,
    72, 1, 76, 1, 56, 1, 80, 1, 68, 1, 57, 1, 62, 1, 50, 1, 29, 1, 13, 1, 56, 1, 42, 1, 36, 1, 57, 1, 71, 1, 52, 1, 40, 1, 32, 1, 84, 1, 41, 1, 47, 1, 74, 1, 48, 1, 30, 1, 64, 1,
    37, 1, 25, 1, 34, 1, 39, 1, 58, 1, 27, 1, 10, 1, 36, 1, 21, 1, 38, 1, 24, 1, 35, 1, 35, 1, 47, 1, 24, 1, 39, 1, 48, 1, 27, 1, 37, 1, 31, 1, 30, 1, 37, 1, 52, 1, 47, 1, 4, 1,
    25, 1, 22, 1, 9, 1, 7, 1, 7, 1, 35, 1, 45, 1, 4, 1, 14, 1, 27, 1, 99, 1, 174, 1, 22, 2, 42, 2, 253, 1, 189, 1, 86, 1, 41, 1, 6, 1, 250, 22, 1, 229, 253, 0, 1, 2, 1, 234, 255, 9,
    1, 14, 1, 250, 50, 1, 32, 1, 241, 3, 1, 242, 253, 253, 1, 1, 0, 1, 231, 223, 255, 239, 2, 1, 245, 243, 220, 248, 246, 244, 12, 1, 10, 1, 247, 232, 208, 240, 234, 2, 1, 231, 242, 232, 244, 250, 239, 246, 234, 228,
    226, 237, 236, 233, 226, 248, 243, 239, 223, 29, 1, 250, 241, 246, 229, 221, 236, 220, 1, 1, 213, 231, 213, 249, 226, 248, 230, 208, 229, 233, 228, 224, 210, 242, 217, 232, 244, 20, 1, 21, 1, 14, 1, 233, 237, 223, 226, 225, 219, 221,
    222, 188, 216, 200, 234, 234, 225, 37, 1, 132, 1, 98, 2, 80, 3, 132, 3, 56, 3, 50, 2, 80, 1, 15, 1, 210, 215, 222, 202, 231, 222, 211, 194, 207, 218, 191, 212, 199, 201, 209, 217, 232, 220, 221, 213, 197, 197, 199, 232, 200,
    228, 245, 244, 237, 227, 214, 225, 213, 193, 212, 189, 194, 187, 208, 207, 218, 204, 206, 212, 222, 195, 207, 201, 202, 222, 171, 184, 207, 202, 189, 200, 196, 188, 213, 199, 181, 229, 227, 215, 218, 208, 204, 233, 203, 217, 193, 197, 201, 192, 184,
    208, 201, 216, 234, 246, 234, 216, 215, 182, 234, 238, 225, 41, 1, 129, 1, 24, 2, 13, 3, 133, 4, 207, 5, 46, 6, 39, 5, 209, 3, 72, 2, 158, 1, 84, 1, 4, 1, 198, 222, 201, 192, 197, 195, 183, 184, 182, 189, 216, 216,
    202, 204, 212, 193, 198, 218, 215, 196, 218, 180, 192, 225, 206, 158, 186, 200, 188, 197, 197, 188, 187, 197, 191, 173, 171, 187, 229, 167, 213, 171, 211, 201, 190, 170, 173, 197, 197, 189, 206, 187, 194, 206, 174, 179, 190, 179, 179, 219, 185, 179,
    161, 164, 199, 228, 224, 196, 202, 182, 214, 192, 184, 179, 187, 201, 189, 197, 198, 149, 196, 163, 166, 214, 192, 177, 178, 162, 178, 196, 173, 183, 199, 171, 187, 208, 236, 15, 1, 23, 1, 45, 1, 47, 1, 231, 209, 177, 176, 192, 189, 174,
    187, 168, 144, 189, 176, 196, 188, 197, 200, 223, 179, 197, 185, 187, 176, 178, 197, 196, 182, 190, 179, 170, 165, 182, 176, 205, 185, 203, 12, 1, 175, 1, 229, 3, 30, 8, 46, 13, 57, 16, 42, 14, 147, 9, 218, 4, 46, 2, 243, 195,
    171, 164, 172, 152, 146, 159, 154, 160, 148, 168, 162, 155, 154, 168, 168, 143, 155, 155, 161, 154, 161, 172, 194, 170, 170, 162, 139, 175, 161, 148, 171, 168, 162, 158, 137, 163, 188, 177, 180, 169, 161, 174, 144, 169, 152, 201, 234, 232, 205, 180,
    169, 143, 150, 157, 138, 158, 144, 160, 149, 173, 146, 169, 178, 154, 154, 152, 162, 163, 185, 167, 174, 160, 166, 131, 137, 171, 156, 149, 172, 135, 154, 154, 153, 198, 155, 164, 161, 161, 169, 146, 146, 151, 166, 151, 154, 158, 168, 127, 178, 156,
    156, 158, 154, 152, 150, 162, 148, 149, 147, 138, 162, 147, 156, 168, 180, 174, 139, 150, 172, 149, 160, 148, 142, 154, 173, 148, 133, 169, 153, 128, 158, 164, 155, 170, 165, 156, 164, 151, 180, 148, 150, 156, 173, 160, 157, 144, 165, 173, 180, 159,
    156, 158, 173, 159, 172, 159, 193, 159, 148, 160, 161, 147, 176, 154, 159, 159, 176, 135, 152, 137, 163, 152, 128, 161, 164, 161, 169, 153, 173, 162, 154, 155, 157, 161, 177, 184, 154, 181, 156, 155, 121, 154, 148, 174, 161, 151, 171, 151, 130, 157,
    161, 176, 145, 172, 152, 172, 156, 136, 160, 177, 156, 142, 143, 135, 165, 147, 151, 151, 139, 161, 142, 152, 153, 166, 152, 170, 192, 157, 153, 170, 154, 147, 172, 161, 189, 189, 181, 189, 173, 156, 160, 161, 152, 152, 159, 147, 163, 163, 183, 164,
    163, 159, 142, 155, 162, 135, 151, 156, 142, 150, 126, 150, 147, 137, 152, 154, 131, 132, 156, 155, 147, 140, 138, 124, 139, 134, 128, 134, 154, 142, 155, 153, 134, 166, 196, 83, 1, 94, 2, 144, 3, 198, 3, 138, 3, 161, 2, 161, 1, 221,
    161, 136, 132, 112, 142, 124, 114, 124, 120, 133, 132, 117, 110, 119, 118, 131, 128, 140, 130, 125, 143, 133, 126, 123, 132, 122, 126, 114, 126, 128, 120, 113, 132, 124, 141, 110, 119, 104, 120, 119, 132, 140, 124, 123, 138, 120, 116, 138, 123, 120,
    175, 229, 251, 233, 224, 175, 139, 128, 119, 141, 108, 111, 112, 120, 114, 131, 143, 159, 152, 148, 159, 158, 112, 113, 113, 100, 122, 107, 107, 117, 118, 109, 115, 108, 142, 195, 249, 45, 1, 12, 1, 239, 196, 114, 115, 108, 98, 103, 109, 94,
    81, 91, 97, 80, 95, 91, 95, 104, 128, 165, 137, 147, 131, 116, 162, 158, 205, 215, 167, 133, 96, 123, 96, 91, 89, 103, 87, 98, 97, 90, 112, 122, 168, 22, 1, 185, 1, 80, 2, 94, 2, 248, 1, 70, 1, 187, 127, 86, 84, 59,
    75, 77, 70, 64, 83, 75, 56, 80, 67, 86, 73, 77, 77, 77, 71, 84, 60, 62, 77, 54, 72, 55, 67, 79, 78, 57, 72, 60, 63, 73, 72, 78, 73, 72, 81, 84, 69, 85, 66, 67, 79, 68, 67, 74, 68, 79, 65, 78, 83, 67,
    85, 70, 67, 82, 72, 56, 68, 82, 71, 70, 75, 97, 111, 121, 136, 153, 112, 89, 71, 72, 59, 77, 112, 186, 245, 18, 1, 236, 181, 114, 91, 65, 75, 106, 139, 182, 171, 149, 130, 88, 62, 66, 69, 57, 64, 60, 53, 51, 61, 65,
    55, 53, 59, 52, 56, 59, 64, 59, 54, 56, 66, 55, 65, 72, 68, 48, 51, 63, 57, 58, 64, 58, 75, 116, 196, 56, 1, 213, 1, 250, 1, 212, 1, 77, 1, 179, 119, 93, 59, 55, 56, 65, 46, 49, 48, 53, 46, 53, 57, 64,
    55, 59, 51, 67, 59, 63, 51, 56, 55, 64, 67, 54, 62, 57, 47, 57, 52, 58, 41, 54, 49, 47, 45, 62, 45, 45, 50, 51, 60, 41, 56, 64, 56, 64, 55, 58, 49, 49, 47, 52, 49, 51, 48, 59, 56, 61, 65, 61, 98, 69,
    81, 81, 56, 55, 52, 50, 52, 45, 53, 46, 52, 48, 55, 50, 60, 48, 69, 56, 62, 106, 106, 109, 125, 100, 93, 70, 53, 51, 57, 66, 120, 28, 1, 207, 2, 102, 5, 141, 8, 103, 11, 20, 12, 144, 9, 147, 5, 201, 2, 15,
    1, 110, 46, 39, 39, 49, 38, 50, 43, 55, 41, 32, 29, 35, 43, 37, 37, 46, 37, 51, 36, 35, 38, 44, 39, 36, 31, 42, 36, 42, 37, 44, 42, 48, 31, 39, 46, 43, 48, 35, 39, 43, 43, 39, 47, 35, 29, 42, 33, 38,
    42, 32, 27, 41, 36, 37, 28, 41, 50, 37, 42, 32, 52, 45, 30, 43, 40, 33, 39, 44, 42, 44, 42, 59, 42, 38, 46, 54, 58, 55, 51, 45, 48, 54, 41, 40, 41, 35, 46, 60, 49, 74, 54, 65, 48, 39, 30, 40, 44, 38,
    61, 113, 209, 62, 1, 188, 1, 69, 2, 16, 2, 119, 1, 13, 1, 252, 130, 1, 5, 3, 231, 4, 107, 6, 254, 6, 153, 5, 236, 3, 201, 1, 204, 89, 45, 33, 27, 31, 34, 34, 38, 34, 48, 34, 29, 35, 35, 30, 29, 36,
    29, 23, 43, 36, 43, 39, 32, 40, 22, 38, 39, 29, 29, 23, 35, 29, 52, 45, 52, 35, 35, 33, 33, 39, 23, 27, 25, 17, 35, 32, 26, 29, 24, 20, 31, 45, 27, 28, 26, 21, 25, 27, 27, 21, 27, 28, 36, 36, 25, 32,
    27, 27, 34, 32, 29, 22, 26, 28, 24, 35, 26, 27, 25, 29, 29, 25, 31, 29, 32, 30, 33, 28, 23, 41, 27, 36, 26, 30, 32, 27, 26, 34, 34, 41, 31, 25, 40, 34, 28, 32, 23, 31, 25, 38, 29, 31, 29, 32, 31, 29,
    18, 35, 21, 26, 27, 33, 27, 33, 33, 36, 44, 48, 34, 29, 26, 39, 29, 21, 34, 23, 36, 40, 29, 42, 33, 40, 39, 26, 23, 31, 18, 22, 27, 21, 29, 30, 22, 28, 35, 30, 31, 32, 26, 30, 26, 31, 29, 22, 28, 30,
    23, 35, 30, 28, 26, 20, 32, 30, 23, 36, 33, 19, 29, 30, 29, 30, 35, 33, 28, 31, 17, 37, 40, 56, 34, 46, 41, 39, 23, 29, 30, 25, 41, 30, 32, 35, 25, 23, 22, 29, 30, 16, 33, 31, 28, 26, 28, 29, 43, 37,
    42, 51, 71, 78, 64, 48, 41, 35, 23, 26, 23, 36, 26, 26, 28, 29, 25, 31, 20, 26, 36, 25, 33, 31, 25, 30, 25, 37, 26, 38, 35, 52, 60, 69, 61, 82, 53, 52, 40, 39, 28, 33, 34, 16, 24, 32, 35, 22, 30, 36,
    25, 32, 40, 26, 34, 32, 29, 32, 22, 17, 19, 27, 17, 27, 33, 40, 45, 51, 65, 47, 45, 32, 30, 27, 24, 31, 26, 34, 27, 23, 37, 29, 40, 43, 47, 35, 42, 43, 39, 39, 33, 39, 38, 39, 20, 25, 35, 29, 24, 32,
    21, 31, 24, 12, 30, 24, 20, 27, 22, 21, 29, 30, 28, 28, 24, 32, 23, 24, 32, 29, 29, 27, 24, 30, 23, 19, 27, 23, 26, 23, 24, 21, 23, 32, 21, 24, 24, 32, 30, 32, 30, 33, 21, 23, 22, 16, 31, 33, 32, 18,
    22, 23, 34, 38, 40, 43, 42, 33, 38, 40, 33, 31, 26, 37, 29, 24, 27, 25, 20, 25, 23, 33, 19, 14, 26, 29, 37, 29, 33, 27, 28, 24, 20, 27, 23, 36, 23, 37, 37, 33, 24, 24, 32, 20, 30, 23, 22, 19, 32, 29,
    28, 27, 30, 32, 30, 30, 25, 24, 35, 23, 31, 28, 30, 24, 16, 19, 25, 23, 28, 23, 28, 25, 36, 32, 27, 31, 22, 27, 24, 35, 34, 27, 39, 30, 26, 25, 26, 39, 21, 32, 35, 24, 16, 22, 29, 41, 32, 22, 18, 32,
    28, 26, 28, 32, 24, 25, 27, 28, 20, 22, 26, 38, 25, 26, 28, 25, 30, 27, 28, 32, 29, 27, 27, 20, 26, 37, 23, 28, 26, 21, 20, 20, 21, 22, 24, 25, 37, 37, 33, 31, 23, 25, 22, 35, 19, 32, 27, 16, 24, 23,
    25, 24, 39, 23, 23, 25, 33, 35, 31, 25, 26, 42, 28, 22, 24, 30, 30, 30, 24, 36, 29, 33, 28, 34, 21, 26, 40, 37, 41, 36, 35, 25, 31, 26, 26, 21, 34, 32, 35, 28, 40, 39, 29, 51, 63, 57, 60, 69, 48, 47,
    37, 41, 26, 43, 36, 22, 23, 29, 15, 33, 31, 25, 20, 29, 28, 17, 21, 22, 32, 34, 35, 37, 42, 28, 31, 26, 26, 29, 25, 26, 26, 28, 29, 16, 24, 33, 27, 33, 28, 18, 24, 25, 21, 26, 25, 27, 28, 26, 27, 33,
    27, 30, 27, 22, 25, 27, 26, 21, 22, 23, 27, 24, 24, 27, 35, 26, 31, 25, 21, 32, 33, 26, 39, 22, 26, 31, 31, 30, 41, 35, 41, 28, 21, 33, 19, 27, 31, 23, 30, 27, 32, 19, 30, 32, 20, 19, 24, 24, 35, 26,
    26, 24, 25, 31, 31, 20, 28, 23, 15, 35, 26, 26, 17, 24, 23, 20, 32, 27, 35, 28, 28, 25, 25, 21, 29, 28, 32, 35, 22, 24, 26, 22, 23, 24, 30, 30, 31, 32, 27, 28, 24, 28, 33, 32, 36, 18, 24, 19, 21, 34,
    33, 24, 29, 22, 25, 23, 30, 24, 32, 27, 25, 25, 15, 27, 28, 28, 30, 22, 17, 18, 25, 33, 30, 29, 27, 26, 17, 21, 29, 25, 27, 30, 30, 25, 20, 24, 25, 33, 20, 21, 32, 28, 30, 25, 26, 24, 29, 28, 36, 30,
    25, 26, 27, 27, 22, 18, 25, 26, 23, 26, 24, 19, 28, 25, 23, 23, 18, 25, 22, 21, 26, 24, 25, 27, 35, 27, 29, 23, 22, 26, 32, 24, 28, 25, 31, 26, 32, 37, 22, 26, 27, 32, 30, 29, 16, 28, 27, 32, 21, 24,
    24, 21, 25, 25, 36, 34, 29, 29, 34, 39, 35, 24, 23, 27, 29, 33, 20, 29, 31, 25, 26, 20, 17, 26, 19, 28, 18, 29, 21, 21, 24, 23, 28, 28, 26, 21, 23, 30, 23, 27, 18, 26, 19, 21, 29, 27, 27, 22, 25, 24,
    30, 12, 23, 25, 17, 29, 20, 22, 23, 22, 28, 27, 23, 30, 14, 25, 27, 24, 30, 24, 25, 17, 21, 22, 23, 33, 24, 18, 27, 25, 24, 21, 16, 24, 28, 17, 23, 27, 13, 19, 21, 23, 20, 18, 17, 21, 34, 27, 15, 27,
    12, 17, 14, 29, 23, 17, 20, 15, 16, 18, 27, 14, 22, 19, 24, 33, 17, 26, 12, 18, 17, 20, 28, 18, 28, 19, 20, 18, 23, 19, 20, 23, 19, 16, 17, 18, 22, 22, 29, 21, 24, 16, 17, 21, 20, 30, 19, 19, 21, 21,
    24, 21, 16, 17, 15, 9, 18, 19, 16, 14, 18, 17, 12, 28, 18, 21, 25, 17, 11, 32, 13, 20, 26, 17, 14, 17, 23, 32, 49, 69, 74, 111, 125, 136, 146, 138, 117, 67, 45, 37, 16, 15, 15, 19, 14, 16, 22, 23, 14, 20,
    23, 23, 10, 21, 16, 24, 16, 20, 15, 15, 20, 20, 21, 17, 12, 19, 20, 18, 16, 15, 18, 27, 14, 17, 14, 15, 18, 12, 14, 12, 16, 20, 22, 22, 15, 12, 15, 22, 15, 17, 11, 14, 13, 9, 10, 17, 17, 20, 22, 18,
    16, 24, 26, 40, 51, 63, 74, 77, 58, 42, 27, 20, 21, 30, 28, 27, 45, 39, 48, 39, 37, 36, 32, 25, 14, 16, 16, 14, 18, 12, 11, 15, 11, 19, 22, 24, 17, 23, 26, 30, 42, 41, 30, 26, 25, 21, 12, 17, 18, 17,
    6, 11, 16, 14, 18, 17, 18, 20, 19, 14, 16, 15, 20, 17, 18, 15, 12, 16, 13, 16, 11, 10, 11, 14, 19, 20, 18, 27, 14, 14, 13, 15, 10, 9, 21, 15, 15, 9, 13, 29, 10, 18, 15, 19, 17, 14, 15, 13, 14, 10,
    21, 15, 13, 18, 16, 15, 16, 16, 9, 16, 15, 11, 15, 17, 18, 19, 18, 17, 13, 11, 14, 18, 19, 10, 11, 17, 8, 16, 20, 23, 23, 27, 25, 24, 21, 15, 16, 17, 13, 10, 11, 14, 18, 15, 19, 11, 14, 9, 11, 22,
    9, 14, 16, 17, 14, 14, 18, 14, 13, 14, 13, 15, 14, 15, 22, 16, 14, 11, 11, 14, 18, 9, 12, 14, 21, 21, 21, 39, 46, 43, 52, 46, 39, 25, 14, 16, 13, 19, 25, 23, 46, 77, 106, 131, 201, 208, 195, 162, 149, 99,
    107, 107, 111, 126, 130, 89, 66, 49, 46, 26, 20, 20, 19, 16, 13, 16, 20, 19, 11, 12, 6, 18, 8, 14, 17, 19, 13, 19, 17, 7, 10, 12, 11, 13, 11, 16, 13, 16, 13, 19, 11, 15, 16, 8, 18, 13, 8, 16, 12, 15,
    15, 27, 13, 18, 21, 11, 25, 29, 39, 55, 85, 95, 111, 105, 70, 52, 46, 30, 28, 32, 30, 34, 37, 32, 22, 12, 18, 25, 34, 50, 91, 107, 138, 132, 104, 78, 46, 26, 22, 19, 13, 14, 22, 18, 19, 20, 22, 34, 48, 45,
    46, 27, 30, 22, 15, 11, 16, 14, 14, 16, 17, 5, 14, 7, 11, 15, 15, 15, 9, 10, 13, 17, 10, 13, 12, 23, 14, 11, 19, 18, 9, 8, 13, 16, 18, 14, 14, 15, 12, 19, 10, 15, 10, 15, 13, 10, 25, 18, 14, 15,
    17, 19, 18, 19, 25, 30, 39, 24, 23, 31, 14, 18, 13, 11, 17, 8, 21, 17, 10, 17, 10, 16, 15, 16, 8, 13, 14, 16, 19, 16, 27, 20, 15, 20, 19, 15, 19, 22, 19, 19, 16, 16, 14, 18, 26, 17, 17, 22, 17, 18,
    16, 11, 12, 11, 18, 14, 11, 12, 16, 18, 23, 15, 23, 11, 22, 22, 19, 15, 14, 11, 19, 12, 11, 11, 16, 19, 16, 14, 16, 17, 20, 18, 15, 16, 17, 15, 25, 15, 10, 19, 12, 16, 14, 21, 22, 15, 15, 13, 16, 15,
    12, 10, 13, 10, 17, 15, 17, 14, 17, 16, 8, 15, 11, 13, 12, 13, 7, 14, 16, 14, 20, 9, 16, 15, 16, 14, 16, 10, 13, 7, 20, 17, 11, 19, 14, 12, 15, 21, 17, 18, 12, 14, 19, 11, 16, 7, 13, 8, 16, 15,
    14, 11, 18, 11, 15, 11, 18, 21, 11, 16, 11, 14, 14, 13, 17, 16, 14, 22, 11, 14, 9, 13, 13, 15, 15, 21, 20, 16, 21, 8, 10, 14, 17, 13, 17, 10, 16, 14, 19, 15, 23, 14, 13, 11, 15, 18, 11, 13, 18, 9,
    21, 19, 13, 22, 24, 18, 27, 27, 26, 22, 20, 13, 22, 17, 10, 16, 20, 10, 16, 11, 9, 12, 17, 17, 9, 12, 18, 22, 14, 12, 22, 16, 15, 11, 9, 13, 14, 17, 16, 23, 12, 10, 17, 16, 10, 11, 13, 16, 10, 22,
    14, 13, 13, 17, 11, 19, 5, 12, 12, 15, 8, 12, 20, 22, 21, 20, 12, 11, 17, 21, 14, 14, 18, 10, 17, 16, 17, 8, 7, 19, 16, 12, 20, 14, 21, 18, 14, 17, 21, 22, 20, 27, 22, 19, 14, 18, 19, 16, 9, 11,
    9, 12, 13, 16, 23, 19, 12, 16, 18, 15, 15, 16, 14, 13, 18, 16, 15, 12, 11, 9, 15, 16, 14, 13, 13, 18, 24, 17, 13, 18, 17, 12, 15, 16, 11, 16, 20, 15, 15, 12, 16, 16, 19, 18, 11, 19, 11, 12, 15, 13,
    19, 20, 13, 9, 12, 22, 9, 21, 11, 10, 16, 17, 24, 17, 18, 23, 23, 18, 13, 20, 17, 20, 9, 14, 15, 11, 22, 12, 12, 16, 11, 18, 18, 15, 13, 17, 15, 11, 20, 13, 18, 16, 12, 13, 19, 8, 11, 20, 16, 16,
    17, 20, 26, 17, 20, 16, 14, 20, 10, 17, 17, 14, 17, 22, 11, 11, 19, 20, 13, 14, 18, 17, 17, 16, 16, 17, 16, 16, 12, 18, 16, 18, 15, 23, 11, 14, 15, 24, 20, 20, 14, 17, 19, 7, 16, 20, 18, 20, 16, 19,
    14, 7, 16, 19, 14, 22, 21, 23, 28, 21, 13, 19, 15, 16, 19, 12, 15, 16, 14, 13, 18, 16, 18, 17, 11, 20, 18, 16, 19, 9, 17, 19, 10, 18, 17, 18, 9, 13, 15, 15, 15, 19, 20, 16, 15, 17, 12, 16, 13, 23,
    16, 15, 18, 14, 28, 18, 16, 22, 22, 12, 16, 12, 11, 12, 17, 16, 19, 14, 13, 20, 19, 6, 18, 13, 22, 15, 10, 12, 9, 13, 21, 13, 21, 17, 18, 18, 20, 16, 16, 17, 18, 16, 18, 17, 19, 26, 20, 21, 12, 18,
    17, 16, 13, 7, 23, 20, 15, 9, 12, 21, 17, 20, 14, 19, 15, 20, 26, 14, 19, 16, 19, 15, 11, 10, 16, 5, 21, 16, 10, 14, 23, 22, 26, 15, 15, 15, 14, 14, 17, 18, 18, 19, 16, 18, 22, 16, 20, 13, 13, 19,
    14, 16, 24, 12, 16, 20, 17, 20, 22, 18, 18, 20, 9, 17, 18, 12, 16, 24, 24, 19, 16, 17, 6, 11, 14, 14, 17, 21, 17, 14, 16, 16, 14, 7, 14, 13, 13, 15, 20, 17, 12, 15, 16, 15, 13, 20, 19, 13, 10, 12,
    20, 21, 16, 13, 13, 17, 16, 12, 15, 17, 12, 21, 19, 14, 10, 21, 15, 14, 13, 16, 17, 22, 15, 18, 12, 24, 22, 20, 21, 14, 11, 14, 21, 11, 18, 13, 14, 11, 20, 14, 13, 27, 12, 21, 11, 13, 16, 16, 16, 14,
    11, 27, 23, 22, 16, 11, 10, 20, 14, 20, 20, 14, 6, 20, 14, 19, 22, 13, 19, 13, 20, 14, 18, 14, 17, 14, 15, 14, 16, 11, 17, 15, 17, 16, 14, 9, 13, 16, 18, 19, 16, 16, 13, 15, 12, 16, 23, 14, 20, 19,
    16, 19, 9, 15, 22, 12, 14, 15, 15, 25, 13, 24, 16, 13, 15, 16, 16, 20, 18, 17, 13, 14, 15, 30, 24, 13, 17, 15, 22, 16, 18, 15, 15, 19, 13, 8, 15, 14, 16, 15, 11, 14, 22, 16, 14, 14, 25, 13, 19, 15,
    16, 11, 16, 18, 18, 12, 24, 23, 18, 16, 16, 24, 15, 22, 9, 10, 14, 19, 16, 18, 17, 18, 10, 13, 14, 9, 20, 16, 19, 10, 13, 22, 15, 16, 27, 13, 20, 21, 18, 22, 20, 10, 14, 16, 17, 20, 15, 18, 19, 19,
    15, 21, 20, 13, 17, 15, 16, 27, 10, 12, 18, 14, 15, 15, 9, 17, 17, 16, 14, 6, 15, 13, 22, 20, 9, 14, 16, 26, 12, 16, 23, 17, 21, 18, 16, 19, 15, 16, 23, 17, 20, 21, 30, 37, 42, 57, 74, 93, 102, 101,
    142, 121, 130, 106, 94, 69, 48, 38, 29, 12, 21, 14, 21, 25, 26, 23, 16, 14, 18, 24, 24, 21, 19, 14, 18, 21, 22, 19, 7, 26, 19, 13, 24, 16, 19, 23, 26, 13, 13, 19, 23, 17, 18, 24, 24, 25, 10, 19, 19, 22,
    19, 10, 15, 19, 18, 14, 17, 13, 18, 20, 17, 19, 18, 21, 11, 18, 13, 18, 18, 10, 25, 25, 16, 21, 14, 20, 31, 19, 21, 21, 11, 24, 17, 15, 20, 17, 19, 19, 29, 25, 11, 17, 21, 19, 21, 14, 31, 22, 13, 17,
    20, 17, 23, 17, 16, 16, 15, 19, 21, 21, 15, 18, 13, 15, 14, 18, 17, 13, 30, 18, 17, 13, 19, 22, 19, 24, 27, 28, 22, 18, 17, 16, 19, 19, 20, 18, 24, 20, 16, 18, 19, 17, 27, 20, 28, 24, 20, 25, 22, 20,
    19, 28, 20, 19, 18, 21, 22, 16, 22, 24, 24, 19, 19, 22, 18, 16, 17, 21, 30, 19, 16, 20, 26, 23, 24, 18, 26, 24, 18, 19, 15, 20, 15, 18, 20, 13, 19, 23, 23, 25, 22, 21, 23, 22, 21, 16, 21, 26, 20, 16,
    21, 24, 20, 21, 21, 23, 30, 19, 23, 22, 20, 24, 20, 25, 25, 24, 23, 23, 26, 33, 24, 18, 20, 17, 12, 22, 24, 13, 11, 20, 23, 22, 18, 32, 10, 19, 25, 24, 26, 22, 29, 17, 15, 21, 22, 28, 21, 21, 21, 13,
    15, 24, 19, 21, 20, 27, 28, 24, 15, 26, 17, 22, 17, 20, 28, 24, 23, 33, 27, 25, 21, 22, 21, 14, 23, 20, 22, 25, 22, 17, 22, 25, 26, 19, 15, 16, 32, 17, 21, 25, 26, 22, 30, 15, 22, 22, 29, 21, 23, 20,
    28, 20, 29, 23, 24, 28, 26, 20, 16, 23, 30, 27, 26, 26, 26, 23, 30, 17, 24, 25, 20, 30, 17, 20, 21, 27, 25, 21, 21, 22, 31, 28, 27, 17, 24, 27, 24, 21, 14, 18, 15, 22, 30, 21, 31, 23, 22, 26, 33, 32,
    24, 22, 20, 29, 15, 28, 27, 24, 20, 29, 22, 28, 27, 25, 23, 26, 25, 22, 21, 17, 19, 17, 23, 24, 23, 34, 26, 17, 32, 23, 27, 31, 27, 29, 25, 30, 20, 18, 18, 14, 23, 33, 21, 15, 19, 23, 26, 18, 25, 32,
    22, 33, 23, 16, 21, 30, 29, 33, 31, 31, 26, 29, 24, 24, 25, 30, 26, 27, 23, 23, 28, 23, 30, 34, 29, 31, 28, 28, 27, 35, 35, 29, 22, 28, 27, 24, 26, 22, 31, 20, 21, 30, 24, 27, 29, 21, 32, 25, 18, 31,
    31, 28, 29, 27, 27, 28, 32, 29, 20, 22, 22, 27, 28, 30, 26, 26, 37, 29, 29, 33, 31, 29, 36, 18, 30, 31, 31, 34, 20, 29, 28, 38, 32, 23, 39, 39, 31, 27, 23, 32, 32, 21, 38, 34, 33, 28, 35, 23, 33, 33,
    29, 26, 26, 30, 29, 37, 32, 25, 30, 30, 26, 26, 32, 30, 30, 20, 37, 34, 30, 37, 28, 35, 29, 22, 18, 41, 24, 32, 25, 23, 39, 38, 31, 37, 23, 28, 29, 29, 30, 29, 18, 34, 33, 24, 35, 33, 35, 23, 33, 27,
    30, 28, 35, 20, 37, 31, 38, 29, 35, 25, 34, 35, 22, 23, 32, 32, 28, 33, 24, 18, 37, 31, 21, 19, 23, 19, 24, 22, 24, 22, 18, 28, 15, 19, 28, 18, 23, 17, 30, 18, 33, 17, 14, 16, 19, 30, 25, 20, 16, 15,
    21, 17, 18, 19, 18, 13, 21, 20, 15, 13, 28, 14, 15, 24, 15, 21, 15, 18, 19, 9, 30, 21, 19, 17, 16, 28, 18, 16, 20, 24, 17, 19, 23, 22, 21, 23, 18, 21, 13, 19, 17, 24, 26, 14, 23, 20, 24, 13, 25, 22,
    18, 22, 20, 16, 18, 10, 20, 12, 24, 16, 14, 26, 16, 15, 15, 24, 18, 22, 14, 9, 23, 18, 20, 14, 23, 12, 17, 11, 17, 16, 17, 15, 16, 19, 17, 14, 26, 18, 20, 13, 21, 18, 14, 17, 11, 12, 16, 16, 16, 18,
    17, 19, 15, 16, 25, 16, 14, 18, 20, 24, 13, 14, 18, 12, 11, 12, 21, 11, 13, 10, 11, 20, 17, 17, 13, 11, 12, 7, 17, 13, 11, 15, 16, 12, 9, 7, 11, 10, 16, 15, 20, 13, 18, 14, 11, 13, 14, 17, 17, 14,
    14, 11, 14, 6, 17, 13, 9, 13, 13, 13, 9, 12, 6, 10, 15, 8, 13, 16, 14, 9, 11, 11, 11, 13, 17, 10, 19, 8, 8, 15, 9, 10, 12, 9, 9, 9, 9, 7, 10, 10, 9, 12, 18, 12, 12, 13, 8, 8, 7, 15,
    7, 10, 14, 11, 13, 5, 11, 10, 12, 13, 14, 13, 12, 8, 10, 12, 12, 2, 7, 9, 3, 7, 12, 9, 7, 9, 4, 4, 5, 12, 3, 11, 8, 8, 7, 9, 6, 6, 11, 9, 10, 9, 9, 4, 9, 9, 3, 10, 9, 10,
    7, 8, 4, 13, 2, 8, 6, 5, 5, 6, 5, 11, 5, 7, 6, 4, 4, 7, 7, 4, 6, 7, 6, 7, 5, 6, 5, 1, 9, 8, 4, 6, 5, 5, 5, 4, 2, 7, 7, 7, 4, 4, 3, 10, 4, 7, 3, 5, 5, 2,
    9, 4, 7, 5, 2, 5, 9, 6, 6, 4, 7, 6, 6, 6, 10, 7, 6, 5, 5, 6, 4, 5, 4, 5, 12, 2, 6, 3, 5, 3, 8, 5, 3, 4, 4, 4, 6, 4, 6, 3, 3, 5, 4, 6, 6, 3, 7, 4, 6, 7,
    8, 4, 9, 5, 1, 3, 4, 5, 5, 5, 3, 7, 4, 8, 2, 9, 5, 6, 6, 4, 7, 6, 9, 7, 2, 4, 3, 5, 5, 7, 5, 8, 1, 6, 4, 6, 3, 5, 6, 5, 7, 3, 2, 8, 8, 6, 1, 3, 2, 9,
    8, 5, 4, 9, 5, 2, 2, 7, 6, 4, 8, 4, 5, 4, 6, 4, 11, 5, 7, 5, 7, 3, 4, 2, 5, 3, 9, 12, 7, 7, 6, 8, 5, 5, 9, 9, 14, 18, 48, 58, 107, 187, 26, 1, 200, 1, 106, 2, 43, 3,
    159, 3, 152, 3, 174, 3, 75, 3, 95, 2, 150, 1, 16, 1, 131, 55, 28, 15, 5, 4, 2, 3, 3, 3, 6, 2, 1, 2, 2, 3, 5, 2, 1, 4, 5, 2, 4, 5, 2, 1, 5, 4, 3, 2, 1, 3, 3, 4, 3, 3,
    3, 1, 4, 4, 3, 5, 3, 3, 2, 5, 1, 4, 3, 4, 3, 3, 3, 5, 4, 6, 0, 1, 4, 4, 0, 1, 1, 2, 1, 6, 1, 2, 2, 3, 4, 3, 4, 4, 4, 2, 4, 3, 3, 1, 1, 3, 5, 3, 4, 2,
    2, 3, 0, 1, 6, 3, 2, 2, 1, 5, 5, 1, 2, 4, 2, 5, 4, 4, 3, 2, 3, 4, 2, 2, 3, 4, 2, 1, 3, 2, 3, 7, 4, 1, 3, 2, 3, 1, 6, 3, 0, 1, 2, 2, 1, 0, 1, 1, 0, 1,
    7, 3, 1, 3, 1, 8, 2, 2, 6, 4, 6, 1, 3, 3, 1, 4, 2, 5, 3, 3, 4, 2, 3, 2, 1, 1, 3, 2, 4, 2, 2, 2, 4, 3, 1, 4, 1, 4, 2, 4, 3, 1, 4, 3, 4, 1, 3, 3, 2, 2,
    1, 2, 3, 4, 3, 5, 2, 0, 1, 1, 4, 1, 3, 3, 1, 5, 2, 2, 2, 3, 0, 1, 4, 1, 0, 1, 3, 0, 2, 3, 2, 4, 1, 0, 1, 2, 0, 1, 3, 5, 2, 1, 3, 1, 0, 1, 4, 0, 1, 3,
    4, 0, 1, 3, 3, 4, 3, 0, 1, 1, 2, 2, 1, 3, 2, 1, 2, 3, 2, 1, 7, 3, 3, 3, 0, 3, 1, 2, 3, 2, 3, 4, 1, 1, 1, 1, 1, 3, 1, 2, 2, 3, 2, 1, 6, 1, 4, 4, 2, 1,
    3, 2, 3, 2, 0, 1, 5, 3, 2, 1, 2, 1, 2, 3, 5, 3, 1, 0, 1, 1, 1, 6, 3, 3, 4, 1, 2, 1, 1, 5, 5, 3, 0, 1, 1, 3, 0, 1, 2, 2, 5, 0, 2, 1, 1, 6, 2, 1, 0, 1,
    1, 1, 2, 1, 4, 0, 1, 2, 1, 1, 3, 2, 3, 2, 1, 3, 4, 2, 4, 3, 4, 3, 3, 2, 3, 0, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 2, 3, 1, 3, 3, 1, 3, 2, 3, 5, 3, 0, 1, 5,
    2, 0, 1, 2, 1, 1, 0, 1, 1, 5, 2, 1, 0, 1, 1, 1, 1, 0, 1, 2, 0, 2, 3, 3, 4, 3, 3, 2, 0, 1, 1, 2, 1, 2, 1, 1, 0, 2, 2, 2, 2, 2, 0, 2, 1, 0, 3, 5, 1, 3,
    2, 1, 1, 2, 2, 0, 1, 1, 1, 0, 1, 1, 4, 2, 4, 0, 1, 2, 5, 0, 1, 5, 4, 1, 2, 2, 0, 1, 1, 2, 2, 2, 1, 3, 2, 1, 0, 1, 1, 4, 0, 1, 5, 0, 1, 9, 2, 1, 3, 0,
    2, 3, 2, 2, 1, 0, 1, 3, 3, 3, 1, 2, 2, 1, 3, 1, 1, 1, 1, 3, 2, 2, 3, 1, 1, 4, 1, 1, 0, 1, 6, 1, 0, 1, 1, 1, 2, 1, 3, 0, 1, 2, 1, 2, 4, 5, 2, 4, 2, 3,
    3, 5, 1, 0, 2, 1, 1, 1, 4, 2, 3, 1, 1, 4, 1, 2, 3, 1, 2, 2, 1, 2, 0, 1, 3, 4, 1, 2, 2, 0, 1, 2, 4, 1, 2, 0, 1, 1, 4, 2, 0, 1, 2, 3, 2, 5, 3, 1, 2, 1,
    2, 1, 2, 0, 1, 3, 2, 2, 0, 1, 2, 0, 1, 1, 1, 1, 3, 1, 3, 5, 4, 1, 0, 1, 2, 3, 1, 3, 1, 2, 3, 0, 1, 3, 2, 1, 4, 1, 1, 0, 1, 2, 2, 4, 0, 1, 1, 2, 0, 1,
    3, 3, 2, 1, 2, 0, 3, 2, 1, 2, 3, 2, 1, 1, 1, 3, 1, 2, 3, 3, 1, 2, 0, 1, 2, 1, 2, 1, 1, 0, 1, 2, 0, 1, 3, 2, 3, 1, 0, 1, 1, 0, 2, 1, 1, 2, 3, 1, 1, 3,
    2, 1, 3, 1, 1, 4, 1, 0, 1, 2, 3, 1, 4, 1, 2, 1, 3, 3, 3, 1, 1, 3, 1, 0, 1, 1, 4, 4, 1, 1, 2, 2, 0, 1, 1, 0, 1, 1, 0, 2, 1, 3, 2, 2, 2, 4, 3, 1, 3, 2,
    1, 1, 1, 0, 2, 3, 0, 1, 1, 1, 5, 2, 0, 1, 2, 2, 1, 4, 1, 0, 1, 3, 1, 2, 1, 1, 2, 3, 1, 1, 1, 0, 1, 2, 1, 0, 1, 2, 3, 2, 3, 1, 3, 1, 1, 0, 1, 2, 2, 1,
    2, 1, 3, 0, 1, 3, 3, 1, 0, 1, 1, 0, 1, 2, 1, 0, 1, 3, 1, 1, 1, 1, 2, 1, 2, 0, 1, 1, 0, 1, 3, 1, 1, 1, 2, 2, 1, 1, 0, 2, 2, 1, 1, 0, 1, 2, 1, 2, 1, 0,
    1, 1, 2, 0, 1, 4, 3, 1, 0, 3, 1, 2, 1, 0, 1, 1, 1, 1, 0, 2, 1, 1, 3, 1, 0, 1, 1, 1, 0, 2, 3, 4, 1, 0, 1, 3, 0, 1, 4, 1, 0, 1, 1, 2, 1, 1, 1, 3, 0, 1,
    2, 1, 1, 2, 1, 1, 4, 0, 1, 2, 0, 1, 1, 1, 4, 0, 2, 1, 1, 2, 1, 0, 2, 2, 1, 1, 1, 1, 1, 2, 0, 1, 1, 3, 0, 1, 2, 0, 1, 1, 0, 1, 2, 4, 0, 1, 3, 0, 1, 1,
    0, 3, 2, 0, 1, 1, 0, 1, 1, 0, 3, 5, 2, 2, 1, 2, 1, 3, 2, 0, 1, 1, 0, 1, 1, 1, 1, 0, 2, 2, 0, 4, 1, 2, 2, 2, 2, 2, 1, 2, 0, 1, 1, 0, 2, 2, 0, 3, 1, 0,
    1, 1, 2, 0, 3, 2, 0, 1, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 0, 2, 3, 0, 1, 1, 1, 1, 0, 2, 3, 0, 1, 1, 0, 3, 2, 0, 1, 2, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1,
    1, 0, 5, 1, 0, 3, 3, 0, 1, 1, 1, 0, 4, 1, 0, 3, 1, 1, 2, 1, 0, 1, 2, 0, 3, 1, 0, 1, 1, 1, 0, 1, 2, 0, 3, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 4,
    1, 0, 3, 3, 2, 2, 1, 0, 3, 2, 0, 3, 2, 0, 4, 2, 0, 2, 1, 2, 0, 1, 3, 0, 7, 1, 0, 2, 2, 2, 1, 3, 6, 7, 4, 4, 6, 1, 3, 2, 2, 1, 1, 0, 2, 1, 1, 0, 1, 3,
    1, 0, 2, 1, 0, 8, 1, 0, 1, 1, 0, 3, 1, 0, 2, 1, 0, 3, 1, 1, 0, 3, 1, 0, 5, 1, 2, 0, 1, 1, 1, 0, 1, 1, 0, 8, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 2, 2, 1,
    1, 1, 0, 1, 1, 1, 0, 4, 1, 2, 0, 3, 1, 0, 3, 1, 0, 5, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 2, 0, 2, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0,
    4, 2, 0, 3, 1, 0, 6, 2, 1, 3, 4, 7, 5, 15, 12, 21, 18, 12, 13, 10, 9, 3, 2, 0, 4, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 1, 1, 0, 5, 1, 1, 0, 6, 1, 0, 5, 1,
    0, 3, 1, 2, 0, 8, 1, 0, 2, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0, 1, 1, 0, 7, 1, 0, 3, 2, 0, 5, 2, 0, 3, 1, 0, 4, 1, 0, 6, 1, 0, 7, 1, 1, 0, 3, 1, 0, 3, 1,
    1, 0, 3, 1, 0, 8, 1, 0, 3, 1, 0, 1, 1, 0, 3, 1, 1, 0, 2, 1, 0, 7, 1, 0, 2, 1, 0, 6, 1, 0, 7, 1, 0, 7, 1, 0, 9, 1, 0, 2, 1, 0, 3, 1, 0, 16, 1, 0, 6, 1,
    0, 3, 2, 0, 25, 2, 0, 1, 1, 0, 14, 1, 0, 10, 1, 0, 48, 1, 0, 5, 1, 0, 5, 1, 0, 22, 1, 1, 0, 3, 1, 0, 8, 1, 0, 10, 1, 0, 5, 1, 0, 30, 1, 0, 4, 1, 0, 22, 1, 0,
    4, 1, 0, 7, 1, 0, 13, 1, 0, 2, 1, 0, 70, 2, 0, 2, 1, 0, 2, 1, 2, 4, 3, 2, 4, 4, 0, 1, 1, 0, 12, 1, 0, 5, 1, 0, 12, 1, 0, 5, 1, 0, 19, 1, 0, 1, 1, 0, 22, 1,
    0, 6, 1, 0, 8, 1, 0, 10, 1, 0, 14, 1, 0, 88, 2, 0, 28, 1, 0, 14, 1, 0, 9, 1, 0, 5, 1, 0, 10, 1, 0, 19, 1, 0, 96, 1, 0, 27, 1, 0, 21, 1, 0, 22, 1, 0, 65, 1, 0, 11,
    1, 0, 10, 1, 0, 24, 1, 0, 50, 1, 0, 30, 1, 0, 11, 1, 0, 79, 1, 0, 54, 1, 0, 9, 1, 0, 70, 1, 0, 52, 1, 0, 74, 1, 0, 37, 1, 0, 5, 1, 0, 9, 1, 0, 5, 1, 0, 19, 1, 0,
    54, 1, 0, 7, 1, 0, 18, 1, 0, 9, 1, 0, 15, 1, 0, 12, 1, 0, 30, 1, 0, 22, 1, 0, 125, 1, 0, 66, 1, 0, 100, 1, 0, 1, 1, 0, 31, 1, 0, 100, 1, 0, 52, 1, 0, 17, 1, 0, 116, 1,
    0, 128, 1, 0, 81, 1, 0, 17, 1, 0, 33, 1, 0, 57, 1, 0, 9, 1, 1, 0, 108, 1, 0, 82, 1, 0, 140, 1, 0, 31, 1, 0, 81, 1, 0, 99, 1, 0, 14, 1, 0, 4, 1, 0, 114, 1, 0, 80, 1, 0,
    50, 1, 0, 42, 1, 0, 48, 1, 0, 122, 1, 0, 15, 1, 0, 16, 1, 0, 1, 1, 0, 13, 1, 0, 127, 1, 0, 46, 1, 0, 31, 1, 0, 66, 1, 0, 125, 1, 0, 45, 1, 0, 19, 1, 0, 50, 1, 0, 3, 1,
    0, 8, 1, 0, 63, 1, 0, 30, 1, 0, 58, 1, 0, 38, 1, 0, 9, 1, 0, 18, 1, 0, 42, 1, 0, 11, 1, 0, 77, 1, 0, 76, 1, 0, 33, 1, 0, 29, 1, 0, 33, 1, 0, 80, 1, 0, 72, 1, 0, 49,
    1, 0, 213, 1, 0, 192, 1, 0, 9, 1, 0, 35, 1, 0, 142, 1, 0, 16, 1, 0, 36, 1, 0, 28, 1, 0, 14, 1, 0, 227, 1, 0, 53, 1, 0, 40, 1, 0, 3, 1, 0, 20, 1, 0, 134, 1, 0, 95, 1, 0,
    106, 1, 0, 195, 1, 0, 51, 1, 0, 3, 1, 0, 20, 1, 0, 21, 1, 0, 46, 1, 0, 21, 1, 0, 108, 1, 0, 24, 1, 0, 67, 1, 1, 0, 124, 1, 1, 0, 4, 1, 0, 66, 1, 0, 34, 1, 0, 106, 1, 0,
    127, 1, 0, 28, 1, 0, 17, 1, 0, 195, 1, 0, 7, 1, 1, 0, 2, 1, 0, 188, 1, 0, 11, 1, 0, 69, 1, 0, 14, 1, 0, 126, 1, 0, 34, 1, 0, 100, 1, 0, 124, 1, 0, 37, 1, 0, 140, 1, 0, 135,
    1, 0, 14
  };
  assert( test_19_packed.size() == 10003 );
  const vector<uint8_t> test_19_encoded = QRSpecDev::encode_stream_vbyte( test_19_chan_cnts );
  assert( test_19_encoded == test_19_packed );
  vector<uint32_t> test_19_dec;
  const size_t test_19_nbytedec = QRSpecDev::decode_stream_vbyte(test_19_encoded,test_19_dec);
  assert( test_19_nbytedec == test_19_packed.size() );
  assert( test_19_dec == test_19_chan_cnts );
  
  
  
  
  // Test case 20
  const vector<uint32_t> test_20_chan_cnts{
    0, 39, 1, 30, 91, 80, 80, 81, 72, 68, 75, 69, 58, 80, 81, 89, 68, 88, 67, 68,
    73, 72, 68, 76, 77, 74, 71, 79, 73, 80, 64, 63, 58, 74, 70, 57, 43, 50, 60, 62,
    65, 54, 45, 58, 56, 53, 52, 60, 63, 56, 56, 54, 42, 59, 59, 57, 65, 43, 60, 41,
    40, 59, 54, 59, 49, 63, 62, 34, 43, 44, 60, 61, 55, 33, 51, 50, 60, 57, 54, 50,
    38, 49, 62, 45, 48, 53, 54, 56, 46, 44, 55, 73, 57, 50, 49, 59, 65, 46, 58, 60,
    52, 66, 78, 58, 55, 80, 66, 51, 58, 58, 58, 64, 44, 50, 61, 54, 66, 64, 54, 72,
    65, 57, 83, 72, 73, 59, 73, 67, 60, 59, 65, 64, 64, 66, 66, 55, 73, 67, 76, 69,
    69, 73, 46, 67, 76, 76, 66, 75, 70, 70, 72, 66, 70, 62, 73, 59, 76, 83, 72, 57,
    82, 80, 60, 66, 75, 62, 67, 54, 76, 77, 71, 68, 71, 68, 74, 88, 57, 83, 62, 88,
    92, 74, 67, 73, 69, 85, 79, 72, 79, 70, 74, 71, 76, 72, 72, 80, 77, 69, 75, 74,
    62, 67, 63, 82, 90, 79, 69, 73, 78, 82, 84, 77, 72, 72, 75, 75, 79, 71, 68, 67,
    82, 82, 68, 84, 74, 78, 74, 84, 83, 69, 66, 80, 74, 64, 81, 80, 75, 63, 81, 71,
    59, 70, 69, 70, 73, 66, 66, 72, 62, 58, 52, 80, 63, 62, 69, 57, 68, 76, 75, 58,
    73, 61, 63, 71, 76, 65, 71, 55, 65, 59, 69, 67, 75, 67, 54, 60, 56, 57, 67, 62,
    64, 73, 76, 89, 64, 68, 53, 65, 59, 59, 64, 60, 55, 76, 59, 58, 56, 70, 43, 65,
    68, 56, 46, 57, 58, 59, 43, 60, 63, 47, 52, 46, 50, 59, 59, 51, 45, 51, 57, 54,
    54, 53, 58, 51, 73, 54, 55, 54, 43, 53, 47, 44, 56, 61, 52, 60, 56, 43, 45, 53,
    46, 59, 47, 55, 47, 47, 50, 49, 43, 48, 45, 53, 49, 51, 47, 44, 50, 59, 62, 43,
    52, 57, 46, 51, 39, 50, 46, 43, 43, 52, 58, 50, 48, 44, 31, 42, 40, 48, 59, 53,
    50, 43, 36, 42, 41, 45, 37, 49, 50, 32, 41, 36, 47, 49, 45, 39, 34, 36, 52, 47,
    43, 52, 33, 46, 35, 45, 42, 33, 41, 34, 39, 50, 29, 32, 42, 36, 41, 37, 35, 39,
    40, 27, 46, 46, 42, 28, 45, 34, 44, 31, 34, 39, 27, 37, 32, 46, 41, 33, 37, 25,
    39, 27, 38, 29, 20, 29, 37, 40, 39, 40, 46, 37, 39, 34, 31, 34, 30, 38, 32, 31,
    36, 30, 42, 22, 26, 28, 35, 36, 30, 25, 26, 29, 21, 36, 34, 28, 40, 32, 28, 22,
    24, 22, 40, 26, 28, 28, 30, 21, 23, 33, 26, 28, 18, 31, 35, 27, 29, 24, 35, 18,
    27, 32, 21, 16, 28, 31, 26, 34, 24, 25, 23, 22, 24, 32, 21, 29, 19, 16, 24, 29,
    26, 36, 21, 22, 21, 23, 26, 24, 20, 22, 27, 25, 26, 30, 18, 32, 28, 29, 21, 16,
    29, 26, 24, 18, 16, 31, 21, 20, 28, 22, 27, 17, 19, 25, 26, 16, 25, 20, 18, 19,
    15, 15, 22, 18, 26, 24, 17, 26, 25, 19, 19, 20, 26, 16, 12, 16, 26, 21, 30, 25,
    15, 22, 27, 26, 23, 22, 17, 18, 26, 24, 18, 27, 19, 18, 20, 18, 24, 18, 17, 26,
    22, 18, 22, 23, 14, 25, 17, 20, 17, 17, 18, 14, 19, 17, 13, 13, 17, 32, 23, 19,
    21, 14, 25, 14, 28, 12, 18, 23, 12, 18, 20, 19, 26, 21, 20, 23, 17, 19, 29, 15,
    19, 17, 13, 22, 15, 13, 18, 23, 19, 26, 17, 12, 17, 15, 23, 21, 29, 22, 20, 20,
    26, 24, 12, 18, 11, 18, 19, 15, 21, 17, 17, 16, 18, 20, 20, 12, 17, 17, 10, 18,
    18, 20, 20, 20, 34, 15, 14, 14, 11, 14, 17, 10, 12, 18, 28, 17, 10, 20, 19, 15,
    17, 15, 9, 19, 18, 15, 13, 12, 15, 16, 23, 15, 17, 20, 10, 14, 14, 14, 6, 13,
    15, 20, 17, 15, 19, 10, 18, 9, 12, 16, 19, 15, 8, 18, 19, 10, 11, 12, 22, 9,
    13, 18, 11, 14, 13, 13, 15, 15, 11, 13, 8, 11, 18, 10, 9, 16, 16, 15, 10, 11,
    18, 9, 7, 13, 8, 13, 16, 16, 13, 15, 17, 8, 14, 13, 9, 21, 12, 15, 12, 14,
    13, 10, 12, 16, 15, 18, 14, 12, 21, 10, 15, 16, 13, 14, 13, 12, 14, 7, 10, 19,
    13, 17, 11, 16, 11, 14, 21, 8, 10, 13, 11, 13, 11, 14, 15, 11, 16, 16, 15, 17,
    14, 14, 7, 15, 8, 6, 9, 10, 8, 16, 15, 9, 10, 18, 15, 9, 8, 10, 10, 12,
    18, 10, 12, 18, 8, 12, 10, 6, 6, 7, 3, 12, 13, 9, 9, 10, 8, 11, 9, 5,
    15, 15, 7, 11, 7, 11, 17, 10, 7, 7, 10, 14, 11, 16, 9, 14, 12, 16, 8, 11,
    8, 11, 12, 10, 8, 5, 5, 11, 7, 7, 9, 4, 7, 7, 10, 10, 8, 11, 11, 13,
    9, 12, 8, 14, 11, 10, 11, 12, 13, 15, 7, 13, 6, 8, 9, 15, 5, 11, 17, 10,
    10, 5, 8, 7, 9, 13, 7, 8, 12, 14, 6, 8, 11, 6, 5, 12, 11, 13, 11, 8,
    5, 14, 8, 7, 6, 16, 10, 10, 5, 10, 9, 5, 7, 5, 10, 7, 5, 6, 8, 9,
    9, 7, 1, 9, 14, 10, 15, 7, 8, 11, 10, 6, 13, 10, 11, 12, 14, 12, 5, 3,
    7, 11, 16, 8, 17, 9, 4, 9, 7, 6, 6, 10, 5, 8, 12, 8, 9, 4, 5, 8,
    9, 10, 9, 9, 5, 11, 14, 12, 16, 19, 22, 19, 19, 15, 13, 8, 11, 9, 8, 7,
    14, 2, 5, 9, 11, 10, 6, 5, 6, 11, 8, 10, 11, 10, 8, 10, 5, 11, 9, 6,
    8, 5, 10, 9, 11, 14, 9, 10, 7, 7, 10, 9, 7, 10, 6, 9, 4, 3, 12, 7,
    5, 10, 10, 6, 5, 8, 8, 5, 9, 7, 13, 7, 8, 9, 12, 7, 7, 3, 9, 14,
    5, 9, 9, 8, 4, 4, 9, 10, 3, 11, 9, 7, 7, 5, 8, 7, 6, 4, 5, 7,
    8, 5, 8, 8, 6, 6, 7, 4, 11, 9, 8, 10, 8, 7, 7, 10, 7, 6, 9, 12,
    6, 3, 2, 5, 8, 8, 7, 8, 4, 7, 7, 8, 9, 5, 5, 5, 5, 5, 5, 9,
    7, 6, 4, 6, 4, 9, 10, 7, 4, 5, 2, 10, 10, 12, 12, 7, 6, 12, 14, 7,
    3, 5, 4, 5, 7, 8, 6, 4, 5, 12, 8, 4, 5, 12, 8, 7, 12, 8, 6, 10,
    9, 3, 6, 10, 6, 7, 4, 10, 5, 6, 4, 7, 3, 7, 2, 16, 5, 5, 6, 6,
    7, 11, 4, 7, 7, 3, 8, 15, 8, 22, 41, 47, 47, 28, 15, 7, 7, 7, 8, 6,
    4, 7, 7, 9, 9, 3, 6, 5, 7, 2, 4, 8, 5, 5, 6, 4, 3, 6, 4, 7,
    4, 5, 4, 2, 10, 8, 5, 3, 3, 4, 6, 9, 5, 4, 3, 3, 5, 6, 4, 11,
    6, 4, 4, 12, 5, 6, 2, 6, 3, 6, 3, 3, 6, 2, 5, 3, 3, 5, 4, 7,
    4, 5, 5, 7, 8, 5, 4, 6, 5, 6, 5, 7, 6, 5, 5, 5, 5, 3, 5, 8,
    5, 7, 5, 7, 6, 5, 6, 2, 5, 7, 4, 9, 5, 3, 6, 8, 2, 12, 10, 10,
    6, 9, 5, 7, 4, 4, 5, 7, 8, 4, 8, 3, 8, 4, 6, 6, 3, 11, 3, 4,
    5, 3, 4, 8, 8, 7, 7, 7, 5, 3, 3, 2, 2, 6, 0, 1, 4, 7, 7, 4,
    4, 3, 10, 3, 6, 4, 2, 9, 4, 1, 5, 7, 2, 2, 6, 3, 6, 2, 6, 5,
    6, 7, 3, 2, 4, 4, 6, 2, 8, 3, 4, 3, 5, 6, 4, 6, 3, 1, 8, 5,
    4, 8, 5, 5, 5, 2, 3, 7, 11, 6, 1, 1, 3, 4, 5, 5, 7, 2, 6, 7,
    6, 5, 4, 12, 3, 2, 6, 3, 5, 5, 2, 1, 5, 5, 5, 0, 1, 2, 8, 9,
    2, 5, 2, 3, 2, 8, 5, 2, 3, 4, 3, 4, 2, 9, 7, 7, 5, 6, 5, 2,
    5, 6, 2, 2, 1, 6, 6, 7, 1, 3, 3, 7, 3, 8, 5, 6, 4, 5, 6, 3,
    4, 3, 5, 7, 2, 7, 10, 5, 4, 7, 6, 7, 3, 3, 3, 6, 3, 4, 6, 4,
    2, 1, 0, 1, 5, 5, 8, 4, 7, 1, 8, 7, 4, 5, 6, 3, 6, 3, 2, 3,
    4, 1, 4, 4, 2, 6, 2, 4, 2, 2, 3, 3, 5, 5, 7, 1, 6, 6, 5, 7,
    8, 10, 6, 5, 5, 6, 3, 7, 4, 3, 4, 4, 3, 7, 4, 4, 1, 3, 3, 4,
    2, 1, 5, 8, 1, 2, 5, 5, 1, 6, 4, 8, 7, 9, 4, 4, 3, 5, 3, 2,
    6, 2, 3, 8, 2, 4, 4, 5, 2, 2, 4, 3, 2, 4, 6, 10, 3, 4, 5, 6,
    5, 4, 6, 5, 4, 7, 2, 9, 5, 3, 3, 4, 6, 2, 1, 10, 4, 6, 4, 7,
    1, 3, 1, 3, 12, 3, 8, 5, 4, 5, 3, 7, 5, 2, 4, 2, 3, 5, 4, 1,
    2, 5, 4, 1, 5, 1, 7, 6, 7, 4, 5, 6, 8, 1, 6, 6, 5, 3, 6, 4,
    6, 2, 5, 0, 1, 6, 4, 2, 5, 6, 4, 6, 5, 3, 1, 1, 3, 7, 4, 2,
    3, 9, 3, 3, 5, 0, 1, 5, 2, 4, 4, 3, 0, 1, 5, 4, 5, 5, 4, 1,
    5, 4, 5, 8, 5, 2, 4, 2, 5, 3, 4, 3, 4, 0, 1, 6, 6, 1, 7, 3,
    3, 3, 3, 0, 1, 4, 2, 3, 1, 4, 2, 5, 4, 4, 6, 2, 0, 1, 3, 4,
    6, 3, 3, 2, 3, 3, 8, 5, 1, 2, 3, 2, 0, 1, 2, 3, 3, 2, 4, 0,
    1, 3, 2, 6, 5, 6, 4, 8, 2, 6, 4, 6, 6, 2, 4, 3, 1, 7, 2, 3,
    1, 5, 5, 1, 5, 2, 2, 5, 5, 3, 6, 5, 5, 4, 4, 4, 4, 2, 2, 6,
    4, 3, 7, 4, 2, 3, 2, 4, 4, 4, 2, 3, 5, 2, 3, 6, 1, 3, 4, 5,
    1, 2, 4, 6, 4, 2, 4, 5, 4, 0, 1, 2, 8, 3, 0, 1, 1, 2, 4, 5,
    6, 8, 5, 5, 4, 3, 3, 0, 1, 3, 3, 3, 5, 3, 3, 4, 1, 2, 2, 4,
    3, 2, 3, 2, 5, 6, 6, 5, 6, 5, 6, 3, 2, 2, 2, 2, 1, 2, 5, 3,
    3, 2, 1, 2, 7, 2, 10, 4, 4, 4, 1, 7, 6, 2, 2, 2, 4, 3, 4, 1,
    4, 2, 1, 6, 1, 4, 4, 5, 2, 3, 3, 4, 1, 3, 3, 3, 4, 3, 2, 0,
    1, 3, 3, 0, 1, 4, 2, 1, 4, 3, 2, 1, 7, 3, 2, 0, 1, 2, 2, 2,
    3, 3, 1, 3, 6, 3, 2, 2, 7, 1, 4, 4, 5, 2, 2, 6, 4, 0, 1, 3,
    5, 2, 6, 4, 5, 6, 1, 5, 5, 2, 4, 5, 2, 2, 6, 0, 1, 4, 1, 1,
    3, 4, 3, 2, 1, 3, 0, 1, 6, 1, 4, 2, 1, 3, 0, 1, 3, 1, 1, 5,
    3, 2, 5, 1, 6, 4, 7, 4, 1, 5, 2, 2, 2, 3, 3, 2, 1, 2, 0, 1,
    1, 2, 0, 1, 5, 1, 3, 2, 2, 2, 3, 3, 3, 3, 4, 4, 6, 3, 0, 1,
    3, 2, 4, 1, 2, 0, 1, 1, 1, 6, 0, 1, 5, 4, 2, 2, 5, 1, 1, 3,
    2, 0, 1, 1, 2, 4, 4, 4, 2, 2, 3, 0, 1, 1, 3, 1, 4, 1, 0, 1,
    5, 0, 1, 1, 2, 0, 1, 3, 4, 4, 4, 3, 3, 1, 1, 1, 5, 3, 2, 2,
    1, 1, 3, 1, 6, 1, 6, 3, 2, 0, 1, 5, 0, 1, 1, 2, 3, 0, 1, 2,
    5, 2, 3, 3, 4, 4, 1, 2, 1, 1, 4, 3, 3, 4, 1, 1, 4, 5, 2, 3,
    2, 2, 6, 1, 1, 2, 2, 3, 0, 1, 2, 0, 1, 4, 2, 2, 1, 1, 1, 3,
    1, 3, 3, 2, 1, 2, 2, 3, 3, 1, 2, 4, 4, 4, 2, 4, 2, 5, 1, 3,
    4, 4, 1, 3, 5, 0, 1, 6, 2, 1, 1, 5, 5, 2, 2, 5, 4, 4, 2, 5,
    1, 5, 2, 2, 2, 3, 4, 4, 1, 2, 6, 3, 1, 3, 1, 3, 0, 1, 3, 2,
    2, 4, 3, 2, 2, 5, 4, 1, 1, 4, 3, 1, 2, 2, 1, 2, 4, 5, 1, 1,
    4, 3, 2, 2, 5, 3, 3, 5, 0, 1, 3, 1, 1, 0, 1, 3, 4, 7, 1, 1,
    2, 3, 4, 2, 0, 1, 5, 4, 0, 1, 2, 1, 4, 4, 3, 1, 0, 1, 3, 2,
    4, 3, 0, 1, 3, 2, 3, 3, 1, 3, 1, 2, 3, 2, 1, 2, 4, 3, 5, 6,
    18, 28, 20, 11, 10, 10, 1, 3, 1, 3, 1, 4, 5, 2, 1, 4, 1, 2, 3, 2,
    1, 2, 3, 0, 1, 3, 2, 3, 3, 1, 2, 3, 2, 2, 0, 1, 1, 2, 1, 4,
    2, 2, 2, 1, 0, 1, 1, 3, 2, 1, 5, 4, 3, 3, 5, 2, 1, 1, 5, 4,
    3, 3, 3, 1, 4, 4, 2, 1, 3, 2, 3, 4, 1, 1, 5, 3, 4, 2, 3, 3,
    1, 4, 2, 1, 5, 3, 1, 1, 7, 2, 7, 1, 4, 5, 3, 0, 1, 1, 3, 6,
    4, 7, 4, 1, 2, 3, 2, 3, 4, 3, 3, 2, 1, 3, 2, 1, 7, 2, 5, 3,
    0, 1, 2, 5, 1, 1, 4, 1, 0, 1, 1, 4, 1, 4, 3, 3, 3, 1, 1, 2,
    3, 1, 2, 2, 1, 2, 2, 1, 5, 3, 2, 5, 4, 1, 3, 0, 1, 1, 1, 4,
    1, 2, 4, 3, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 1, 3, 3, 0, 1,
    4, 3, 0, 2, 1, 4, 2, 3, 3, 3, 5, 3, 1, 2, 2, 0, 1, 1, 2, 2,
    2, 2, 2, 4, 3, 1, 1, 2, 4, 2, 0, 2, 4, 4, 1, 2, 3, 0, 1, 3,
    1, 5, 1, 1, 2, 3, 0, 1, 2, 4, 1, 1, 0, 1, 5, 2, 1, 0, 2, 3,
    1, 1, 1, 5, 0, 2, 1, 1, 2, 1, 4, 3, 1, 4, 5, 12, 7, 9, 7, 8,
    6, 1, 2, 2, 0, 1, 1, 0, 1, 3, 4, 1, 0, 1, 2, 2, 2, 1, 0, 1,
    1, 2, 1, 0, 1, 1, 2, 3, 1, 1, 3, 0, 1, 1, 0, 1, 1, 4, 3, 1,
    2, 1, 5, 3, 2, 2, 2, 5, 1, 2, 2, 1, 0, 1, 2, 2, 3, 1, 3, 0,
    2, 1, 0, 1, 4, 0, 1, 3, 2, 1, 0, 1, 2, 0, 3, 2, 2, 2, 2, 2,
    2, 2, 0, 1, 2, 2, 2, 3, 5, 2, 3, 4, 4, 0, 1, 1, 6, 2, 5, 1,
    4, 8, 1, 2, 1, 3, 3, 0, 1, 1, 3, 1, 4, 0, 1, 2, 0, 1, 1, 3,
    1, 0, 1, 1, 0, 1, 1, 3, 3, 3, 3, 1, 0, 1, 3, 2, 0, 2, 2, 1,
    0, 1, 1, 3, 1, 2, 4, 5, 1, 1, 2, 1, 4, 1, 3, 3, 1, 2, 3, 0,
    1, 1, 0, 1, 3, 2, 4, 1, 2, 1, 1, 1, 0, 1, 1, 2, 2, 1, 4, 1,
    1, 0, 2, 1, 1, 1, 0, 1, 3, 3, 0, 2, 1, 4, 0, 1, 1, 0, 1, 1,
    0, 1, 1, 4, 2, 0, 1, 2, 1, 1, 2, 3, 2, 1, 2, 3, 3, 2, 2, 3,
    0, 2, 1, 1, 3, 0, 2, 2, 0, 1, 1, 2, 0, 1, 2, 3, 0, 2, 1, 3,
    0, 1, 1, 3, 2, 2, 1, 1, 2, 0, 1, 2, 1, 0, 2, 3, 1, 0, 1, 1,
    0, 2, 1, 1, 3, 1, 0, 1, 1, 1, 1, 0, 1, 2, 1, 1, 1, 0, 1, 3,
    1, 1, 2, 0, 1, 1, 2, 0, 2, 1, 3, 2, 0, 3, 1, 2, 1, 3, 1, 5,
    1, 1, 1, 1, 1, 0, 1, 1, 2, 1, 1, 1, 2, 3, 3, 2, 3, 3, 2, 8,
    4, 5, 2, 1, 0, 2, 1, 1, 3, 2, 1, 0, 2, 3, 2, 1, 2, 2, 0, 2,
    1, 1, 1, 1, 1, 0, 4, 1, 0, 1, 2, 0, 2, 3, 0, 1, 1, 1, 3, 1,
    1, 1, 1, 3, 4, 2, 4, 3, 3, 1, 2, 1, 1, 2, 4, 1, 0, 1, 1, 4,
    3, 3, 2, 4, 4, 1, 5, 2, 1, 0, 1, 2, 2, 0, 2, 1, 1, 3, 2, 1,
    2, 1, 0, 2, 1, 0, 1, 1, 1, 0, 3, 1, 1, 0, 3, 3, 0, 2, 1, 4,
    1, 3, 0, 1, 1, 0, 1, 1, 2, 2, 1, 1, 0, 1, 2, 1, 1, 0, 2, 2,
    1, 2, 0, 2, 3, 0, 1, 2, 1, 2, 1, 1, 0, 1, 3, 0, 1, 1, 1, 1,
    0, 1, 1, 3, 1, 2, 1, 3, 0, 1, 2, 0, 2, 2, 0, 3, 1, 1, 1, 1,
    1, 1, 1, 3, 0, 1, 1, 0, 1, 3, 0, 1, 4, 3, 5, 4, 8, 14, 28, 20,
    35, 31, 21, 14, 8, 4, 4, 2, 0, 5, 1, 2, 1, 0, 4, 1, 2, 0, 3, 1,
    0, 2, 1, 0, 1, 1, 0, 1, 2, 1, 0, 1, 1, 0, 1, 1, 4, 0, 1, 2,
    0, 1, 1, 1, 6, 0, 1, 1, 0, 6, 1, 0, 1, 1, 2, 2, 0, 1, 1, 0,
    2, 1, 2, 1, 0, 1, 2, 1, 2, 1, 0, 2, 2, 2, 1, 1, 2, 0, 1, 1,
    0, 1, 1, 1, 1, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 4, 3, 2, 1,
    1, 3, 2, 1, 2, 2, 4, 6, 2, 1, 3, 3, 0, 1, 1, 3, 1, 1, 1, 0,
    1, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    0, 1, 1, 1, 0, 2, 1, 0, 1, 3, 0, 3, 1, 1, 2, 1, 0, 1, 2, 2,
    1, 2, 1, 0, 1, 1, 0, 3, 1, 1, 1, 0, 1, 1, 1, 4, 4, 2, 2, 0,
    3, 1, 1, 0, 1, 4, 0, 2, 1, 1, 0, 1, 1, 1, 2, 1, 0, 1, 2, 2,
    1, 1, 0, 1, 2, 1, 1, 1, 1, 0, 7, 1, 2, 0, 1, 1, 1, 0, 1, 2,
    1, 2, 0, 1, 1, 0, 2, 2, 0, 3, 1, 0, 2, 2, 1, 1, 0, 1, 1, 0,
    5, 1, 0, 2, 1, 0, 7, 1, 0, 1, 2, 0, 1, 1, 2, 1, 0, 2, 4, 0,
    1, 1, 3, 0, 1, 1, 1, 0, 1, 1, 1, 3, 1, 1, 2, 1, 0, 1, 1, 2,
    1, 1, 2, 2, 0, 1, 2, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 3,
    2, 1, 1, 1, 1, 0, 2, 1, 0, 2, 2, 0, 1, 2, 1, 0, 1, 2, 1, 1,
    0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 4, 2, 0, 1, 1, 1, 1, 0, 1,
    1, 1, 0, 3, 1, 3, 1, 0, 1, 2, 0, 1, 1, 0, 3, 1, 1, 0, 4, 1,
    0, 1, 2, 0, 1, 2, 0, 5, 1, 0, 1, 1, 1, 0, 2, 1, 0, 1, 1, 1,
    1, 1, 1, 0, 1, 1, 2, 0, 1, 1, 1, 1, 1, 2, 0, 6, 1, 2, 0, 1,
    2, 0, 6, 1, 0, 4, 1, 1, 1, 2, 2, 0, 2, 1, 0, 4, 1, 1, 1, 2,
    3, 2, 1, 1, 1, 1, 1, 0, 2, 1, 0, 3, 1, 0, 2, 3, 0, 2, 1, 0,
    4, 1, 0, 1, 1, 0, 1, 1, 2, 1, 0, 3, 1, 0, 1, 1, 1, 0, 4, 1,
    0, 1, 1, 1, 0, 2, 2, 1, 1, 0, 1, 3, 0, 2, 1, 0, 5, 1, 0, 4,
    2, 1, 0, 1, 1, 0, 2, 1, 1, 1, 0, 4, 1, 0, 1, 1, 1, 2, 1, 0,
    2, 1, 0, 1, 1, 1, 2, 1, 1, 1, 0, 2, 1, 0, 4, 1, 0, 5, 1, 0,
    1, 1, 0, 3, 2, 0, 1, 2, 0, 4, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1,
    0, 3, 1, 1, 0, 1, 1, 2, 2, 2, 0, 1, 1, 3, 4, 4, 4, 1, 0, 3,
    1, 0, 2, 1, 0, 1, 2, 0, 6, 2, 1, 0, 2, 1, 0, 1, 2, 0, 1, 1,
    0, 9, 1, 1, 0, 2, 1, 2, 0, 2, 1, 0, 2, 1, 1, 0, 3, 2, 0, 4,
    1, 1, 0, 5, 1, 6, 5, 10, 12, 16, 13, 18, 11, 11, 5, 2, 0, 1, 1, 0,
    4, 1, 1, 0, 3, 1, 0, 4, 1, 0, 5, 1, 0, 2, 3, 0, 5, 1, 0, 7,
    1, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0,
    2, 2, 0, 8, 1, 0, 1, 2, 0, 10, 1, 0, 4, 2, 1, 0, 6, 1, 1, 0,
    9, 2, 0, 1, 2, 0, 2, 2, 0, 2, 1, 0, 2, 1, 0, 2, 1, 1, 1, 0,
    1, 1, 0, 5, 1, 0, 2, 1, 0, 7, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1,
    1, 1, 0, 5, 1, 0, 5, 1, 3, 4, 3, 3, 3, 1, 1, 1, 0, 10, 1, 1,
    0, 3, 1, 0, 5, 1, 1, 0, 1, 1, 1, 0, 6, 1, 0, 4, 1, 0, 1, 1,
    0, 2, 1, 0, 3, 2, 1, 1, 1, 0, 2, 1, 0, 8, 1, 0, 2, 1, 0, 4,
    1, 0, 1, 1, 0, 8, 2, 0, 1, 1, 1, 1, 0, 5, 1, 1, 1, 1, 0, 6,
    1, 1, 0, 3, 1, 0, 6, 1, 2, 0, 2, 3, 0, 5, 1, 1, 1, 0, 2, 1,
    0, 7, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2, 1, 1, 1, 0, 3, 1, 1, 0,
    1, 1, 1, 1, 0, 9, 2, 0, 1, 1, 1, 1, 0, 1, 1, 0, 5, 1, 0, 4,
    2, 0, 3, 1, 0, 8, 1, 2, 0, 2, 1, 1, 1, 0, 4, 1, 0, 5, 1, 0,
    3, 1, 1, 0, 5, 1, 1, 0, 3, 2, 1, 0, 5, 1, 0, 5, 1, 0, 5, 1,
    0, 7, 2, 0, 4, 1, 1, 2, 0, 1, 1, 0, 2, 1, 0, 12, 1, 2, 2, 0,
    1, 2, 1, 0, 1, 2, 0, 2, 1, 0, 3, 1, 0, 1, 1, 1, 1, 0, 2, 1,
    1, 1, 0, 4, 1, 1, 1, 0, 2, 1, 0, 7, 1, 1, 0, 13, 3, 0, 2, 1,
    1, 0, 1, 1, 0, 4, 2, 1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 8, 1, 0,
    2, 1, 0, 8, 1, 0, 8, 1, 0, 2, 1, 0, 10, 1, 0, 6, 1, 0, 1, 1,
    3, 1, 0, 2, 2, 0, 10, 1, 1, 0, 1, 2, 0, 3, 2, 0, 5, 1, 0, 2,
    1, 0, 9, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 4, 1, 1, 1, 0, 2,
    1, 0, 6, 1, 0, 5, 1, 0, 1, 1, 1, 0, 3, 2, 2, 0, 7, 1, 1, 0,
    1, 1, 0, 3, 1, 0, 5, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0,
    1, 1, 0, 1, 1, 0, 10, 1, 1, 0, 3, 1, 0, 2, 2, 1, 2, 1, 0, 1,
    2, 1, 0, 3, 1, 0, 8, 1, 1, 0, 4, 2, 0, 1, 1, 0, 2, 1, 0, 5,
    1, 0, 4, 1, 0, 7, 2, 1, 3, 0, 3, 1, 0, 7, 1, 0, 2, 1, 0, 13,
    1, 0, 3, 2, 0, 16, 1, 0, 12, 1, 0, 5, 1, 0, 4, 1, 0, 6, 1, 0,
    3, 1, 0, 11, 1, 1, 1, 0, 5, 1, 0, 7, 1, 0, 5, 1, 1, 1, 1, 2,
    3, 2, 4, 3, 4, 2, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 5, 1, 0, 3,
    1, 0, 18, 2, 0, 1, 1, 1, 0, 2, 1, 1, 0, 6, 1, 0, 3, 1, 0, 12,
    2, 0, 7, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 4, 1, 0, 1, 1, 0,
    1, 1, 0, 11, 1, 0, 2, 1, 0, 7, 1, 1, 0, 6, 1, 0, 1, 1, 0, 1,
    1, 1, 0, 3, 1, 0, 13, 1, 1, 0, 21, 1, 1, 0, 3, 1, 1, 0, 3, 1,
    1, 2, 1, 0, 4, 2, 0, 10, 1, 0, 1, 1, 2, 0, 3, 1, 0, 3, 1, 0,
    3, 1, 0, 12, 1, 0, 2, 1, 0, 2, 1, 0, 12, 1, 0, 8, 1, 0, 7, 1,
    1, 1, 1, 0, 4, 1, 0, 19, 1, 0, 8, 1, 0, 4, 1, 0, 5, 1, 0, 2,
    1, 0, 18, 1, 3, 1, 1, 0, 15, 1, 0, 14, 1, 0, 3, 1, 0, 16, 1, 0,
    4, 1, 0, 4, 1, 0, 6, 1, 0, 3, 2, 0, 14, 1, 0, 3, 1, 0, 16, 1,
    0, 10, 1, 0, 2, 1, 0, 3, 1, 0, 11, 1, 0, 7, 1, 0, 10, 1, 1, 3,
    1, 0, 1, 1, 0, 4, 1, 0, 2, 1, 0, 17, 1, 0, 4, 1, 0, 6, 1, 0,
    11, 1, 0, 2, 1, 0, 1, 2, 0, 6, 1, 0, 8, 1, 0, 7, 1, 0, 4, 1,
    0, 52, 1, 0, 1, 1, 0, 5, 1, 0, 32, 1, 0, 2, 1, 0, 27, 1, 0, 9,
    1, 1, 0, 36, 1, 0, 5, 1, 0, 26, 1, 0, 9, 1, 0, 17, 1, 0, 17, 2,
    4, 1, 8, 7, 2, 2, 2, 0, 29, 1, 0, 63, 1, 0, 27, 1, 0, 6, 1, 0,
    1, 1, 0, 1, 1, 0, 14, 1, 0, 27, 1, 0, 25, 1, 0, 29, 1, 0, 26, 1,
    0, 39, 1, 0, 2, 1, 0, 26, 1, 0, 19, 1, 0, 3, 1, 0, 22, 1, 0, 2,
    1, 0, 7, 1, 0, 41, 1, 0, 22, 1, 0, 12, 1, 0, 12, 1, 0, 21, 1, 0,
    18, 1, 0, 50, 1, 0, 44, 1, 0, 10, 1, 0, 1, 1, 1, 0, 14, 1, 0, 11,
    1, 0, 63, 1, 0, 61, 1, 0, 9, 1, 0, 18, 1, 0, 60, 1, 0, 8, 1, 0,
    10, 1, 0, 62, 1, 0, 6, 1, 0, 60, 1, 0, 8, 1, 0, 64, 1, 0, 23, 1,
    0, 14, 1, 0, 18, 1, 0, 18, 1, 0, 28, 1, 0, 101, 1, 0, 25, 1, 0, 2,
    1, 0, 18, 1, 0, 72, 1, 0, 13, 1, 0, 42, 1, 0, 11, 1, 0, 36, 1, 0,
    20, 1, 0, 41, 1, 0, 2, 1, 0, 60, 1, 0, 198, 1, 0, 6, 1, 0, 69, 1,
    0, 48, 1, 0, 2, 1, 0, 66, 1, 0, 52, 1, 0, 3, 1, 0, 91, 1, 0, 101,
    1, 0, 24, 1, 0, 11, 1, 0, 12, 1, 0, 5, 1, 0, 12, 1, 0, 28, 1, 0,
    76, 1, 0, 28, 1, 0, 63, 1, 0, 36, 1, 0, 48, 1, 0, 31, 1, 0, 296, 1,
    0, 15, 1, 0, 6, 1, 0, 9, 1, 0, 3, 1, 0, 1, 1, 0, 17, 1, 0, 6,
    1, 0, 43, 1, 0, 35, 1, 0, 10, 1, 0, 18, 1, 0, 8, 1, 0, 2, 1, 0,
    111, 1, 0, 30, 1, 0, 77, 1, 0, 51, 1, 0, 127, 1, 0, 8, 1, 0, 30, 1,
    0, 16, 1, 1, 0, 81, 1, 0, 9, 1, 0, 61, 1, 0, 25, 1, 0, 180, 1, 0,
    22, 1, 0, 63, 1, 0, 16, 1, 0, 91, 1, 0, 31, 1, 0, 50, 1, 0, 35, 1,
    0, 36, 1, 0, 86, 1, 0, 128, 1, 0, 26, 1, 0, 147, 1, 0, 26, 1, 0, 48,
    1, 0, 105, 1, 0, 67, 1, 0, 54, 1, 0, 8, 1, 0, 106, 1, 0, 115, 2, 0,
    55, 2, 0, 21, 1, 0, 5, 1, 0, 75, 1, 0, 147, 1, 0, 62, 1, 0, 20, 1,
    0, 115, 1, 0, 72, 1, 1, 0, 43, 1, 0, 16, 1, 0, 56, 1, 0, 9, 1, 0,
    5, 1, 0, 80, 1, 0, 38, 1, 0, 26, 1, 0, 143, 1, 0, 60, 1, 0, 35, 1,
    0, 66, 1, 0, 5, 1, 0, 3, 1, 0, 123, 1, 0, 40, 1, 0, 147, 1, 0, 24,
    1, 0, 214, 1, 0, 40, 1, 0, 9, 1, 0, 6, 1, 0, 18, 1, 0, 24, 1, 0,
    1, 1, 0, 70, 1, 0, 14, 1, 0, 14, 1, 0, 3, 1, 0, 61, 1, 0, 4, 1,
    0, 14, 1, 0, 12, 1, 0, 50, 1, 0, 29, 1, 0, 72, 1, 0, 153, 1, 0, 8,
    1, 0, 76, 1, 0, 24, 1, 0, 53, 1, 0, 65, 1, 0, 4, 1, 0, 7, 1, 0,
    25, 1, 0, 44, 1, 0, 41, 1, 0, 131, 1, 0, 176, 1, 0, 167, 1, 0, 15, 1,
    0, 33, 1, 0, 1, 1, 0, 49, 1, 0, 35, 1, 0, 5, 1, 0, 164, 1, 0, 10,
    1, 0, 10, 1, 0, 119, 1, 0, 2, 1, 0, 70, 1, 0, 15, 1, 0, 6, 1, 0,
    38, 1, 0, 83, 1, 0, 109, 1, 0, 256, 1, 0, 137, 1, 0, 81, 1, 0, 54, 1,
    0, 180, 1, 0, 45, 1, 0, 270, 1, 0, 28, 1, 0, 136, 1, 0, 17, 1, 0, 65,
    1, 0, 15, 1, 0, 263, 1, 0, 172, 1, 0, 56  };
  assert( test_20_chan_cnts.size() == 5212 );
  const vector<uint8_t> test_20_packed{
    92, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 64, 0,
    0, 0, 0, 4, 0, 0, 39, 1, 30, 91, 80, 80, 81, 72, 68, 75, 69, 58, 80, 81, 89, 68, 88, 67, 68, 73, 72, 68, 76, 77, 74, 71, 79, 73, 80, 64, 63, 58, 74, 70, 57, 43, 50, 60, 62, 65, 54, 45, 58, 56,
    53, 52, 60, 63, 56, 56, 54, 42, 59, 59, 57, 65, 43, 60, 41, 40, 59, 54, 59, 49, 63, 62, 34, 43, 44, 60, 61, 55, 33, 51, 50, 60, 57, 54, 50, 38, 49, 62, 45, 48, 53, 54, 56, 46, 44, 55, 73, 57, 50, 49,
    59, 65, 46, 58, 60, 52, 66, 78, 58, 55, 80, 66, 51, 58, 58, 58, 64, 44, 50, 61, 54, 66, 64, 54, 72, 65, 57, 83, 72, 73, 59, 73, 67, 60, 59, 65, 64, 64, 66, 66, 55, 73, 67, 76, 69, 69, 73, 46, 67, 76,
    76, 66, 75, 70, 70, 72, 66, 70, 62, 73, 59, 76, 83, 72, 57, 82, 80, 60, 66, 75, 62, 67, 54, 76, 77, 71, 68, 71, 68, 74, 88, 57, 83, 62, 88, 92, 74, 67, 73, 69, 85, 79, 72, 79, 70, 74, 71, 76, 72, 72,
    80, 77, 69, 75, 74, 62, 67, 63, 82, 90, 79, 69, 73, 78, 82, 84, 77, 72, 72, 75, 75, 79, 71, 68, 67, 82, 82, 68, 84, 74, 78, 74, 84, 83, 69, 66, 80, 74, 64, 81, 80, 75, 63, 81, 71, 59, 70, 69, 70, 73,
    66, 66, 72, 62, 58, 52, 80, 63, 62, 69, 57, 68, 76, 75, 58, 73, 61, 63, 71, 76, 65, 71, 55, 65, 59, 69, 67, 75, 67, 54, 60, 56, 57, 67, 62, 64, 73, 76, 89, 64, 68, 53, 65, 59, 59, 64, 60, 55, 76, 59,
    58, 56, 70, 43, 65, 68, 56, 46, 57, 58, 59, 43, 60, 63, 47, 52, 46, 50, 59, 59, 51, 45, 51, 57, 54, 54, 53, 58, 51, 73, 54, 55, 54, 43, 53, 47, 44, 56, 61, 52, 60, 56, 43, 45, 53, 46, 59, 47, 55, 47,
    47, 50, 49, 43, 48, 45, 53, 49, 51, 47, 44, 50, 59, 62, 43, 52, 57, 46, 51, 39, 50, 46, 43, 43, 52, 58, 50, 48, 44, 31, 42, 40, 48, 59, 53, 50, 43, 36, 42, 41, 45, 37, 49, 50, 32, 41, 36, 47, 49, 45,
    39, 34, 36, 52, 47, 43, 52, 33, 46, 35, 45, 42, 33, 41, 34, 39, 50, 29, 32, 42, 36, 41, 37, 35, 39, 40, 27, 46, 46, 42, 28, 45, 34, 44, 31, 34, 39, 27, 37, 32, 46, 41, 33, 37, 25, 39, 27, 38, 29, 20,
    29, 37, 40, 39, 40, 46, 37, 39, 34, 31, 34, 30, 38, 32, 31, 36, 30, 42, 22, 26, 28, 35, 36, 30, 25, 26, 29, 21, 36, 34, 28, 40, 32, 28, 22, 24, 22, 40, 26, 28, 28, 30, 21, 23, 33, 26, 28, 18, 31, 35,
    27, 29, 24, 35, 18, 27, 32, 21, 16, 28, 31, 26, 34, 24, 25, 23, 22, 24, 32, 21, 29, 19, 16, 24, 29, 26, 36, 21, 22, 21, 23, 26, 24, 20, 22, 27, 25, 26, 30, 18, 32, 28, 29, 21, 16, 29, 26, 24, 18, 16,
    31, 21, 20, 28, 22, 27, 17, 19, 25, 26, 16, 25, 20, 18, 19, 15, 15, 22, 18, 26, 24, 17, 26, 25, 19, 19, 20, 26, 16, 12, 16, 26, 21, 30, 25, 15, 22, 27, 26, 23, 22, 17, 18, 26, 24, 18, 27, 19, 18, 20,
    18, 24, 18, 17, 26, 22, 18, 22, 23, 14, 25, 17, 20, 17, 17, 18, 14, 19, 17, 13, 13, 17, 32, 23, 19, 21, 14, 25, 14, 28, 12, 18, 23, 12, 18, 20, 19, 26, 21, 20, 23, 17, 19, 29, 15, 19, 17, 13, 22, 15,
    13, 18, 23, 19, 26, 17, 12, 17, 15, 23, 21, 29, 22, 20, 20, 26, 24, 12, 18, 11, 18, 19, 15, 21, 17, 17, 16, 18, 20, 20, 12, 17, 17, 10, 18, 18, 20, 20, 20, 34, 15, 14, 14, 11, 14, 17, 10, 12, 18, 28,
    17, 10, 20, 19, 15, 17, 15, 9, 19, 18, 15, 13, 12, 15, 16, 23, 15, 17, 20, 10, 14, 14, 14, 6, 13, 15, 20, 17, 15, 19, 10, 18, 9, 12, 16, 19, 15, 8, 18, 19, 10, 11, 12, 22, 9, 13, 18, 11, 14, 13,
    13, 15, 15, 11, 13, 8, 11, 18, 10, 9, 16, 16, 15, 10, 11, 18, 9, 7, 13, 8, 13, 16, 16, 13, 15, 17, 8, 14, 13, 9, 21, 12, 15, 12, 14, 13, 10, 12, 16, 15, 18, 14, 12, 21, 10, 15, 16, 13, 14, 13,
    12, 14, 7, 10, 19, 13, 17, 11, 16, 11, 14, 21, 8, 10, 13, 11, 13, 11, 14, 15, 11, 16, 16, 15, 17, 14, 14, 7, 15, 8, 6, 9, 10, 8, 16, 15, 9, 10, 18, 15, 9, 8, 10, 10, 12, 18, 10, 12, 18, 8,
    12, 10, 6, 6, 7, 3, 12, 13, 9, 9, 10, 8, 11, 9, 5, 15, 15, 7, 11, 7, 11, 17, 10, 7, 7, 10, 14, 11, 16, 9, 14, 12, 16, 8, 11, 8, 11, 12, 10, 8, 5, 5, 11, 7, 7, 9, 4, 7, 7, 10,
    10, 8, 11, 11, 13, 9, 12, 8, 14, 11, 10, 11, 12, 13, 15, 7, 13, 6, 8, 9, 15, 5, 11, 17, 10, 10, 5, 8, 7, 9, 13, 7, 8, 12, 14, 6, 8, 11, 6, 5, 12, 11, 13, 11, 8, 5, 14, 8, 7, 6,
    16, 10, 10, 5, 10, 9, 5, 7, 5, 10, 7, 5, 6, 8, 9, 9, 7, 1, 9, 14, 10, 15, 7, 8, 11, 10, 6, 13, 10, 11, 12, 14, 12, 5, 3, 7, 11, 16, 8, 17, 9, 4, 9, 7, 6, 6, 10, 5, 8, 12,
    8, 9, 4, 5, 8, 9, 10, 9, 9, 5, 11, 14, 12, 16, 19, 22, 19, 19, 15, 13, 8, 11, 9, 8, 7, 14, 2, 5, 9, 11, 10, 6, 5, 6, 11, 8, 10, 11, 10, 8, 10, 5, 11, 9, 6, 8, 5, 10, 9, 11,
    14, 9, 10, 7, 7, 10, 9, 7, 10, 6, 9, 4, 3, 12, 7, 5, 10, 10, 6, 5, 8, 8, 5, 9, 7, 13, 7, 8, 9, 12, 7, 7, 3, 9, 14, 5, 9, 9, 8, 4, 4, 9, 10, 3, 11, 9, 7, 7, 5, 8,
    7, 6, 4, 5, 7, 8, 5, 8, 8, 6, 6, 7, 4, 11, 9, 8, 10, 8, 7, 7, 10, 7, 6, 9, 12, 6, 3, 2, 5, 8, 8, 7, 8, 4, 7, 7, 8, 9, 5, 5, 5, 5, 5, 5, 9, 7, 6, 4, 6, 4,
    9, 10, 7, 4, 5, 2, 10, 10, 12, 12, 7, 6, 12, 14, 7, 3, 5, 4, 5, 7, 8, 6, 4, 5, 12, 8, 4, 5, 12, 8, 7, 12, 8, 6, 10, 9, 3, 6, 10, 6, 7, 4, 10, 5, 6, 4, 7, 3, 7, 2,
    16, 5, 5, 6, 6, 7, 11, 4, 7, 7, 3, 8, 15, 8, 22, 41, 47, 47, 28, 15, 7, 7, 7, 8, 6, 4, 7, 7, 9, 9, 3, 6, 5, 7, 2, 4, 8, 5, 5, 6, 4, 3, 6, 4, 7, 4, 5, 4, 2, 10,
    8, 5, 3, 3, 4, 6, 9, 5, 4, 3, 3, 5, 6, 4, 11, 6, 4, 4, 12, 5, 6, 2, 6, 3, 6, 3, 3, 6, 2, 5, 3, 3, 5, 4, 7, 4, 5, 5, 7, 8, 5, 4, 6, 5, 6, 5, 7, 6, 5, 5,
    5, 5, 3, 5, 8, 5, 7, 5, 7, 6, 5, 6, 2, 5, 7, 4, 9, 5, 3, 6, 8, 2, 12, 10, 10, 6, 9, 5, 7, 4, 4, 5, 7, 8, 4, 8, 3, 8, 4, 6, 6, 3, 11, 3, 4, 5, 3, 4, 8, 8,
    7, 7, 7, 5, 3, 3, 2, 2, 6, 0, 1, 4, 7, 7, 4, 4, 3, 10, 3, 6, 4, 2, 9, 4, 1, 5, 7, 2, 2, 6, 3, 6, 2, 6, 5, 6, 7, 3, 2, 4, 4, 6, 2, 8, 3, 4, 3, 5, 6, 4,
    6, 3, 1, 8, 5, 4, 8, 5, 5, 5, 2, 3, 7, 11, 6, 1, 1, 3, 4, 5, 5, 7, 2, 6, 7, 6, 5, 4, 12, 3, 2, 6, 3, 5, 5, 2, 1, 5, 5, 5, 0, 1, 2, 8, 9, 2, 5, 2, 3, 2,
    8, 5, 2, 3, 4, 3, 4, 2, 9, 7, 7, 5, 6, 5, 2, 5, 6, 2, 2, 1, 6, 6, 7, 1, 3, 3, 7, 3, 8, 5, 6, 4, 5, 6, 3, 4, 3, 5, 7, 2, 7, 10, 5, 4, 7, 6, 7, 3, 3, 3,
    6, 3, 4, 6, 4, 2, 1, 0, 1, 5, 5, 8, 4, 7, 1, 8, 7, 4, 5, 6, 3, 6, 3, 2, 3, 4, 1, 4, 4, 2, 6, 2, 4, 2, 2, 3, 3, 5, 5, 7, 1, 6, 6, 5, 7, 8, 10, 6, 5, 5,
    6, 3, 7, 4, 3, 4, 4, 3, 7, 4, 4, 1, 3, 3, 4, 2, 1, 5, 8, 1, 2, 5, 5, 1, 6, 4, 8, 7, 9, 4, 4, 3, 5, 3, 2, 6, 2, 3, 8, 2, 4, 4, 5, 2, 2, 4, 3, 2, 4, 6,
    10, 3, 4, 5, 6, 5, 4, 6, 5, 4, 7, 2, 9, 5, 3, 3, 4, 6, 2, 1, 10, 4, 6, 4, 7, 1, 3, 1, 3, 12, 3, 8, 5, 4, 5, 3, 7, 5, 2, 4, 2, 3, 5, 4, 1, 2, 5, 4, 1, 5,
    1, 7, 6, 7, 4, 5, 6, 8, 1, 6, 6, 5, 3, 6, 4, 6, 2, 5, 0, 1, 6, 4, 2, 5, 6, 4, 6, 5, 3, 1, 1, 3, 7, 4, 2, 3, 9, 3, 3, 5, 0, 1, 5, 2, 4, 4, 3, 0, 1, 5,
    4, 5, 5, 4, 1, 5, 4, 5, 8, 5, 2, 4, 2, 5, 3, 4, 3, 4, 0, 1, 6, 6, 1, 7, 3, 3, 3, 3, 0, 1, 4, 2, 3, 1, 4, 2, 5, 4, 4, 6, 2, 0, 1, 3, 4, 6, 3, 3, 2, 3,
    3, 8, 5, 1, 2, 3, 2, 0, 1, 2, 3, 3, 2, 4, 0, 1, 3, 2, 6, 5, 6, 4, 8, 2, 6, 4, 6, 6, 2, 4, 3, 1, 7, 2, 3, 1, 5, 5, 1, 5, 2, 2, 5, 5, 3, 6, 5, 5, 4, 4,
    4, 4, 2, 2, 6, 4, 3, 7, 4, 2, 3, 2, 4, 4, 4, 2, 3, 5, 2, 3, 6, 1, 3, 4, 5, 1, 2, 4, 6, 4, 2, 4, 5, 4, 0, 1, 2, 8, 3, 0, 1, 1, 2, 4, 5, 6, 8, 5, 5, 4,
    3, 3, 0, 1, 3, 3, 3, 5, 3, 3, 4, 1, 2, 2, 4, 3, 2, 3, 2, 5, 6, 6, 5, 6, 5, 6, 3, 2, 2, 2, 2, 1, 2, 5, 3, 3, 2, 1, 2, 7, 2, 10, 4, 4, 4, 1, 7, 6, 2, 2,
    2, 4, 3, 4, 1, 4, 2, 1, 6, 1, 4, 4, 5, 2, 3, 3, 4, 1, 3, 3, 3, 4, 3, 2, 0, 1, 3, 3, 0, 1, 4, 2, 1, 4, 3, 2, 1, 7, 3, 2, 0, 1, 2, 2, 2, 3, 3, 1, 3, 6,
    3, 2, 2, 7, 1, 4, 4, 5, 2, 2, 6, 4, 0, 1, 3, 5, 2, 6, 4, 5, 6, 1, 5, 5, 2, 4, 5, 2, 2, 6, 0, 1, 4, 1, 1, 3, 4, 3, 2, 1, 3, 0, 1, 6, 1, 4, 2, 1, 3, 0,
    1, 3, 1, 1, 5, 3, 2, 5, 1, 6, 4, 7, 4, 1, 5, 2, 2, 2, 3, 3, 2, 1, 2, 0, 1, 1, 2, 0, 1, 5, 1, 3, 2, 2, 2, 3, 3, 3, 3, 4, 4, 6, 3, 0, 1, 3, 2, 4, 1, 2,
    0, 1, 1, 1, 6, 0, 1, 5, 4, 2, 2, 5, 1, 1, 3, 2, 0, 1, 1, 2, 4, 4, 4, 2, 2, 3, 0, 1, 1, 3, 1, 4, 1, 0, 1, 5, 0, 1, 1, 2, 0, 1, 3, 4, 4, 4, 3, 3, 1, 1,
    1, 5, 3, 2, 2, 1, 1, 3, 1, 6, 1, 6, 3, 2, 0, 1, 5, 0, 1, 1, 2, 3, 0, 1, 2, 5, 2, 3, 3, 4, 4, 1, 2, 1, 1, 4, 3, 3, 4, 1, 1, 4, 5, 2, 3, 2, 2, 6, 1, 1,
    2, 2, 3, 0, 1, 2, 0, 1, 4, 2, 2, 1, 1, 1, 3, 1, 3, 3, 2, 1, 2, 2, 3, 3, 1, 2, 4, 4, 4, 2, 4, 2, 5, 1, 3, 4, 4, 1, 3, 5, 0, 1, 6, 2, 1, 1, 5, 5, 2, 2,
    5, 4, 4, 2, 5, 1, 5, 2, 2, 2, 3, 4, 4, 1, 2, 6, 3, 1, 3, 1, 3, 0, 1, 3, 2, 2, 4, 3, 2, 2, 5, 4, 1, 1, 4, 3, 1, 2, 2, 1, 2, 4, 5, 1, 1, 4, 3, 2, 2, 5,
    3, 3, 5, 0, 1, 3, 1, 1, 0, 1, 3, 4, 7, 1, 1, 2, 3, 4, 2, 0, 1, 5, 4, 0, 1, 2, 1, 4, 4, 3, 1, 0, 1, 3, 2, 4, 3, 0, 1, 3, 2, 3, 3, 1, 3, 1, 2, 3, 2, 1,
    2, 4, 3, 5, 6, 18, 28, 20, 11, 10, 10, 1, 3, 1, 3, 1, 4, 5, 2, 1, 4, 1, 2, 3, 2, 1, 2, 3, 0, 1, 3, 2, 3, 3, 1, 2, 3, 2, 2, 0, 1, 1, 2, 1, 4, 2, 2, 2, 1, 0,
    1, 1, 3, 2, 1, 5, 4, 3, 3, 5, 2, 1, 1, 5, 4, 3, 3, 3, 1, 4, 4, 2, 1, 3, 2, 3, 4, 1, 1, 5, 3, 4, 2, 3, 3, 1, 4, 2, 1, 5, 3, 1, 1, 7, 2, 7, 1, 4, 5, 3,
    0, 1, 1, 3, 6, 4, 7, 4, 1, 2, 3, 2, 3, 4, 3, 3, 2, 1, 3, 2, 1, 7, 2, 5, 3, 0, 1, 2, 5, 1, 1, 4, 1, 0, 1, 1, 4, 1, 4, 3, 3, 3, 1, 1, 2, 3, 1, 2, 2, 1,
    2, 2, 1, 5, 3, 2, 5, 4, 1, 3, 0, 1, 1, 1, 4, 1, 2, 4, 3, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 1, 3, 3, 0, 1, 4, 3, 0, 2, 1, 4, 2, 3, 3, 3, 5, 3, 1, 2, 2,
    0, 1, 1, 2, 2, 2, 2, 2, 4, 3, 1, 1, 2, 4, 2, 0, 2, 4, 4, 1, 2, 3, 0, 1, 3, 1, 5, 1, 1, 2, 3, 0, 1, 2, 4, 1, 1, 0, 1, 5, 2, 1, 0, 2, 3, 1, 1, 1, 5, 0,
    2, 1, 1, 2, 1, 4, 3, 1, 4, 5, 12, 7, 9, 7, 8, 6, 1, 2, 2, 0, 1, 1, 0, 1, 3, 4, 1, 0, 1, 2, 2, 2, 1, 0, 1, 1, 2, 1, 0, 1, 1, 2, 3, 1, 1, 3, 0, 1, 1, 0,
    1, 1, 4, 3, 1, 2, 1, 5, 3, 2, 2, 2, 5, 1, 2, 2, 1, 0, 1, 2, 2, 3, 1, 3, 0, 2, 1, 0, 1, 4, 0, 1, 3, 2, 1, 0, 1, 2, 0, 3, 2, 2, 2, 2, 2, 2, 2, 0, 1, 2,
    2, 2, 3, 5, 2, 3, 4, 4, 0, 1, 1, 6, 2, 5, 1, 4, 8, 1, 2, 1, 3, 3, 0, 1, 1, 3, 1, 4, 0, 1, 2, 0, 1, 1, 3, 1, 0, 1, 1, 0, 1, 1, 3, 3, 3, 3, 1, 0, 1, 3,
    2, 0, 2, 2, 1, 0, 1, 1, 3, 1, 2, 4, 5, 1, 1, 2, 1, 4, 1, 3, 3, 1, 2, 3, 0, 1, 1, 0, 1, 3, 2, 4, 1, 2, 1, 1, 1, 0, 1, 1, 2, 2, 1, 4, 1, 1, 0, 2, 1, 1,
    1, 0, 1, 3, 3, 0, 2, 1, 4, 0, 1, 1, 0, 1, 1, 0, 1, 1, 4, 2, 0, 1, 2, 1, 1, 2, 3, 2, 1, 2, 3, 3, 2, 2, 3, 0, 2, 1, 1, 3, 0, 2, 2, 0, 1, 1, 2, 0, 1, 2,
    3, 0, 2, 1, 3, 0, 1, 1, 3, 2, 2, 1, 1, 2, 0, 1, 2, 1, 0, 2, 3, 1, 0, 1, 1, 0, 2, 1, 1, 3, 1, 0, 1, 1, 1, 1, 0, 1, 2, 1, 1, 1, 0, 1, 3, 1, 1, 2, 0, 1,
    1, 2, 0, 2, 1, 3, 2, 0, 3, 1, 2, 1, 3, 1, 5, 1, 1, 1, 1, 1, 0, 1, 1, 2, 1, 1, 1, 2, 3, 3, 2, 3, 3, 2, 8, 4, 5, 2, 1, 0, 2, 1, 1, 3, 2, 1, 0, 2, 3, 2,
    1, 2, 2, 0, 2, 1, 1, 1, 1, 1, 0, 4, 1, 0, 1, 2, 0, 2, 3, 0, 1, 1, 1, 3, 1, 1, 1, 1, 3, 4, 2, 4, 3, 3, 1, 2, 1, 1, 2, 4, 1, 0, 1, 1, 4, 3, 3, 2, 4, 4,
    1, 5, 2, 1, 0, 1, 2, 2, 0, 2, 1, 1, 3, 2, 1, 2, 1, 0, 2, 1, 0, 1, 1, 1, 0, 3, 1, 1, 0, 3, 3, 0, 2, 1, 4, 1, 3, 0, 1, 1, 0, 1, 1, 2, 2, 1, 1, 0, 1, 2,
    1, 1, 0, 2, 2, 1, 2, 0, 2, 3, 0, 1, 2, 1, 2, 1, 1, 0, 1, 3, 0, 1, 1, 1, 1, 0, 1, 1, 3, 1, 2, 1, 3, 0, 1, 2, 0, 2, 2, 0, 3, 1, 1, 1, 1, 1, 1, 1, 3, 0,
    1, 1, 0, 1, 3, 0, 1, 4, 3, 5, 4, 8, 14, 28, 20, 35, 31, 21, 14, 8, 4, 4, 2, 0, 5, 1, 2, 1, 0, 4, 1, 2, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 1, 2, 1, 0, 1, 1, 0, 1,
    1, 4, 0, 1, 2, 0, 1, 1, 1, 6, 0, 1, 1, 0, 6, 1, 0, 1, 1, 2, 2, 0, 1, 1, 0, 2, 1, 2, 1, 0, 1, 2, 1, 2, 1, 0, 2, 2, 2, 1, 1, 2, 0, 1, 1, 0, 1, 1, 1, 1,
    1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 4, 3, 2, 1, 1, 3, 2, 1, 2, 2, 4, 6, 2, 1, 3, 3, 0, 1, 1, 3, 1, 1, 1, 0, 1, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 1, 0, 1, 1,
    1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 1, 0, 1, 3, 0, 3, 1, 1, 2, 1, 0, 1, 2, 2, 1, 2, 1, 0, 1, 1, 0, 3, 1, 1, 1, 0, 1, 1, 1, 4, 4, 2, 2, 0, 3, 1, 1, 0, 1,
    4, 0, 2, 1, 1, 0, 1, 1, 1, 2, 1, 0, 1, 2, 2, 1, 1, 0, 1, 2, 1, 1, 1, 1, 0, 7, 1, 2, 0, 1, 1, 1, 0, 1, 2, 1, 2, 0, 1, 1, 0, 2, 2, 0, 3, 1, 0, 2, 2, 1,
    1, 0, 1, 1, 0, 5, 1, 0, 2, 1, 0, 7, 1, 0, 1, 2, 0, 1, 1, 2, 1, 0, 2, 4, 0, 1, 1, 3, 0, 1, 1, 1, 0, 1, 1, 1, 3, 1, 1, 2, 1, 0, 1, 1, 2, 1, 1, 2, 2, 0,
    1, 2, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 3, 2, 1, 1, 1, 1, 0, 2, 1, 0, 2, 2, 0, 1, 2, 1, 0, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 4, 2, 0, 1,
    1, 1, 1, 0, 1, 1, 1, 0, 3, 1, 3, 1, 0, 1, 2, 0, 1, 1, 0, 3, 1, 1, 0, 4, 1, 0, 1, 2, 0, 1, 2, 0, 5, 1, 0, 1, 1, 1, 0, 2, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1,
    1, 2, 0, 1, 1, 1, 1, 1, 2, 0, 6, 1, 2, 0, 1, 2, 0, 6, 1, 0, 4, 1, 1, 1, 2, 2, 0, 2, 1, 0, 4, 1, 1, 1, 2, 3, 2, 1, 1, 1, 1, 1, 0, 2, 1, 0, 3, 1, 0, 2,
    3, 0, 2, 1, 0, 4, 1, 0, 1, 1, 0, 1, 1, 2, 1, 0, 3, 1, 0, 1, 1, 1, 0, 4, 1, 0, 1, 1, 1, 0, 2, 2, 1, 1, 0, 1, 3, 0, 2, 1, 0, 5, 1, 0, 4, 2, 1, 0, 1, 1,
    0, 2, 1, 1, 1, 0, 4, 1, 0, 1, 1, 1, 2, 1, 0, 2, 1, 0, 1, 1, 1, 2, 1, 1, 1, 0, 2, 1, 0, 4, 1, 0, 5, 1, 0, 1, 1, 0, 3, 2, 0, 1, 2, 0, 4, 1, 0, 1, 1, 0,
    1, 1, 0, 1, 1, 0, 3, 1, 1, 0, 1, 1, 2, 2, 2, 0, 1, 1, 3, 4, 4, 4, 1, 0, 3, 1, 0, 2, 1, 0, 1, 2, 0, 6, 2, 1, 0, 2, 1, 0, 1, 2, 0, 1, 1, 0, 9, 1, 1, 0,
    2, 1, 2, 0, 2, 1, 0, 2, 1, 1, 0, 3, 2, 0, 4, 1, 1, 0, 5, 1, 6, 5, 10, 12, 16, 13, 18, 11, 11, 5, 2, 0, 1, 1, 0, 4, 1, 1, 0, 3, 1, 0, 4, 1, 0, 5, 1, 0, 2, 3,
    0, 5, 1, 0, 7, 1, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 2, 2, 0, 8, 1, 0, 1, 2, 0, 10, 1, 0, 4, 2, 1, 0, 6, 1, 1, 0, 9, 2, 0, 1, 2,
    0, 2, 2, 0, 2, 1, 0, 2, 1, 0, 2, 1, 1, 1, 0, 1, 1, 0, 5, 1, 0, 2, 1, 0, 7, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 5, 1, 0, 5, 1, 3, 4, 3, 3, 3, 1, 1,
    1, 0, 10, 1, 1, 0, 3, 1, 0, 5, 1, 1, 0, 1, 1, 1, 0, 6, 1, 0, 4, 1, 0, 1, 1, 0, 2, 1, 0, 3, 2, 1, 1, 1, 0, 2, 1, 0, 8, 1, 0, 2, 1, 0, 4, 1, 0, 1, 1, 0,
    8, 2, 0, 1, 1, 1, 1, 0, 5, 1, 1, 1, 1, 0, 6, 1, 1, 0, 3, 1, 0, 6, 1, 2, 0, 2, 3, 0, 5, 1, 1, 1, 0, 2, 1, 0, 7, 1, 0, 2, 1, 1, 0, 2, 1, 0, 2, 1, 1, 1,
    0, 3, 1, 1, 0, 1, 1, 1, 1, 0, 9, 2, 0, 1, 1, 1, 1, 0, 1, 1, 0, 5, 1, 0, 4, 2, 0, 3, 1, 0, 8, 1, 2, 0, 2, 1, 1, 1, 0, 4, 1, 0, 5, 1, 0, 3, 1, 1, 0, 5,
    1, 1, 0, 3, 2, 1, 0, 5, 1, 0, 5, 1, 0, 5, 1, 0, 7, 2, 0, 4, 1, 1, 2, 0, 1, 1, 0, 2, 1, 0, 12, 1, 2, 2, 0, 1, 2, 1, 0, 1, 2, 0, 2, 1, 0, 3, 1, 0, 1, 1,
    1, 1, 0, 2, 1, 1, 1, 0, 4, 1, 1, 1, 0, 2, 1, 0, 7, 1, 1, 0, 13, 3, 0, 2, 1, 1, 0, 1, 1, 0, 4, 2, 1, 1, 0, 2, 1, 1, 0, 3, 1, 0, 8, 1, 0, 2, 1, 0, 8, 1,
    0, 8, 1, 0, 2, 1, 0, 10, 1, 0, 6, 1, 0, 1, 1, 3, 1, 0, 2, 2, 0, 10, 1, 1, 0, 1, 2, 0, 3, 2, 0, 5, 1, 0, 2, 1, 0, 9, 1, 0, 3, 1, 0, 2, 1, 0, 1, 1, 0, 4,
    1, 1, 1, 0, 2, 1, 0, 6, 1, 0, 5, 1, 0, 1, 1, 1, 0, 3, 2, 2, 0, 7, 1, 1, 0, 1, 1, 0, 3, 1, 0, 5, 1, 1, 1, 0, 1, 1, 0, 3, 1, 0, 1, 2, 0, 1, 1, 0, 1, 1,
    0, 10, 1, 1, 0, 3, 1, 0, 2, 2, 1, 2, 1, 0, 1, 2, 1, 0, 3, 1, 0, 8, 1, 1, 0, 4, 2, 0, 1, 1, 0, 2, 1, 0, 5, 1, 0, 4, 1, 0, 7, 2, 1, 3, 0, 3, 1, 0, 7, 1,
    0, 2, 1, 0, 13, 1, 0, 3, 2, 0, 16, 1, 0, 12, 1, 0, 5, 1, 0, 4, 1, 0, 6, 1, 0, 3, 1, 0, 11, 1, 1, 1, 0, 5, 1, 0, 7, 1, 0, 5, 1, 1, 1, 1, 2, 3, 2, 4, 3, 4,
    2, 1, 1, 0, 1, 1, 0, 2, 1, 1, 0, 5, 1, 0, 3, 1, 0, 18, 2, 0, 1, 1, 1, 0, 2, 1, 1, 0, 6, 1, 0, 3, 1, 0, 12, 2, 0, 7, 1, 0, 1, 1, 0, 3, 1, 0, 1, 1, 0, 4,
    1, 0, 1, 1, 0, 1, 1, 0, 11, 1, 0, 2, 1, 0, 7, 1, 1, 0, 6, 1, 0, 1, 1, 0, 1, 1, 1, 0, 3, 1, 0, 13, 1, 1, 0, 21, 1, 1, 0, 3, 1, 1, 0, 3, 1, 1, 2, 1, 0, 4,
    2, 0, 10, 1, 0, 1, 1, 2, 0, 3, 1, 0, 3, 1, 0, 3, 1, 0, 12, 1, 0, 2, 1, 0, 2, 1, 0, 12, 1, 0, 8, 1, 0, 7, 1, 1, 1, 1, 0, 4, 1, 0, 19, 1, 0, 8, 1, 0, 4, 1,
    0, 5, 1, 0, 2, 1, 0, 18, 1, 3, 1, 1, 0, 15, 1, 0, 14, 1, 0, 3, 1, 0, 16, 1, 0, 4, 1, 0, 4, 1, 0, 6, 1, 0, 3, 2, 0, 14, 1, 0, 3, 1, 0, 16, 1, 0, 10, 1, 0, 2,
    1, 0, 3, 1, 0, 11, 1, 0, 7, 1, 0, 10, 1, 1, 3, 1, 0, 1, 1, 0, 4, 1, 0, 2, 1, 0, 17, 1, 0, 4, 1, 0, 6, 1, 0, 11, 1, 0, 2, 1, 0, 1, 2, 0, 6, 1, 0, 8, 1, 0,
    7, 1, 0, 4, 1, 0, 52, 1, 0, 1, 1, 0, 5, 1, 0, 32, 1, 0, 2, 1, 0, 27, 1, 0, 9, 1, 1, 0, 36, 1, 0, 5, 1, 0, 26, 1, 0, 9, 1, 0, 17, 1, 0, 17, 2, 4, 1, 8, 7, 2,
    2, 2, 0, 29, 1, 0, 63, 1, 0, 27, 1, 0, 6, 1, 0, 1, 1, 0, 1, 1, 0, 14, 1, 0, 27, 1, 0, 25, 1, 0, 29, 1, 0, 26, 1, 0, 39, 1, 0, 2, 1, 0, 26, 1, 0, 19, 1, 0, 3, 1,
    0, 22, 1, 0, 2, 1, 0, 7, 1, 0, 41, 1, 0, 22, 1, 0, 12, 1, 0, 12, 1, 0, 21, 1, 0, 18, 1, 0, 50, 1, 0, 44, 1, 0, 10, 1, 0, 1, 1, 1, 0, 14, 1, 0, 11, 1, 0, 63, 1, 0,
    61, 1, 0, 9, 1, 0, 18, 1, 0, 60, 1, 0, 8, 1, 0, 10, 1, 0, 62, 1, 0, 6, 1, 0, 60, 1, 0, 8, 1, 0, 64, 1, 0, 23, 1, 0, 14, 1, 0, 18, 1, 0, 18, 1, 0, 28, 1, 0, 101, 1,
    0, 25, 1, 0, 2, 1, 0, 18, 1, 0, 72, 1, 0, 13, 1, 0, 42, 1, 0, 11, 1, 0, 36, 1, 0, 20, 1, 0, 41, 1, 0, 2, 1, 0, 60, 1, 0, 198, 1, 0, 6, 1, 0, 69, 1, 0, 48, 1, 0, 2,
    1, 0, 66, 1, 0, 52, 1, 0, 3, 1, 0, 91, 1, 0, 101, 1, 0, 24, 1, 0, 11, 1, 0, 12, 1, 0, 5, 1, 0, 12, 1, 0, 28, 1, 0, 76, 1, 0, 28, 1, 0, 63, 1, 0, 36, 1, 0, 48, 1, 0,
    31, 1, 0, 40, 1, 1, 0, 15, 1, 0, 6, 1, 0, 9, 1, 0, 3, 1, 0, 1, 1, 0, 17, 1, 0, 6, 1, 0, 43, 1, 0, 35, 1, 0, 10, 1, 0, 18, 1, 0, 8, 1, 0, 2, 1, 0, 111, 1, 0, 30,
    1, 0, 77, 1, 0, 51, 1, 0, 127, 1, 0, 8, 1, 0, 30, 1, 0, 16, 1, 1, 0, 81, 1, 0, 9, 1, 0, 61, 1, 0, 25, 1, 0, 180, 1, 0, 22, 1, 0, 63, 1, 0, 16, 1, 0, 91, 1, 0, 31, 1,
    0, 50, 1, 0, 35, 1, 0, 36, 1, 0, 86, 1, 0, 128, 1, 0, 26, 1, 0, 147, 1, 0, 26, 1, 0, 48, 1, 0, 105, 1, 0, 67, 1, 0, 54, 1, 0, 8, 1, 0, 106, 1, 0, 115, 2, 0, 55, 2, 0, 21,
    1, 0, 5, 1, 0, 75, 1, 0, 147, 1, 0, 62, 1, 0, 20, 1, 0, 115, 1, 0, 72, 1, 1, 0, 43, 1, 0, 16, 1, 0, 56, 1, 0, 9, 1, 0, 5, 1, 0, 80, 1, 0, 38, 1, 0, 26, 1, 0, 143, 1,
    0, 60, 1, 0, 35, 1, 0, 66, 1, 0, 5, 1, 0, 3, 1, 0, 123, 1, 0, 40, 1, 0, 147, 1, 0, 24, 1, 0, 214, 1, 0, 40, 1, 0, 9, 1, 0, 6, 1, 0, 18, 1, 0, 24, 1, 0, 1, 1, 0, 70,
    1, 0, 14, 1, 0, 14, 1, 0, 3, 1, 0, 61, 1, 0, 4, 1, 0, 14, 1, 0, 12, 1, 0, 50, 1, 0, 29, 1, 0, 72, 1, 0, 153, 1, 0, 8, 1, 0, 76, 1, 0, 24, 1, 0, 53, 1, 0, 65, 1, 0,
    4, 1, 0, 7, 1, 0, 25, 1, 0, 44, 1, 0, 41, 1, 0, 131, 1, 0, 176, 1, 0, 167, 1, 0, 15, 1, 0, 33, 1, 0, 1, 1, 0, 49, 1, 0, 35, 1, 0, 5, 1, 0, 164, 1, 0, 10, 1, 0, 10, 1,
    0, 119, 1, 0, 2, 1, 0, 70, 1, 0, 15, 1, 0, 6, 1, 0, 38, 1, 0, 83, 1, 0, 109, 1, 0, 0, 1, 1, 0, 137, 1, 0, 81, 1, 0, 54, 1, 0, 180, 1, 0, 45, 1, 0, 14, 1, 1, 0, 28, 1,
    0, 136, 1, 0, 17, 1, 0, 65, 1, 0, 15, 1, 0, 7, 1, 1, 0, 172, 1, 0, 56
  };
  assert( test_20_packed.size() == 6521 );
  const vector<uint8_t> test_20_encoded = QRSpecDev::encode_stream_vbyte( test_20_chan_cnts );
  assert( test_20_encoded == test_20_packed );
  vector<uint32_t> test_20_dec;
  const size_t test_20_nbytedec = QRSpecDev::decode_stream_vbyte(test_20_encoded,test_20_dec);
  assert( test_20_nbytedec == test_20_packed.size() );
  assert( test_20_dec == test_20_chan_cnts );
  

}//void test_bitpacking()


/** Creates the "data" portion of the, split into `num_parts` separate URLs.
 */
vector<string> url_encode_spectrum( const UrlSpectrum &m,
                                    const uint8_t encode_options,
                                    const size_t num_parts,
                                    const unsigned int skip_encode_options )
{
  if( (num_parts == 0) || (num_parts > 9) )
    throw runtime_error( "url_encode_spectrum: invalid number of URL portions." );
  
  if( m.m_channel_data.size() < 1 )
    throw runtime_error( "url_encode_spectrum: invalid Measurement passed in." );
  
  if( m.m_channel_data.size() < num_parts )
    throw runtime_error( "url_encode_spectrum: more parts requested than channels." );
  
  assert( encode_options < 16 );
  
  // Break out the encoding options.
  // The options given by `encode_options` are for the final message - however, we may be
  //  creating a multi-spectrum QR code, which means we dont want to do deflate, base45, or URL
  //  encoding yet
  const bool use_deflate = !(encode_options & EncodeOptions::NoDeflate);
  const bool use_base45 = !(encode_options & EncodeOptions::NoBase45);
  const bool use_bin_chan_data = !(encode_options & EncodeOptions::CsvChannelData);
  const bool zero_compress = !(encode_options & EncodeOptions::NoZeroCompressCounts);
  
  const bool skip_encoding = (skip_encode_options & SkipForEncoding::Encoding);
  const bool skip_energy   = (skip_encode_options & SkipForEncoding::EnergyCal);
  const bool skip_model    = (skip_encode_options & SkipForEncoding::DetectorModel);
  const bool skip_gps      = (skip_encode_options & SkipForEncoding::Gps);
  const bool skip_title    = (skip_encode_options & SkipForEncoding::Title);
  
  assert( !skip_encoding || (num_parts == 1) );
  
  
  vector<string> answer( num_parts );
  
  string &first_url = answer[0];
  first_url.reserve( 100 + 3*m.m_channel_data.size() ); //An absolute guess, has not been checked
  
  switch( m.m_source_type )
  {
    case SpecUtils::SourceType::IntrinsicActivity: first_url += "I:I "; break;
    case SpecUtils::SourceType::Calibration:       first_url += "I:C "; break;
    case SpecUtils::SourceType::Background:        first_url += "I:B "; break;
    case SpecUtils::SourceType::Foreground:        first_url += "I:F "; break;
    case SpecUtils::SourceType::Unknown:                           break;
  }//switch( m->source_type() )
  
  const float lt = (m.m_live_time > 0.0) ? m.m_live_time : m.m_real_time;
  const float rt = (m.m_real_time > 0.0) ? m.m_real_time : m.m_live_time;
  first_url += "T:" + PhysicalUnits::printCompact(rt,6) + "," + PhysicalUnits::printCompact(lt,6) + " ";
  
  if( !skip_energy )
  {
    
    // Lower channel energy not currently implemented
    if( !m.m_energy_cal_coeffs.empty() )
    {
      first_url += "C:";
      for( size_t i = 0; i < m.m_energy_cal_coeffs.size(); ++i )
        first_url += (i ? "," : "") + PhysicalUnits::printCompact(m.m_energy_cal_coeffs[i], 7 );
      first_url += " ";
      
      const vector<pair<float,float>> &dev_pairs = m.m_dev_pairs;
      if( !dev_pairs.empty() )
      {
        first_url += "D:";
        for( size_t i = 0; i < dev_pairs.size(); ++i )
        {
          first_url += (i ? "," : "") + PhysicalUnits::printCompact(dev_pairs[i].first, 6 )
          + "," + PhysicalUnits::printCompact(dev_pairs[i].second, 4 );
        }
        first_url += " ";
      }//if( !dev_pairs.empty() )
    }//if( we have energy calibration info - that we can use )
  }//if( !skip_energy )
  
  
  if( !skip_model )
  {
    // Remove equal signs and quotes
    string det_model = m.m_model;
    SpecUtils::ireplace_all( det_model, ":", "" );
    if( det_model.size() > 30 )
      det_model = det_model.substr(0,30);
    
    SpecUtils::trim( det_model );
    if( !det_model.empty() )
    {
      if( !use_deflate && !use_base45 && !use_bin_chan_data )
        det_model = url_encode_non_base45( det_model );
      
      first_url += "M:" + det_model + " ";
    }
  }//if( !skip_model )
  
  if( !SpecUtils::is_special(m.m_start_time) )
  {
    // TODO: ISO string takes up 15 characters - could represent as a double, a la Microsoft time
    std::string t = SpecUtils::to_iso_string( m.m_start_time );
    const size_t dec_pos = t.find( "." );
    if( dec_pos != string::npos )
      t = t.substr(0, dec_pos);
    first_url += "P:" + t + " ";
  }//if( !SpecUtils::is_special(m->start_time()) )
  
  if( !skip_gps
     && SpecUtils::valid_longitude(m.m_longitude)
     && SpecUtils::valid_latitude(m.m_latitude) )
  {
    first_url += "G:" + PhysicalUnits::printCompact(m.m_latitude, 7)
    + "," + PhysicalUnits::printCompact(m.m_longitude, 7) + " ";
  }
  
  if( m.m_neut_sum >= 0 )
    first_url += "N:" + std::to_string(m.m_neut_sum) + " ";
  
  if( !skip_title && !m.m_title.empty() )
  {
    string operator_notes = m.m_title;
    SpecUtils::ireplace_all( operator_notes, ":", " " );
    if( operator_notes.size() > 60 )
      operator_notes.substr(0, 60);
    
    string remark;
    remark = operator_notes;
    
    if( !use_deflate && !use_base45 && !use_bin_chan_data )
      remark = url_encode_non_base45( operator_notes );
    
    first_url += "O:" + remark + " ";
  }//if( !skip_title && !m->title().empty() )
  
  first_url += "S:";
  
  vector<uint32_t> channel_counts;
  if( zero_compress )
    channel_counts = compress_to_counted_zeros( m.m_channel_data );
  else
    channel_counts = m.m_channel_data;
  
  for( size_t msg_num = 0; msg_num < num_parts; ++msg_num )
  {
    string &url_data = answer[msg_num];
    const size_t num_channel_per_part = channel_counts.size() / num_parts;
    
    if( !num_channel_per_part )
      throw runtime_error( "url_encode_spectrum: more url-parts requested than channels after zero-compressing." );
    
    const size_t start_int_index = num_channel_per_part * msg_num;
    const size_t end_int_index = ((msg_num + 1) == num_parts)
                                        ? channel_counts.size()
                                        : start_int_index + num_channel_per_part;
    
    if( use_bin_chan_data )
    {
      vector<uint32_t> integral_counts;
      for( size_t i = start_int_index; i < end_int_index; ++i )
        integral_counts.push_back( channel_counts[i] );
      
      const uint32_t num_counts = static_cast<uint32_t>( integral_counts.size() );
      if( num_counts > 65535 )
        throw runtime_error( "url_encode_spectrum: a max of 65535 channels are supported." );
      
      const vector<uint8_t> encoded_bytes = encode_stream_vbyte( integral_counts );
      const size_t start_size = url_data.size();
      url_data.resize( start_size + encoded_bytes.size() );
      memcpy( &(url_data[start_size]), (void *)encoded_bytes.data(), encoded_bytes.size() );
      
      {
        const size_t max_comp_size = streamvbyte_max_compressedbytes( num_counts );
        vector<uint8_t> test_bytes( max_comp_size );
        const size_t compsize = streamvbyte_encode( integral_counts.data(), num_counts, test_bytes.data() );
        test_bytes.resize( compsize );
        assert( encoded_bytes.size() >= 2 );
        vector<uint8_t> plain_encoded_bytes = encoded_bytes;
        plain_encoded_bytes.erase(begin(plain_encoded_bytes), begin(plain_encoded_bytes) + 2 );
        assert( plain_encoded_bytes == test_bytes );
        
        //const uint16_t nints = static_cast<uint16_t>( num_counts );
        //url_data.resize( url_data.size() + 2 );
        //memcpy( &(url_data[url_data.size() - 2]), &nints, 2 );
        //const size_t start_size = url_data.size();
        //url_data.resize( start_size + compsize );
        //memcpy( (void *)(&url_data[start_size]), test_bytes.data(), compsize );
      }
    }else
    {
      for( size_t i = start_int_index; i < end_int_index; ++i )
        url_data += ((i == start_int_index) ? "" : ",") + std::to_string( channel_counts[i] );
    }//if( use_bin_chan_data ) / else
  }//for( size_t msg_num = 0; msg_num < num_parts. ++msg_num )
  
  
  if( use_deflate && !skip_encoding )
  {
    for( size_t msg_num = 0; msg_num < num_parts; ++msg_num )
    {
      string &url_data = answer[msg_num];
      
      //cout << "During encoding, before compression: " << to_hex_bytes_str(url_data.substr(0,60)) << endl;
#ifndef NDEBUG
      const string orig = url_data;
      deflate_compress( &(url_data[0]), url_data.size(), url_data );
      
      string decompressed;
      deflate_decompress( &(url_data[0]), url_data.size(), decompressed );
      assert( orig == decompressed );
#else
      deflate_compress( &(url_data[0]), url_data.size(), url_data );
#endif
      //cout << "During encoding, after compression, before base-45: '" << to_hex_bytes_str(url_data.substr(0,60)) << "'" << endl << endl;
    
      assert( !url_data.empty() );
    }//for( size_t msg_num = 0; msg_num < num_parts; ++msg_num )
  }//if( use_deflate )
  
  if( use_base45 && !skip_encoding )
  {
    for( size_t msg_num = 0; msg_num < num_parts; ++msg_num )
    {
#ifndef NDEBUG
      const string orig = answer[msg_num];
      answer[msg_num] = base45_encode( answer[msg_num] );
      
      vector<uint8_t> decoded_bytes = base45_decode( answer[msg_num] );
      assert( !decoded_bytes.empty() );
      string decoded( decoded_bytes.size(), 0x0 );
      //memcpy( &(decoded[0]), &(decoded_bytes[0]), decoded_bytes.size() );
      for( size_t i = 0; i < decoded_bytes.size(); ++i )
        decoded[i] = decoded_bytes[i];
      
      if( decoded != orig )
        cerr << "\n\nNot matching:\n\tOrig='" << to_hex_bytes_str(orig) << "'" << endl
        << "\tDecs='" << to_hex_bytes_str(decoded) << "'\n\n";
      assert( decoded == orig );
#else
      answer[msg_num] = base45_encode( answer[msg_num] );
#endif
      //cout << "During encoding, after  base-45: '" << to_hex_bytes_str(answer[msg_num].substr(0,60)) << "'" << endl << endl;
    }
  }//if( use_base45 )
  
  if( !skip_encoding )
  {
    for( size_t msg_num = 0; msg_num < num_parts; ++msg_num )
    {
#ifndef NDEBUG
      string urlencoded = url_encode( answer[msg_num] );
      assert( Wt::Utils::urlDecode(urlencoded) == answer[msg_num] );
      answer[msg_num].swap( urlencoded );
#else
      answer[msg_num] = url_encode( answer[msg_num] );
#endif
      //cout << "During encoding, after  URL encode: '" << to_hex_bytes_str(answer[msg_num].substr(0,60)) << "'" << endl << endl;
    }//for( size_t msg_num = 0; msg_num < num_parts; ++msg_num )
  }//if( !skip_encoding )
  
#ifndef NDEBUG
  if( (use_base45 || !skip_encoding) || (!use_deflate && !use_bin_chan_data)  )
  {
    for( const string &url : answer )
    {
      for( const char val : url )
      {
        assert( std::find( begin(sm_base42_chars), end(sm_base42_chars), val ) != end(sm_base42_chars) );
      }
    }
  }//if( use_base45 || (!use_deflate && !use_bin_chan_data) )
#endif
  
  return answer;
}//vector<string> url_encode_spectrum(...)


std::vector<UrlEncodedSpec> url_encode_spectra( const std::vector<UrlSpectrum> &measurements,
                                           const QrErrorCorrection minErrorCorrection,
                                           const uint8_t encode_options
                                           )
{
  if( measurements.empty() )
    throw runtime_error( "url_encode_spectra: no measurements passed in." );
  if( measurements.size() > 9 )
    throw runtime_error( "url_encode_spectra: to many measurements passed in." );
  
  if( encode_options & ~(EncodeOptions::NoDeflate | EncodeOptions::NoBase45 | EncodeOptions::CsvChannelData | EncodeOptions::NoZeroCompressCounts) )
    throw runtime_error( "url_encode_spectra: invalid option passed in - see EncodeOptions." );
  
  assert( encode_options < 16 );
  
  const bool use_deflate = !(encode_options & EncodeOptions::NoDeflate);
  const bool use_base45 = !(encode_options & EncodeOptions::NoBase45);
  const bool use_bin_chan_data = !(encode_options & EncodeOptions::CsvChannelData);
  const bool zero_compress = !(encode_options & EncodeOptions::NoZeroCompressCounts);
  
  const bool alpha_num_qr_encode = (use_base45 || (!use_deflate && !use_bin_chan_data));
  
  qrcodegen::QrCode::Ecc ecc = qrcodegen::QrCode::Ecc::LOW;
  switch( minErrorCorrection )
  {
    case QrErrorCorrection::Low:      ecc = qrcodegen::QrCode::Ecc::LOW;      break;
    case QrErrorCorrection::Medium:   ecc = qrcodegen::QrCode::Ecc::MEDIUM;   break;
    case QrErrorCorrection::Quartile: ecc = qrcodegen::QrCode::Ecc::QUARTILE; break;
    case QrErrorCorrection::High:     ecc = qrcodegen::QrCode::Ecc::HIGH;     break;
  }//switch( minErrorCorrection )
  
  vector<string> urls;
  vector<qrcodegen::QrCode> qrs;
  
  const string url_start = string("RADDATA://G0/") + sm_hex_digits[encode_options & 0x0F];
  
  if( measurements.size() == 1 )
  {
    const UrlSpectrum &m = measurements[0];
    
    //Now need to check that it will encode into a QR
    bool success_encoding = false;
    for( size_t num_parts = 1; num_parts < 10; ++num_parts )
    {
      vector<string> trial_urls = url_encode_spectrum( m, encode_options, num_parts, 0x00 );
      assert( trial_urls.size() == num_parts );
      
      urls.resize( trial_urls.size() );
      qrs.clear();
      
      string crc16;
      if( trial_urls.size() > 1 )
      {
        boost::crc_16_type crc_computer;
        
        for( const string &v : trial_urls )
          crc_computer.process_bytes( (void const *)v.data(), v.size() );
        
        const uint16_t crc = crc_computer.checksum();
        crc16 = std::to_string( static_cast<unsigned int>(crc) );
        crc16 += "/";
      }//if( trial_urls.size() > 1 )
      
      bool all_urls_small_enough = true;
      
      for( size_t url_num = 0; url_num < trial_urls.size(); ++url_num )
      {
        string &url = urls[url_num];
        
        url = url_start + std::to_string(num_parts - 1)
        + std::to_string(url_num) + "/" + crc16
        + trial_urls[url_num];
        
        try
        {
          if( alpha_num_qr_encode )
          {
            qrs.push_back( qrcodegen::QrCode::encodeText( url.c_str(), ecc ) );
          }else
          {
            vector<uint8_t> data( url.size() );
            memcpy( &(data[0]), &(url[0]), url.size() );
            qrs.push_back( qrcodegen::QrCode::encodeBinary( data, ecc ) );
          }
        }catch( ... )
        {
          all_urls_small_enough = false;
          break;
        }
      }//for( size_t url_num = 0; url_num < trial_urls.size(); ++url_num )
      
      if( all_urls_small_enough )
      {
        success_encoding = true;
        break;
      }//if( all_urls_small_enough )
    }//for( size_t num_parts = 0; num_parts < 10; ++num_parts )
    
    if( !success_encoding )
      throw runtime_error( "url_encode_spectra: Failed to encode spectrum into less than 10 QR codes at the desired error correction level" );
  }else //
  {
    // Multiple measurements to put in single URL
    urls.resize( 1 );
    string &url = urls[0];
    url = "";
    
    qrs.clear();
    
    for( size_t meas_num = 0; meas_num < measurements.size(); ++meas_num )
    {
      const UrlSpectrum &m = measurements[meas_num];
      const UrlSpectrum &m_0 = measurements[0];
      
      unsigned int skip_encode_options = SkipForEncoding::Encoding;
      if( meas_num )
        skip_encode_options |= SkipForEncoding::DetectorModel;
      
      if( meas_num && ((m.m_energy_cal_coeffs == m_0.m_energy_cal_coeffs) && (m.m_dev_pairs == m_0.m_dev_pairs)) )
        skip_encode_options |= SkipForEncoding::EnergyCal;
      
      if( meas_num && ((m.m_latitude == m_0.m_latitude) && (m.m_longitude == m.m_longitude)) )
        skip_encode_options |= SkipForEncoding::Gps;
      
      if( meas_num && (m.m_title == m_0.m_title) )
        skip_encode_options |= SkipForEncoding::Title;
      
      vector<string> spec_url = url_encode_spectrum( m, encode_options, 1, skip_encode_options );
      assert( spec_url.size() == 1 );
      
      if( meas_num )
        url += ":0A:"; //"/G" + std::to_string(meas_num)+ "/"; //Arbitrary
      url += spec_url[0];
    }//for( size_t meas_num = 0; meas_num < measurements.size(); ++meas_num )
    
    if( use_deflate )
      deflate_compress( &(url[0]), url.size(), url );
    
    if( use_base45 )
    {
      //cout << "During encoding, before base-45 encoded: '" << to_hex_bytes_str(url.substr(0,60)) << "'" << endl << endl;
      
      url = base45_encode( url );
      
      //cout << "During encoding, after base-45, before url-encoding: '" << to_hex_bytes_str(url.substr(0,60)) << "'" << endl << endl;
      
#ifndef NDEBUG
      const vector<uint8_t> raw = base45_decode( url );
      assert( !raw.empty() );
      string decoded( raw.size(), 0x0 );
      memcpy( &(decoded[0]), &(raw[0]), raw.size() );
      assert( decoded == url );
#endif
    }//if( use_base45 )
    
#ifndef NDEBUG
    string urlencoded = url_encode( url );
    assert( Wt::Utils::urlDecode(urlencoded) == url );
    url.swap( urlencoded );
#else
    url = url_encode( url );
#endif
    
    // cout << "During encoding, after URL encode: '" << to_hex_bytes_str(url.substr(0,60)) << "'" << endl;
    url = url_start + "0" + std::to_string( measurements.size() - 1 ) + "/" + url;
    
    try
    {
      if( alpha_num_qr_encode )
      {
        qrs.push_back( qrcodegen::QrCode::encodeText( url.c_str(), ecc ) );
      }else
      {
        vector<uint8_t> data( url.size() );
        memcpy( &(data[0]), &(url[0]), url.size() );
        qrs.push_back( qrcodegen::QrCode::encodeBinary( data, ecc ) );
      }
    }catch( std::exception &e )
    {
      throw runtime_error( "url_encode_spectra: could not fit all the spectra into a QR code - "
                          + std::string(e.what()) );
    }
  }//if( measurements.size() == 1 ) / else
  
  assert( (urls.size() == qrs.size()) && (qrs.size() >= 1) );
  
  std::vector<UrlEncodedSpec> answer;
  for( size_t i = 0; (i < urls.size()) && (i < qrs.size()); ++i )
  {
    UrlEncodedSpec spec;
    spec.m_url = urls[i];
    spec.m_qr_svg = QrCode::to_svg_string( qrs[i], 1 );
    
    spec.m_qr_size = qrs[i].getSize();
    spec.m_qr_version = qrs[i].getVersion();
    
    switch( qrs[i].getErrorCorrectionLevel() )
    {
      case qrcodegen::QrCode::Ecc::LOW:      spec.m_error_level = QrErrorCorrection::Low; break;
      case qrcodegen::QrCode::Ecc::MEDIUM:   spec.m_error_level = QrErrorCorrection::Medium; break;
      case qrcodegen::QrCode::Ecc::QUARTILE: spec.m_error_level = QrErrorCorrection::Quartile; break;
      case qrcodegen::QrCode::Ecc::HIGH:     spec.m_error_level = QrErrorCorrection::High; break;
    }//switch( qrs[1].getErrorCorrectionLevel() )
    
    answer.push_back( spec );
  }//for( size_t i = 0; (i < urls.size()) && (i < qrs.size()); ++i )
  
  return answer;
}//encode_spectra(...)


EncodedSpectraInfo get_spectrum_url_info( std::string url )
{
  EncodedSpectraInfo answer;
  
  //"RADDATA://G0/111/[CRC-16 ex. 65535]/"
  if( SpecUtils::istarts_with(url, "RADDATA://G0/") )
    url = url.substr(13);
  else if( SpecUtils::istarts_with(url, "INTERSPEC://G0/") )
    url = url.substr(15);
  else
    throw runtime_error( "get_spectrum_url_info: URL does not start with 'RADDATA://G0/'" );
  
  if( url.size() < 4 )
    throw runtime_error( "get_spectrum_url_info: URL too short" );
  
  try
  {
    answer.m_encode_options = hex_to_dec( url[0] );
    
    if( answer.m_encode_options
       & ~(EncodeOptions::NoDeflate | EncodeOptions::NoBase45
           | EncodeOptions::CsvChannelData | EncodeOptions::NoZeroCompressCounts) )
    {
      throw runtime_error( string("Encoding option had invalid bit set (hex digit ") + url[0] + ")" );
    }
    
    answer.m_number_urls = hex_to_dec( url[1] ) + 1;
    if( answer.m_number_urls > 10 )
      throw std::runtime_error( "Invalid number of total URLs specified" );
    
    if( answer.m_number_urls > 1 )
    {
      answer.m_spectrum_number = hex_to_dec( url[2] );
      if( answer.m_spectrum_number >= answer.m_number_urls )
        throw runtime_error( "Spectrum number larger than total number URLs" );
    }else
    {
      answer.m_num_spectra = hex_to_dec( url[2] ) + 1;
      if( answer.m_num_spectra > 10 )
        throw std::runtime_error( "Invalid number of spectra in URL." );
    }
    
    if( url[3] != '/' )
      throw runtime_error( "options not followed by a '/' character." );
    
    url = url.substr( 4 );
  }catch( std::exception &e )
  {
    throw runtime_error( "get_spectrum_url_info: options portion (three hex digits after"
                        " the //G/) of url is invalid: " + string(e.what()) );
  }//try / catch
  
  if( answer.m_number_urls > 1 )
  {
    const size_t pos = url.find( '/' );
    if( (pos == string::npos) || (pos > 5) )
      throw runtime_error( "get_spectrum_url_info: missing or invalid CRC-16 for multi-url spectrum" );
    
    string crcstr = url.substr(0, pos);
    int crc_val;
    if( !SpecUtils::parse_int(crcstr.c_str(), crcstr.size(), crc_val) )
      throw runtime_error( "get_spectrum_url_info: CRC-16 portion of URL ('" + crcstr + "') was not int" );
      
    if( (crc_val < 0) || (crc_val > std::numeric_limits<uint16_t>::max()) )
      throw runtime_error( "get_spectrum_url_info: CRC-16 portion of URL ('" + crcstr + "') was not not a uint16_t" );
      
    answer.m_crc = static_cast<uint16_t>( crc_val );
    
    url = url.substr( pos + 1 );
  }//if( answer.m_number_urls > 1 )
  
  answer.m_raw_data = url;
  
  //cout << "Before being URL decoded: '" << to_hex_bytes_str(url.substr(0,60)) << "'" << endl << endl;
  
  url = Wt::Utils::urlDecode( url );
  if( !(answer.m_encode_options & EncodeOptions::NoBase45) )
  {
    //cout << "Before being base-45 decoded: '" << to_hex_bytes_str(url.substr(0,60)) << "'" << endl << endl;
    const vector<uint8_t> raw = base45_decode( url );
    assert( !raw.empty() );
    url.resize( raw.size() );
    memcpy( &(url[0]), &(raw[0]), raw.size() );
  }//if( use_base45 )
  
  if( !(answer.m_encode_options & EncodeOptions::NoDeflate) )
  {
    // cout << "Going into deflate decompress: '" << to_hex_bytes_str(url.substr(0,60)) << "'" << endl << endl;
    deflate_decompress( &(url[0]), url.size(), url );
  }
  
  // cout << "After deflate decompress: '" << to_hex_bytes_str(url.substr(0,60)) << "'" << endl << endl;
  
  answer.m_data = url;
  
  return answer;
}//EncodedSpectraInfo get_spectrum_url_info( const std::string &url )


std::vector<UrlSpectrum> spectrum_decode_first_url( const std::string &url, const EncodedSpectraInfo &info )
{
  size_t pos = url.find( " S:" );
  
  if( pos == string::npos )
    throw runtime_error( "spectrum_decode_first_url: No ' S:' marker (i.e. the channel counts) found" );
  
  const string metainfo = " " + info.m_data.substr(0, pos);
  string counts_str = info.m_data.substr( pos + 3 ); //may have additional meas after the counts
  
  // Check to make sure the metainfo doesnt have the between spectrum delimiter
  if( metainfo.find(":0A:") != string::npos ) //"/G\\d/"
    throw runtime_error( "Found the measurement delimiter in pre-information - probably means a missing channel counts." );
  
  //We'll go through and get the non-spectrum info first
  UrlSpectrum spec;
  spec.m_source_type = SpecUtils::SourceType::Unknown;
  
  pos = metainfo.find( " I:" );
  if( (pos != string::npos) && ((pos + 4) < metainfo.size()) )
  {
    switch( metainfo[pos + 3] )
    {
      case 'I': spec.m_source_type = SpecUtils::SourceType::IntrinsicActivity; break;
      case 'C': spec.m_source_type = SpecUtils::SourceType::Calibration;       break;
      case 'B': spec.m_source_type = SpecUtils::SourceType::Background;        break;
      case 'F': spec.m_source_type = SpecUtils::SourceType::Foreground;        break;
        
      default:
        throw runtime_error( "Invalid source type character following ' I:'" );
    }//switch( v )
  }//if( (pos != string::npos) && ((pos + 4) < metainfo.size()) )
  
  auto get_str_field = [&metainfo]( char val ) -> string {
    const string key = string(" ") + val + string(":");
    const size_t index = metainfo.find( key );
    if( index == string::npos )
      return "";
    
    const size_t start_pos = index + 3;
    size_t end_pos = start_pos;
    
    // Look for the next string like " X:", where X can be any upper-case letter
    while( (end_pos < metainfo.size())
          && ((metainfo[end_pos] != ' ')
              || ((end_pos >= metainfo.size()) || ( (metainfo[end_pos+1] < 'A') && (metainfo[end_pos+1] > 'Z') ))
              || (((end_pos+1) >= metainfo.size()) ||(metainfo[end_pos + 2] != ':')) ) )
    {
      ++end_pos;
    }
    
    return metainfo.substr( start_pos, end_pos - start_pos);
  };//get_str_field(...)
  
  
  const string cal_str = get_str_field( 'C' );
  if( !cal_str.empty() )
  {
    if( !SpecUtils::split_to_floats( cal_str, spec.m_energy_cal_coeffs ) )
      throw runtime_error( "Invalid CSV for energy calibration coefficients." );
    
    if( spec.m_energy_cal_coeffs.size() < 2 )
      throw runtime_error( "Not enough energy calibration coefficients." );
  }//if( pos != string::npos )
  
  const string dev_pair_str = get_str_field( 'D' );
  if( !dev_pair_str.empty() )
  {
    vector<float> dev_pairs;
    if( !SpecUtils::split_to_floats( dev_pair_str, dev_pairs ) )
      throw runtime_error( "Invalid CSV for deviation pairs." );
    
    if( (dev_pairs.size() % 2) != 0 )
      throw runtime_error( "Not an even number of deviation pairs." );
    
    for( size_t i = 0; (i + 1) < dev_pairs.size(); ++i )
      spec.m_dev_pairs.push_back( {dev_pairs[i], dev_pairs[i+1]} );
  }//if( pos != string::npos )
  
  
  spec.m_model = get_str_field( 'M' );
  spec.m_title = get_str_field( 'O' );
  
  if( (info.m_encode_options & EncodeOptions::NoDeflate)
     && (info.m_encode_options & EncodeOptions::NoBase45)
     && (info.m_encode_options & EncodeOptions::CsvChannelData) )
  {
    spec.m_model = Wt::Utils::urlDecode( spec.m_model );
    spec.m_title = Wt::Utils::urlDecode( spec.m_title );
  }
  
  const string neut_str = get_str_field( 'N' );
  if( !neut_str.empty() )
  {
    if( !SpecUtils::parse_int( neut_str.c_str(), neut_str.size(), spec.m_neut_sum ) )
      throw runtime_error( "Failed to parse neutron count." );
  }
  
  const string lt_rt_str = get_str_field( 'T' );
  if( lt_rt_str.empty() )
    throw runtime_error( "Real and Live times not given." );
  
  vector<float> rt_lt;
  if( !SpecUtils::split_to_floats( lt_rt_str, rt_lt ) )
    throw runtime_error( "Could not parse real and live times." );
  
  if( rt_lt.size() != 2 )
    throw runtime_error( "Did not get exactly two times in Real/Live time field." );
  
  spec.m_real_time = rt_lt[0];
  spec.m_live_time = rt_lt[1];
  
  const string starttime_str = get_str_field( 'P' ); //Formatted by to_iso_string
  if( !starttime_str.empty() )
  {
    spec.m_start_time = SpecUtils::time_from_string( starttime_str );
    if( SpecUtils::is_special(spec.m_start_time) )
      throw runtime_error( "Invalid start time given (" + starttime_str + ")" );
  }//if( !starttime_str.empty() )
  
  const string gps_str = get_str_field( 'G' );
  if( !gps_str.empty() )
  {
    //latitude,longitude
    vector<string> parts;
    SpecUtils::split( parts, gps_str, "," );
    if( parts.size() != 2 )
      throw runtime_error( "GPS does not have exactly two comma separated doubles" );
    
    if( !SpecUtils::parse_double( parts[0].c_str(), parts[0].size(), spec.m_latitude ) )
      throw runtime_error( "Invalid latitude given" );
    
    if( !SpecUtils::parse_double( parts[1].c_str(), parts[1].size(), spec.m_longitude ) )
      throw runtime_error( "Invalid longitude given" );
  }//if( !gps_str.empty() )
  

  //UrlSpectrum
  string next_spec_info;
  if( info.m_encode_options & EncodeOptions::CsvChannelData )
  {
    //if( info.m_num_spectra
    const size_t end_pos = counts_str.find(":0A:");
    if( end_pos != string::npos )
    {
      next_spec_info = counts_str.substr( end_pos + 4 );
      counts_str = counts_str.substr( end_pos );
    }
    
    vector<long long> counts;
    if( !SpecUtils::split_to_long_longs( counts_str.c_str(), counts_str.size(), counts )
       || counts.empty() )
      throw runtime_error( "Failed to parse CSV channel counts." );
    
    spec.m_channel_data.resize( counts.size() );
    for( size_t i = 0; i < counts.size(); ++i )
      spec.m_channel_data[i] = static_cast<uint32_t>( counts[i] );
  }else
  {
    if( counts_str.size() < 2 )
      throw runtime_error( "Missing binary channel data info." );
    
    const size_t nread = decode_stream_vbyte( counts_str.data(), counts_str.size(), spec.m_channel_data );
    next_spec_info = counts_str.substr( nread );
    
    {// Begin compare to streamvbyte_decode
      uint16_t nints;
      memcpy( &nints, &(counts_str[0]), 2 );
      const uint32_t num_ints = static_cast<uint32_t>( nints );
      
      const size_t max_comp_size = streamvbyte_max_compressedbytes( num_ints );
      
      vector<uint32_t> test_channel_data( num_ints );
      
      vector<uint8_t> indata( counts_str.size() - 2 );
      memcpy( indata.data(), &(counts_str[2]), counts_str.size() - 2 );
      indata.resize( max_comp_size, 0 );
      
      
      const size_t nread_test = streamvbyte_decode( indata.data(), test_channel_data.data(), num_ints );
      if( nread_test > (counts_str.size() - 2) )
        throw runtime_error( "Invalid binary channel data." );
      assert( nread == (nread_test + 2) );
      assert( test_channel_data == spec.m_channel_data );
    }// End compare to streamvbyte_decode
    
    //cout << "next_spec_info.len=" << next_spec_info.length() << endl;
  }//if( info.m_encode_options & EncodeOptions::CsvChannelData ) / else
  
  
  if( !(info.m_encode_options & EncodeOptions::NoZeroCompressCounts)
     && (info.m_number_urls == 1) )
  {
    spec.m_channel_data = zero_compress_expand( spec.m_channel_data );
  }//if( !(info.m_encode_options & EncodeOptions::NoZeroCompressCounts) )
  
  vector<UrlSpectrum> answer;
  answer.push_back( spec );
  
  if( next_spec_info.length() > 1 )
  {
    vector<UrlSpectrum> the_rest = spectrum_decode_first_url( next_spec_info, info );
    answer.insert( end(answer), begin(the_rest), end(the_rest) );
  }
  
  return answer;
}//std::vector<UrlSpectrum> spectrum_decode_first_url( string url, const EncodedSpectraInfo & )


std::vector<UrlSpectrum> spectrum_decode_first_url( const std::string &url )
{
  EncodedSpectraInfo info = get_spectrum_url_info( url );
  
  if( info.m_spectrum_number != 0 )
    throw runtime_error( "spectrum_decode_first_url: URL indicates this is not first URL" );
  
  return spectrum_decode_first_url( info.m_data, info );
}//std::vector<UrlSpectrum> spectrum_decode_first_url( std::string url )


std::vector<uint32_t> spectrum_decode_not_first_url( std::string url )
{
  EncodedSpectraInfo info = get_spectrum_url_info( url );
  
  if( info.m_spectrum_number == 0 )
    throw runtime_error( "spectrum_decode_not_first_url: URL indicates it is first URL" );
  
  if( info.m_data.size() < 4 )
    throw runtime_error( "spectrum_decode_not_first_url: data too short" );
  
  vector<uint32_t> answer;
  const size_t nread = decode_stream_vbyte( info.m_data.data(), info.m_data.size(), answer );
  
  {// Begin test against streamvbyte_decode
    uint16_t num_ints;
    memcpy( &num_ints, &(info.m_data[0]), 2 );
    const size_t max_comp_size = streamvbyte_max_compressedbytes( num_ints );
    
    vector<uint32_t> test_answer( num_ints );
    vector<uint8_t> indata( info.m_data.size() - 2 );
    memcpy( indata.data(), &(info.m_data[2]), info.m_data.size() - 2 );
    indata.resize( max_comp_size, 0 );
    
    const size_t nbyte_read = streamvbyte_decode( indata.data(), test_answer.data(), num_ints );
    if( nbyte_read != (info.m_data.size() - 2) )
      throw runtime_error( "spectrum_decode_not_first_url: Invalid binary channel data." );
    
    assert( nread == (nbyte_read + 2) );
    assert( answer == test_answer );
  }// End test against streamvbyte_decode
  
  return answer;
}//std::vector<uint32_t> spectrum_decode_not_first_url( std::string url )


std::vector<UrlSpectrum> decode_spectrum_urls( vector<string> urls )
{
  if( urls.empty() )
    throw runtime_error( "decode_spectrum_urls: no input" );
  
  const EncodedSpectraInfo info = get_spectrum_url_info( urls[0] );
  
  if( info.m_spectrum_number != 0 )
    throw runtime_error( "decode_spectrum_urls: URL indicates this is not first URL" );
  
  vector<UrlSpectrum> spec_infos = spectrum_decode_first_url( info.m_data, info );
  
  if( (urls.size() > 1) && (spec_infos.size() > 1) )
    throw runtime_error( "decode_spectrum_urls: Multiple spectra were in first URL, but multiple URLs passed in." );
  
  assert( !spec_infos.empty() );
  if( spec_infos.empty() )
    throw logic_error( "decode_spectrum_urls: no spectra in URL." );
  
  if( urls.size() == 1 )
  {
    const UrlSpectrum &first_spec = spec_infos[0];
    
    for( size_t i = 1; i < spec_infos.size(); ++i )
    {
      UrlSpectrum &spec = spec_infos[i];
      if( spec.m_model.empty() )
        spec.m_model = first_spec.m_model;
      
      if( spec.m_energy_cal_coeffs.empty()
         && (spec.m_channel_data.size() == first_spec.m_energy_cal_coeffs.size()) )
        spec.m_energy_cal_coeffs = first_spec.m_energy_cal_coeffs;
      
      if( spec.m_dev_pairs.empty()
         && (spec.m_channel_data.size() == first_spec.m_energy_cal_coeffs.size()) )
        spec.m_dev_pairs = first_spec.m_dev_pairs;
      
      if( SpecUtils::valid_latitude(first_spec.m_latitude) && !SpecUtils::valid_latitude(spec.m_latitude) )
        spec.m_latitude = first_spec.m_latitude;
      if( SpecUtils::valid_longitude(first_spec.m_longitude) && !SpecUtils::valid_longitude(spec.m_longitude) )
        spec.m_longitude = first_spec.m_longitude;
    }//for( size_t i = 1; i < spec_infos.size(); ++i )
  }else
  {
    UrlSpectrum &spec = spec_infos[0];
    for( size_t i = 1; i < urls.size(); ++i )
    {
      const vector<uint32_t> more_counts = spectrum_decode_not_first_url( urls[i] );
      spec.m_channel_data.insert( end(spec.m_channel_data), begin(more_counts), end(more_counts) );
    }
    
    if( !(info.m_encode_options & EncodeOptions::NoZeroCompressCounts) )
      spec.m_channel_data = zero_compress_expand( spec.m_channel_data );
  }//if( urls.size() == 1 ) / else
  
  
  return spec_infos;
}//shared_ptr<SpecUtils::SpecFile> decode_spectrum_urls( vector<string> urls )



vector<UrlSpectrum> to_url_spectra( vector<shared_ptr<const SpecUtils::Measurement>> specs, string det_model )
{
  vector<UrlSpectrum> answer;
  for( size_t i = 0; i < specs.size(); ++i )
  {
    shared_ptr<const SpecUtils::Measurement> spec = specs[i];
    assert( spec );
    if( !spec || (spec->num_gamma_channels() < 1) )
      throw runtime_error( "to_url_spectra: invalid Measurement" );
    
    UrlSpectrum urlspec;
    urlspec.m_source_type = spec->source_type();
    
    shared_ptr<const SpecUtils::EnergyCalibration> cal = spec->energy_calibration();
    assert( cal );
    // Lower channel energy not currently implemented
    if( (cal->type() != SpecUtils::EnergyCalType::LowerChannelEdge)
       && (cal->type() != SpecUtils::EnergyCalType::InvalidEquationType) )
    {
      vector<float> coefs = cal->coefficients();
      switch( cal->type() )
      {
        case SpecUtils::EnergyCalType::FullRangeFraction:
          coefs = SpecUtils::fullrangefraction_coef_to_polynomial( coefs, cal->num_channels() );
          break;
          
        case SpecUtils::EnergyCalType::LowerChannelEdge:
        case SpecUtils::EnergyCalType::InvalidEquationType:
          assert( 0 );
          break;
          
        case SpecUtils::EnergyCalType::Polynomial:
        case SpecUtils::EnergyCalType::UnspecifiedUsingDefaultPolynomial:
          break;
      }//switch( cal->type() )
      
      urlspec.m_energy_cal_coeffs = coefs;
      urlspec.m_dev_pairs = cal->deviation_pairs();
    }//if( poly or FWF )
      
    
    urlspec.m_model = det_model;
    
    string operator_notes = spec->title();
    SpecUtils::ireplace_all( operator_notes, ":", " " );
    if( operator_notes.size() > 60 )
      operator_notes.substr(0, 60);
    
    // TODO: look for this info in the "Remarks" - I think Ortec Detectives and some other models will end up getting user input to there maybe?
    urlspec.m_title = operator_notes;
    urlspec.m_start_time = spec->start_time();
    
    if( spec->has_gps_info() )
    {
      urlspec.m_latitude = spec->latitude();
      urlspec.m_longitude = spec->longitude();
    }
    
    if( spec->contained_neutron() )
    {
      const float neut_sum = static_cast<float>( spec->neutron_counts_sum() );
      urlspec.m_neut_sum = SpecUtils::float_to_integral<int>( neut_sum );
    }
    
    
    urlspec.m_live_time = spec->live_time();
    urlspec.m_real_time = spec->real_time();
    
    const vector<float> &counts = *spec->gamma_counts();
    urlspec.m_channel_data.resize( counts.size() );
    for( size_t i = 0; i < counts.size(); ++i )
      urlspec.m_channel_data[i] = static_cast<uint32_t>( counts[i] );
    
    answer.push_back( urlspec );
  }//for( size_t i = 0; i < specs.size(); ++i )
  
  return answer;
}//vector<UrlSpectrum> to_url_spectra( vector<shared_ptr<const SpecUtils::Measurement>> specs, string det_model )


std::shared_ptr<SpecUtils::SpecFile> to_spec_file( const std::vector<UrlSpectrum> &spec_infos )
{
  auto specfile = make_shared<SpecUtils::SpecFile>();
  specfile->set_instrument_model( spec_infos[0].m_model );
  
  shared_ptr<SpecUtils::EnergyCalibration> first_cal;
  for( size_t spec_index = 0; spec_index < spec_infos.size(); ++spec_index )
  {
    const UrlSpectrum &spec = spec_infos[spec_index];
    
    auto m = make_shared<SpecUtils::Measurement>();
    m->set_source_type( spec.m_source_type );
    m->set_start_time( spec.m_start_time );
    m->set_position( spec.m_longitude, spec.m_latitude, {} );
    m->set_title( spec.m_title );
    
    if( spec.m_neut_sum >= 0 )
      m->set_neutron_counts( { static_cast<float>(spec.m_neut_sum) } );
    
    auto counts = make_shared<vector<float>>();
    counts->insert( end(*counts), begin(spec.m_channel_data), end(spec.m_channel_data) );
    
    m->set_gamma_counts( counts, spec.m_live_time, spec.m_real_time );
    
    if( !spec.m_energy_cal_coeffs.empty() )
    {
      // Check to see if we can re-use energy cal from previous measurement
      if( spec_index
         && (spec.m_energy_cal_coeffs == first_cal->coefficients())
         && (spec.m_dev_pairs == first_cal->deviation_pairs())
         && (spec.m_channel_data.size() == first_cal->num_channels()) )
      {
        m->set_energy_calibration( first_cal );
      }else
      {
        first_cal = make_shared<SpecUtils::EnergyCalibration>();
        
        try
        {
          first_cal->set_polynomial( spec.m_channel_data.size(), spec.m_energy_cal_coeffs, spec.m_dev_pairs );
        }catch( std::exception &e )
        {
          throw runtime_error( "Energy cal given is invalid: " + string(e.what()) );
        }
        
        m->set_energy_calibration( first_cal );
      }//if( we can re-use energy cal ) / else
    }//if( !spec.m_energy_cal_coeffs.empty() )
    
    
    specfile->add_measurement( m, false );
  }//for( const UrlSpectrum &spec : spec_infos )
  
  specfile->cleanup_after_load();
  
  return specfile;
}//std::shared_ptr<SpecUtils::SpecFile> to_spec_file( const std::vector<UrlSpectrum> &meas );


int dev_code()
{
  //test_base45();
  //return;
  
  //test_bitpacking();
  //return;
  
#define DELETE_UNWANTED_FILES 0
#define SAVE_ASCII_OUTPUT 0
#define USE_ZSTDLIB_CL 0

  const char *base_dir = "/Users/wcjohns/rad_ana/qrspec_test_data";
#if( SAVE_ASCII_OUTPUT )
  const char *out_dir = "/Users/wcjohns/rad_ana/processed_qrspec";
#endif
  
  const vector<string> files = SpecUtils::recursive_ls(base_dir);
  
  map<pair<size_t,string>, vector<size_t>> data_sizes_ascii, data_sizes_raw_bin,
                                           data_sizes_zlib, data_sizes_zlib_url, data_sizes_ascii_zlib,
                                           data_sizes_ascii_zlib_url, data_sizes_ascii_zlib_base_45_url,
                                           data_sizes_bin_base45, data_sizes_bin_base45_url;
  
#if( USE_ZSTDLIB_CL )
  map<pair<size_t,string>, vector<size_t>> data_sizes_zstdlib;
#endif
    
  
  for( string filename : files )
  {
    if( SpecUtils::likely_not_spec_file(filename) )
    {
#if( DELETE_UNWANTED_FILES )
      SpecUtils::remove_file( filename );
#endif
      continue;
    }
    
    SpecUtils::SpecFile spec;
    if( !spec.load_file( filename, SpecUtils::ParserType::Auto, filename) )
    {
#if( DELETE_UNWANTED_FILES )
      SpecUtils::remove_file( filename );
#endif
      continue;
    }
    
    vector<shared_ptr<const SpecUtils::Measurement>> foreground, background, usable_spectra;
    for( shared_ptr<const SpecUtils::Measurement> m : spec.measurements() )
    {
      if( (m->gamma_count_sum() < 100) || (m->num_gamma_channels() < 255)
         || ((m->live_time() <= 0.0) && (m->real_time() <= 0.0)) )
        continue;
      
      shared_ptr<const SpecUtils::EnergyCalibration> cal = m->energy_calibration();
      if( !cal || !cal->valid() || (cal->type() != SpecUtils::EnergyCalType::Polynomial) )
        continue;
      
      switch( m->source_type() )
      {
        case SpecUtils::SourceType::IntrinsicActivity:
        case SpecUtils::SourceType::Calibration:
          break;
          
        case SpecUtils::SourceType::Background:
          background.push_back( m );
          usable_spectra.push_back( m );
          break;
          
        case SpecUtils::SourceType::Foreground:
          foreground.push_back( m );
          usable_spectra.push_back( m );
          break;
          
        case SpecUtils::SourceType::Unknown:
          if( spec.detector_type() != SpecUtils::DetectorType::KromekD3S )
          {
            foreground.push_back( m );
            usable_spectra.push_back( m );
          }
          break;
      }//switch( m->source_type() )
    }//for( shared_ptr<const SpecUtils::Measurement> m : spec.measurements() )
    
    if( (foreground.size() > 1) || (background.size() > 1) || (usable_spectra.size() < 1) )
    {
#if( DELETE_UNWANTED_FILES )
      SpecUtils::remove_file( filename );
#endif
      continue;
    }
    
    for( shared_ptr<const SpecUtils::Measurement> m : usable_spectra )
    {
      stringstream strm;
      
      switch( m->source_type() )
      {
        case SpecUtils::SourceType::IntrinsicActivity: strm << "I=I "; break;
        case SpecUtils::SourceType::Calibration:       strm << "I=C "; break;
        case SpecUtils::SourceType::Background:        strm << "I=B "; break;
        case SpecUtils::SourceType::Foreground:        strm << "I=F "; break;
        case SpecUtils::SourceType::Unknown:                           break;
      }//switch( m->source_type() )
      
      const float lt = (m->live_time() > 0.0) ? m->live_time() : m->real_time();
      const float rt = (m->real_time() > 0.0) ? m->real_time() : m->live_time();
      strm << "T:" << PhysicalUnits::printCompact(rt,6) << "," << PhysicalUnits::printCompact(lt,6) << " ";
      
      shared_ptr<const SpecUtils::EnergyCalibration> cal = m->energy_calibration();
      assert( cal->type() == SpecUtils::EnergyCalType::Polynomial );
      strm << "C:";
      for( size_t i = 0; i < cal->coefficients().size(); ++i )
        strm << (i ? "," : "") << PhysicalUnits::printCompact(cal->coefficients()[i], i ? 4 : 7 );
      strm << " ";
      
      const vector<pair<float,float>> &dev_pairs = cal->deviation_pairs();
      if( !dev_pairs.empty() )
      {
        strm << "D:";
        for( size_t i = 0; i < dev_pairs.size(); ++i )
        {
          strm << (i ? "," : "") << PhysicalUnits::printCompact(dev_pairs[i].first, 5 )
          << "," << PhysicalUnits::printCompact(dev_pairs[i].second, 4 );
        }
        strm << " ";
      }//if( !dev_pairs.empty() )
      
      string model;
      if( spec.detector_type() != SpecUtils::DetectorType::Unknown )
        model = detectorTypeToString( spec.detector_type() );
      if( model.empty() )
        model = spec.instrument_model();
      
      // Remove equal signs and quotes
      SpecUtils::ireplace_all( model, ":", "" );
      SpecUtils::ireplace_all( model, "\"", "" );
      SpecUtils::ireplace_all( model, "'", "" );
      
      if( !model.empty() )
        strm << "M:" << model << " ";
      
      if( !SpecUtils::is_special(m->start_time()) )
      {
        // TODO: ISO string takes up 15 characters - could represent as a double, a la Microsoft time
        std::string t = SpecUtils::to_iso_string(m->start_time());
        const size_t dec_pos = t.find( "." );
        if( dec_pos != string::npos )
          t = t.substr(0, dec_pos);
        strm << "P:" << t << " ";
      }//if( !SpecUtils::is_special(m->start_time()) )
      
      if( m->has_gps_info() )
      {
        strm << "G:" << PhysicalUnits::printCompact(m->latitude(), 7)
             << "," << PhysicalUnits::printCompact(m->longitude(), 7) << " ";
      }
      
      if( m->contained_neutron() )
      {
        const float neut_sum = static_cast<float>( m->neutron_counts_sum() );
        const int ineut_sum = SpecUtils::float_to_integral<int>( neut_sum );
        strm << "N:" << ineut_sum << " ";
      }
      
      if( !m->title().empty() )
      {
        // TODO: look for this info in the "Remarks" - I think Ortec Detectives and some other models will end up getting user input to there maybe?
        string operator_notes = m->title();
        SpecUtils::ireplace_all( operator_notes, ":", " " );
        if( operator_notes.size() > 60 )
          operator_notes.substr(0, 60);
        
        string remark;
        remark = operator_notes;
        
        // If we arent gzipping, and not base-45 encoding, and channel data as ascii, then we should make sure this is ascii so the QR code can be generated in ascii mode - however, URL encoding here is wont work, because we will URL encode things again later on...
        //for( const char val : operator_notes )
        //{
        //  if( std::find( begin(sm_base42_chars), end(sm_base42_chars), val ) == end(sm_base42_chars) )
        //  {
        //    unsigned char c = (unsigned char)val;
        //    remark += '%';
        //    remark += sm_hex_digits[ ((c >> 4) & 0x0F) ];
        //    remark += sm_hex_digits[ (c & 0x0F) ];
        //  }else
        //  {
        //    remark += val;
        //  }
        //}
        
        strm << "O:" << remark << " ";
      }//if( !m->title().empty() )
      
      strm << "S:";
      const string data_up_to_spectrum = strm.str();
      
      vector<float> zero_compressed_counts;
      SpecUtils::compress_to_counted_zeros( *m->gamma_counts(), zero_compressed_counts );
      
      for( size_t i = 0; i < zero_compressed_counts.size(); ++i )
        strm << (i ? "," : "") << SpecUtils::float_to_integral<int>( zero_compressed_counts[i] );
      
      vector<int32_t> signed_compressed_integral_counts;
      vector<uint32_t> compressed_integral_counts;
      for( const float f : zero_compressed_counts )
      {
        compressed_integral_counts.push_back( SpecUtils::float_to_integral<uint32_t>( f ) );
        signed_compressed_integral_counts.push_back( SpecUtils::float_to_integral<int32_t>( f ) );
      }
      
      const string ascii_data = strm.str();
      
      if( model.empty() )
        model = "other";
      
#if( SAVE_ASCII_OUTPUT )
      const string save_dir = SpecUtils::append_path(out_dir, model);
      if( !SpecUtils::is_directory(save_dir) )
      {
        if( !SpecUtils::create_directory(save_dir) )
        {
          cerr << "Failed to make directory '" << save_dir << "'" << endl;
          assert( 0 );
        }
      }//if( !SpecUtils::is_directory(save_dir) )

      const string data_hash = Wt::Utils::hexEncode( Wt::Utils::md5(ascii_data) );
      const string save_file_name = SpecUtils::append_path(save_dir, data_hash + ".spec.txt" );
      
      ofstream output( save_file_name.c_str(), ios::out | ios::binary );
      assert( output.is_open() );
      output.write( ascii_data.c_str(), ascii_data.size() );
      assert( output.good() );
#endif
      
      if( compressed_integral_counts.size() > 65535 )
        throw runtime_error( "A max of 65535 is supported." );
      
      const vector<uint8_t> encoded_bytes = encode_stream_vbyte( compressed_integral_counts );
      
      
      auto print_test_case = [=](){
        assert( encoded_bytes == encode_stream_vbyte( compressed_integral_counts ) );
        
        static int test_num = 1;
        const auto inflags = cout.flags();
        
        cout << "\n\n\n  // Test case " << test_num << endl;
        cout << "  const vector<uint32_t> test_" << test_num << "_chan_cnts{ ";
        for( size_t i = 0; i < compressed_integral_counts.size(); ++i )
        {
          cout << (i ? ", " : "");
          if( !(i % 20) )
            cout << "\n    ";
          cout << compressed_integral_counts[i];
        }
        cout << "  };\n";
        cout << "  assert( test_" << test_num << "_chan_cnts.size() == " << compressed_integral_counts.size() << " );\n";
        
        cout << "  const vector<uint8_t> test_" << test_num << "_packed{ ";
        
        for( size_t i = 0; i < encoded_bytes.size(); ++i )
        {
          cout << (i ? ", " : "");
          if( !(i % 50) )
            cout << "\n    ";
          
          unsigned int val = encoded_bytes[i];
          //cout << "0x" << ios::hex << ios::uppercase << val;
          cout << val;
        }
        cout << "\n  };\n";
        cout << "  assert( test_" << test_num << "_packed.size() == " << encoded_bytes.size() << " );\n";
        
        cout << "  const vector<uint8_t> test_" << test_num << "_encoded = QRSpecDev::encode_stream_vbyte( test_" << dec << test_num << "_chan_cnts );\n"
        "  assert( test_" << dec << test_num << "_encoded == test_" << test_num << "_packed );\n"
        "  vector<uint32_t> test_" << test_num << "_dec;\n"
        "  const size_t test_"<< test_num << "_nbytedec = QRSpecDev::decode_stream_vbyte(test_" << test_num << "_encoded,test_" << test_num << "_dec);\n"
        "  assert( test_" << test_num << "_nbytedec == test_" << test_num << "_packed.size() );\n"
        "  assert( test_" << test_num << "_dec == test_" << dec << test_num << "_chan_cnts );\n"
        << endl;
        
        cout.flags(inflags);
        test_num += 1;
      };
      
      if( m->gamma_counts()->size() == 512 )
      {
        static int nprinted512 = 0;
        if( nprinted512++ < 5 )
          print_test_case();
      }else if( m->gamma_counts()->size() == 1024 )
      {
        static int nprinted1k = 0;
        if( nprinted1k++ < 5 )
          print_test_case();
      }else if( m->gamma_counts()->size() == 8192 )
      {
        static int nprinted8k = 0;
        if( nprinted8k++ < 5 )
          print_test_case();
      }else if( m->gamma_counts()->size() == 16384 )
      {
        static int nprinted16k = 0;
        if( nprinted16k++ < 5 )
          print_test_case();
      }
      
      {// Begin quick check we can recover things
        vector<uint32_t> recovdata;
        const size_t nread = decode_stream_vbyte( encoded_bytes.data(), encoded_bytes.size(), recovdata );
        assert( nread == encoded_bytes.size() );
        assert( recovdata == compressed_integral_counts );
      }// End quick check we can recover things
      
      
      {// begin comparison against streamvbyte_encode
        const uint32_t num_counts = static_cast<uint32_t>( compressed_integral_counts.size() );
        const uint32_t * const datain = compressed_integral_counts.data();
        const size_t comp_size = streamvbyte_max_compressedbytes( num_counts );
        vector<uint8_t> test_bytes( comp_size );
        uint8_t * compressedbuffer = test_bytes.data();
        const size_t compsize = streamvbyte_encode( datain, num_counts, compressedbuffer );
        //const size_t compsize = streamvbyte_delta_encode( datain, num_counts, compressedbuffer, 0 ); // much worse than streamvbyte_encode
        test_bytes.resize( compsize );
        
        {// Begin quick check we can recover things
          vector<uint32_t> recovdata( num_counts );
          
          size_t compsize2 = streamvbyte_decode(test_bytes.data(), recovdata.data(), num_counts );
          //size_t compsize2 = streamvbyte_delta_decode(test_bytes.data(), recovdata.data(), num_counts, 0 );
          assert( compsize2 == compsize );
          for( size_t i = 0; i < num_counts; ++i )
          {
            assert( recovdata[i] == compressed_integral_counts[i] );
          }
        }// End quick check we can recover things
        
        test_bytes.insert( begin(test_bytes), static_cast<uint8_t>( num_counts >> 8 ) );
        test_bytes.insert( begin(test_bytes), static_cast<uint8_t>( (num_counts & 0x00FF) ) );
        
        assert( encoded_bytes == test_bytes );
      }// end comparison against streamvbyte_encode
      
      
      /*
       oroch compressed better than streamvbyte before zlib compression, but after zlib, streamvbyte is better
      const size_t comp_size = oroch::varint_codec<uint32_t>::space(compressed_integral_counts.begin(), compressed_integral_counts.end());
      std::vector<uint8_t> encoded_bytes( comp_size );
      uint8_t *d = encoded_bytes.data();
      oroch::varint_codec<uint32_t>::encode(d, compressed_integral_counts.begin(), compressed_integral_counts.end());
      {
        vector<uint32_t> output( compressed_integral_counts.size() );
        oroch::src_bytes_t b_it = encoded_bytes.data();
        oroch::varint_codec<uint32_t>::decode( begin(output), end(output), b_it );
        
        for (int i = 0; i < compressed_integral_counts.size(); i++) {
          assert( compressed_integral_counts[i] == output[i] );
        }
      }
       */
      
      
      /*
      //No bitpacking
      std::vector<uint8_t> encoded_bytes( 4*compressed_integral_counts.size() );
      memcpy( (void *)encoded_bytes.data(), (void *)compressed_integral_counts.data(), 4*compressed_integral_counts.size() );
      */
      
      /*
#define ENCODE_BITS 7
#define ENCODE_FREF 0
      using codec = oroch::bitfor_codec<uint32_t>;
      std::vector<uint8_t> encoded_bytes( codec::basic_codec::space(compressed_integral_counts.size(),ENCODE_BITS)  );
      codec::parameters params(ENCODE_FREF, ENCODE_BITS);
      oroch::dst_bytes_t enc_dest = (unsigned char *)encoded_bytes.data();
      codec::encode( enc_dest, begin(compressed_integral_counts), end(compressed_integral_counts), params );
      encoded_bytes.resize( enc_dest - (unsigned char *)encoded_bytes.data() );
      
      {// Begin quick check we can decode
        codec::parameters params(ENCODE_FREF, ENCODE_BITS);
        vector<uint32_t> output( compressed_integral_counts.size() );
        
        oroch::src_bytes_t b_it = encoded_bytes.data();
        codec::decode( begin(output), end(output), b_it, params);
        
        for (int i = 0; i < compressed_integral_counts.size(); i++) {
          if( compressed_integral_counts[i] != output[i] )
       throw runtime_error(
        }
      }// End check we can decode
      */
      
      
      //TODO:
      //  - Try https://github.com/lemire/LittleIntPacker
      //  - try out lzma compression
      vector<uint8_t> raw_bin_data( data_up_to_spectrum.size() + encoded_bytes.size() );
      
      
      
      memcpy( raw_bin_data.data(), data_up_to_spectrum.data(), data_up_to_spectrum.size() );
      memcpy( raw_bin_data.data() + data_up_to_spectrum.size(), encoded_bytes.data(), encoded_bytes.size() );
      
#if( SAVE_ASCII_OUTPUT )
      const string bin_save_file_name = SpecUtils::append_path(save_dir, data_hash + ".spec.bin" );
      
      {// Begin block to write bin output
        ofstream bin_output( bin_save_file_name.c_str(), ios::out | ios::binary );
        assert( bin_output.is_open() );
        bin_output.write( (const char *)raw_bin_data.data(), raw_bin_data.size() );
        assert( bin_output.good() );
      }// End block to write bin output
#endif
      
#if( USE_ZSTDLIB_CL )
      vector<char> plain_zstdlib_data;
      {// Begin block to do zstd compression, and read back in
        const string out_stdzlib_name = bin_save_file_name + ".zstd";
        const string cmd = "/usr/local/bin/zstd --ultra -22 -q -D '/Users/wcjohns/rad_ana/processed_qrspec/zstd_spec.dict' '" + bin_save_file_name + "' -o '" + out_stdzlib_name + "'";
        const int rval = system( cmd.c_str() );
        assert( rval == 0 );
        SpecUtils::load_file_data( out_stdzlib_name.c_str(), plain_zstdlib_data );
        SpecUtils::remove_file( out_stdzlib_name );
      }// End block to do zstd compression, and read back in
#endif
      
      // Need to generate a dictionary from all files
      //vector<char> dict_zstdlib_data;
      //system( ("cd '" + save_dir + "'; zstd --ultra -22 " + data_hash + ".spec.bin").c_str() );
      //SpecUtils::load_file_data( (bin_save_file_name + ".zst").c_str(), dict_zstdlib_data );
      //SpecUtils::remove_file( bin_save_file_name + ".zst" );
      
      
      vector<uint8_t> zlib_data, ascii_zlib_data;
      deflate_compress( raw_bin_data.data(), raw_bin_data.size(), zlib_data );
      deflate_compress( (const void *)&(ascii_data[0]), ascii_data.size(), ascii_zlib_data );
      
      {
        vector<uint8_t> decomp_out_data;
        deflate_decompress( zlib_data.data(), zlib_data.size(), decomp_out_data );
        assert( decomp_out_data == raw_bin_data );
        
        deflate_decompress( ascii_zlib_data.data(), ascii_zlib_data.size(), decomp_out_data );
        vector<uint8_t> ascii_in( (uint8_t *)ascii_data.data(), (uint8_t *)(ascii_data.data() + ascii_data.size()) );
        assert( decomp_out_data == ascii_in );
      }
      
      
      //Now add in base45 encoding, then URL encoding.
      const string base45_data = base45_encode( zlib_data );
      const string url_base45_data = url_encode( base45_data );
      const string zlib_url = url_encode( zlib_data );
      
      const string ascii_base45_data = base45_encode( ascii_zlib_data );
      const string ascii_url_base45_data = url_encode( ascii_base45_data );
      
      const string ascii_url = url_encode( ascii_data );
      
      
      const size_t nchannel = m->num_gamma_channels();
      const auto key = pair<size_t,string>( m->num_gamma_channels(), model );
      
      data_sizes_ascii[key].push_back( ascii_url.size() );
      data_sizes_raw_bin[key].push_back( raw_bin_data.size() );
      data_sizes_zlib[key].push_back( zlib_data.size() );
      data_sizes_zlib_url[key].push_back( zlib_url.size() );
      
#if( USE_ZSTDLIB_CL )
      data_sizes_zstdlib[key].push_back( plain_zstdlib_data.size() );
#endif
      
      data_sizes_ascii_zlib[key].push_back( ascii_zlib_data.size() );
      data_sizes_bin_base45[key].push_back( base45_data.size() );
      data_sizes_bin_base45_url[key].push_back( url_base45_data.size() );
      data_sizes_ascii_zlib_url[key].push_back( ascii_zlib_data.size() );
      data_sizes_ascii_zlib_base_45_url[key].push_back( ascii_url_base45_data.size() );
      
      
      //Add in statistics for how much zero compress helps, then how much encoding helps
      //data_up_to_spectrum
      //vector<float> zero_compressed_counts;
      //vector<uint32_t> compressed_integral_counts;

      try
      {
        const vector<UrlSpectrum> start_meas = to_url_spectra( {m}, model );

        uint8_t encode_options = 0;
        const QrErrorCorrection ecc = QrErrorCorrection::Low;
        
        vector<UrlEncodedSpec> encoded = QRSpecDev::url_encode_spectra( start_meas, ecc, 0 );
        assert( encoded.size() >= 1 );
        
        string eccstr;
        switch( encoded[0].m_error_level )
        {
          case QrErrorCorrection::Low:      eccstr = "Low";      break;
          case QrErrorCorrection::Medium:   eccstr = "Medium";   break;
          case QrErrorCorrection::Quartile: eccstr = "Quartile"; break;
          case QrErrorCorrection::High:     eccstr = "High";     break;
        }//switch( encoded[0].m_error_level )
        
        //cout << "Encoded model: " << model << " nchannel: " << m->num_gamma_channels() << " to "
        //<< encoded.size() << " URLS, with size=" << encoded[0].m_qr_size
        //<< ", version=" << encoded[0].m_qr_version << " and ECC=" << eccstr << endl;
        
        vector<string> urls;
        for( const auto &g : encoded )
          urls.push_back( g.m_url );
        
#define TEST_EQUAL_ENOUGH(lhs,rhs) \
          assert( (fabs((lhs) - (rhs)) < 1.0E-12) \
                 || (fabs((lhs) - (rhs)) < 1.0E-5*std::max(fabs(lhs), fabs(rhs))) );
        
        try
        {
          const vector<UrlSpectrum> decoded_specs = decode_spectrum_urls( urls );
          //cout << "Decoded URL!" << endl;
          
          assert( decoded_specs.size() == start_meas.size() );
          for( size_t i = 0; i < decoded_specs.size(); ++i )
          {
            const auto &orig = start_meas[i];
            const auto &decoded = decoded_specs[i];
            
            assert( orig.m_source_type == decoded.m_source_type );
            assert( orig.m_energy_cal_coeffs.size() == decoded.m_energy_cal_coeffs.size() );
            for( size_t cal_index = 0; cal_index < orig.m_energy_cal_coeffs.size(); ++cal_index )
            {
              TEST_EQUAL_ENOUGH( orig.m_energy_cal_coeffs[cal_index], decoded.m_energy_cal_coeffs[cal_index] );
            }
    
            assert( orig.m_dev_pairs.size() == decoded.m_dev_pairs.size() );
            for( size_t cal_index = 0; cal_index < orig.m_dev_pairs.size(); ++cal_index )
            {
              TEST_EQUAL_ENOUGH( orig.m_dev_pairs[cal_index].first, decoded.m_dev_pairs[cal_index].first );
              TEST_EQUAL_ENOUGH( orig.m_dev_pairs[cal_index].second, decoded.m_dev_pairs[cal_index].second );
            }
            
            assert( orig.m_model == decoded.m_model );
            assert( orig.m_title == decoded.m_title );
            
            const auto tdiff = orig.m_start_time - decoded.m_start_time;
            assert( (tdiff < std::chrono::seconds(2)) && (tdiff > std::chrono::seconds(-2)) );
            
            TEST_EQUAL_ENOUGH( orig.m_latitude, decoded.m_latitude );
            TEST_EQUAL_ENOUGH( orig.m_longitude, decoded.m_longitude );
            
            assert( orig.m_neut_sum == decoded.m_neut_sum );
            
            TEST_EQUAL_ENOUGH( orig.m_live_time, decoded.m_live_time );
            TEST_EQUAL_ENOUGH( orig.m_real_time, decoded.m_real_time );
            
            assert( orig.m_channel_data.size() == decoded.m_channel_data.size() );
            for( size_t chan_index = 0; chan_index < decoded.m_channel_data.size(); ++chan_index )
            {
              assert( orig.m_channel_data[chan_index] == decoded.m_channel_data[chan_index] );
            }
          }//for( size_t i = 0; i < decoded_specs.size(); ++i )
          
          
          // Implement testing blah blah blah
          
        }catch( std::exception &e )
        {
          cerr << "Failed to decode UTR: " << e.what() << endl;
        }
        
      }catch( std::exception &e )
      {
        cerr << "Failed to encode model: " << model << " nchannel: " << m->num_gamma_channels() << " reason: " << e.what() << endl;
      }//try / catch
      
      //Now do:
      //  - Read compressed data back in, and encode it to base-45
      //  - create a url: interspec://s/111/CRC-16/ + base-45-encoded(data) (where first 1 is version, and "11" is optional 1 of 1 - and CRC-32 is only for multi-part QR codes, and only needs to be in the first one).
      // Check using https://github.com/lemire/simdcomp
      //    - If multi-part QR code, then should give a checksum, or total size, or something
      //  - create QR code, and decoder
      //
      //Consider:
      //  - Adding two byte int to say how long the spectrum data should be - this is to allow multiple spectra in a QR code
      //  - Maybe compression type can be specified?  We'll check out zlib, see how this performs.
      //    - Maybe this could also apply to the integer series method
      //  - Maybe add serial number (dont allow space or equal)
      
    }//for( shared_ptr<const SpecUtils::Measurement> m : usable_spectra )
  }//for( string filename : files )
  
  
  auto avrg_size = []( const vector<size_t> &sizes ) -> size_t {
    if( sizes.empty() )
      return 0;
    const size_t totalbytes = std::accumulate( begin(sizes), end(sizes), size_t(0) );
    return totalbytes / sizes.size();
  };
  
  // https://en.wikipedia.org/wiki/QR_code#Storage
  const size_t max_ascii_chars_v25 = 1269;
  const size_t max_ascii_chars_v40 = 1852;
  const size_t max_ascii_chars_v40L = 4296; //Low error correction (7% of data bytes can be restored)
  const size_t max_binary_v40L = 2953;
  
  const size_t header_size_single = 17;  // "RADDATA://G0/111/"
  const size_t header_size_multi = 23;   // "RADDATA://G0/111/[CRC-16 ex. 65535]/"
  
  // TODO: print out percentage that fit into max_ascii_chars_v25, max_ascii_chars_v40, max_ascii_chars_v40L
  
  for( const auto &key : data_sizes_ascii )
  {
    if( key.second.size() < 20 )
      continue;
    
    const size_t nchannel = key.first.first;
    const string model = (key.first.second.size() > 15) ? key.first.second.substr(0,15) : key.first.second;
    
    auto print_stats = [=]( vector<size_t> sizes ){
      std::sort( begin(sizes), end(sizes) );
      
      size_t num_max_ascii_chars_v25 = 0, num_max_ascii_chars_v40 = 0,
             num_max_ascii_chars_v40L = 0, num_max_binary_v40L = 0;
      for( const auto i : sizes )
      {
        num_max_ascii_chars_v25 += ((i+header_size_single) < max_ascii_chars_v25);
        num_max_ascii_chars_v40 += ((i+header_size_single) < max_ascii_chars_v40);
        num_max_ascii_chars_v40L += ((i+header_size_single) < max_ascii_chars_v40L);
        num_max_binary_v40L += ((i+header_size_single) < max_binary_v40L);
      }
      
      cout
      << "," << setw(11) << sizes.front()
      << "," << setw(11) << avrg_size(sizes)
      << "," << setw(11) << sizes.back()
      << "," << setw(11) << (100.0*num_max_ascii_chars_v25) / sizes.size()
      << "," << setw(11) << (100.0*num_max_ascii_chars_v40) / sizes.size()
      << "," << setw(11) << (100.0*num_max_ascii_chars_v40L) / sizes.size()
      << "," << setw(11) << (100.0*num_max_binary_v40L) / sizes.size()
      << endl;
    };//auto print_stats = [=]( vector<size_t> sizes ){
    
    cout
    << setw(15) << "Model" << ","
    << setw(7) << "#Chan" << ","
    << setw(7) << "#Files" << ","
    << setw(17) << "Rep." << ","
    << setw(11) << "MinByte" << ","
    << setw(11) << "AvgByte" << ","
    << setw(11) << "MaxByte" << ","
    << setw(11) << "%<v25" << ","
    << setw(11) << "%<v40" << ","
    << setw(11) << "%<v40L" << ","
    << setw(11) << "%<bin-v40L"
    << endl;
    
    cout << setw(15) << model << "," << setw(7) << nchannel
         << "," << setw(7) << data_sizes_ascii[key.first].size()
         << endl;
    
    
    cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << "," << setw(17) << "ASCII URL";
    print_stats( data_sizes_ascii[key.first] );
    
    //cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << "," << setw(17) << "RAW BIN";
    //print_stats( data_sizes_raw_bin[key.first] );
    
    //cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << ","<< setw(17) << "BIN ZLIB";
    //print_stats( data_sizes_zlib[key.first] );
    
#if( USE_ZSTDLIB_CL )
    //cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << ","<< setw(17) << "ZSTDLIB";
    //print_stats( data_sizes_zstdlib[key.first] );
#endif
    
    //cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << "," << setw(17) << "ASCII ZLIB";
    //print_stats( data_sizes_ascii_zlib_url[key.first] );
    
    //cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << "," << setw(17) << "BIN B45 ZLIB";
    //print_stats( data_sizes_bin_base45[key.first] );
    
    cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << "," << setw(17) << "BIN B45URL ZLIB";
    print_stats( data_sizes_bin_base45_url[key.first] );
    
    cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << "," << setw(17) << "ASCII B45URL ZLIB";
    print_stats( data_sizes_ascii_zlib_base_45_url[key.first] );
    
    cout << setw(15) << "" << "," << setw(7) << "" << "," << setw(7) << "" << "," << setw(17) << "BIN ZLIB URL";
    print_stats( data_sizes_zlib_url[key.first] );
    
    cout << endl << endl;
  }//for( const auto &key : data_sizes_ascii )
}//int dev_code()



void deflate_compress( const void *in_data, size_t in_data_size, std::string &out_data )
{
  deflate_compress_internal( in_data, in_data_size, out_data );
}

void deflate_compress( const void *in_data, size_t in_data_size, std::vector<uint8_t> &out_data )
{
  deflate_compress_internal( in_data, in_data_size, out_data );
}

void deflate_decompress( void *in_data, size_t in_data_size, std::string &out_data )
{
  deflate_decompress_internal( in_data, in_data_size, out_data );
}

void deflate_decompress( void *in_data, size_t in_data_size, std::vector<uint8_t> &out_data )
{
  deflate_decompress_internal( in_data, in_data_size, out_data );
}


}//namespace QRSpecDev

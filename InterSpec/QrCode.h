#ifndef QrCode_h
#define QrCode_h
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

#include <string>
#include <vector>
#include <utility>
#include <cstdint>

// Forward declarations
class SimpleDialog;
namespace qrcodegen
{
  class QrCode;
}


namespace QrCode
{
  /**
   
   Will succeed for strings that have 2953 or fewer UTF-8 code units.
   
   Throws exception on error.
   */
  std::pair<std::string,int> utf8_string_to_svg_qr( const std::string &input );

  std::pair<std::string,int> binary_to_svg_qr( const std::vector<std::uint8_t> &data );

  /** Turns a QrCode result into a SVG with a viewBox of height/width of `qr.getSize()`.
   
   The border size provides the "quiet zone" around the QR code.
   At least a 4 module wide margin around the QR code is required; see https://www.qrcode.com/en/howto/code.html
   */
  std::string to_svg_string( const qrcodegen::QrCode &qr, int border = 4 );


  /** Returns nullptr if an error is encountered. */
  SimpleDialog *displayTxtAsQrCode( const std::string &url,
                                    const std::string &title,
                                    const std::string &description );
}//namespace QrCode

#endif //QrCode_h

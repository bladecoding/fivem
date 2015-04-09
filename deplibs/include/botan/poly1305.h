/*
* Poly1305
* (C) 2014 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#ifndef BOTAN_MAC_POLY1305_H__
#define BOTAN_MAC_POLY1305_H__

#include <botan/mac.h>
#include <memory>

namespace Botan {

/**
* DJB's Poly1305
* Important note: each key can only be used once
*/
class BOTAN_DLL Poly1305 : public MessageAuthenticationCode
   {
   public:
      std::string name() const override { return "Poly1305"; }

      MessageAuthenticationCode* clone() const { return new Poly1305; }

      void clear();

      size_t output_length() const { return 16; }

      Key_Length_Specification key_spec() const
         {
         return Key_Length_Specification(32);
         }

   private:
      void add_data(const byte[], size_t);
      void final_result(byte[]);
      void key_schedule(const byte[], size_t);

      secure_vector<u64bit> m_poly;
      secure_vector<byte> m_buf;
      size_t m_buf_pos = 0;
   };

}

#endif

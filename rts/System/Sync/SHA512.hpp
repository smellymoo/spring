/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SHA512_HDR
#define SHA512_HDR

#include <cstddef>
#include <cstdint>

#include <array>
#include <vector>
#include <string>


namespace sha512 {
	static constexpr uint8_t SHA_LEN =  64; // digest size
	static constexpr uint8_t BLK_LEN = 128;

	static constexpr uint8_t NUM_STATE_CONSTS =  8;
	static constexpr uint8_t NUM_ROUND_CONSTS = 80;

	static constexpr uint64_t STATE_CONSTS[NUM_STATE_CONSTS] = {
		0x6A09E667F3BCC908ull, 0xBB67AE8584CAA73Bull, 0x3C6EF372FE94F82Bull, 0xA54FF53A5F1D36F1ull,
		0x510E527FADE682D1ull, 0x9B05688C2B3E6C1Full, 0x1F83D9ABFB41BD6Bull, 0x5BE0CD19137E2179ull,
	};
	static constexpr uint64_t ROUND_CONSTS[NUM_ROUND_CONSTS] = {
		0x428A2F98D728AE22ull, 0x7137449123EF65CDull, 0xB5C0FBCFEC4D3B2Full, 0xE9B5DBA58189DBBCull,
		0x3956C25BF348B538ull, 0x59F111F1B605D019ull, 0x923F82A4AF194F9Bull, 0xAB1C5ED5DA6D8118ull,
		0xD807AA98A3030242ull, 0x12835B0145706FBEull, 0x243185BE4EE4B28Cull, 0x550C7DC3D5FFB4E2ull,
		0x72BE5D74F27B896Full, 0x80DEB1FE3B1696B1ull, 0x9BDC06A725C71235ull, 0xC19BF174CF692694ull,
		0xE49B69C19EF14AD2ull, 0xEFBE4786384F25E3ull, 0x0FC19DC68B8CD5B5ull, 0x240CA1CC77AC9C65ull,
		0x2DE92C6F592B0275ull, 0x4A7484AA6EA6E483ull, 0x5CB0A9DCBD41FBD4ull, 0x76F988DA831153B5ull,
		0x983E5152EE66DFABull, 0xA831C66D2DB43210ull, 0xB00327C898FB213Full, 0xBF597FC7BEEF0EE4ull,
		0xC6E00BF33DA88FC2ull, 0xD5A79147930AA725ull, 0x06CA6351E003826Full, 0x142929670A0E6E70ull,
		0x27B70A8546D22FFCull, 0x2E1B21385C26C926ull, 0x4D2C6DFC5AC42AEDull, 0x53380D139D95B3DFull,
		0x650A73548BAF63DEull, 0x766A0ABB3C77B2A8ull, 0x81C2C92E47EDAEE6ull, 0x92722C851482353Bull,
		0xA2BFE8A14CF10364ull, 0xA81A664BBC423001ull, 0xC24B8B70D0F89791ull, 0xC76C51A30654BE30ull,
		0xD192E819D6EF5218ull, 0xD69906245565A910ull, 0xF40E35855771202Aull, 0x106AA07032BBD1B8ull,
		0x19A4C116B8D2D0C8ull, 0x1E376C085141AB53ull, 0x2748774CDF8EEB99ull, 0x34B0BCB5E19B48A8ull,
		0x391C0CB3C5C95A63ull, 0x4ED8AA4AE3418ACBull, 0x5B9CCA4F7763E373ull, 0x682E6FF3D6B2B8A3ull,
		0x748F82EE5DEFB2FCull, 0x78A5636F43172F60ull, 0x84C87814A1F0AB72ull, 0x8CC702081A6439ECull,
		0x90BEFFFA23631E28ull, 0xA4506CEBDE82BDE9ull, 0xBEF9A3F7B2C67915ull, 0xC67178F2E372532Bull,
		0xCA273ECEEA26619Cull, 0xD186B8C721C0C207ull, 0xEADA7DD6CDE0EB1Eull, 0xF57D4F7FEE6ED178ull,
		0x06F067AA72176FBAull, 0x0A637DC5A2C898A6ull, 0x113F9804BEF90DAEull, 0x1B710B35131C471Bull,
		0x28DB77F523047D84ull, 0x32CAAB7B40C72493ull, 0x3C9EBE0A15C9BEBCull, 0x431D67C49C100D4Cull,
		0x4CC5D4BECB3E42B6ull, 0x597F299CFC657E2Aull, 0x5FCB6FAB3AD6FAECull, 0x6C44198C4A475817ull,
	};

	static constexpr const char* TEST_STR_PAIR[2] = {
		"",
		"cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"
	};


	using raw_digest = std::array<uint8_t, SHA_LEN        >;
	using hex_digest = std::array<   char, SHA_LEN * 2 + 1>; // null-terminated

	void read_digest(const std::string& hex, raw_digest& sha_bytes); // hex to raw
	raw_digest read_digest(const std::string& hex); // hex to raw
	void read_digest(const hex_digest& hex_chars, raw_digest& sha_bytes); // hex to raw
	void dump_digest(const raw_digest& sha_bytes, hex_digest& hex_chars); // raw to hex
	std::string dump_digest(const raw_digest& sha_bytes); // raw to hex
	void calc_digest(const std::vector<uint8_t>& msg_bytes, raw_digest& sha_bytes);
	void calc_digest(const uint8_t msg_bytes[], size_t len, uint8_t sha_bytes[SHA_LEN]);
	void dm_compress(uint64_t state[NUM_STATE_CONSTS], const uint8_t blocks[], size_t len);

	bool unit_test(const char* msg_str = TEST_STR_PAIR[0], const char* sha_str = TEST_STR_PAIR[1]);

	static constexpr raw_digest NULL_RAW_DIGEST = { 0 };
	static constexpr hex_digest NULL_HEX_DIGEST = {
		'0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0', 0
	};
};

#endif


/* $OpenBSD: dh.c,v 1.63 2018/02/07 02:06:50 jsing Exp $ */
/*
 * Copyright (c) 2000 Niels Provos.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "includes.h"

#ifdef WITH_OPENSSL

#include <openssl/bn.h>
#include <openssl/dh.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "dh.h"
#include "pathnames.h"
#include "log.h"
#include "misc.h"
#include "ssherr.h"

static int
parse_prime(int linenum, char *line, struct dhgroup *dhg)
{
	char *cp, *arg;
	char *strsize, *gen, *prime;
	const char *errstr = NULL;
	long long n;

	dhg->p = dhg->g = NULL;
	cp = line;
	if ((arg = strdelim(&cp)) == NULL)
		return 0;
	/* Ignore leading whitespace */
	if (*arg == '\0')
		arg = strdelim(&cp);
	if (!arg || !*arg || *arg == '#')
		return 0;

	/* time */
	if (cp == NULL || *arg == '\0')
		goto truncated;
	arg = strsep(&cp, " "); /* type */
	if (cp == NULL || *arg == '\0')
		goto truncated;
	/* Ensure this is a safe prime */
	n = strtonum(arg, 0, 5, &errstr);
	if (errstr != NULL || n != MODULI_TYPE_SAFE) {
		error("moduli:%d: type is not %d", linenum, MODULI_TYPE_SAFE);
		goto fail;
	}
	arg = strsep(&cp, " "); /* tests */
	if (cp == NULL || *arg == '\0')
		goto truncated;
	/* Ensure prime has been tested and is not composite */
	n = strtonum(arg, 0, 0x1f, &errstr);
	if (errstr != NULL ||
	    (n & MODULI_TESTS_COMPOSITE) || !(n & ~MODULI_TESTS_COMPOSITE)) {
		error("moduli:%d: invalid moduli tests flag", linenum);
		goto fail;
	}
	arg = strsep(&cp, " "); /* tries */
	if (cp == NULL || *arg == '\0')
		goto truncated;
	n = strtonum(arg, 0, 1<<30, &errstr);
	if (errstr != NULL || n == 0) {
		error("moduli:%d: invalid primality trial count", linenum);
		goto fail;
	}
	strsize = strsep(&cp, " "); /* size */
	if (cp == NULL || *strsize == '\0' ||
	    (dhg->size = (int)strtonum(strsize, 0, 64*1024, &errstr)) == 0 ||
	    errstr) {
		error("moduli:%d: invalid prime length", linenum);
		goto fail;
	}
	/* The whole group is one bit larger */
	dhg->size++;
	gen = strsep(&cp, " "); /* gen */
	if (cp == NULL || *gen == '\0')
		goto truncated;
	prime = strsep(&cp, " "); /* prime */
	if (cp != NULL || *prime == '\0') {
 truncated:
		error("moduli:%d: truncated", linenum);
		goto fail;
	}

	if ((dhg->g = BN_new()) == NULL ||
	    (dhg->p = BN_new()) == NULL) {
		error("parse_prime: BN_new failed");
		goto fail;
	}
	if (BN_hex2bn(&dhg->g, gen) == 0) {
		error("moduli:%d: could not parse generator value", linenum);
		goto fail;
	}
	if (BN_hex2bn(&dhg->p, prime) == 0) {
		error("moduli:%d: could not parse prime value", linenum);
		goto fail;
	}
	if (BN_num_bits(dhg->p) != dhg->size) {
		error("moduli:%d: prime has wrong size: actual %d listed %d",
		    linenum, BN_num_bits(dhg->p), dhg->size - 1);
		goto fail;
	}
	if (BN_cmp(dhg->g, BN_value_one()) <= 0) {
		error("moduli:%d: generator is invalid", linenum);
		goto fail;
	}
	return 1;

 fail:
	BN_clear_free(dhg->g);
	BN_clear_free(dhg->p);
	dhg->g = dhg->p = NULL;
	return 0;
}

DH *
choose_dh(int min, int wantbits, int max)
{
	FILE *f;
	char line[4096];
	int best, bestcount, which;
	int linenum;
	struct dhgroup dhg;

	if ((f = fopen(_PATH_DH_MODULI, "r")) == NULL) {
		logit("WARNING: could not open %s (%s), using fixed modulus",
		    _PATH_DH_MODULI, strerror(errno));
		return (dh_new_group_fallback(max));
	}

	linenum = 0;
	best = bestcount = 0;
	while (fgets(line, sizeof(line), f)) {
		linenum++;
		if (!parse_prime(linenum, line, &dhg))
			continue;
		BN_clear_free(dhg.g);
		BN_clear_free(dhg.p);

		if (dhg.size > max || dhg.size < min)
			continue;

		if ((dhg.size > wantbits && dhg.size < best) ||
		    (dhg.size > best && best < wantbits)) {
			best = dhg.size;
			bestcount = 0;
		}
		if (dhg.size == best)
			bestcount++;
	}
	rewind(f);

	if (bestcount == 0) {
		fclose(f);
		logit("WARNING: no suitable primes in %s", _PATH_DH_MODULI);
		return (dh_new_group_fallback(max));
	}

	linenum = 0;
	which = arc4random_uniform(bestcount);
	while (fgets(line, sizeof(line), f)) {
		if (!parse_prime(linenum, line, &dhg))
			continue;
		if ((dhg.size > max || dhg.size < min) ||
		    dhg.size != best ||
		    linenum++ != which) {
			BN_clear_free(dhg.g);
			BN_clear_free(dhg.p);
			continue;
		}
		break;
	}
	fclose(f);
	if (linenum != which+1) {
		logit("WARNING: line %d disappeared in %s, giving up",
		    which, _PATH_DH_MODULI);
		return (dh_new_group_fallback(max));
	}

	return (dh_new_group(dhg.g, dhg.p));
}

/* diffie-hellman-groupN-sha1 */

int
#if OPENSSL_VERSION_NUMBER <= 0x10100000UL
dh_pub_is_valid(const DH *dh, const BIGNUM *dh_pub)
#else
dh_pub_is_valid(DH *dh, BIGNUM *dh_pub)
#endif
{
	int i;
	int n = BN_num_bits(dh_pub);
	int bits_set = 0;
	BIGNUM *tmp;
#if OPENSSL_VERSION_NUMBER >= 0x10100000UL
	const BIGNUM *p;
#endif
	if (BN_is_negative(dh_pub)) {
		logit("invalid public DH value: negative");
		return 0;
	}
	if (BN_cmp(dh_pub, BN_value_one()) != 1) {	/* pub_exp <= 1 */
		logit("invalid public DH value: <= 1");
		return 0;
	}

	if ((tmp = BN_new()) == NULL) {
		error("%s: BN_new failed", __func__);
		return 0;
	}
#if OPENSSL_VERSION_NUMBER >= 0x10100000UL
	DH_get0_pqg(dh, &p, NULL, NULL);
	if (!BN_sub(tmp, p, BN_value_one()) ||
	    BN_cmp(dh_pub, tmp) != -1) {		/* pub_exp > p-2 */
		BN_clear_free(tmp);
		logit("invalid public DH value: >= p-1");
		return 0;
	}
	BN_clear_free(tmp);

	for (i = 0; i <= n; i++)
		if (BN_is_bit_set(dh_pub, i))
			bits_set++;

	debug2("bits set: %d/%d", bits_set, BN_num_bits(p));

	/*
	 * if g==2 and bits_set==1 then computing log_g(dh_pub) is trivial
	 */
	if (bits_set < 4) {
		logit("invalid public DH value (%d/%d)",
		      bits_set, BN_num_bits(p));
	  return 0;
	}
#else
	if (!BN_sub(tmp, dh->p, BN_value_one()) ||
	    BN_cmp(dh_pub, tmp) != -1) {                /* pub_exp > p-2 */
	  BN_clear_free(tmp);
	  logit("invalid public DH value: >= p-1");
	  return 0;
	}
	BN_clear_free(tmp);

	for (i = 0; i <= n; i++)
	  if (BN_is_bit_set(dh_pub, i))
	    bits_set++;
	debug2("bits set: %d/%d", bits_set, BN_num_bits(dh->p));

	/*
         * if g==2 and bits_set==1 then computing log_g(dh_pub) is trivial
         */
	if (bits_set < 4) {
	  logit("invalid public DH value (%d/%d)",
		bits_set, BN_num_bits(dh->p));
	  return 0;
	}
#endif
	return 1;
}

int
dh_gen_key(DH *dh, int need)
{
	int pbits;
#if OPENSSL_VERSION_NUMBER >= 0x10100000UL
	const BIGNUM *p, *pub_key;
	BIGNUM *priv_key;

	DH_get0_pqg(dh, &p, NULL, NULL);
	if (need < 0 || p == NULL ||
	    (pbits = BN_num_bits(p)) <= 0 ||
	    need > INT_MAX / 2 || 2 * need > pbits)
#else
	if (need < 0 || dh->p == NULL ||
	    (pbits = BN_num_bits(dh->p)) <= 0 ||
	    need > INT_MAX / 2 || 2 * need > pbits)
#endif
		return SSH_ERR_INVALID_ARGUMENT;
	if (need < 256)
		need = 256;
	/*
	 * Pollard Rho, Big step/Little Step attacks are O(sqrt(n)),
	 * so double requested need here.
	 */
#if OPENSSL_VERSION_NUMBER >= 0x10100000UL
	DH_set_length(dh, MIN(need * 2, pbits - 1));
	if (DH_generate_key(dh) == 0) {
		return SSH_ERR_LIBCRYPTO_ERROR;
	}
	DH_get0_key(dh, &pub_key, &priv_key);
	if (!dh_pub_is_valid(dh, pub_key)) {
		BN_clear(priv_key);
		return SSH_ERR_LIBCRYPTO_ERROR;
	}
#else
	dh->length = MINIMUM(need * 2, pbits - 1);
	if (DH_generate_key(dh) == 0 ||
	    !dh_pub_is_valid(dh, dh->pub_key)) {
		BN_clear_free(dh->priv_key);
		return SSH_ERR_LIBCRYPTO_ERROR;
	}
#endif
	return 0;
}

DH *
dh_new_group_asc(const char *gen, const char *modulus)
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000UL
	DH *dh = NULL;
	BIGNUM *p=NULL, *g=NULL;

	if ((dh = DH_new()) == NULL ||
	    (p = BN_new()) == NULL ||
	    (g = BN_new()) == NULL)
		goto null;
	if (BN_hex2bn(&p, modulus) == 0 ||
	    BN_hex2bn(&g, gen) == 0) {
		goto null;
	}
	if (DH_set0_pqg(dh, p, NULL, g) == 0) {
		goto null;
	}
	p = g = NULL;
	return (dh);
null:
	BN_free(p);
	BN_free(g);
	DH_free(dh);
	return NULL;
#else
	DH *dh;
        if ((dh = DH_new()) == NULL)
		return NULL;
	if (BN_hex2bn(&dh->p, modulus) == 0 ||
	    BN_hex2bn(&dh->g, gen) == 0) {
		DH_free(dh);
		return NULL;
	}
	return (dh);
#endif
}

/*
 * This just returns the group, we still need to generate the exchange
 * value.
 */

DH *
dh_new_group(BIGNUM *gen, BIGNUM *modulus)
{
	DH *dh;

	if ((dh = DH_new()) == NULL)
		return NULL;
#if OPENSSL_VERSION_NUMBER >= 0x10100000UL
	if (DH_set0_pqg(dh, modulus, NULL, gen) == 0)
		return NULL;
#else
        dh->p = modulus;
	dh->g = gen;
#endif

	return (dh);
}

/* rfc2409 "Second Oakley Group" (1024 bits) */
DH *
dh_new_group1(void)
{
	static char *gen = "2", *group1 =
	    "FFFFFFFF" "FFFFFFFF" "C90FDAA2" "2168C234" "C4C6628B" "80DC1CD1"
	    "29024E08" "8A67CC74" "020BBEA6" "3B139B22" "514A0879" "8E3404DD"
	    "EF9519B3" "CD3A431B" "302B0A6D" "F25F1437" "4FE1356D" "6D51C245"
	    "E485B576" "625E7EC6" "F44C42E9" "A637ED6B" "0BFF5CB6" "F406B7ED"
	    "EE386BFB" "5A899FA5" "AE9F2411" "7C4B1FE6" "49286651" "ECE65381"
	    "FFFFFFFF" "FFFFFFFF";

	return (dh_new_group_asc(gen, group1));
}

/* rfc3526 group 14 "2048-bit MODP Group" */
DH *
dh_new_group14(void)
{
	static char *gen = "2", *group14 =
	    "FFFFFFFF" "FFFFFFFF" "C90FDAA2" "2168C234" "C4C6628B" "80DC1CD1"
	    "29024E08" "8A67CC74" "020BBEA6" "3B139B22" "514A0879" "8E3404DD"
	    "EF9519B3" "CD3A431B" "302B0A6D" "F25F1437" "4FE1356D" "6D51C245"
	    "E485B576" "625E7EC6" "F44C42E9" "A637ED6B" "0BFF5CB6" "F406B7ED"
	    "EE386BFB" "5A899FA5" "AE9F2411" "7C4B1FE6" "49286651" "ECE45B3D"
	    "C2007CB8" "A163BF05" "98DA4836" "1C55D39A" "69163FA8" "FD24CF5F"
	    "83655D23" "DCA3AD96" "1C62F356" "208552BB" "9ED52907" "7096966D"
	    "670C354E" "4ABC9804" "F1746C08" "CA18217C" "32905E46" "2E36CE3B"
	    "E39E772C" "180E8603" "9B2783A2" "EC07A28F" "B5C55DF0" "6F4C52C9"
	    "DE2BCBF6" "95581718" "3995497C" "EA956AE5" "15D22618" "98FA0510"
	    "15728E5A" "8AACAA68" "FFFFFFFF" "FFFFFFFF";

	return (dh_new_group_asc(gen, group14));
}

/* rfc3526 group 16 "4096-bit MODP Group" */
DH *
dh_new_group16(void)
{
	static char *gen = "2", *group16 =
	    "FFFFFFFF" "FFFFFFFF" "C90FDAA2" "2168C234" "C4C6628B" "80DC1CD1"
	    "29024E08" "8A67CC74" "020BBEA6" "3B139B22" "514A0879" "8E3404DD"
	    "EF9519B3" "CD3A431B" "302B0A6D" "F25F1437" "4FE1356D" "6D51C245"
	    "E485B576" "625E7EC6" "F44C42E9" "A637ED6B" "0BFF5CB6" "F406B7ED"
	    "EE386BFB" "5A899FA5" "AE9F2411" "7C4B1FE6" "49286651" "ECE45B3D"
	    "C2007CB8" "A163BF05" "98DA4836" "1C55D39A" "69163FA8" "FD24CF5F"
	    "83655D23" "DCA3AD96" "1C62F356" "208552BB" "9ED52907" "7096966D"
	    "670C354E" "4ABC9804" "F1746C08" "CA18217C" "32905E46" "2E36CE3B"
	    "E39E772C" "180E8603" "9B2783A2" "EC07A28F" "B5C55DF0" "6F4C52C9"
	    "DE2BCBF6" "95581718" "3995497C" "EA956AE5" "15D22618" "98FA0510"
	    "15728E5A" "8AAAC42D" "AD33170D" "04507A33" "A85521AB" "DF1CBA64"
	    "ECFB8504" "58DBEF0A" "8AEA7157" "5D060C7D" "B3970F85" "A6E1E4C7"
	    "ABF5AE8C" "DB0933D7" "1E8C94E0" "4A25619D" "CEE3D226" "1AD2EE6B"
	    "F12FFA06" "D98A0864" "D8760273" "3EC86A64" "521F2B18" "177B200C"
	    "BBE11757" "7A615D6C" "770988C0" "BAD946E2" "08E24FA0" "74E5AB31"
	    "43DB5BFC" "E0FD108E" "4B82D120" "A9210801" "1A723C12" "A787E6D7"
	    "88719A10" "BDBA5B26" "99C32718" "6AF4E23C" "1A946834" "B6150BDA"
	    "2583E9CA" "2AD44CE8" "DBBBC2DB" "04DE8EF9" "2E8EFC14" "1FBECAA6"
	    "287C5947" "4E6BC05D" "99B2964F" "A090C3A2" "233BA186" "515BE7ED"
	    "1F612970" "CEE2D7AF" "B81BDD76" "2170481C" "D0069127" "D5B05AA9"
	    "93B4EA98" "8D8FDDC1" "86FFB7DC" "90A6C08F" "4DF435C9" "34063199"
	    "FFFFFFFF" "FFFFFFFF";

	return (dh_new_group_asc(gen, group16));
}

/* rfc3526 group 18 "8192-bit MODP Group" */
DH *
dh_new_group18(void)
{
	static char *gen = "2", *group16 =
	    "FFFFFFFF" "FFFFFFFF" "C90FDAA2" "2168C234" "C4C6628B" "80DC1CD1"
	    "29024E08" "8A67CC74" "020BBEA6" "3B139B22" "514A0879" "8E3404DD"
	    "EF9519B3" "CD3A431B" "302B0A6D" "F25F1437" "4FE1356D" "6D51C245"
	    "E485B576" "625E7EC6" "F44C42E9" "A637ED6B" "0BFF5CB6" "F406B7ED"
	    "EE386BFB" "5A899FA5" "AE9F2411" "7C4B1FE6" "49286651" "ECE45B3D"
	    "C2007CB8" "A163BF05" "98DA4836" "1C55D39A" "69163FA8" "FD24CF5F"
	    "83655D23" "DCA3AD96" "1C62F356" "208552BB" "9ED52907" "7096966D"
	    "670C354E" "4ABC9804" "F1746C08" "CA18217C" "32905E46" "2E36CE3B"
	    "E39E772C" "180E8603" "9B2783A2" "EC07A28F" "B5C55DF0" "6F4C52C9"
	    "DE2BCBF6" "95581718" "3995497C" "EA956AE5" "15D22618" "98FA0510"
	    "15728E5A" "8AAAC42D" "AD33170D" "04507A33" "A85521AB" "DF1CBA64"
	    "ECFB8504" "58DBEF0A" "8AEA7157" "5D060C7D" "B3970F85" "A6E1E4C7"
	    "ABF5AE8C" "DB0933D7" "1E8C94E0" "4A25619D" "CEE3D226" "1AD2EE6B"
	    "F12FFA06" "D98A0864" "D8760273" "3EC86A64" "521F2B18" "177B200C"
	    "BBE11757" "7A615D6C" "770988C0" "BAD946E2" "08E24FA0" "74E5AB31"
	    "43DB5BFC" "E0FD108E" "4B82D120" "A9210801" "1A723C12" "A787E6D7"
	    "88719A10" "BDBA5B26" "99C32718" "6AF4E23C" "1A946834" "B6150BDA"
	    "2583E9CA" "2AD44CE8" "DBBBC2DB" "04DE8EF9" "2E8EFC14" "1FBECAA6"
	    "287C5947" "4E6BC05D" "99B2964F" "A090C3A2" "233BA186" "515BE7ED"
	    "1F612970" "CEE2D7AF" "B81BDD76" "2170481C" "D0069127" "D5B05AA9"
	    "93B4EA98" "8D8FDDC1" "86FFB7DC" "90A6C08F" "4DF435C9" "34028492"
	    "36C3FAB4" "D27C7026" "C1D4DCB2" "602646DE" "C9751E76" "3DBA37BD"
	    "F8FF9406" "AD9E530E" "E5DB382F" "413001AE" "B06A53ED" "9027D831"
	    "179727B0" "865A8918" "DA3EDBEB" "CF9B14ED" "44CE6CBA" "CED4BB1B"
	    "DB7F1447" "E6CC254B" "33205151" "2BD7AF42" "6FB8F401" "378CD2BF"
	    "5983CA01" "C64B92EC" "F032EA15" "D1721D03" "F482D7CE" "6E74FEF6"
	    "D55E702F" "46980C82" "B5A84031" "900B1C9E" "59E7C97F" "BEC7E8F3"
	    "23A97A7E" "36CC88BE" "0F1D45B7" "FF585AC5" "4BD407B2" "2B4154AA"
	    "CC8F6D7E" "BF48E1D8" "14CC5ED2" "0F8037E0" "A79715EE" "F29BE328"
	    "06A1D58B" "B7C5DA76" "F550AA3D" "8A1FBFF0" "EB19CCB1" "A313D55C"
	    "DA56C9EC" "2EF29632" "387FE8D7" "6E3C0468" "043E8F66" "3F4860EE"
	    "12BF2D5B" "0B7474D6" "E694F91E" "6DBE1159" "74A3926F" "12FEE5E4"
	    "38777CB6" "A932DF8C" "D8BEC4D0" "73B931BA" "3BC832B6" "8D9DD300"
	    "741FA7BF" "8AFC47ED" "2576F693" "6BA42466" "3AAB639C" "5AE4F568"
	    "3423B474" "2BF1C978" "238F16CB" "E39D652D" "E3FDB8BE" "FC848AD9"
	    "22222E04" "A4037C07" "13EB57A8" "1A23F0C7" "3473FC64" "6CEA306B"
	    "4BCBC886" "2F8385DD" "FA9D4B7F" "A2C087E8" "79683303" "ED5BDD3A"
	    "062B3CF5" "B3A278A6" "6D2A13F8" "3F44F82D" "DF310EE0" "74AB6A36"
	    "4597E899" "A0255DC1" "64F31CC5" "0846851D" "F9AB4819" "5DED7EA1"
	    "B1D510BD" "7EE74D73" "FAF36BC3" "1ECFA268" "359046F4" "EB879F92"
	    "4009438B" "481C6CD7" "889A002E" "D5EE382B" "C9190DA6" "FC026E47"
	    "9558E447" "5677E9AA" "9E3050E2" "765694DF" "C81F56E8" "80B96E71"
	    "60C980DD" "98EDD3DF" "FFFFFFFF" "FFFFFFFF";

	return (dh_new_group_asc(gen, group16));
}

/* Select fallback group used by DH-GEX if moduli file cannot be read. */
DH *
dh_new_group_fallback(int max)
{
	debug3("%s: requested max size %d", __func__, max);
	if (max < 3072) {
		debug3("using 2k bit group 14");
		return dh_new_group14();
	} else if (max < 6144) {
		debug3("using 4k bit group 16");
		return dh_new_group16();
	}
	debug3("using 8k bit group 18");
	return dh_new_group18();
}

/*
 * Estimates the group order for a Diffie-Hellman group that has an
 * attack complexity approximately the same as O(2**bits).
 * Values from NIST Special Publication 800-57: Recommendation for Key
 * Management Part 1 (rev 3) limited by the recommended maximum value
 * from RFC4419 section 3.
 */
u_int
dh_estimate(int bits)
{
	if (bits <= 112)
		return 2048;
	if (bits <= 128)
		return 3072;
	if (bits <= 192)
		return 7680;
	return 8192;
}

#endif /* WITH_OPENSSL */

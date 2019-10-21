/**********************************************************************
 * Copyright (c) 2013-2015 Pieter Wuille                              *
 * Distributed under the MIT software license, see the accompanying   *
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.*
 **********************************************************************/


#ifndef SECP256K1_ECDSA_IMPL_H
#define SECP256K1_ECDSA_IMPL_H

#include "scalar.h"
#include "field.h"
#include "group.h"
#include "ecmult.h"
#include "ecmult_gen.h"
#include "ecdsa.h"

/** Group order for secp256k1 defined as 'n' in "Standards for Efficient Cryptography" (SEC2) 2.7.1
 *  sage: for t in xrange(1023, -1, -1):
 *     ..   p = 2**256 - 2**32 - t
 *     ..   if p.is_prime():
 *     ..     print '%x'%p
 *     ..     break
 *   'fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f'
 *  sage: a = 0
 *  sage: b = 7
 *  sage: F = FiniteField (p)
 *  sage: '%x' % (EllipticCurve ([F (a), F (b)]).order())
 *   'fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141'
 */
static const rustsecp256k1_v0_1_0_fe rustsecp256k1_v0_1_0_ecdsa_const_order_as_fe = SECP256K1_FE_CONST(
    0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFEUL,
    0xBAAEDCE6UL, 0xAF48A03BUL, 0xBFD25E8CUL, 0xD0364141UL
);

/** Difference between field and order, values 'p' and 'n' values defined in
 *  "Standards for Efficient Cryptography" (SEC2) 2.7.1.
 *  sage: p = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
 *  sage: a = 0
 *  sage: b = 7
 *  sage: F = FiniteField (p)
 *  sage: '%x' % (p - EllipticCurve ([F (a), F (b)]).order())
 *   '14551231950b75fc4402da1722fc9baee'
 */
static const rustsecp256k1_v0_1_0_fe rustsecp256k1_v0_1_0_ecdsa_const_p_minus_order = SECP256K1_FE_CONST(
    0, 0, 0, 1, 0x45512319UL, 0x50B75FC4UL, 0x402DA172UL, 0x2FC9BAEEUL
);

static int rustsecp256k1_v0_1_0_der_read_len(const unsigned char **sigp, const unsigned char *sigend) {
    int lenleft, b1;
    size_t ret = 0;
    if (*sigp >= sigend) {
        return -1;
    }
    b1 = *((*sigp)++);
    if (b1 == 0xFF) {
        /* X.690-0207 8.1.3.5.c the value 0xFF shall not be used. */
        return -1;
    }
    if ((b1 & 0x80) == 0) {
        /* X.690-0207 8.1.3.4 short form length octets */
        return b1;
    }
    if (b1 == 0x80) {
        /* Indefinite length is not allowed in DER. */
        return -1;
    }
    /* X.690-207 8.1.3.5 long form length octets */
    lenleft = b1 & 0x7F;
    if (lenleft > sigend - *sigp) {
        return -1;
    }
    if (**sigp == 0) {
        /* Not the shortest possible length encoding. */
        return -1;
    }
    if ((size_t)lenleft > sizeof(size_t)) {
        /* The resulting length would exceed the range of a size_t, so
         * certainly longer than the passed array size.
         */
        return -1;
    }
    while (lenleft > 0) {
        ret = (ret << 8) | **sigp;
        if (ret + lenleft > (size_t)(sigend - *sigp)) {
            /* Result exceeds the length of the passed array. */
            return -1;
        }
        (*sigp)++;
        lenleft--;
    }
    if (ret < 128) {
        /* Not the shortest possible length encoding. */
        return -1;
    }
    return ret;
}

static int rustsecp256k1_v0_1_0_der_parse_integer(rustsecp256k1_v0_1_0_scalar *r, const unsigned char **sig, const unsigned char *sigend) {
    int overflow = 0;
    unsigned char ra[32] = {0};
    int rlen;

    if (*sig == sigend || **sig != 0x02) {
        /* Not a primitive integer (X.690-0207 8.3.1). */
        return 0;
    }
    (*sig)++;
    rlen = rustsecp256k1_v0_1_0_der_read_len(sig, sigend);
    if (rlen <= 0 || (*sig) + rlen > sigend) {
        /* Exceeds bounds or not at least length 1 (X.690-0207 8.3.1).  */
        return 0;
    }
    if (**sig == 0x00 && rlen > 1 && (((*sig)[1]) & 0x80) == 0x00) {
        /* Excessive 0x00 padding. */
        return 0;
    }
    if (**sig == 0xFF && rlen > 1 && (((*sig)[1]) & 0x80) == 0x80) {
        /* Excessive 0xFF padding. */
        return 0;
    }
    if ((**sig & 0x80) == 0x80) {
        /* Negative. */
        overflow = 1;
    }
    while (rlen > 0 && **sig == 0) {
        /* Skip leading zero bytes */
        rlen--;
        (*sig)++;
    }
    if (rlen > 32) {
        overflow = 1;
    }
    if (!overflow) {
        memcpy(ra + 32 - rlen, *sig, rlen);
        rustsecp256k1_v0_1_0_scalar_set_b32(r, ra, &overflow);
    }
    if (overflow) {
        rustsecp256k1_v0_1_0_scalar_set_int(r, 0);
    }
    (*sig) += rlen;
    return 1;
}

static int rustsecp256k1_v0_1_0_ecdsa_sig_parse(rustsecp256k1_v0_1_0_scalar *rr, rustsecp256k1_v0_1_0_scalar *rs, const unsigned char *sig, size_t size) {
    const unsigned char *sigend = sig + size;
    int rlen;
    if (sig == sigend || *(sig++) != 0x30) {
        /* The encoding doesn't start with a constructed sequence (X.690-0207 8.9.1). */
        return 0;
    }
    rlen = rustsecp256k1_v0_1_0_der_read_len(&sig, sigend);
    if (rlen < 0 || sig + rlen > sigend) {
        /* Tuple exceeds bounds */
        return 0;
    }
    if (sig + rlen != sigend) {
        /* Garbage after tuple. */
        return 0;
    }

    if (!rustsecp256k1_v0_1_0_der_parse_integer(rr, &sig, sigend)) {
        return 0;
    }
    if (!rustsecp256k1_v0_1_0_der_parse_integer(rs, &sig, sigend)) {
        return 0;
    }

    if (sig != sigend) {
        /* Trailing garbage inside tuple. */
        return 0;
    }

    return 1;
}

static int rustsecp256k1_v0_1_0_ecdsa_sig_serialize(unsigned char *sig, size_t *size, const rustsecp256k1_v0_1_0_scalar* ar, const rustsecp256k1_v0_1_0_scalar* as) {
    unsigned char r[33] = {0}, s[33] = {0};
    unsigned char *rp = r, *sp = s;
    size_t lenR = 33, lenS = 33;
    rustsecp256k1_v0_1_0_scalar_get_b32(&r[1], ar);
    rustsecp256k1_v0_1_0_scalar_get_b32(&s[1], as);
    while (lenR > 1 && rp[0] == 0 && rp[1] < 0x80) { lenR--; rp++; }
    while (lenS > 1 && sp[0] == 0 && sp[1] < 0x80) { lenS--; sp++; }
    if (*size < 6+lenS+lenR) {
        *size = 6 + lenS + lenR;
        return 0;
    }
    *size = 6 + lenS + lenR;
    sig[0] = 0x30;
    sig[1] = 4 + lenS + lenR;
    sig[2] = 0x02;
    sig[3] = lenR;
    memcpy(sig+4, rp, lenR);
    sig[4+lenR] = 0x02;
    sig[5+lenR] = lenS;
    memcpy(sig+lenR+6, sp, lenS);
    return 1;
}

static int rustsecp256k1_v0_1_0_ecdsa_sig_verify(const rustsecp256k1_v0_1_0_ecmult_context *ctx, const rustsecp256k1_v0_1_0_scalar *sigr, const rustsecp256k1_v0_1_0_scalar *sigs, const rustsecp256k1_v0_1_0_ge *pubkey, const rustsecp256k1_v0_1_0_scalar *message) {
    unsigned char c[32];
    rustsecp256k1_v0_1_0_scalar sn, u1, u2;
#if !defined(EXHAUSTIVE_TEST_ORDER)
    rustsecp256k1_v0_1_0_fe xr;
#endif
    rustsecp256k1_v0_1_0_gej pubkeyj;
    rustsecp256k1_v0_1_0_gej pr;

    if (rustsecp256k1_v0_1_0_scalar_is_zero(sigr) || rustsecp256k1_v0_1_0_scalar_is_zero(sigs)) {
        return 0;
    }

    rustsecp256k1_v0_1_0_scalar_inverse_var(&sn, sigs);
    rustsecp256k1_v0_1_0_scalar_mul(&u1, &sn, message);
    rustsecp256k1_v0_1_0_scalar_mul(&u2, &sn, sigr);
    rustsecp256k1_v0_1_0_gej_set_ge(&pubkeyj, pubkey);
    rustsecp256k1_v0_1_0_ecmult(ctx, &pr, &pubkeyj, &u2, &u1);
    if (rustsecp256k1_v0_1_0_gej_is_infinity(&pr)) {
        return 0;
    }

#if defined(EXHAUSTIVE_TEST_ORDER)
{
    rustsecp256k1_v0_1_0_scalar computed_r;
    rustsecp256k1_v0_1_0_ge pr_ge;
    rustsecp256k1_v0_1_0_ge_set_gej(&pr_ge, &pr);
    rustsecp256k1_v0_1_0_fe_normalize(&pr_ge.x);

    rustsecp256k1_v0_1_0_fe_get_b32(c, &pr_ge.x);
    rustsecp256k1_v0_1_0_scalar_set_b32(&computed_r, c, NULL);
    return rustsecp256k1_v0_1_0_scalar_eq(sigr, &computed_r);
}
#else
    rustsecp256k1_v0_1_0_scalar_get_b32(c, sigr);
    rustsecp256k1_v0_1_0_fe_set_b32(&xr, c);

    /** We now have the recomputed R point in pr, and its claimed x coordinate (modulo n)
     *  in xr. Naively, we would extract the x coordinate from pr (requiring a inversion modulo p),
     *  compute the remainder modulo n, and compare it to xr. However:
     *
     *        xr == X(pr) mod n
     *    <=> exists h. (xr + h * n < p && xr + h * n == X(pr))
     *    [Since 2 * n > p, h can only be 0 or 1]
     *    <=> (xr == X(pr)) || (xr + n < p && xr + n == X(pr))
     *    [In Jacobian coordinates, X(pr) is pr.x / pr.z^2 mod p]
     *    <=> (xr == pr.x / pr.z^2 mod p) || (xr + n < p && xr + n == pr.x / pr.z^2 mod p)
     *    [Multiplying both sides of the equations by pr.z^2 mod p]
     *    <=> (xr * pr.z^2 mod p == pr.x) || (xr + n < p && (xr + n) * pr.z^2 mod p == pr.x)
     *
     *  Thus, we can avoid the inversion, but we have to check both cases separately.
     *  rustsecp256k1_v0_1_0_gej_eq_x implements the (xr * pr.z^2 mod p == pr.x) test.
     */
    if (rustsecp256k1_v0_1_0_gej_eq_x_var(&xr, &pr)) {
        /* xr * pr.z^2 mod p == pr.x, so the signature is valid. */
        return 1;
    }
    if (rustsecp256k1_v0_1_0_fe_cmp_var(&xr, &rustsecp256k1_v0_1_0_ecdsa_const_p_minus_order) >= 0) {
        /* xr + n >= p, so we can skip testing the second case. */
        return 0;
    }
    rustsecp256k1_v0_1_0_fe_add(&xr, &rustsecp256k1_v0_1_0_ecdsa_const_order_as_fe);
    if (rustsecp256k1_v0_1_0_gej_eq_x_var(&xr, &pr)) {
        /* (xr + n) * pr.z^2 mod p == pr.x, so the signature is valid. */
        return 1;
    }
    return 0;
#endif
}

static int rustsecp256k1_v0_1_0_ecdsa_sig_sign(const rustsecp256k1_v0_1_0_ecmult_gen_context *ctx, rustsecp256k1_v0_1_0_scalar *sigr, rustsecp256k1_v0_1_0_scalar *sigs, const rustsecp256k1_v0_1_0_scalar *seckey, const rustsecp256k1_v0_1_0_scalar *message, const rustsecp256k1_v0_1_0_scalar *nonce, int *recid) {
    unsigned char b[32];
    rustsecp256k1_v0_1_0_gej rp;
    rustsecp256k1_v0_1_0_ge r;
    rustsecp256k1_v0_1_0_scalar n;
    int overflow = 0;

    rustsecp256k1_v0_1_0_ecmult_gen(ctx, &rp, nonce);
    rustsecp256k1_v0_1_0_ge_set_gej(&r, &rp);
    rustsecp256k1_v0_1_0_fe_normalize(&r.x);
    rustsecp256k1_v0_1_0_fe_normalize(&r.y);
    rustsecp256k1_v0_1_0_fe_get_b32(b, &r.x);
    rustsecp256k1_v0_1_0_scalar_set_b32(sigr, b, &overflow);
    /* These two conditions should be checked before calling */
    VERIFY_CHECK(!rustsecp256k1_v0_1_0_scalar_is_zero(sigr));
    VERIFY_CHECK(overflow == 0);

    if (recid) {
        /* The overflow condition is cryptographically unreachable as hitting it requires finding the discrete log
         * of some P where P.x >= order, and only 1 in about 2^127 points meet this criteria.
         */
        *recid = (overflow ? 2 : 0) | (rustsecp256k1_v0_1_0_fe_is_odd(&r.y) ? 1 : 0);
    }
    rustsecp256k1_v0_1_0_scalar_mul(&n, sigr, seckey);
    rustsecp256k1_v0_1_0_scalar_add(&n, &n, message);
    rustsecp256k1_v0_1_0_scalar_inverse(sigs, nonce);
    rustsecp256k1_v0_1_0_scalar_mul(sigs, sigs, &n);
    rustsecp256k1_v0_1_0_scalar_clear(&n);
    rustsecp256k1_v0_1_0_gej_clear(&rp);
    rustsecp256k1_v0_1_0_ge_clear(&r);
    if (rustsecp256k1_v0_1_0_scalar_is_zero(sigs)) {
        return 0;
    }
    if (rustsecp256k1_v0_1_0_scalar_is_high(sigs)) {
        rustsecp256k1_v0_1_0_scalar_negate(sigs, sigs);
        if (recid) {
            *recid ^= 1;
        }
    }
    return 1;
}

#endif /* SECP256K1_ECDSA_IMPL_H */

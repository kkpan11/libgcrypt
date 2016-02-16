/*
 * DRBG: Deterministic Random Bits Generator
 *       Based on NIST Recommended DRBG from NIST SP800-90A with the following
 *       properties:
 *		* CTR DRBG with DF with AES-128, AES-192, AES-256 cores
 * 		* Hash DRBG with DF with SHA-1, SHA-256, SHA-384, SHA-512 cores
 * 		* HMAC DRBG with DF with SHA-1, SHA-256, SHA-384, SHA-512 cores
 * 		* with and without prediction resistance
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * LGPLv2+, in which case the provisions of the LGPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the LGPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 *
 * gcry_control GCRYCTL_DRBG_REINIT
 * ================================
 * This control request re-initializes the DRBG completely, i.e. the entire
 * state of the DRBG is zeroized (with two exceptions listed in
 * GCRYCTL_DRBG_SET_ENTROPY).
 *
 * The control request takes the following values which influences how the DRBG
 * is re-initialized:
 *   * u32 flags: This variable specifies the DRBG type to be used for the
 *		    next initialization. If set to 0, the previous DRBG type is
 * 		    used for the initialization. The DRBG type is an OR of the
 * 		    mandatory flags of the requested DRBG strength and DRBG
 * 		    cipher type. Optionally, the prediction resistance flag
 * 		    can be ORed into the flags variable. For example:
 * 		    - CTR-DRBG with AES-128 without prediction resistance:
 * 			DRBG_CTRAES128
 * 		    - HMAC-DRBG with SHA-512 with prediction resistance:
 * 			DRBG_HMACSHA512 | DRBG_PREDICTION_RESIST
 *   * struct gcry_drbg_string *pers: personalization string to be used for
 *			   	 initialization.
 * The variable of flags is independent from the pers/perslen variables. If
 * flags is set to 0 and perslen is set to 0, the current DRBG type is
 * completely reset without using a personalization string.
 *
 * DRBG Usage
 * ==========
 * The SP 800-90A DRBG allows the user to specify a personalization string
 * for initialization as well as an additional information string for each
 * random number request. The following code fragments show how a caller
 * uses the kernel crypto API to use the full functionality of the DRBG.
 *
 * Usage without any additional data
 * ---------------------------------
 * gcry_randomize(outbuf, OUTLEN, GCRY_STRONG_RANDOM);
 *
 *
 * Usage with personalization string during initialization
 * -------------------------------------------------------
 * struct gcry_drbg_string pers;
 * char personalization[11] = "some-string";
 *
 * gcry_drbg_string_fill(&pers, personalization, strlen(personalization));
 * // The reset completely re-initializes the DRBG with the provided
 * // personalization string without changing the DRBG type
 * ret = gcry_control(GCRYCTL_DRBG_REINIT, 0, &pers);
 * gcry_randomize(outbuf, OUTLEN, GCRY_STRONG_RANDOM);
 *
 *
 * Usage with additional information string during random number request
 * ---------------------------------------------------------------------
 * struct gcry_drbg_string addtl;
 * char addtl_string[11] = "some-string";
 *
 * gcry_drbg_string_fill(&addtl, addtl_string, strlen(addtl_string));
 * // The following call is a wrapper to gcry_randomize() and returns
 * // the same error codes.
 * gcry_randomize_drbg(outbuf, OUTLEN, GCRY_STRONG_RANDOM, &addtl);
 *
 *
 * Usage with personalization and additional information strings
 * -------------------------------------------------------------
 * Just mix both scenarios above.
 *
 *
 * Switch the DRBG type to some other type
 * ---------------------------------------
 * // Switch to CTR DRBG AES-128 without prediction resistance
 * ret = gcry_control(GCRYCTL_DRBG_REINIT, DRBG_NOPR_CTRAES128, NULL);
 * gcry_randomize(outbuf, OUTLEN, GCRY_STRONG_RANDOM);
 */

#include <sys/types.h>
#include <asm/types.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <config.h>

#include "g10lib.h"
#include "random.h"
#include "rand-internal.h"
#include "../cipher/bithelp.h"

/******************************************************************
 * Common data structures
 ******************************************************************/

struct gcry_drbg_state;

struct gcry_drbg_core
{
  u32 flags;			/* flags for the cipher */
  ushort statelen;		/* maximum state length */
  ushort blocklen_bytes;	/* block size of output in bytes */
  int backend_cipher;		/* libgcrypt backend cipher */
};

struct gcry_drbg_state_ops
{
  gpg_err_code_t (*update) (struct gcry_drbg_state * drbg,
			    struct gcry_drbg_string * seed, int reseed);
  gpg_err_code_t (*generate) (struct gcry_drbg_state * drbg,
			      unsigned char *buf, unsigned int buflen,
			      struct gcry_drbg_string * addtl);
};

/* DRBG test data */
struct gcry_drbg_test_data
{
  struct gcry_drbg_string *testentropy;	/* TEST PARAMETER: test entropy */
  int fail_seed_source:1;	/* if set, the seed function will return an error */
};


struct gcry_drbg_state
{
  unsigned char *V;		/* internal state 10.1.1.1 1a) */
  unsigned char *C;		/* hash: static value 10.1.1.1 1b)
				 * hmac / ctr: key */
  size_t reseed_ctr;		/* Number of RNG requests since last reseed --
				 * 10.1.1.1 1c) */
  unsigned char *scratchpad;	/* some memory the DRBG can use for its
				 * operation -- allocated during init */
  int seeded:1;			/* DRBG fully seeded? */
  int pr:1;			/* Prediction resistance enabled? */
  /* Taken from libgcrypt ANSI X9.31 DRNG: We need to keep track of the
   * process which did the initialization so that we can detect a fork.
   * The volatile modifier is required so that the compiler does not
   * optimize it away in case the getpid function is badly attributed. */
  pid_t seed_init_pid;
  const struct gcry_drbg_state_ops *d_ops;
  const struct gcry_drbg_core *core;
  struct gcry_drbg_test_data *test_data;
};

enum gcry_drbg_prefixes
{
  DRBG_PREFIX0 = 0x00,
  DRBG_PREFIX1,
  DRBG_PREFIX2,
  DRBG_PREFIX3
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/***************************************************************
 * Backend cipher definitions available to DRBG
 ***************************************************************/

static const struct gcry_drbg_core gcry_drbg_cores[] = {
  /* Hash DRBGs */
  {GCRY_DRBG_HASHSHA1, 55, 20, GCRY_MD_SHA1},
  {GCRY_DRBG_HASHSHA256, 55, 32, GCRY_MD_SHA256},
  {GCRY_DRBG_HASHSHA384, 111, 48, GCRY_MD_SHA384},
  {GCRY_DRBG_HASHSHA512, 111, 64, GCRY_MD_SHA512},
  /* HMAC DRBGs */
  {GCRY_DRBG_HASHSHA1 | GCRY_DRBG_HMAC, 20, 20, GCRY_MD_SHA1},
  {GCRY_DRBG_HASHSHA256 | GCRY_DRBG_HMAC, 32, 32, GCRY_MD_SHA256},
  {GCRY_DRBG_HASHSHA384 | GCRY_DRBG_HMAC, 48, 48, GCRY_MD_SHA384},
  {GCRY_DRBG_HASHSHA512 | GCRY_DRBG_HMAC, 64, 64, GCRY_MD_SHA512},
  /* block ciphers */
  {GCRY_DRBG_CTRAES | GCRY_DRBG_SYM128, 32, 16, GCRY_CIPHER_AES128},
  {GCRY_DRBG_CTRAES | GCRY_DRBG_SYM192, 40, 16, GCRY_CIPHER_AES192},
  {GCRY_DRBG_CTRAES | GCRY_DRBG_SYM256, 48, 16, GCRY_CIPHER_AES256},
};

static gpg_err_code_t gcry_drbg_sym (struct gcry_drbg_state *drbg,
				     const unsigned char *key,
				     unsigned char *outval,
				     const struct gcry_drbg_string *buf);
static gpg_err_code_t gcry_drbg_hmac (struct gcry_drbg_state *drbg,
				      const unsigned char *key,
				      unsigned char *outval,
				      const struct gcry_drbg_string *buf);

/******************************************************************
 ******************************************************************
 ******************************************************************
 * Generic DRBG code
 ******************************************************************
 ******************************************************************
 ******************************************************************/

/******************************************************************
 * Generic helper functions
 ******************************************************************/

#if 0
#define dbg(x) do { log_debug x; } while(0)
#else
#define dbg(x)
#endif

static inline ushort
gcry_drbg_statelen (struct gcry_drbg_state *drbg)
{
  if (drbg && drbg->core)
    return drbg->core->statelen;
  return 0;
}

static inline ushort
gcry_drbg_blocklen (struct gcry_drbg_state *drbg)
{
  if (drbg && drbg->core)
    return drbg->core->blocklen_bytes;
  return 0;
}

static inline ushort
gcry_drbg_keylen (struct gcry_drbg_state *drbg)
{
  if (drbg && drbg->core)
    return (drbg->core->statelen - drbg->core->blocklen_bytes);
  return 0;
}

static inline size_t
gcry_drbg_max_request_bytes (void)
{
  /* SP800-90A requires the limit 2**19 bits, but we return bytes */
  return (1 << 16);
}

static inline size_t
gcry_drbg_max_addtl (void)
{
  /* SP800-90A requires 2**35 bytes additional info str / pers str */
#ifdef __LP64__
  return (1UL << 35);
#else
  /*
   * SP800-90A allows smaller maximum numbers to be returned -- we
   * return SIZE_MAX - 1 to allow the verification of the enforcement
   * of this value in gcry_drbg_healthcheck_sanity.
   */
  return (SIZE_MAX - 1);
#endif
}

static inline size_t
gcry_drbg_max_requests (void)
{
  /* SP800-90A requires 2**48 maximum requests before reseeding */
#ifdef __LP64__
  return (1UL << 48);
#else
  return SIZE_MAX;
#endif
}

/*
 * Return strength of DRBG according to SP800-90A section 8.4
 *
 * flags: DRBG flags reference
 *
 * Return: normalized strength value or 32 as a default to counter
 * 	   programming errors
 */
static inline unsigned short
gcry_drbg_sec_strength (u32 flags)
{
  if ((flags & GCRY_DRBG_HASHSHA1) || (flags & GCRY_DRBG_SYM128))
    return 16;
  else if (flags & GCRY_DRBG_SYM192)
    return 24;
  else if ((flags & GCRY_DRBG_SYM256) || (flags & GCRY_DRBG_HASHSHA256) ||
	   (flags & GCRY_DRBG_HASHSHA384) || (flags & GCRY_DRBG_HASHSHA512))
    return 32;
  else
    return 32;
}

/*
 * Convert an integer into a byte representation of this integer.
 * The byte representation is big-endian
 *
 * @val value to be converted
 * @buf buffer holding the converted integer -- caller must ensure that
 *      buffer size is at least 32 bit
 */
static inline void
gcry_drbg_cpu_to_be32 (u32 val, unsigned char *buf)
{
  struct s
  {
    u32 conv;
  };
  struct s *conversion = (struct s *) buf;

  conversion->conv = be_bswap32 (val);
}

static void
gcry_drbg_add_buf (unsigned char *dst, size_t dstlen,
		   unsigned char *add, size_t addlen)
{
  /* implied: dstlen > addlen */
  unsigned char *dstptr, *addptr;
  unsigned int remainder = 0;
  size_t len = addlen;

  dstptr = dst + (dstlen - 1);
  addptr = add + (addlen - 1);
  while (len)
    {
      remainder += *dstptr + *addptr;
      *dstptr = remainder & 0xff;
      remainder >>= 8;
      len--;
      dstptr--;
      addptr--;
    }
  len = dstlen - addlen;
  while (len && remainder > 0)
    {
      remainder = *dstptr + 1;
      *dstptr = remainder & 0xff;
      remainder >>= 8;
      len--;
      dstptr--;
    }
}

/* Helper variables for read_cb().
 *
 *   The _gcry_rnd*_gather_random interface does not allow to provide a
 *   data pointer.  Thus we need to use a global variable for
 *   communication.  However, the then required locking is anyway a good
 *   idea because it does not make sense to have several readers of (say
 *   /dev/random).  It is easier to serve them one after the other.  */
static unsigned char *read_cb_buffer;	/* The buffer.  */
static size_t read_cb_size;	/* Size of the buffer.  */
static size_t read_cb_len;	/* Used length.  */

/* Callback for generating seed from kernel device. */
static void
gcry_drbg_read_cb (const void *buffer, size_t length,
		   enum random_origins origin)
{
  const unsigned char *p = buffer;

  (void) origin;
  gcry_assert (read_cb_buffer);

  /* Note that we need to protect against gatherers returning more
   * than the requested bytes (e.g. rndw32).  */
  while (length-- && read_cb_len < read_cb_size)
    read_cb_buffer[read_cb_len++] = *p++;
}

static inline int
gcry_drbg_get_entropy (struct gcry_drbg_state *drbg, unsigned char *buffer,
		       size_t len)
{
  int rc = 0;

  /* Perform testing as defined in 11.3.2 */
  if (drbg->test_data && drbg->test_data->fail_seed_source)
    return -1;

  read_cb_buffer = buffer;
  read_cb_size = len;
  read_cb_len = 0;
#if USE_RNDLINUX
  rc = _gcry_rndlinux_gather_random (gcry_drbg_read_cb, 0, len,
				     GCRY_VERY_STRONG_RANDOM);
#elif USE_RNDUNIX
  rc = _gcry_rndunix_gather_random (read_cb, 0, length,
				    GCRY_VERY_STRONG_RANDOM);
#elif USE_RNDW32
  do
    {
      rc = _gcry_rndw32_gather_random (read_cb, 0, length,
				       GCRY_VERY_STRONG_RANDOM);
    }
  while (rc >= 0 && read_cb_len < read_cb_size);
#else
  rc = -1;
#endif
  return rc;
}

/******************************************************************
 * CTR DRBG callback functions
 ******************************************************************/

/* BCC function for CTR DRBG as defined in 10.4.3 */
static gpg_err_code_t
gcry_drbg_ctr_bcc (struct gcry_drbg_state *drbg,
		   unsigned char *out, const unsigned char *key,
		   struct gcry_drbg_string *in)
{
  gpg_err_code_t ret = GPG_ERR_GENERAL;
  struct gcry_drbg_string *curr = in;
  size_t inpos = curr->len;
  const unsigned char *pos = curr->buf;
  struct gcry_drbg_string data;

  gcry_drbg_string_fill (&data, out, gcry_drbg_blocklen (drbg));

  /* 10.4.3 step 1 */
  memset (out, 0, gcry_drbg_blocklen (drbg));

  /* 10.4.3 step 2 / 4 */
  while (inpos)
    {
      short cnt = 0;
      /* 10.4.3 step 4.1 */
      for (cnt = 0; cnt < gcry_drbg_blocklen (drbg); cnt++)
	{
	  out[cnt] ^= *pos;
	  pos++;
	  inpos--;
	  /* the following branch implements the linked list
	   * iteration. If we are at the end of the current data
	   * set, we have to start using the next data set if
	   * available -- the inpos value always points to the
	   * current byte and will be zero if we have processed
	   * the last byte of the last linked list member */
	  if (0 == inpos)
	    {
	      curr = curr->next;
	      if (NULL != curr)
		{
		  pos = curr->buf;
		  inpos = curr->len;
		}
	      else
		{
		  inpos = 0;
		  break;
		}
	    }
	}
      /* 10.4.3 step 4.2 */
      ret = gcry_drbg_sym (drbg, key, out, &data);
      if (ret)
	return ret;
      /* 10.4.3 step 2 */
    }
  return 0;
}


/*
 * scratchpad usage: gcry_drbg_ctr_update is interlinked with gcry_drbg_ctr_df
 * (and gcry_drbg_ctr_bcc, but this function does not need any temporary buffers),
 * the scratchpad is used as follows:
 * gcry_drbg_ctr_update:
 *	temp
 *		start: drbg->scratchpad
 *		length: gcry_drbg_statelen(drbg) + gcry_drbg_blocklen(drbg)
 *			note: the cipher writing into this variable works
 *			blocklen-wise. Now, when the statelen is not a multiple
 *			of blocklen, the generateion loop below "spills over"
 *			by at most blocklen. Thus, we need to give sufficient
 *			memory.
 *	df_data
 *		start: drbg->scratchpad +
 *				gcry_drbg_statelen(drbg) +
 *				gcry_drbg_blocklen(drbg)
 *		length: gcry_drbg_statelen(drbg)
 *
 * gcry_drbg_ctr_df:
 *	pad
 *		start: df_data + gcry_drbg_statelen(drbg)
 *		length: gcry_drbg_blocklen(drbg)
 *	iv
 *		start: pad + gcry_drbg_blocklen(drbg)
 *		length: gcry_drbg_blocklen(drbg)
 *	temp
 *		start: iv + gcry_drbg_blocklen(drbg)
 *		length: gcry_drbg_satelen(drbg) + gcry_drbg_blocklen(drbg)
 *			note: temp is the buffer that the BCC function operates
 *			on. BCC operates blockwise. gcry_drbg_statelen(drbg)
 *			is sufficient when the DRBG state length is a multiple
 *			of the block size. For AES192 (and maybe other ciphers)
 *			this is not correct and the length for temp is
 *			insufficient (yes, that also means for such ciphers,
 *			the final output of all BCC rounds are truncated).
 *			Therefore, add gcry_drbg_blocklen(drbg) to cover all
 *			possibilities.
 */

/* Derivation Function for CTR DRBG as defined in 10.4.2 */
static gpg_err_code_t
gcry_drbg_ctr_df (struct gcry_drbg_state *drbg, unsigned char *df_data,
		  size_t bytes_to_return, struct gcry_drbg_string *addtl)
{
  gpg_err_code_t ret = GPG_ERR_GENERAL;
  unsigned char L_N[8];
  /* S3 is input */
  struct gcry_drbg_string S1, S2, S4, cipherin;
  struct gcry_drbg_string *tempstr = addtl;
  unsigned char *pad = df_data + gcry_drbg_statelen (drbg);
  unsigned char *iv = pad + gcry_drbg_blocklen (drbg);
  unsigned char *temp = iv + gcry_drbg_blocklen (drbg);
  size_t padlen = 0;
  unsigned int templen = 0;
  /* 10.4.2 step 7 */
  unsigned int i = 0;
  /* 10.4.2 step 8 */
  const unsigned char *K = (unsigned char *)
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
    "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f";
  unsigned char *X;
  size_t generated_len = 0;
  size_t inputlen = 0;

  memset (pad, 0, gcry_drbg_blocklen (drbg));
  memset (iv, 0, gcry_drbg_blocklen (drbg));
  memset (temp, 0, gcry_drbg_statelen (drbg));

  /* 10.4.2 step 1 is implicit as we work byte-wise */

  /* 10.4.2 step 2 */
  if ((512 / 8) < bytes_to_return)
    return GPG_ERR_INV_ARG;

  /* 10.4.2 step 2 -- calculate the entire length of all input data */
  for (; NULL != tempstr; tempstr = tempstr->next)
    inputlen += tempstr->len;
  gcry_drbg_cpu_to_be32 (inputlen, &L_N[0]);

  /* 10.4.2 step 3 */
  gcry_drbg_cpu_to_be32 (bytes_to_return, &L_N[4]);

  /* 10.4.2 step 5: length is size of L_N, input_string, one byte, padding */
  padlen = (inputlen + sizeof (L_N) + 1) % (gcry_drbg_blocklen (drbg));
  /* wrap the padlen appropriately */
  if (padlen)
    padlen = gcry_drbg_blocklen (drbg) - padlen;
  /* pad / padlen contains the 0x80 byte and the following zero bytes, so
   * add one for byte for 0x80 */
  padlen++;
  pad[0] = 0x80;

  /* 10.4.2 step 4 -- first fill the linked list and then order it */
  gcry_drbg_string_fill (&S1, iv, gcry_drbg_blocklen (drbg));
  gcry_drbg_string_fill (&S2, L_N, sizeof (L_N));
  gcry_drbg_string_fill (&S4, pad, padlen);
  S1.next = &S2;
  S2.next = addtl;

  /* Splice in addtl between S2 and S4 -- we place S4 at the end of the
   * input data chain. As this code is only triggered when addtl is not
   * NULL, no NULL checks are necessary.*/
  tempstr = addtl;
  while (tempstr->next)
    tempstr = tempstr->next;
  tempstr->next = &S4;

  /* 10.4.2 step 9 */
  while (templen < (gcry_drbg_keylen (drbg) + (gcry_drbg_blocklen (drbg))))
    {
      /* 10.4.2 step 9.1 - the padding is implicit as the buffer
       * holds zeros after allocation -- even the increment of i
       * is irrelevant as the increment remains within length of i */
      gcry_drbg_cpu_to_be32 (i, iv);
      /* 10.4.2 step 9.2 -- BCC and concatenation with temp */
      ret = gcry_drbg_ctr_bcc (drbg, temp + templen, K, &S1);
      if (ret)
	goto out;
      /* 10.4.2 step 9.3 */
      i++;
      templen += gcry_drbg_blocklen (drbg);
    }

  /* 10.4.2 step 11 */
  /* implicit key len with seedlen - blocklen according to table 3 */
  X = temp + (gcry_drbg_keylen (drbg));
  gcry_drbg_string_fill (&cipherin, X, gcry_drbg_blocklen (drbg));

  /* 10.4.2 step 12: overwriting of outval */

  /* 10.4.2 step 13 */
  while (generated_len < bytes_to_return)
    {
      short blocklen = 0;
      /* 10.4.2 step 13.1 */
      /* the truncation of the key length is implicit as the key
       * is only gcry_drbg_blocklen in size -- check for the implementation
       * of the cipher function callback */
      ret = gcry_drbg_sym (drbg, temp, X, &cipherin);
      if (ret)
	goto out;
      blocklen = (gcry_drbg_blocklen (drbg) <
		  (bytes_to_return - generated_len)) ?
	gcry_drbg_blocklen (drbg) : (bytes_to_return - generated_len);
      /* 10.4.2 step 13.2 and 14 */
      memcpy (df_data + generated_len, X, blocklen);
      generated_len += blocklen;
    }

  ret = 0;

out:
  memset (iv, 0, gcry_drbg_blocklen (drbg));
  memset (temp, 0, gcry_drbg_statelen (drbg));
  memset (pad, 0, gcry_drbg_blocklen (drbg));
  return ret;
}

/*
 * update function of CTR DRBG as defined in 10.2.1.2
 *
 * The reseed variable has an enhanced meaning compared to the update
 * functions of the other DRBGs as follows:
 * 0 => initial seed from initialization
 * 1 => reseed via gcry_drbg_seed
 * 2 => first invocation from gcry_drbg_ctr_update when addtl is present. In
 *      this case, the df_data scratchpad is not deleted so that it is
 *      available for another calls to prevent calling the DF function
 *      again.
 * 3 => second invocation from gcry_drbg_ctr_update. When the update function
 *      was called with addtl, the df_data memory already contains the
 *      DFed addtl information and we do not need to call DF again.
 */
static gpg_err_code_t
gcry_drbg_ctr_update (struct gcry_drbg_state *drbg,
		      struct gcry_drbg_string *addtl, int reseed)
{
  gpg_err_code_t ret = GPG_ERR_GENERAL;
  /* 10.2.1.2 step 1 */
  unsigned char *temp = drbg->scratchpad;
  unsigned char *df_data = drbg->scratchpad +
    gcry_drbg_statelen (drbg) + gcry_drbg_blocklen (drbg);
  unsigned char *temp_p, *df_data_p;	/* pointer to iterate over buffers */
  unsigned int len = 0;
  struct gcry_drbg_string cipherin;
  unsigned char prefix = DRBG_PREFIX1;

  memset (temp, 0, gcry_drbg_statelen (drbg) + gcry_drbg_blocklen (drbg));
  if (3 > reseed)
    memset (df_data, 0, gcry_drbg_statelen (drbg));

  /* 10.2.1.3.2 step 2 and 10.2.1.4.2 step 2 */
  /* TODO use reseed variable to avoid re-doing DF operation */
  (void) reseed;
  if (addtl && 0 < addtl->len)
    {
      ret =
	gcry_drbg_ctr_df (drbg, df_data, gcry_drbg_statelen (drbg), addtl);
      if (ret)
	goto out;
    }

  gcry_drbg_string_fill (&cipherin, drbg->V, gcry_drbg_blocklen (drbg));
  /* 10.2.1.3.2 step 2 and 3 -- are already covered as we memset(0)
   * all memory during initialization */
  while (len < (gcry_drbg_statelen (drbg)))
    {
      /* 10.2.1.2 step 2.1 */
      gcry_drbg_add_buf (drbg->V, gcry_drbg_blocklen (drbg), &prefix, 1);
      /* 10.2.1.2 step 2.2 */
      /* using target of temp + len: 10.2.1.2 step 2.3 and 3 */
      ret = gcry_drbg_sym (drbg, drbg->C, temp + len, &cipherin);
      if (ret)
	goto out;
      /* 10.2.1.2 step 2.3 and 3 */
      len += gcry_drbg_blocklen (drbg);
    }

  /* 10.2.1.2 step 4 */
  temp_p = temp;
  df_data_p = df_data;
  for (len = 0; len < gcry_drbg_statelen (drbg); len++)
    {
      *temp_p ^= *df_data_p;
      df_data_p++;
      temp_p++;
    }

  /* 10.2.1.2 step 5 */
  memcpy (drbg->C, temp, gcry_drbg_keylen (drbg));
  /* 10.2.1.2 step 6 */
  memcpy (drbg->V, temp + gcry_drbg_keylen (drbg), gcry_drbg_blocklen (drbg));
  ret = 0;

out:
  memset (temp, 0, gcry_drbg_statelen (drbg) + gcry_drbg_blocklen (drbg));
  if (2 != reseed)
    memset (df_data, 0, gcry_drbg_statelen (drbg));
  return ret;
}

/*
 * scratchpad use: gcry_drbg_ctr_update is called independently from
 * gcry_drbg_ctr_extract_bytes. Therefore, the scratchpad is reused
 */
/* Generate function of CTR DRBG as defined in 10.2.1.5.2 */
static gpg_err_code_t
gcry_drbg_ctr_generate (struct gcry_drbg_state *drbg,
			unsigned char *buf, unsigned int buflen,
			struct gcry_drbg_string *addtl)
{
  gpg_err_code_t ret = 0;
  unsigned int len = 0;
  struct gcry_drbg_string data;
  unsigned char prefix = DRBG_PREFIX1;

  memset (drbg->scratchpad, 0, gcry_drbg_blocklen (drbg));

  /* 10.2.1.5.2 step 2 */
  if (addtl && 0 < addtl->len)
    {
      addtl->next = NULL;
      ret = gcry_drbg_ctr_update (drbg, addtl, 2);
      if (ret)
	return ret;
    }

  /* 10.2.1.5.2 step 4.1 */
  gcry_drbg_add_buf (drbg->V, gcry_drbg_blocklen (drbg), &prefix, 1);
  gcry_drbg_string_fill (&data, drbg->V, gcry_drbg_blocklen (drbg));
  while (len < buflen)
    {
      unsigned int outlen = 0;
      /* 10.2.1.5.2 step 4.2 */
      ret = gcry_drbg_sym (drbg, drbg->C, drbg->scratchpad, &data);
      if (ret)
	goto out;
      outlen = (gcry_drbg_blocklen (drbg) < (buflen - len)) ?
	gcry_drbg_blocklen (drbg) : (buflen - len);
      /* 10.2.1.5.2 step 4.3 */
      memcpy (buf + len, drbg->scratchpad, outlen);
      len += outlen;
      /* 10.2.1.5.2 step 6 */
      if (len < buflen)
	gcry_drbg_add_buf (drbg->V, gcry_drbg_blocklen (drbg), &prefix, 1);
    }

  /* 10.2.1.5.2 step 6 */
  if (addtl)
    addtl->next = NULL;
  ret = gcry_drbg_ctr_update (drbg, addtl, 3);

out:
  memset (drbg->scratchpad, 0, gcry_drbg_blocklen (drbg));
  return ret;
}

static struct gcry_drbg_state_ops gcry_drbg_ctr_ops = {
  gcry_drbg_ctr_update,
  gcry_drbg_ctr_generate,
};

/******************************************************************
 * HMAC DRBG callback functions
 ******************************************************************/

static gpg_err_code_t
gcry_drbg_hmac_update (struct gcry_drbg_state *drbg,
		       struct gcry_drbg_string *seed, int reseed)
{
  gpg_err_code_t ret = GPG_ERR_GENERAL;
  int i = 0;
  struct gcry_drbg_string seed1, seed2, cipherin;

  if (!reseed)
    /* 10.1.2.3 step 2 already implicitly covered with
     * the initial memset(0) of drbg->C */
    memset (drbg->V, 1, gcry_drbg_statelen (drbg));

  /* build linked list which implements the concatenation and fill
   * first part*/
  gcry_drbg_string_fill (&seed1, drbg->V, gcry_drbg_statelen (drbg));
  /* buffer will be filled in for loop below with one byte */
  gcry_drbg_string_fill (&seed2, NULL, 1);
  seed1.next = &seed2;
  /* seed may be NULL */
  seed2.next = seed;

  gcry_drbg_string_fill (&cipherin, drbg->V, gcry_drbg_statelen (drbg));
  /* we execute two rounds of V/K massaging */
  for (i = 2; 0 < i; i--)
    {
      /* first round uses 0x0, second 0x1 */
      unsigned char prefix = DRBG_PREFIX0;
      if (1 == i)
	prefix = DRBG_PREFIX1;
      /* 10.1.2.2 step 1 and 4 -- concatenation and HMAC for key */
      seed2.buf = &prefix;
      ret = gcry_drbg_hmac (drbg, drbg->C, drbg->C, &seed1);
      if (ret)
	return ret;

      /* 10.1.2.2 step 2 and 5 -- HMAC for V */
      ret = gcry_drbg_hmac (drbg, drbg->C, drbg->V, &cipherin);
      if (ret)
	return ret;

      /* 10.1.2.2 step 3 */
      if (!seed || 0 == seed->len)
	return ret;
    }
  return 0;
}

/* generate function of HMAC DRBG as defined in 10.1.2.5 */
static gpg_err_code_t
gcry_drbg_hmac_generate (struct gcry_drbg_state *drbg,
			 unsigned char *buf,
			 unsigned int buflen, struct gcry_drbg_string *addtl)
{
  gpg_err_code_t ret = 0;
  unsigned int len = 0;
  struct gcry_drbg_string data;

  /* 10.1.2.5 step 2 */
  if (addtl && 0 < addtl->len)
    {
      addtl->next = NULL;
      ret = gcry_drbg_hmac_update (drbg, addtl, 1);
      if (ret)
	return ret;
    }

  gcry_drbg_string_fill (&data, drbg->V, gcry_drbg_statelen (drbg));
  while (len < buflen)
    {
      unsigned int outlen = 0;
      /* 10.1.2.5 step 4.1 */
      ret = gcry_drbg_hmac (drbg, drbg->C, drbg->V, &data);
      if (ret)
	return ret;
      outlen = (gcry_drbg_blocklen (drbg) < (buflen - len)) ?
	gcry_drbg_blocklen (drbg) : (buflen - len);

      /* 10.1.2.5 step 4.2 */
      memcpy (buf + len, drbg->V, outlen);
      len += outlen;
    }

  /* 10.1.2.5 step 6 */
  if (addtl)
    addtl->next = NULL;
  ret = gcry_drbg_hmac_update (drbg, addtl, 1);

  return ret;
}

static struct gcry_drbg_state_ops gcry_drbg_hmac_ops = {
  gcry_drbg_hmac_update,
  gcry_drbg_hmac_generate,
};

/******************************************************************
 * Hash DRBG callback functions
 ******************************************************************/

/*
 * scratchpad usage: as gcry_drbg_hash_update and gcry_drbg_hash_df are used
 * interlinked, the scratchpad is used as follows:
 * gcry_drbg_hash_update
 *	start: drbg->scratchpad
 *	length: gcry_drbg_statelen(drbg)
 * gcry_drbg_hash_df:
 *	start: drbg->scratchpad + gcry_drbg_statelen(drbg)
 *	length: gcry_drbg_blocklen(drbg)
 */
/* Derivation Function for Hash DRBG as defined in 10.4.1 */
static gpg_err_code_t
gcry_drbg_hash_df (struct gcry_drbg_state *drbg,
		   unsigned char *outval, size_t outlen,
		   struct gcry_drbg_string *entropy)
{
  gpg_err_code_t ret = 0;
  size_t len = 0;
  unsigned char input[5];
  unsigned char *tmp = drbg->scratchpad + gcry_drbg_statelen (drbg);
  struct gcry_drbg_string data1;

  memset (tmp, 0, gcry_drbg_blocklen (drbg));

  /* 10.4.1 step 3 */
  input[0] = 1;
  gcry_drbg_cpu_to_be32 ((outlen * 8), &input[1]);

  /* 10.4.1 step 4.1 -- concatenation of data for input into hash */
  gcry_drbg_string_fill (&data1, input, 5);
  data1.next = entropy;

  /* 10.4.1 step 4 */
  while (len < outlen)
    {
      short blocklen = 0;
      /* 10.4.1 step 4.1 */
      ret = gcry_drbg_hmac (drbg, NULL, tmp, &data1);
      if (ret)
	goto out;
      /* 10.4.1 step 4.2 */
      input[0]++;
      blocklen = (gcry_drbg_blocklen (drbg) < (outlen - len)) ?
	gcry_drbg_blocklen (drbg) : (outlen - len);
      memcpy (outval + len, tmp, blocklen);
      len += blocklen;
    }

out:
  memset (tmp, 0, gcry_drbg_blocklen (drbg));
  return ret;
}

/* update function for Hash DRBG as defined in 10.1.1.2 / 10.1.1.3 */
static gpg_err_code_t
gcry_drbg_hash_update (struct gcry_drbg_state *drbg,
		       struct gcry_drbg_string *seed, int reseed)
{
  gpg_err_code_t ret = 0;
  struct gcry_drbg_string data1, data2;
  unsigned char *V = drbg->scratchpad;
  unsigned char prefix = DRBG_PREFIX1;

  memset (drbg->scratchpad, 0, gcry_drbg_statelen (drbg));
  if (!seed)
    return GPG_ERR_INV_ARG;

  if (reseed)
    {
      /* 10.1.1.3 step 1: string length is concatenation of
       * 1 byte, V and seed (which is concatenated entropy/addtl
       * input)
       */
      memcpy (V, drbg->V, gcry_drbg_statelen (drbg));
      gcry_drbg_string_fill (&data1, &prefix, 1);
      gcry_drbg_string_fill (&data2, V, gcry_drbg_statelen (drbg));
      data1.next = &data2;
      data2.next = seed;
    }
  else
    {
      gcry_drbg_string_fill (&data1, seed->buf, seed->len);
      data1.next = seed->next;
    }

  /* 10.1.1.2 / 10.1.1.3 step 2 and 3 */
  ret = gcry_drbg_hash_df (drbg, drbg->V, gcry_drbg_statelen (drbg), &data1);
  if (ret)
    goto out;

  /* 10.1.1.2 / 10.1.1.3 step 4 -- concatenation  */
  prefix = DRBG_PREFIX0;
  gcry_drbg_string_fill (&data1, &prefix, 1);
  gcry_drbg_string_fill (&data2, drbg->V, gcry_drbg_statelen (drbg));
  data1.next = &data2;
  /* 10.1.1.2 / 10.1.1.3 step 4 -- df operation */
  ret = gcry_drbg_hash_df (drbg, drbg->C, gcry_drbg_statelen (drbg), &data1);

out:
  memset (drbg->scratchpad, 0, gcry_drbg_statelen (drbg));
  return ret;
}

/* processing of additional information string for Hash DRBG */
static gpg_err_code_t
gcry_drbg_hash_process_addtl (struct gcry_drbg_state *drbg,
			      struct gcry_drbg_string *addtl)
{
  gpg_err_code_t ret = 0;
  struct gcry_drbg_string data1, data2;
  struct gcry_drbg_string *data3;
  unsigned char prefix = DRBG_PREFIX2;

  /* this is value w as per documentation */
  memset (drbg->scratchpad, 0, gcry_drbg_blocklen (drbg));

  /* 10.1.1.4 step 2 */
  if (!addtl || 0 == addtl->len)
    return 0;

  /* 10.1.1.4 step 2a -- concatenation */
  gcry_drbg_string_fill (&data1, &prefix, 1);
  gcry_drbg_string_fill (&data2, drbg->V, gcry_drbg_statelen (drbg));
  data3 = addtl;
  data1.next = &data2;
  data2.next = data3;
  data3->next = NULL;
  /* 10.1.1.4 step 2a -- cipher invocation */
  ret = gcry_drbg_hmac (drbg, NULL, drbg->scratchpad, &data1);
  if (ret)
    goto out;

  /* 10.1.1.4 step 2b */
  gcry_drbg_add_buf (drbg->V, gcry_drbg_statelen (drbg),
		     drbg->scratchpad, gcry_drbg_blocklen (drbg));

out:
  memset (drbg->scratchpad, 0, gcry_drbg_blocklen (drbg));
  return ret;
}

/*
 * Hashgen defined in 10.1.1.4
 */
static gpg_err_code_t
gcry_drbg_hash_hashgen (struct gcry_drbg_state *drbg,
			unsigned char *buf, unsigned int buflen)
{
  gpg_err_code_t ret = 0;
  unsigned int len = 0;
  unsigned char *src = drbg->scratchpad;
  unsigned char *dst = drbg->scratchpad + gcry_drbg_statelen (drbg);
  struct gcry_drbg_string data;
  unsigned char prefix = DRBG_PREFIX1;

  /* use the scratchpad as a lookaside buffer */
  memset (src, 0, gcry_drbg_statelen (drbg));
  memset (dst, 0, gcry_drbg_blocklen (drbg));

  /* 10.1.1.4 step hashgen 2 */
  memcpy (src, drbg->V, gcry_drbg_statelen (drbg));

  gcry_drbg_string_fill (&data, src, gcry_drbg_statelen (drbg));
  while (len < buflen)
    {
      unsigned int outlen = 0;
      /* 10.1.1.4 step hashgen 4.1 */
      ret = gcry_drbg_hmac (drbg, NULL, dst, &data);
      if (ret)
	goto out;
      outlen = (gcry_drbg_blocklen (drbg) < (buflen - len)) ?
	gcry_drbg_blocklen (drbg) : (buflen - len);
      /* 10.1.1.4 step hashgen 4.2 */
      memcpy (buf + len, dst, outlen);
      len += outlen;
      /* 10.1.1.4 hashgen step 4.3 */
      if (len < buflen)
	gcry_drbg_add_buf (src, gcry_drbg_statelen (drbg), &prefix, 1);
    }

out:
  memset (drbg->scratchpad, 0,
	  (gcry_drbg_statelen (drbg) + gcry_drbg_blocklen (drbg)));
  return ret;
}

/* generate function for Hash DRBG as defined in  10.1.1.4 */
static gpg_err_code_t
gcry_drbg_hash_generate (struct gcry_drbg_state *drbg,
			 unsigned char *buf, unsigned int buflen,
			 struct gcry_drbg_string *addtl)
{
  gpg_err_code_t ret = 0;
  unsigned char prefix = DRBG_PREFIX3;
  struct gcry_drbg_string data1, data2;
  union
  {
    unsigned char req[8];
    u64 req_int;
  } u;

  /*
   * scratchpad usage: gcry_drbg_hash_process_addtl uses the scratchpad, but
   * fully completes before returning. Thus, we can reuse the scratchpad
   */
  /* 10.1.1.4 step 2 */
  ret = gcry_drbg_hash_process_addtl (drbg, addtl);
  if (ret)
    return ret;
  /* 10.1.1.4 step 3 -- invocation of the Hashgen function defined in
   * 10.1.1.4 */
  ret = gcry_drbg_hash_hashgen (drbg, buf, buflen);
  if (ret)
    return ret;

  /* this is the value H as documented in 10.1.1.4 */
  memset (drbg->scratchpad, 0, gcry_drbg_blocklen (drbg));
  /* 10.1.1.4 step 4 */
  gcry_drbg_string_fill (&data1, &prefix, 1);
  gcry_drbg_string_fill (&data2, drbg->V, gcry_drbg_statelen (drbg));
  data1.next = &data2;
  ret = gcry_drbg_hmac (drbg, NULL, drbg->scratchpad, &data1);
  if (ret)
    goto out;

  /* 10.1.1.4 step 5 */
  gcry_drbg_add_buf (drbg->V, gcry_drbg_statelen (drbg),
		     drbg->scratchpad, gcry_drbg_blocklen (drbg));
  gcry_drbg_add_buf (drbg->V, gcry_drbg_statelen (drbg), drbg->C,
		     gcry_drbg_statelen (drbg));
  u.req_int = be_bswap64 (drbg->reseed_ctr);
  gcry_drbg_add_buf (drbg->V, gcry_drbg_statelen (drbg), u.req,
		     sizeof (u.req));

out:
  memset (drbg->scratchpad, 0, gcry_drbg_blocklen (drbg));
  return ret;
}

/*
 * scratchpad usage: as update and generate are used isolated, both
 * can use the scratchpad
 */
static struct gcry_drbg_state_ops gcry_drbg_hash_ops = {
  gcry_drbg_hash_update,
  gcry_drbg_hash_generate,
};

/******************************************************************
 * Functions common for DRBG implementations
 ******************************************************************/

/*
 * Seeding or reseeding of the DRBG
 *
 * @drbg: DRBG state struct
 * @pers: personalization / additional information buffer
 * @reseed: 0 for initial seed process, 1 for reseeding
 *
 * return:
 *	0 on success
 *	error value otherwise
 */
static gpg_err_code_t
gcry_drbg_seed (struct gcry_drbg_state *drbg, struct gcry_drbg_string *pers,
		int reseed)
{
  gpg_err_code_t ret = 0;
  unsigned char *entropy = NULL;
  size_t entropylen = 0;
  struct gcry_drbg_string data1;

  /* 9.1 / 9.2 / 9.3.1 step 3 */
  if (pers && pers->len > (gcry_drbg_max_addtl ()))
    {
      dbg (("DRBG: personalization string too long %lu\n", pers->len));
      return GPG_ERR_INV_ARG;
    }
  if (drbg->test_data && drbg->test_data->testentropy)
    {
      gcry_drbg_string_fill (&data1, drbg->test_data->testentropy->buf,
			     drbg->test_data->testentropy->len);
      dbg (("DRBG: using test entropy\n"));
    }
  else
    {
      /* Gather entropy equal to the security strength of the DRBG.
       * With a derivation function, a nonce is required in addition
       * to the entropy. A nonce must be at least 1/2 of the security
       * strength of the DRBG in size. Thus, entropy * nonce is 3/2
       * of the strength. The consideration of a nonce is only
       * applicable during initial seeding. */
      entropylen = gcry_drbg_sec_strength (drbg->core->flags);
      if (!entropylen)
	return GPG_ERR_GENERAL;
      if (0 == reseed)
	/* make sure we round up strength/2 in
	 * case it is not divisible by 2 */
	entropylen = ((entropylen + 1) / 2) * 3;
      dbg (("DRBG: (re)seeding with %lu bytes of entropy\n", entropylen));
      entropy = xcalloc_secure (1, entropylen);
      if (!entropy)
	return GPG_ERR_ENOMEM;
      ret = gcry_drbg_get_entropy (drbg, entropy, entropylen);
      if (ret)
	goto out;
      gcry_drbg_string_fill (&data1, entropy, entropylen);
    }

  /* concatenation of entropy with personalization str / addtl input)
   * the variable pers is directly handed by the caller, check its
   * contents whether it is appropriate */
  if (pers && pers->buf && 0 < pers->len && NULL == pers->next)
    {
      data1.next = pers;
      dbg (("DRBG: using personalization string\n"));
    }

  ret = drbg->d_ops->update (drbg, &data1, reseed);
  dbg (("DRBG: state updated with seed\n"));
  if (ret)
    goto out;
  drbg->seeded = 1;
  /* 10.1.1.2 / 10.1.1.3 step 5 */
  drbg->reseed_ctr = 1;

out:
  xfree (entropy);
  return ret;
}

/*************************************************************************
 * exported interfaces
 *************************************************************************/

/*
 * DRBG generate function as required by SP800-90A - this function
 * generates random numbers
 *
 * @drbg DRBG state handle
 * @buf Buffer where to store the random numbers -- the buffer must already
 *      be pre-allocated by caller
 * @buflen Length of output buffer - this value defines the number of random
 *	   bytes pulled from DRBG
 * @addtl Additional input that is mixed into state, may be NULL -- note
 *	  the entropy is pulled by the DRBG internally unconditionally
 *	  as defined in SP800-90A. The additional input is mixed into
 *	  the state in addition to the pulled entropy.
 *
 * return: generated number of bytes
 */
static gpg_err_code_t
gcry_drbg_generate (struct gcry_drbg_state *drbg,
		    unsigned char *buf, unsigned int buflen,
		    struct gcry_drbg_string *addtl)
{
  gpg_err_code_t ret = GPG_ERR_INV_ARG;

  if (0 == buflen || !buf)
    {
      dbg (("DRBG: no buffer provided\n"));
      return ret;
    }
  if (addtl && NULL == addtl->buf && 0 < addtl->len)
    {
      dbg (("DRBG: wrong format of additional information\n"));
      return ret;
    }

  /* 9.3.1 step 2 */
  if (buflen > (gcry_drbg_max_request_bytes ()))
    {
      dbg (("DRBG: requested random numbers too large %u\n", buflen));
      return ret;
    }
  /* 9.3.1 step 3 is implicit with the chosen DRBG */
  /* 9.3.1 step 4 */
  if (addtl && addtl->len > (gcry_drbg_max_addtl ()))
    {
      dbg (("DRBG: additional information string too long %lu\n",
	    addtl->len));
      return ret;
    }
  /* 9.3.1 step 5 is implicit with the chosen DRBG */
  /* 9.3.1 step 6 and 9 supplemented by 9.3.2 step c -- the spec is a
   * bit convoluted here, we make it simpler */
  if ((gcry_drbg_max_requests ()) < drbg->reseed_ctr)
    drbg->seeded = 0;

  if (drbg->pr || !drbg->seeded)
    {
      dbg (("DRBG: reseeding before generation (prediction resistance: %s, state %s)\n", drbg->pr ? "true" : "false", drbg->seeded ? "seeded" : "unseeded"));
      /* 9.3.1 steps 7.1 through 7.3 */
      ret = gcry_drbg_seed (drbg, addtl, 1);
      if (ret)
	return ret;
      /* 9.3.1 step 7.4 */
      addtl = NULL;
    }

  if (addtl && addtl->buf)
    {
      dbg (("DRBG: using additional information string\n"));
    }

  /* 9.3.1 step 8 and 10 */
  ret = drbg->d_ops->generate (drbg, buf, buflen, addtl);

  /* 10.1.1.4 step 6, 10.1.2.5 step 7, 10.2.1.5.2 step 7 */
  drbg->reseed_ctr++;
  if (ret)
    return ret;

  /* 11.3.3 -- re-perform self tests after some generated random
   * numbers, the chosen value after which self test is performed
   * is arbitrary, but it should be reasonable */
  /* Here we do not perform the self tests because of the following
   * reasons: it is mathematically impossible that the initial self tests
   * were successfully and the following are not. If the initial would
   * pass and the following would not, the system integrity is violated.
   * In this case, the entire system operation is questionable and it
   * is unlikely that the integrity violation only affects to the
   * correct operation of the DRBG.
   */
#if 0
  if (drbg->reseed_ctr && !(drbg->reseed_ctr % 4096))
    {
      dbg (("DRBG: start to perform self test\n"));
      ret = gcry_drbg_healthcheck ();
      if (ret)
	{
	  log_fatal (("DRBG: self test failed\n"));
	  return ret;
	}
      else
	{
	  dbg (("DRBG: self test successful\n"));
	}
    }
#endif

  return ret;
}

/*
 * Wrapper around gcry_drbg_generate which can pull arbitrary long strings
 * from the DRBG without hitting the maximum request limitation.
 *
 * Parameters: see gcry_drbg_generate
 * Return codes: see gcry_drbg_generate -- if one gcry_drbg_generate request fails,
 *		 the entire gcry_drbg_generate_long request fails
 */
static gpg_err_code_t
gcry_drbg_generate_long (struct gcry_drbg_state *drbg,
			 unsigned char *buf, unsigned int buflen,
			 struct gcry_drbg_string *addtl)
{
  gpg_err_code_t ret = 0;
  unsigned int slice = 0;
  unsigned char *buf_p = buf;
  unsigned len = 0;
  do
    {
      unsigned int chunk = 0;
      slice = ((buflen - len) / gcry_drbg_max_request_bytes ());
      chunk = slice ? gcry_drbg_max_request_bytes () : (buflen - len);
      ret = gcry_drbg_generate (drbg, buf_p, chunk, addtl);
      if (ret)
	return ret;
      buf_p += chunk;
      len += chunk;
    }
  while (slice > 0 && (len < buflen));
  return ret;
}

/*
 * DRBG uninstantiate function as required by SP800-90A - this function
 * frees all buffers and the DRBG handle
 *
 * @drbg DRBG state handle
 *
 * return
 * 	0 on success
 */
static gpg_err_code_t
gcry_drbg_uninstantiate (struct gcry_drbg_state *drbg)
{
  if (!drbg)
    return GPG_ERR_INV_ARG;
  xfree (drbg->V);
  drbg->V = NULL;
  xfree (drbg->C);
  drbg->C = NULL;
  drbg->reseed_ctr = 0;
  xfree (drbg->scratchpad);
  drbg->scratchpad = NULL;
  drbg->seeded = 0;
  drbg->pr = 0;
  drbg->seed_init_pid = 0;
  return 0;
}

/*
 * DRBG instantiation function as required by SP800-90A - this function
 * sets up the DRBG handle, performs the initial seeding and all sanity
 * checks required by SP800-90A
 *
 * @drbg memory of state -- if NULL, new memory is allocated
 * @pers Personalization string that is mixed into state, may be NULL -- note
 *	 the entropy is pulled by the DRBG internally unconditionally
 *	 as defined in SP800-90A. The additional input is mixed into
 *	 the state in addition to the pulled entropy.
 * @coreref reference to core
 * @flags Flags defining the requested DRBG type and cipher type. The flags
 * 	  are defined in drbg.h and may be XORed. Beware, if you XOR multiple
 * 	  cipher types together, the code picks the core on a first come first
 * 	  serve basis as it iterates through the available cipher cores and
 * 	  uses the one with the first match. The minimum required flags are:
 * 		cipher type flag
 *
 * return
 *	0 on success
 *	error value otherwise
 */
static gpg_err_code_t
gcry_drbg_instantiate (struct gcry_drbg_state *drbg,
		       struct gcry_drbg_string *pers, int coreref, int pr)
{
  gpg_err_code_t ret = GPG_ERR_ENOMEM;
  unsigned int sb_size = 0;

  if (!drbg)
    return GPG_ERR_INV_ARG;

  dbg (("DRBG: Initializing DRBG core %d with prediction resistance %s\n",
	coreref, pr ? "enabled" : "disabled"));
  drbg->core = &gcry_drbg_cores[coreref];
  drbg->pr = pr;
  drbg->seeded = 0;
  if (drbg->core->flags & GCRY_DRBG_HMAC)
    drbg->d_ops = &gcry_drbg_hmac_ops;
  else if (drbg->core->flags & GCRY_DRBG_HASH_MASK)
    drbg->d_ops = &gcry_drbg_hash_ops;
  else if (drbg->core->flags & GCRY_DRBG_CTR_MASK)
    drbg->d_ops = &gcry_drbg_ctr_ops;
  else
    return GPG_ERR_GENERAL;
  /* 9.1 step 1 is implicit with the selected DRBG type -- see
   * gcry_drbg_sec_strength() */

  /* 9.1 step 2 is implicit as caller can select prediction resistance
   * and the flag is copied into drbg->flags --
   * all DRBG types support prediction resistance */

  /* 9.1 step 4 is implicit in  gcry_drbg_sec_strength */

  /* no allocation of drbg as this is done by the kernel crypto API */
  drbg->V = xcalloc_secure (1, gcry_drbg_statelen (drbg));
  if (!drbg->V)
    goto err;
  drbg->C = xcalloc_secure (1, gcry_drbg_statelen (drbg));
  if (!drbg->C)
    goto err;
  /* scratchpad is only generated for CTR and Hash */
  if (drbg->core->flags & GCRY_DRBG_HMAC)
    sb_size = 0;
  else if (drbg->core->flags & GCRY_DRBG_CTR_MASK)
    sb_size = gcry_drbg_statelen (drbg) + gcry_drbg_blocklen (drbg) +	/* temp */
      gcry_drbg_statelen (drbg) +	/* df_data */
      gcry_drbg_blocklen (drbg) +	/* pad */
      gcry_drbg_blocklen (drbg) +	/* iv */
      gcry_drbg_statelen (drbg) + gcry_drbg_blocklen (drbg);	/* temp */
  else
    sb_size = gcry_drbg_statelen (drbg) + gcry_drbg_blocklen (drbg);

  if (0 < sb_size)
    {
      drbg->scratchpad = xcalloc_secure (1, sb_size);
      if (!drbg->scratchpad)
	goto err;
    }
  dbg (("DRBG: state allocated with scratchpad size %u bytes\n", sb_size));

  /* 9.1 step 6 through 11 */
  ret = gcry_drbg_seed (drbg, pers, 0);
  if (ret)
    goto err;

  dbg (("DRBG: core %d %s prediction resistance successfully initialized\n",
	coreref, pr ? "with" : "without"));
  return 0;

err:
  gcry_drbg_uninstantiate (drbg);
  return ret;
}

/*
 * DRBG reseed function as required by SP800-90A
 *
 * @drbg DRBG state handle
 * @addtl Additional input that is mixed into state, may be NULL -- note
 * 		the entropy is pulled by the DRBG internally unconditionally
 * 		as defined in SP800-90A. The additional input is mixed into
 * 		the state in addition to the pulled entropy.
 *
 * return
 * 	0 on success
 * 	error value otherwise
 */
static gpg_err_code_t
gcry_drbg_reseed (struct gcry_drbg_state *drbg,
		  struct gcry_drbg_string *addtl)
{
  gpg_err_code_t ret = 0;
  ret = gcry_drbg_seed (drbg, addtl, 1);
  return ret;
}

/******************************************************************
 ******************************************************************
 ******************************************************************
 * libgcrypt integration code
 ******************************************************************
 ******************************************************************
 ******************************************************************/

/***************************************************************
 * libgcrypt backend functions to the RNG API code
 ***************************************************************/

/* global state variable holding the current instance of the DRBG -- the
 * default DRBG type is defined in _gcry_gcry_drbg_init */
static struct gcry_drbg_state *gcry_drbg = NULL;

/* This is the lock we use to serialize access to this RNG. */
GPGRT_LOCK_DEFINE(drbg_lock);

static inline void
gcry_drbg_lock (void)
{
  gpg_err_code_t my_errno;

  my_errno = gpgrt_lock_lock (&drbg_lock);
  if (my_errno)
    log_fatal ("failed to acquire the RNG lock: %s\n", strerror (my_errno));
}

static inline void
gcry_drbg_unlock (void)
{
  gpg_err_code_t my_errno;

  my_errno = gpgrt_lock_unlock (&drbg_lock);
  if (my_errno)
    log_fatal ("failed to release the RNG lock: %s\n", strerror (my_errno));
}

/* Basic initialization is required to initialize mutexes and
   do a few checks on the implementation.  */
static void
basic_initialization (void)
{
  static int initialized;

  if (initialized)
    return;
  initialized = 1;

  /* Make sure that we are still using the values we have
     traditionally used for the random levels.  */
  gcry_assert (GCRY_WEAK_RANDOM == 0
               && GCRY_STRONG_RANDOM == 1
               && GCRY_VERY_STRONG_RANDOM == 2);
}

/****** helper functions where lock must be held by caller *****/

/* Check whether given flags are known to point to an applicable DRBG */
static gpg_err_code_t
gcry_drbg_algo_available (u32 flags, int *coreref)
{
  int i = 0;
  for (i = 0; ARRAY_SIZE (gcry_drbg_cores) > i; i++)
    {
      if ((gcry_drbg_cores[i].flags & GCRY_DRBG_CIPHER_MASK) ==
	  (flags & GCRY_DRBG_CIPHER_MASK))
	{
	  *coreref = i;
	  return 0;
	}
    }
  return GPG_ERR_GENERAL;
}

static gpg_err_code_t
_gcry_drbg_init_internal (u32 flags, struct gcry_drbg_string *pers)
{
  gpg_err_code_t ret = 0;
  static u32 oldflags = 0;
  int coreref = 0;
  int pr = 0;

  /* If a caller provides 0 as flags, use the flags of the previous
   * initialization, otherwise use the current flags and remember them
   * for the next invocation
   */
  if (0 == flags)
    flags = oldflags;
  else
    oldflags = flags;

  ret = gcry_drbg_algo_available (flags, &coreref);
  if (ret)
    return ret;

  if (NULL != gcry_drbg)
    {
      gcry_drbg_uninstantiate (gcry_drbg);
    }
  else
    {
      gcry_drbg = xcalloc_secure (1, sizeof (struct gcry_drbg_state));
      if (!gcry_drbg)
	return GPG_ERR_ENOMEM;
    }
  if (flags & GCRY_DRBG_PREDICTION_RESIST)
    pr = 1;
  ret = gcry_drbg_instantiate (gcry_drbg, pers, coreref, pr);
  if (ret)
    fips_signal_error ("DRBG cannot be initialized");
  else
    gcry_drbg->seed_init_pid = getpid ();
  return ret;
}

/************* calls available to common RNG code **************/

/*
 * Initialize one DRBG invoked by the libgcrypt API
 */
void
_gcry_drbg_init (int full)
{
  /* default DRBG */
  u32 flags = GCRY_DRBG_NOPR_HMACSHA256;
  basic_initialization ();
  if (!full)
      return;
  gcry_drbg_lock ();
  if (NULL == gcry_drbg)
    _gcry_drbg_init_internal (flags, NULL);
  gcry_drbg_unlock ();
}

/*
 * Backend handler function for GCRYCTL_DRBG_REINIT
 *
 * Select a different DRBG type and initialize it.
 * Function checks whether requested DRBG type exists and returns an error in
 * case it does not. In case of an error, the previous instantiated DRBG is
 * left untouched and alive. Thus, in case of an error, a DRBG is always
 * available, even if it is not the chosen one.
 *
 * Re-initialization will be performed in any case regardless whether flags
 * or personalization string are set.
 *
 * If flags == 0, do not change current DRBG
 * If personalization string is NULL or its length is 0, re-initialize without
 * personalization string
 */
gpg_err_code_t
_gcry_drbg_reinit (u32 flags, struct gcry_drbg_string *pers)
{
  gpg_err_code_t ret = GPG_ERR_GENERAL;
  dbg (("DRBG: reinitialize internal DRBG state with flags %u\n", flags));
  gcry_drbg_lock ();
  ret = _gcry_drbg_init_internal (flags, pers);
  gcry_drbg_unlock ();
  return ret;
}

/* Try to close the FDs of the random gather module.  This is
 * currently only implemented for rndlinux. */
void
_gcry_drbg_close_fds (void)
{
#if USE_RNDLINUX
  gcry_drbg_lock ();
  _gcry_rndlinux_gather_random (NULL, 0, 0, 0);
  gcry_drbg_unlock ();
#endif
}

/* Print some statistics about the RNG.  */
void
_gcry_drbg_dump_stats (void)
{
  /* Not yet implemented.  */
  /* Maybe dumping of reseed counter? */
}

/* This function returns true if no real RNG is available or the
 * quality of the RNG has been degraded for test purposes.  */
int
_gcry_drbg_is_faked (void)
{
  return 0;			/* Faked random is not allowed.  */
}

/* Add BUFLEN bytes from BUF to the internal random pool.  QUALITY
 * should be in the range of 0..100 to indicate the goodness of the
 * entropy added, or -1 for goodness not known. */
gcry_error_t
_gcry_drbg_add_bytes (const void *buf, size_t buflen, int quality)
{
  gpg_err_code_t ret = 0;
  struct gcry_drbg_string seed;
  (void) quality;
  _gcry_drbg_init(1); /* Auto-initialize if needed */
  if (NULL == gcry_drbg)
    return GPG_ERR_GENERAL;
  gcry_drbg_string_fill (&seed, (unsigned char *) buf, buflen);
  gcry_drbg_lock ();
  ret = gcry_drbg_reseed (gcry_drbg, &seed);
  gcry_drbg_unlock ();
  return ret;
}

/* This function is to be used for all types of random numbers, including
 * nonces
 */
void
_gcry_drbg_randomize (void *buffer, size_t length,
		      enum gcry_random_level level)
{
  (void) level;
  _gcry_drbg_init(1); /* Auto-initialize if needed */
  gcry_drbg_lock ();
  if (NULL == gcry_drbg)
    {
      fips_signal_error ("DRBG is not initialized");
      goto bailout;
    }

  /* As reseeding changes the entire state of the DRBG, including any
   * key, either a re-init or a reseed is sufficient for a fork */
  if (gcry_drbg->seed_init_pid != getpid ())
    {
      /* We are in a child of us. Perform a reseeding. */
      if (gcry_drbg_reseed (gcry_drbg, NULL))
	{
	  fips_signal_error ("reseeding upon fork failed");
	  log_fatal ("severe error getting random\n");
	  goto bailout;
	}
    }
  /* potential integer overflow is covered by gcry_drbg_generate which
   * ensures that length cannot overflow an unsigned int */
  if (0 < length)
    {
      if (!buffer)
	goto bailout;
      if (gcry_drbg_generate_long
	  (gcry_drbg, buffer, (unsigned int) length, NULL))
	log_fatal ("No random numbers generated\n");
    }
  else
    {
      struct gcry_drbg_gen *data = (struct gcry_drbg_gen *) buffer;
      /* catch NULL pointer */
      if (!data || !data->outbuf)
	{
	  fips_signal_error ("No output buffer provided");
	  goto bailout;
	}
      if (gcry_drbg_generate_long (gcry_drbg, data->outbuf, data->outlen,
				   data->addtl))
	log_fatal ("No random numbers generated\n");
    }
bailout:
  gcry_drbg_unlock ();
  return;

}

/***************************************************************
 * Self-test code
 ***************************************************************/

/*
 * Test vectors from
 * http://csrc.nist.gov/groups/STM/cavp/documents/drbg/drbgtestvectors.zip
 */
struct gcry_drbg_test_vector gcry_drbg_test_pr[] = {
  {
   .flags = (GCRY_DRBG_PR_HASHSHA256),
   .entropy = (unsigned char *)
   "\x5d\xf2\x14\xbc\xf6\xb5\x4e\x0b\xf0\x0d\x6f\x2d"
   "\xe2\x01\x66\x7b\xd0\xa4\x73\xa4\x21\xdd\xb0\xc0"
   "\x51\x79\x09\xf4\xea\xa9\x08\xfa\xa6\x67\xe0\xe1"
   "\xd1\x88\xa8\xad\xee\x69\x74\xb3\x55\x06\x9b\xf6",
   .entropylen = 48,
   .entpra = (unsigned char *)
   "\xef\x48\x06\xa2\xc2\x45\xf1\x44\xfa\x34\x2c\xeb"
   "\x8d\x78\x3c\x09\x8f\x34\x72\x20\xf2\xe7\xfd\x13"
   "\x76\x0a\xf6\xdc\x3c\xf5\xc0\x15",
   .entprb = (unsigned char *)
   "\x4b\xbe\xe5\x24\xed\x6a\x2d\x0c\xdb\x73\x5e\x09"
   "\xf9\xad\x67\x7c\x51\x47\x8b\x6b\x30\x2a\xc6\xde"
   "\x76\xaa\x55\x04\x8b\x0a\x72\x95",
   .entprlen = 32,
   .expected = (unsigned char *)
   "\x3b\x14\x71\x99\xa1\xda\xa0\x42\xe6\xc8\x85\x32"
   "\x70\x20\x32\x53\x9a\xbe\xd1\x1e\x15\xef\xfb\x4c"
   "\x25\x6e\x19\x3a\xf0\xb9\xcb\xde\xf0\x3b\xc6\x18"
   "\x4d\x85\x5a\x9b\xf1\xe3\xc2\x23\x03\x93\x08\xdb"
   "\xa7\x07\x4b\x33\x78\x40\x4d\xeb\x24\xf5\x6e\x81"
   "\x4a\x1b\x6e\xa3\x94\x52\x43\xb0\xaf\x2e\x21\xf4"
   "\x42\x46\x8e\x90\xed\x34\x21\x75\xea\xda\x67\xb6"
   "\xe4\xf6\xff\xc6\x31\x6c\x9a\x5a\xdb\xb3\x97\x13"
   "\x09\xd3\x20\x98\x33\x2d\x6d\xd7\xb5\x6a\xa8\xa9"
   "\x9a\x5b\xd6\x87\x52\xa1\x89\x2b\x4b\x9c\x64\x60"
   "\x50\x47\xa3\x63\x81\x16\xaf\x19",
   .expectedlen = 128,
   .addtla = (unsigned char *)
   "\xbe\x13\xdb\x2a\xe9\xa8\xfe\x09\x97\xe1\xce\x5d"
   "\xe8\xbb\xc0\x7c\x4f\xcb\x62\x19\x3f\x0f\xd2\xad"
   "\xa9\xd0\x1d\x59\x02\xc4\xff\x70",
   .addtlb = (unsigned char *)
   "\x6f\x96\x13\xe2\xa7\xf5\x6c\xfe\xdf\x66\xe3\x31"
   "\x63\x76\xbf\x20\x27\x06\x49\xf1\xf3\x01\x77\x41"
   "\x9f\xeb\xe4\x38\xfe\x67\x00\xcd",
   .addtllen = 32,
   .pers = NULL,
   .perslen = 0,
   },
  {
   .flags = (GCRY_DRBG_PR_HMACSHA256),
   .entropy = (unsigned char *)
   "\x13\x54\x96\xfc\x1b\x7d\x28\xf3\x18\xc9\xa7\x89"
   "\xb6\xb3\xc8\x72\xac\x00\xd4\x59\x36\x25\x05\xaf"
   "\xa5\xdb\x96\xcb\x3c\x58\x46\x87\xa5\xaa\xbf\x20"
   "\x3b\xfe\x23\x0e\xd1\xc7\x41\x0f\x3f\xc9\xb3\x67",
   .entropylen = 48,
   .entpra = (unsigned char *)
   "\xe2\xbd\xb7\x48\x08\x06\xf3\xe1\x93\x3c\xac\x79"
   "\xa7\x2b\x11\xda\xe3\x2e\xe1\x91\xa5\x02\x19\x57"
   "\x20\x28\xad\xf2\x60\xd7\xcd\x45",
   .entprb = (unsigned char *)
   "\x8b\xd4\x69\xfc\xff\x59\x95\x95\xc6\x51\xde\x71"
   "\x68\x5f\xfc\xf9\x4a\xab\xec\x5a\xcb\xbe\xd3\x66"
   "\x1f\xfa\x74\xd3\xac\xa6\x74\x60",
   .entprlen = 32,
   .expected = (unsigned char *)
   "\x1f\x9e\xaf\xe4\xd2\x46\xb7\x47\x41\x4c\x65\x99"
   "\x01\xe9\x3b\xbb\x83\x0c\x0a\xb0\xc1\x3a\xe2\xb3"
   "\x31\x4e\xeb\x93\x73\xee\x0b\x26\xc2\x63\xa5\x75"
   "\x45\x99\xd4\x5c\x9f\xa1\xd4\x45\x87\x6b\x20\x61"
   "\x40\xea\x78\xa5\x32\xdf\x9e\x66\x17\xaf\xb1\x88"
   "\x9e\x2e\x23\xdd\xc1\xda\x13\x97\x88\xa5\xb6\x5e"
   "\x90\x14\x4e\xef\x13\xab\x5c\xd9\x2c\x97\x9e\x7c"
   "\xd7\xf8\xce\xea\x81\xf5\xcd\x71\x15\x49\x44\xce"
   "\x83\xb6\x05\xfb\x7d\x30\xb5\x57\x2c\x31\x4f\xfc"
   "\xfe\x80\xb6\xc0\x13\x0c\x5b\x9b\x2e\x8f\x3d\xfc"
   "\xc2\xa3\x0c\x11\x1b\x80\x5f\xf3",
   .expectedlen = 128,
   .addtla = NULL,
   .addtlb = NULL,
   .addtllen = 0,
   .pers = (unsigned char *)
   "\x64\xb6\xfc\x60\xbc\x61\x76\x23\x6d\x3f\x4a\x0f"
   "\xe1\xb4\xd5\x20\x9e\x70\xdd\x03\x53\x6d\xbf\xce"
   "\xcd\x56\x80\xbc\xb8\x15\xc8\xaa",
   .perslen = 32,
   },
  {
   .flags = (GCRY_DRBG_PR_CTRAES128),
   .entropy = (unsigned char *)
   "\x92\x89\x8f\x31\xfa\x1c\xff\x6d\x18\x2f\x26\x06"
   "\x43\xdf\xf8\x18\xc2\xa4\xd9\x72\xc3\xb9\xb6\x97",
   .entropylen = 24,
   .entpra = (unsigned char *)
   "\x20\x72\x8a\x06\xf8\x6f\x8d\xd4\x41\xe2\x72\xb7"
   "\xc4\x2c\xe8\x10",
   .entprb = (unsigned char *)
   "\x3d\xb0\xf0\x94\xf3\x05\x50\x33\x17\x86\x3e\x22"
   "\x08\xf7\xa5\x01",
   .entprlen = 16,
   .expected = (unsigned char *)
   "\x5a\x35\x39\x87\x0f\x4d\x22\xa4\x09\x24\xee\x71"
   "\xc9\x6f\xac\x72\x0a\xd6\xf0\x88\x82\xd0\x83\x28"
   "\x73\xec\x3f\x93\xd8\xab\x45\x23\xf0\x7e\xac\x45"
   "\x14\x5e\x93\x9f\xb1\xd6\x76\x43\x3d\xb6\xe8\x08"
   "\x88\xf6\xda\x89\x08\x77\x42\xfe\x1a\xf4\x3f\xc4"
   "\x23\xc5\x1f\x68",
   .expectedlen = 64,
   .addtla = (unsigned char *)
   "\x1a\x40\xfa\xe3\xcc\x6c\x7c\xa0\xf8\xda\xba\x59"
   "\x23\x6d\xad\x1d",
   .addtlb = (unsigned char *)
   "\x9f\x72\x76\x6c\xc7\x46\xe5\xed\x2e\x53\x20\x12"
   "\xbc\x59\x31\x8c",
   .addtllen = 16,
   .pers = (unsigned char *)
   "\xea\x65\xee\x60\x26\x4e\x7e\xb6\x0e\x82\x68\xc4"
   "\x37\x3c\x5c\x0b",
   .perslen = 16,
   },
};

struct gcry_drbg_test_vector gcry_drbg_test_nopr[] = {
  {
   .flags = GCRY_DRBG_NOPR_HASHSHA256,
   .entropy = (unsigned char *)
   "\x73\xd3\xfb\xa3\x94\x5f\x2b\x5f\xb9\x8f\xf6\x9c"
   "\x8a\x93\x17\xae\x19\xc3\x4c\xc3\xd6\xca\xa3\x2d"
   "\x16\xfc\x42\xd2\x2d\xd5\x6f\x56\xcc\x1d\x30\xff"
   "\x9e\x06\x3e\x09\xce\x58\xe6\x9a\x35\xb3\xa6\x56",
   .entropylen = 48,
   .expected = (unsigned char *)
   "\x71\x7b\x93\x46\x1a\x40\xaa\x35\xa4\xaa\xc5\xe7"
   "\x6d\x5b\x5b\x8a\xa0\xdf\x39\x7d\xae\x71\x58\x5b"
   "\x3c\x7c\xb4\xf0\x89\xfa\x4a\x8c\xa9\x5c\x54\xc0"
   "\x40\xdf\xbc\xce\x26\x81\x34\xf8\xba\x7d\x1c\xe8"
   "\xad\x21\xe0\x74\xcf\x48\x84\x30\x1f\xa1\xd5\x4f"
   "\x81\x42\x2f\xf4\xdb\x0b\x23\xf8\x73\x27\xb8\x1d"
   "\x42\xf8\x44\x58\xd8\x5b\x29\x27\x0a\xf8\x69\x59"
   "\xb5\x78\x44\xeb\x9e\xe0\x68\x6f\x42\x9a\xb0\x5b"
   "\xe0\x4e\xcb\x6a\xaa\xe2\xd2\xd5\x33\x25\x3e\xe0"
   "\x6c\xc7\x6a\x07\xa5\x03\x83\x9f\xe2\x8b\xd1\x1c"
   "\x70\xa8\x07\x59\x97\xeb\xf6\xbe",
   .expectedlen = 128,
   .addtla = (unsigned char *)
   "\xf4\xd5\x98\x3d\xa8\xfc\xfa\x37\xb7\x54\x67\x73"
   "\xc7\xc3\xdd\x47\x34\x71\x02\x5d\xc1\xa0\xd3\x10"
   "\xc1\x8b\xbd\xf5\x66\x34\x6f\xdd",
   .addtlb = (unsigned char *)
   "\xf7\x9e\x6a\x56\x0e\x73\xe9\xd9\x7a\xd1\x69\xe0"
   "\x6f\x8c\x55\x1c\x44\xd1\xce\x6f\x28\xcc\xa4\x4d"
   "\xa8\xc0\x85\xd1\x5a\x0c\x59\x40",
   .addtllen = 32,
   .pers = NULL,
   .perslen = 0,
   },
  {
   .flags = GCRY_DRBG_NOPR_HMACSHA256,
   .entropy = (unsigned char *)
   "\x8d\xf0\x13\xb4\xd1\x03\x52\x30\x73\x91\x7d\xdf"
   "\x6a\x86\x97\x93\x05\x9e\x99\x43\xfc\x86\x54\x54"
   "\x9e\x7a\xb2\x2f\x7c\x29\xf1\x22\xda\x26\x25\xaf"
   "\x2d\xdd\x4a\xbc\xce\x3c\xf4\xfa\x46\x59\xd8\x4e",
   .entropylen = 48,
   .expected = (unsigned char *)
   "\xb9\x1c\xba\x4c\xc8\x4f\xa2\x5d\xf8\x61\x0b\x81"
   "\xb6\x41\x40\x27\x68\xa2\x09\x72\x34\x93\x2e\x37"
   "\xd5\x90\xb1\x15\x4c\xbd\x23\xf9\x74\x52\xe3\x10"
   "\xe2\x91\xc4\x51\x46\x14\x7f\x0d\xa2\xd8\x17\x61"
   "\xfe\x90\xfb\xa6\x4f\x94\x41\x9c\x0f\x66\x2b\x28"
   "\xc1\xed\x94\xda\x48\x7b\xb7\xe7\x3e\xec\x79\x8f"
   "\xbc\xf9\x81\xb7\x91\xd1\xbe\x4f\x17\x7a\x89\x07"
   "\xaa\x3c\x40\x16\x43\xa5\xb6\x2b\x87\xb8\x9d\x66"
   "\xb3\xa6\x0e\x40\xd4\xa8\xe4\xe9\xd8\x2a\xf6\xd2"
   "\x70\x0e\x6f\x53\x5c\xdb\x51\xf7\x5c\x32\x17\x29"
   "\x10\x37\x41\x03\x0c\xcc\x3a\x56",
   .expectedlen = 128,
   .addtla = NULL,
   .addtlb = NULL,
   .addtllen = 0,
   .pers = (unsigned char *)
   "\xb5\x71\xe6\x6d\x7c\x33\x8b\xc0\x7b\x76\xad\x37"
   "\x57\xbb\x2f\x94\x52\xbf\x7e\x07\x43\x7a\xe8\x58"
   "\x1c\xe7\xbc\x7c\x3a\xc6\x51\xa9",
   .perslen = 32,
   },
  {
   .flags = GCRY_DRBG_NOPR_CTRAES128,
   .entropy = (unsigned char *)
   "\xc0\x70\x1f\x92\x50\x75\x8f\xcd\xf2\xbe\x73\x98"
   "\x80\xdb\x66\xeb\x14\x68\xb4\xa5\x87\x9c\x2d\xa6",
   .entropylen = 24,
   .expected = (unsigned char *)
   "\x97\xc0\xc0\xe5\xa0\xcc\xf2\x4f\x33\x63\x48\x8a"
   "\xdb\x13\x0a\x35\x89\xbf\x80\x65\x62\xee\x13\x95"
   "\x7c\x33\xd3\x7d\xf4\x07\x77\x7a\x2b\x65\x0b\x5f"
   "\x45\x5c\x13\xf1\x90\x77\x7f\xc5\x04\x3f\xcc\x1a"
   "\x38\xf8\xcd\x1b\xbb\xd5\x57\xd1\x4a\x4c\x2e\x8a"
   "\x2b\x49\x1e\x5c",
   .expectedlen = 64,
   .addtla = (unsigned char *)
   "\xf9\x01\xf8\x16\x7a\x1d\xff\xde\x8e\x3c\x83\xe2"
   "\x44\x85\xe7\xfe",
   .addtlb = (unsigned char *)
   "\x17\x1c\x09\x38\xc2\x38\x9f\x97\x87\x60\x55\xb4"
   "\x82\x16\x62\x7f",
   .addtllen = 16,
   .pers = (unsigned char *)
   "\x80\x08\xae\xe8\xe9\x69\x40\xc5\x08\x73\xc7\x9f"
   "\x8e\xcf\xe0\x02",
   .perslen = 16,
   },
  {
   .flags = GCRY_DRBG_NOPR_HASHSHA1,
   .entropy = (unsigned char *)
   "\x16\x10\xb8\x28\xcc\xd2\x7d\xe0\x8c\xee\xa0\x32"
   "\xa2\x0e\x92\x08\x49\x2c\xf1\x70\x92\x42\xf6\xb5",
   .entropylen = 24,
   .expected = (unsigned char *)
   "\x56\xf3\x3d\x4f\xdb\xb9\xa5\xb6\x4d\x26\x23\x44"
   "\x97\xe9\xdc\xb8\x77\x98\xc6\x8d\x08\xf7\xc4\x11"
   "\x99\xd4\xbd\xdf\x97\xeb\xbf\x6c\xb5\x55\x0e\x5d"
   "\x14\x9f\xf4\xd5\xbd\x0f\x05\xf2\x5a\x69\x88\xc1"
   "\x74\x36\x39\x62\x27\x18\x4a\xf8\x4a\x56\x43\x35"
   "\x65\x8e\x2f\x85\x72\xbe\xa3\x33\xee\xe2\xab\xff"
   "\x22\xff\xa6\xde\x3e\x22\xac\xa2",
   .expectedlen = 80,
   .addtla = NULL,
   .addtlb = NULL,
   .addtllen = 0,
   .pers = NULL,
   .perslen = 0,
   .entropyreseed = (unsigned char *)
   "\x72\xd2\x8c\x90\x8e\xda\xf9\xa4\xd1\xe5\x26\xd8"
   "\xf2\xde\xd5\x44",
   .entropyreseed_len = 16,
   .addtl_reseed = NULL,
   .addtl_reseed_len = 0,
   },
  {
   .flags = GCRY_DRBG_NOPR_HASHSHA1,
   .entropy = (unsigned char *)
   "\xd9\xba\xb5\xce\xdc\xa9\x6f\x61\x78\xd6\x45\x09"
   "\xa0\xdf\xdc\x5e\xda\xd8\x98\x94\x14\x45\x0e\x01",
   .entropylen = 24,
   .expected = (unsigned char *)
   "\xc4\x8b\x89\xf9\xda\x3f\x74\x82\x45\x55\x5d\x5d"
   "\x03\x3b\x69\x3d\xd7\x1a\x4d\xf5\x69\x02\x05\xce"
   "\xfc\xd7\x20\x11\x3c\xc2\x4e\x09\x89\x36\xff\x5e"
   "\x77\xb5\x41\x53\x58\x70\xb3\x39\x46\x8c\xdd\x8d"
   "\x6f\xaf\x8c\x56\x16\x3a\x70\x0a\x75\xb2\x3e\x59"
   "\x9b\x5a\xec\xf1\x6f\x3b\xaf\x6d\x5f\x24\x19\x97"
   "\x1f\x24\xf4\x46\x72\x0f\xea\xbe",
   .expectedlen = 80,
   .addtla = (unsigned char *)
   "\x04\xfa\x28\x95\xaa\x5a\x6f\x8c\x57\x43\x34\x3b"
   "\x80\x5e\x5e\xa4",
   .addtlb = (unsigned char *)
   "\xdf\x5d\xc4\x59\xdf\xf0\x2a\xa2\xf0\x52\xd7\x21"
   "\xec\x60\x72\x30",
   .addtllen = 16,
   .pers = NULL,
   .perslen = 0,
   .entropyreseed = (unsigned char *)
   "\xc6\xba\xd0\x74\xc5\x90\x67\x86\xf5\xe1\xf3\x20"
   "\x99\xf5\xb4\x91",
   .entropyreseed_len = 16,
   .addtl_reseed = (unsigned char *)
   "\x3e\x6b\xf4\x6f\x4d\xaa\x38\x25\xd7\x19\x4e\x69"
   "\x4e\x77\x52\xf7",
   .addtl_reseed_len = 16,
   },
};


/*
 * Tests implement the CAVS test approach as documented in
 * http://csrc.nist.gov/groups/STM/cavp/documents/drbg/DRBGVS.pdf
 */

/*
 * CAVS test
 *
 * This function is not static as it is needed for as a private API
 * call for the CAVS test tool.
 */
gpg_err_code_t
gcry_drbg_cavs_test (struct gcry_drbg_test_vector *test, unsigned char *buf)
{
  gpg_err_code_t ret = 0;
  struct gcry_drbg_state *drbg = NULL;
  struct gcry_drbg_test_data test_data;
  struct gcry_drbg_string addtl, pers, testentropy;
  int coreref = 0;
  int pr = 0;

  ret = gcry_drbg_algo_available (test->flags, &coreref);
  if (ret)
    goto outbuf;

  drbg = xcalloc_secure (1, sizeof (struct gcry_drbg_state));
  if (!drbg)
    {
      ret = GPG_ERR_ENOMEM;
      goto outbuf;
    }

  if (test->flags & GCRY_DRBG_PREDICTION_RESIST)
    pr = 1;

  test_data.testentropy = &testentropy;
  gcry_drbg_string_fill (&testentropy, test->entropy, test->entropylen);
  drbg->test_data = &test_data;
  gcry_drbg_string_fill (&pers, test->pers, test->perslen);
  ret = gcry_drbg_instantiate (drbg, &pers, coreref, pr);
  if (ret)
    goto outbuf;

  if (test->entropyreseed)
    {
      gcry_drbg_string_fill (&testentropy, test->entropyreseed,
			     test->entropyreseed_len);
      gcry_drbg_string_fill (&addtl, test->addtl_reseed,
			     test->addtl_reseed_len);
      if (gcry_drbg_reseed (drbg, &addtl))
	goto outbuf;
    }

  gcry_drbg_string_fill (&addtl, test->addtla, test->addtllen);
  if (test->entpra)
    {
      gcry_drbg_string_fill (&testentropy, test->entpra, test->entprlen);
      drbg->test_data = &test_data;
    }
  gcry_drbg_generate_long (drbg, buf, test->expectedlen, &addtl);

  gcry_drbg_string_fill (&addtl, test->addtlb, test->addtllen);
  if (test->entprb)
    {
      gcry_drbg_string_fill (&testentropy, test->entprb, test->entprlen);
      drbg->test_data = &test_data;
    }
  gcry_drbg_generate_long (drbg, buf, test->expectedlen, &addtl);
  gcry_drbg_uninstantiate (drbg);

outbuf:
  xfree (drbg);
  return ret;
}

/*
 * Invoke the CAVS test and perform the final check whether the
 * calculated random value matches the expected one.
 *
 * This function is not static as it is needed for as a private API
 * call for the CAVS test tool.
 */
gpg_err_code_t
gcry_drbg_healthcheck_one (struct gcry_drbg_test_vector * test)
{
  gpg_err_code_t ret = GPG_ERR_ENOMEM;
  unsigned char *buf = xcalloc_secure (1, test->expectedlen);
  if (!buf)
    return GPG_ERR_ENOMEM;

  ret = gcry_drbg_cavs_test (test, buf);
  ret = memcmp (test->expected, buf, test->expectedlen);

  xfree (buf);
  return ret;
}

/*
 * Tests as defined in 11.3.2 in addition to the cipher tests: testing
 * of the error handling.
 *
 * Note, testing the reseed counter is not done as an automatic reseeding
 * is performed in gcry_drbg_generate when the reseed counter is too large.
 */
static gpg_err_code_t
gcry_drbg_healthcheck_sanity (struct gcry_drbg_test_vector *test)
{
  unsigned int len = 0;
  struct gcry_drbg_state *drbg = NULL;
  gpg_err_code_t ret = GPG_ERR_GENERAL;
  gpg_err_code_t tmpret = GPG_ERR_GENERAL;
  struct gcry_drbg_test_data test_data;
  struct gcry_drbg_string addtl, testentropy;
  int coreref = 0;
  unsigned char *buf = NULL;
  size_t max_addtllen, max_request_bytes;

  /* only perform test in FIPS mode */
  if (0 == fips_mode ())
    return 0;

  buf = xcalloc_secure (1, test->expectedlen);
  if (!buf)
    return GPG_ERR_ENOMEM;
  tmpret = gcry_drbg_algo_available (test->flags, &coreref);
  if (tmpret)
    goto outbuf;
  drbg = xcalloc_secure (1, sizeof (struct gcry_drbg_state));
  if (!drbg)
    goto outbuf;

  /* if the following tests fail, it is likely that there is a buffer
   * overflow and we get a SIGSEV */
  ret = gcry_drbg_instantiate (drbg, NULL, coreref, 1);
  if (ret)
    goto outbuf;
  max_addtllen = gcry_drbg_max_addtl ();
  max_request_bytes = gcry_drbg_max_request_bytes ();
  /* overflow addtllen with additonal info string */
  gcry_drbg_string_fill (&addtl, test->addtla, (max_addtllen + 1));
  len = gcry_drbg_generate (drbg, buf, test->expectedlen, &addtl);
  if (len)
    goto outdrbg;

  /* overflow max_bits */
  len = gcry_drbg_generate (drbg, buf, (max_request_bytes + 1), NULL);
  if (len)
    goto outdrbg;
  gcry_drbg_uninstantiate (drbg);

  /* test failing entropy source as defined in 11.3.2 */
  test_data.testentropy = NULL;
  test_data.fail_seed_source = 1;
  drbg->test_data = &test_data;
  tmpret = gcry_drbg_instantiate (drbg, NULL, coreref, 0);
  if (!tmpret)
    goto outdrbg;
  test_data.fail_seed_source = 0;

  test_data.testentropy = &testentropy;
  gcry_drbg_string_fill (&testentropy, test->entropy, test->entropylen);
  /* overflow max addtllen with personalization string */
  tmpret = gcry_drbg_instantiate (drbg, &addtl, coreref, 0);
  if (!tmpret)
    goto outdrbg;

  dbg (("DRBG: Sanity tests for failure code paths successfully completed\n"));
  ret = 0;

outdrbg:
  gcry_drbg_uninstantiate (drbg);
outbuf:
  xfree (buf);
  xfree (drbg);
  return ret;
}

/*
 * DRBG Healthcheck function as required in SP800-90A
 *
 * return:
 * 	0 on success (all tests pass)
 * 	>0 on error (return code indicate the number of failures)
 */
static int
gcry_drbg_healthcheck (void)
{
  int ret = 0;
  ret += gcry_drbg_healthcheck_one (&gcry_drbg_test_nopr[0]);
  ret += gcry_drbg_healthcheck_one (&gcry_drbg_test_nopr[1]);
  ret += gcry_drbg_healthcheck_one (&gcry_drbg_test_nopr[2]);
  ret += gcry_drbg_healthcheck_one (&gcry_drbg_test_nopr[3]);
  ret += gcry_drbg_healthcheck_one (&gcry_drbg_test_nopr[4]);
  ret += gcry_drbg_healthcheck_one (&gcry_drbg_test_pr[0]);
  ret += gcry_drbg_healthcheck_one (&gcry_drbg_test_pr[1]);
  ret += gcry_drbg_healthcheck_one (&gcry_drbg_test_pr[2]);
  ret += gcry_drbg_healthcheck_sanity (&gcry_drbg_test_nopr[0]);
  return ret;
}

/* Run the self-tests.  */
gcry_error_t
_gcry_drbg_selftest (selftest_report_func_t report)
{
  gcry_err_code_t ec;
  const char *errtxt = NULL;
  gcry_drbg_lock ();
  if (0 != gcry_drbg_healthcheck ())
    errtxt = "RNG output does not match known value";
  gcry_drbg_unlock ();
  if (report && errtxt)
    report ("random", 0, "KAT", errtxt);
  ec = errtxt ? GPG_ERR_SELFTEST_FAILED : 0;
  return gpg_error (ec);
}

/***************************************************************
 * Cipher invocations requested by DRBG
 ***************************************************************/

static gpg_err_code_t
gcry_drbg_hmac (struct gcry_drbg_state *drbg, const unsigned char *key,
		unsigned char *outval, const struct gcry_drbg_string *buf)
{
  gpg_error_t err;
  gcry_md_hd_t hd;

  if (key)
    {
      err =
	_gcry_md_open (&hd, drbg->core->backend_cipher, GCRY_MD_FLAG_HMAC);
      if (err)
	return err;
      err = _gcry_md_setkey (hd, key, gcry_drbg_statelen (drbg));
      if (err)
	return err;
    }
  else
    {
      err = _gcry_md_open (&hd, drbg->core->backend_cipher, 0);
      if (err)
	return err;
    }
  for (; NULL != buf; buf = buf->next)
    _gcry_md_write (hd, buf->buf, buf->len);
  _gcry_md_final (hd);
  memcpy (outval, _gcry_md_read (hd, drbg->core->backend_cipher),
	  gcry_drbg_blocklen (drbg));
  _gcry_md_close (hd);
  return 0;
}

static gpg_err_code_t
gcry_drbg_sym (struct gcry_drbg_state *drbg, const unsigned char *key,
	       unsigned char *outval, const struct gcry_drbg_string *buf)
{
  gpg_error_t err;
  gcry_cipher_hd_t hd;

  err =
    _gcry_cipher_open (&hd, drbg->core->backend_cipher, GCRY_CIPHER_MODE_ECB,
		       0);
  if (err)
    return err;
  if (gcry_drbg_blocklen (drbg) !=
      _gcry_cipher_get_algo_blklen (drbg->core->backend_cipher))
    return -GPG_ERR_NO_ERROR;
  if (gcry_drbg_blocklen (drbg) < buf->len)
    return -GPG_ERR_NO_ERROR;
  err = _gcry_cipher_setkey (hd, key, gcry_drbg_keylen (drbg));
  if (err)
    return err;
  /* in is only component */
  _gcry_cipher_encrypt (hd, outval, gcry_drbg_blocklen (drbg), buf->buf,
			buf->len);
  _gcry_cipher_close (hd);
  return 0;
}
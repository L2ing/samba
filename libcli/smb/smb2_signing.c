/*
   Unix SMB/CIFS implementation.
   SMB2 signing

   Copyright (C) Stefan Metzmacher 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "system/filesys.h"
#include "../libcli/smb/smb_common.h"
#include "../lib/crypto/crypto.h"
#include "lib/util/iov_buf.h"

#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>

int smb2_signing_key_destructor(struct smb2_signing_key *key)
{
	if (key->hmac_hnd != NULL) {
		gnutls_hmac_deinit(key->hmac_hnd, NULL);
		key->hmac_hnd = NULL;
	}

	return 0;
}

bool smb2_signing_key_valid(const struct smb2_signing_key *key)
{
	if (key == NULL) {
		return false;
	}

	if (key->blob.length == 0 || key->blob.data == NULL) {
		return false;
	}

	return true;
}

NTSTATUS smb2_signing_sign_pdu(struct smb2_signing_key *signing_key,
			       enum protocol_types protocol,
			       struct iovec *vector,
			       int count)
{
	uint8_t *hdr;
	uint64_t session_id;
	uint8_t res[16];
	int i;

	if (count < 2) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (vector[0].iov_len != SMB2_HDR_BODY) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	hdr = (uint8_t *)vector[0].iov_base;

	session_id = BVAL(hdr, SMB2_HDR_SESSION_ID);
	if (session_id == 0) {
		/*
		 * do not sign messages with a zero session_id.
		 * See MS-SMB2 3.2.4.1.1
		 */
		return NT_STATUS_OK;
	}

	if (!smb2_signing_key_valid(signing_key)) {
		DBG_WARNING("Wrong session key length %zu for SMB2 signing\n",
			    signing_key->blob.length);
		return NT_STATUS_ACCESS_DENIED;
	}

	memset(hdr + SMB2_HDR_SIGNATURE, 0, 16);

	SIVAL(hdr, SMB2_HDR_FLAGS, IVAL(hdr, SMB2_HDR_FLAGS) | SMB2_HDR_FLAG_SIGNED);

	if (protocol >= PROTOCOL_SMB2_24) {
		struct aes_cmac_128_context ctx;
		uint8_t key[AES_BLOCK_SIZE] = {0};

		memcpy(key,
		       signing_key->blob.data,
		       MIN(signing_key->blob.length, 16));

		aes_cmac_128_init(&ctx, key);
		for (i=0; i < count; i++) {
			aes_cmac_128_update(&ctx,
					(const uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
		}
		aes_cmac_128_final(&ctx, res);

		ZERO_ARRAY(key);
	} else {
		uint8_t digest[gnutls_hmac_get_len(GNUTLS_MAC_SHA256)];
		int rc;

		if (signing_key->hmac_hnd == NULL) {
			rc = gnutls_hmac_init(&signing_key->hmac_hnd,
					      GNUTLS_MAC_SHA256,
					      signing_key->blob.data,
					      MIN(signing_key->blob.length, 16));
			if (rc < 0) {
				return NT_STATUS_NO_MEMORY;
			}
		}

		for (i = 0; i < count; i++) {
			rc = gnutls_hmac(signing_key->hmac_hnd,
					 vector[i].iov_base,
					 vector[i].iov_len);
			if (rc < 0) {
				return NT_STATUS_NO_MEMORY;
			}
		}
		gnutls_hmac_output(signing_key->hmac_hnd, digest);
		memcpy(res, digest, sizeof(res));
	}
	DEBUG(5,("signed SMB2 message\n"));

	memcpy(hdr + SMB2_HDR_SIGNATURE, res, 16);

	return NT_STATUS_OK;
}

NTSTATUS smb2_signing_check_pdu(struct smb2_signing_key *signing_key,
				enum protocol_types protocol,
				const struct iovec *vector,
				int count)
{
	const uint8_t *hdr;
	const uint8_t *sig;
	uint64_t session_id;
	uint8_t res[16];
	static const uint8_t zero_sig[16] = { 0, };
	int i;

	if (count < 2) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (vector[0].iov_len != SMB2_HDR_BODY) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	hdr = (const uint8_t *)vector[0].iov_base;

	session_id = BVAL(hdr, SMB2_HDR_SESSION_ID);
	if (session_id == 0) {
		/*
		 * do not sign messages with a zero session_id.
		 * See MS-SMB2 3.2.4.1.1
		 */
		return NT_STATUS_OK;
	}

	if (!smb2_signing_key_valid(signing_key)) {
		/* we don't have the session key yet */
		return NT_STATUS_OK;
	}

	sig = hdr+SMB2_HDR_SIGNATURE;

	if (protocol >= PROTOCOL_SMB2_24) {
		struct aes_cmac_128_context ctx;
		uint8_t key[AES_BLOCK_SIZE] = {0};

		memcpy(key,
		       signing_key->blob.data,
		       MIN(signing_key->blob.length, 16));

		aes_cmac_128_init(&ctx, key);
		aes_cmac_128_update(&ctx, hdr, SMB2_HDR_SIGNATURE);
		aes_cmac_128_update(&ctx, zero_sig, 16);
		for (i=1; i < count; i++) {
			aes_cmac_128_update(&ctx,
					(const uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
		}
		aes_cmac_128_final(&ctx, res);

		ZERO_ARRAY(key);
	} else {
		uint8_t digest[gnutls_hash_get_len(GNUTLS_MAC_SHA256)];
		int rc;

		if (signing_key->hmac_hnd == NULL) {
			rc = gnutls_hmac_init(&signing_key->hmac_hnd,
					      GNUTLS_MAC_SHA256,
					      signing_key->blob.data,
					      MIN(signing_key->blob.length, 16));
			if (rc < 0) {
				return NT_STATUS_NO_MEMORY;
			}
		}

		rc = gnutls_hmac(signing_key->hmac_hnd, hdr, SMB2_HDR_SIGNATURE);
		if (rc < 0) {
			return NT_STATUS_INTERNAL_ERROR;
		}
		rc = gnutls_hmac(signing_key->hmac_hnd, zero_sig, 16);
		if (rc < 0) {
			return NT_STATUS_INTERNAL_ERROR;
		}

		for (i = 1; i < count; i++) {
			rc = gnutls_hmac(signing_key->hmac_hnd,
					 vector[i].iov_base,
					 vector[i].iov_len);
			if (rc < 0) {
				return NT_STATUS_INTERNAL_ERROR;
			}
		}
		gnutls_hmac_output(signing_key->hmac_hnd, digest);
		memcpy(res, digest, 16);
		ZERO_ARRAY(digest);
	}

	if (memcmp_const_time(res, sig, 16) != 0) {
		DEBUG(0,("Bad SMB2 signature for message\n"));
		dump_data(0, sig, 16);
		dump_data(0, res, 16);
		return NT_STATUS_ACCESS_DENIED;
	}

	return NT_STATUS_OK;
}

void smb2_key_derivation(const uint8_t *KI, size_t KI_len,
			 const uint8_t *Label, size_t Label_len,
			 const uint8_t *Context, size_t Context_len,
			 uint8_t KO[16])
{
	gnutls_hmac_hd_t hmac_hnd = NULL;
	uint8_t buf[4];
	static const uint8_t zero = 0;
	uint8_t digest[gnutls_hash_get_len(GNUTLS_MAC_SHA256)];
	uint32_t i = 1;
	uint32_t L = 128;
	int rc;

	/*
	 * a simplified version of
	 * "NIST Special Publication 800-108" section 5.1
	 * using hmac-sha256.
	 */
	rc = gnutls_hmac_init(&hmac_hnd,
			      GNUTLS_MAC_SHA256,
			      KI,
			      KI_len);
	if (rc != 0) {
		return;
	}

	RSIVAL(buf, 0, i);
	rc = gnutls_hmac(hmac_hnd, buf, sizeof(buf));
	if (rc < 0) {
		gnutls_hmac_deinit(hmac_hnd, NULL);
		return;
	}
	rc = gnutls_hmac(hmac_hnd, Label, Label_len);
	if (rc < 0) {
		gnutls_hmac_deinit(hmac_hnd, NULL);
		return;
	}
	rc = gnutls_hmac(hmac_hnd, &zero, 1);
	if (rc < 0) {
		gnutls_hmac_deinit(hmac_hnd, NULL);
		return;
	}
	rc = gnutls_hmac(hmac_hnd, Context, Context_len);
	if (rc < 0) {
		gnutls_hmac_deinit(hmac_hnd, NULL);
		return;
	}
	RSIVAL(buf, 0, L);
	rc = gnutls_hmac(hmac_hnd, buf, sizeof(buf));
	if (rc < 0) {
		gnutls_hmac_deinit(hmac_hnd, NULL);
		return;
	}

	gnutls_hmac_deinit(hmac_hnd, digest);

	memcpy(KO, digest, 16);

	ZERO_ARRAY(digest);
}

NTSTATUS smb2_signing_encrypt_pdu(DATA_BLOB encryption_key,
				  uint16_t cipher_id,
				  struct iovec *vector,
				  int count)
{
	uint8_t *tf;
	uint8_t sig[16];
	int i;
	size_t a_total;
	ssize_t m_total;
	union {
		struct aes_ccm_128_context ccm;
		struct aes_gcm_128_context gcm;
	} c;
	uint8_t key[AES_BLOCK_SIZE];

	if (count < 1) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (vector[0].iov_len != SMB2_TF_HDR_SIZE) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	tf = (uint8_t *)vector[0].iov_base;

	if (encryption_key.length == 0) {
		DEBUG(2,("Wrong encryption key length %u for SMB2 signing\n",
			 (unsigned)encryption_key.length));
		return NT_STATUS_ACCESS_DENIED;
	}

	a_total = SMB2_TF_HDR_SIZE - SMB2_TF_NONCE;

	m_total = iov_buflen(&vector[1], count-1);
	if (m_total == -1) {
		return NT_STATUS_BUFFER_TOO_SMALL;
	}

	SSVAL(tf, SMB2_TF_FLAGS, SMB2_TF_FLAGS_ENCRYPTED);
	SIVAL(tf, SMB2_TF_MSG_SIZE, m_total);

	ZERO_STRUCT(key);
	memcpy(key, encryption_key.data,
	       MIN(encryption_key.length, AES_BLOCK_SIZE));

	switch (cipher_id) {
	case SMB2_ENCRYPTION_AES128_CCM:
		aes_ccm_128_init(&c.ccm, key,
				 tf + SMB2_TF_NONCE,
				 a_total, m_total);
		memset(tf + SMB2_TF_NONCE + AES_CCM_128_NONCE_SIZE, 0,
		       16 - AES_CCM_128_NONCE_SIZE);
		aes_ccm_128_update(&c.ccm, tf + SMB2_TF_NONCE, a_total);
		for (i=1; i < count; i++) {
			aes_ccm_128_update(&c.ccm,
					(const uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
			aes_ccm_128_crypt(&c.ccm,
					(uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
		}
		aes_ccm_128_digest(&c.ccm, sig);
		break;

	case SMB2_ENCRYPTION_AES128_GCM:
		aes_gcm_128_init(&c.gcm, key, tf + SMB2_TF_NONCE);
		memset(tf + SMB2_TF_NONCE + AES_GCM_128_IV_SIZE, 0,
		       16 - AES_GCM_128_IV_SIZE);
		aes_gcm_128_updateA(&c.gcm, tf + SMB2_TF_NONCE, a_total);
		for (i=1; i < count; i++) {
			aes_gcm_128_crypt(&c.gcm,
					(uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
			aes_gcm_128_updateC(&c.gcm,
					(const uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
		}
		aes_gcm_128_digest(&c.gcm, sig);
		break;

	default:
		ZERO_STRUCT(key);
		return NT_STATUS_INVALID_PARAMETER;
	}
	ZERO_STRUCT(key);

	memcpy(tf + SMB2_TF_SIGNATURE, sig, 16);

	DEBUG(5,("encrypt SMB2 message\n"));

	return NT_STATUS_OK;
}

NTSTATUS smb2_signing_decrypt_pdu(DATA_BLOB decryption_key,
				  uint16_t cipher_id,
				  struct iovec *vector,
				  int count)
{
	uint8_t *tf;
	uint16_t flags;
	uint8_t *sig_ptr = NULL;
	uint8_t sig[16];
	int i;
	size_t a_total;
	ssize_t m_total;
	uint32_t msg_size = 0;
	union {
		struct aes_ccm_128_context ccm;
		struct aes_gcm_128_context gcm;
	} c;
	uint8_t key[AES_BLOCK_SIZE];

	if (count < 1) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (vector[0].iov_len != SMB2_TF_HDR_SIZE) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	tf = (uint8_t *)vector[0].iov_base;

	if (decryption_key.length == 0) {
		DEBUG(2,("Wrong decryption key length %u for SMB2 signing\n",
			 (unsigned)decryption_key.length));
		return NT_STATUS_ACCESS_DENIED;
	}

	a_total = SMB2_TF_HDR_SIZE - SMB2_TF_NONCE;

	m_total = iov_buflen(&vector[1], count-1);
	if (m_total == -1) {
		return NT_STATUS_BUFFER_TOO_SMALL;
	}

	flags = SVAL(tf, SMB2_TF_FLAGS);
	msg_size = IVAL(tf, SMB2_TF_MSG_SIZE);

	if (flags != SMB2_TF_FLAGS_ENCRYPTED) {
		return NT_STATUS_ACCESS_DENIED;
	}

	if (msg_size != m_total) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	ZERO_STRUCT(key);
	memcpy(key, decryption_key.data,
	       MIN(decryption_key.length, AES_BLOCK_SIZE));

	switch (cipher_id) {
	case SMB2_ENCRYPTION_AES128_CCM:
		aes_ccm_128_init(&c.ccm, key,
				 tf + SMB2_TF_NONCE,
				 a_total, m_total);
		aes_ccm_128_update(&c.ccm, tf + SMB2_TF_NONCE, a_total);
		for (i=1; i < count; i++) {
			aes_ccm_128_crypt(&c.ccm,
					(uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
			aes_ccm_128_update(&c.ccm,
					( uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
		}
		aes_ccm_128_digest(&c.ccm, sig);
		break;

	case SMB2_ENCRYPTION_AES128_GCM:
		aes_gcm_128_init(&c.gcm, key, tf + SMB2_TF_NONCE);
		aes_gcm_128_updateA(&c.gcm, tf + SMB2_TF_NONCE, a_total);
		for (i=1; i < count; i++) {
			aes_gcm_128_updateC(&c.gcm,
					(const uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
			aes_gcm_128_crypt(&c.gcm,
					(uint8_t *)vector[i].iov_base,
					vector[i].iov_len);
		}
		aes_gcm_128_digest(&c.gcm, sig);
		break;

	default:
		ZERO_STRUCT(key);
		return NT_STATUS_INVALID_PARAMETER;
	}
	ZERO_STRUCT(key);

	sig_ptr = tf + SMB2_TF_SIGNATURE;
	if (memcmp(sig_ptr, sig, 16) != 0) {
		return NT_STATUS_ACCESS_DENIED;
	}

	DEBUG(5,("decrypt SMB2 message\n"));

	return NT_STATUS_OK;
}

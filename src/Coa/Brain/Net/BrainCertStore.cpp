/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainCertStore.h"

// wincrypt.h defines X509_NAME and friends as macros that break OpenSSL's declarations: undefine them before
// OpenSSL comes in. crypt32 is linked from here, so the CMake files need no change.
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#undef X509_NAME
#undef X509_EXTENSIONS
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#if defined(_MSC_VER)
#pragma comment(lib, "crypt32.lib")
#endif
#endif

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

uint32 BrainCertStore::AddSystemRoots(void* sslContext, std::string& error)
{
    error.clear();

#ifdef _WIN32
    if (!sslContext)
    {
        error = "no TLS context";
        return 0;
    }

    X509_STORE* store = SSL_CTX_get_cert_store(static_cast<SSL_CTX*>(sslContext));
    if (!store)
    {
        error = "the TLS context has no certificate store";
        return 0;
    }

    HCERTSTORE system = CertOpenSystemStoreW(0, L"ROOT");
    if (!system)
    {
        error = "cannot open the Windows ROOT certificate store (error " + std::to_string(GetLastError()) + ")";
        return 0;
    }

    uint32 added = 0;
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(system, cert)) != nullptr)
    {
        unsigned char const* bytes = cert->pbCertEncoded;
        X509* x509 = d2i_X509(nullptr, &bytes, long(cert->cbCertEncoded));
        if (!x509)
        {
            ERR_clear_error();
            continue;
        }
        // A certificate already in the store is refused by older OpenSSL versions: not an error here.
        if (X509_STORE_add_cert(store, x509) == 1)
            ++added;
        else
            ERR_clear_error();
        X509_free(x509);
    }
    CertCloseStore(system, 0);

    if (!added)
        error = "the Windows ROOT certificate store gave no usable certificate";
    return added;
#else
    (void)sslContext;
    return 0;
#endif
}

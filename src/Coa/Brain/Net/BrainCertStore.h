/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINCERTSTORE_H
#define PLAYERBOTS_BRAINCERTSTORE_H

#include "Define.h"
#include <string>

// OpenSSL on Windows knows no system roots, so hosted engines would fail every handshake without a CaBundle.
// BrainCertStore.cpp alone includes <windows.h>/<wincrypt.h> (whose X509_NAME macros clash with OpenSSL's) and
// links crypt32 with #pragma comment(lib, "crypt32.lib"), so no CMake change is needed.
namespace BrainCertStore
{
// Adds the certificates of the Windows "ROOT" system store to the X509_STORE of `sslContext` (an SSL_CTX*, passed
// untyped to keep OpenSSL out of this header). Returns how many were added. Elsewhere returns 0 with an empty
// `error`: callers there use SSL_CTX_set_default_verify_paths. Called once per client, on its worker thread.
uint32 AddSystemRoots(void* sslContext, std::string& error);
}  // namespace BrainCertStore

#endif

/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */
#include "client.h"
#include "errors.h"
#include "http.h"
#include "limits.h"
#include "mkcert.h"
#include "modules/logger.h"
#include "xml.h"
#include <ctype.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspiofilemgr_stat.h>
#include <pspkernel.h>

#include <Limelight.h>

extern int AppVersionQuad[4];
#define IS_SUNSHINE() (AppVersionQuad[3] < 0)

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>
#include <mbedtls/aes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LEN_AS_HEX_STR(x) ((x) * 2 + 1)
#define SIZEOF_AS_HEX_STR(x) LEN_AS_HEX_STR(sizeof(x))

static void RAND_bytes(unsigned char *buf, size_t len) {
  size_t i;
  unsigned long long time;
  for (i = 0; i < len; i++) {
    time = sceKernelGetSystemTimeWide();
    buf[i] = (unsigned char)((time >> (i % 8)) ^ (rand() % 256));
  }
}

static int mbedtls_entropy_rand(void *data, unsigned char *output, size_t len) {
  (void)data;
  RAND_bytes(output, len);
  return 0;
}

#define UNIQUE_FILE_NAME "uniqueid.dat"
#define UUID_FILE_NAME "uuid.dat"
#define P12_FILE_NAME "client.p12"

#define UNIQUEID_BYTES 8
#define UNIQUEID_CHARS (UNIQUEID_BYTES * 2)
#define UUID_STRLEN 37

static char unique_id[UNIQUEID_CHARS + 1] = {0};
static char uuid_str[UUID_STRLEN] = {0};
static char device_name[128] = "roth";

static mbedtls_x509_crt *cert;
static char cert_hex[16384];
static mbedtls_pk_context *privateKey;
static char countdown_msg[128]; // Global buffer for dynamic status updates

const char *gs_error;

static void bytes_to_hex(unsigned char *in, char *out, size_t len);
static void hex_to_bytes(const char *in, unsigned char *out, size_t len);

static int mkdirtree(const char *directory) {
  char buffer[PATH_MAX];
  char *p = buffer;

  if (directory == NULL || directory[0] == '\0')
    return -1;

  strncpy(buffer, directory, PATH_MAX - 1);
  buffer[PATH_MAX - 1] = '\0';

  /* Absolute Perfection: Cleanup path separators */
  for (char *c = buffer; *c; c++) {
    if (*c == '\\')
      *c = '/';
  }

  while (*p != 0) {
    if (*p == '/') {
      char oldChar = *p;
      *p = 0;

      /* Skip device prefixes like "ms0:", "host0:", or root "/" more robustly
       */
      char *colon = strchr(buffer, ':');
      bool should_skip = false;
      if (colon != NULL) {
        // If the buffer is just "device:" or "device:/", don't try to mkdir it
        if (colon[1] == '\0' || (colon[1] == oldChar && colon[2] == '\0')) {
          should_skip = true;
        }
      } else if (p == buffer) {
        // Skip root "/"
        should_skip = true;
      }

      if (!should_skip && buffer[0] != '\0') {
        SceIoStat stat;
        if (sceIoGetstat(buffer, &stat) < 0) {
          int res = sceIoMkdir(buffer, 0777);
          if (res == 0) {
            LOG_INFO(COMPONENT_NETWORK, "mkdirtree: Created directory %s",
                     buffer);
          } else if (res != (int)0x80010011) { // 0x80010011 is EEXIST
            LOG_ERROR(COMPONENT_NETWORK,
                      "mkdirtree: Failed to create %s (0x%08X)", buffer, res);
          }
        }
      }
      *p = oldChar;
      /* Absolute Perfection: Skip multiple consecutive slashes */
      while (*(p + 1) == '/')
        p++;
    }
    p++;
  }

  // Final component (if no trailing slash)
  SceIoStat stat;
  if (sceIoGetstat(buffer, &stat) < 0) {
    int res = sceIoMkdir(buffer, 0777);
    if (res == 0) {
      LOG_INFO(COMPONENT_NETWORK, "mkdirtree: Created final directory %s",
               buffer);
    } else if (res != (int)0x80010011) {
      LOG_ERROR(COMPONENT_NETWORK,
                "mkdirtree: Failed to create final %s (0x%08X)", buffer, res);
    }
  }

  /* Force a sync of the filesystem state */
  sceIoSync("ms0:", 0);

  return 0;
}

static int load_unique_id(const char *keyDirectory) {
  (void)keyDirectory;
  /* Standard Handheld Unique ID */
  strncpy(unique_id, "0123456789abcdef", UNIQUEID_CHARS);
  unique_id[UNIQUEID_CHARS] = '\0';
  LOG_INFO(COMPONENT_NETWORK, "Using static 'Standard' Unique ID: %s", unique_id);
  return 0;
}

static int load_uuid() {
  /* Standard Handheld UUID */
  strncpy(uuid_str, "01234567-89ab-cdef-0123-456789abcdef", UUID_STRLEN - 1);
  uuid_str[UUID_STRLEN - 1] = '\0';
  return 0;
}

static int load_device_name(const char *keyDirectory) {
  (void)keyDirectory;
  strncpy(device_name, "PSPMoonlight", sizeof(device_name));
  return 0;
}


static int load_cert(const char *keyDirectory) {
  // If cert and key are already loaded in memory, skip re-parsing.
  if (cert != NULL && privateKey != NULL && cert_hex[0] != '\0') {
    return GS_OK;
  }

  char certPath[PATH_MAX];
  snprintf(certPath, sizeof(certPath), "%s/%s", keyDirectory,
           CERTIFICATE_FILE_NAME);
  char p12Path[PATH_MAX];
  snprintf(p12Path, sizeof(p12Path), "%s/client.p12", keyDirectory);
  char keyPath[PATH_MAX];
  snprintf(keyPath, sizeof(keyPath), "%s/%s", keyDirectory, KEY_FILE_NAME);

  char derFile[PATH_MAX];
  strncpy(derFile, certPath, PATH_MAX - 1);
  char *dot = strrchr(derFile, '.');
  if (dot)
    strcpy(dot, ".der");
  else
    strcat(derFile, ".der");

  char pem_text[8192]; // PEM text (ASCII) for hex-encoding to Sunshine
  int pem_text_len = 0;
  bool pem_loaded = false;

  char der_buf[4096]; // DER binary for mbedtls internal parsing
  int der_len = 0;
  bool der_loaded = false;

  // Step 1: Try to load PEM text from client.pem
  SceUID fd = sceIoOpen(certPath, PSP_O_RDONLY, 0);
  if (fd >= 0) {
    pem_text_len = sceIoRead(fd, pem_text, sizeof(pem_text) - 1);
    sceIoClose(fd);
    if (pem_text_len > 0) {
      pem_text[pem_text_len] = '\0';
      pem_loaded = true;
      LOG_INFO(COMPONENT_NETWORK, "Loaded PEM certificate from %s (%d bytes)",
               certPath, pem_text_len);
    }
  }

  // Step 2: Try to load DER from client.der (for internal mbedtls parsing)
  fd = sceIoOpen(derFile, PSP_O_RDONLY, 0);
  if (fd >= 0) {
    der_len = sceIoRead(fd, der_buf, sizeof(der_buf));
    sceIoClose(fd);
    if (der_len > 0) {
      der_loaded = true;
      LOG_INFO(COMPONENT_NETWORK, "Loaded DER certificate from %s (%d bytes)",
               derFile, der_len);
    }
  }

  //  // Step 2: Absolute Perfection Certificate/Identity Synchronization
  // We store the unique_id used for the cert in 'cert_id.dat'
  char certIdPath[1024];
  snprintf(certIdPath, sizeof(certIdPath), "%s/cert_id.dat", keyDirectory);
  char saved_cert_id[UNIQUEID_CHARS + 1] = {0};
  fd = sceIoOpen(certIdPath, PSP_O_RDONLY, 0);
  if (fd >= 0) {
      sceIoRead(fd, saved_cert_id, UNIQUEID_CHARS);
      sceIoClose(fd);
  }

  bool identity_match = (strcmp(saved_cert_id, unique_id) == 0);
  
  // If files missing or identity changed, (re)generate everything
  if (!pem_loaded || !der_loaded || !identity_match) {
    LOG_INFO(COMPONENT_NETWORK, "Certificate/Identity mismatch or missing. (Re)generating for %s...", unique_id);
    CERT_KEY_PAIR cert_pair = mkcert_generate(unique_id);
    mkcert_save(certPath, p12Path, keyPath, cert_pair);
    mkcert_free(cert_pair);

    // Save the new cert_id to link this cert to this unique_id
    fd = sceIoOpen(certIdPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, unique_id, UNIQUEID_CHARS);
        sceIoClose(fd);
    }

    // Reload the new PEM
    fd = sceIoOpen(certPath, PSP_O_RDONLY, 0);
    if (fd >= 0) {
      pem_text_len = sceIoRead(fd, pem_text, sizeof(pem_text) - 1);
      sceIoClose(fd);
      if (pem_text_len > 0) {
        pem_text[pem_text_len] = '\0';
        pem_loaded = true;
      }
    }
    // Reload the new DER
    fd = sceIoOpen(derFile, PSP_O_RDONLY, 0);
    if (fd >= 0) {
      der_len = sceIoRead(fd, der_buf, sizeof(der_buf));
      sceIoClose(fd);
      if (der_len > 0) der_loaded = true;
    }
  }

  if (!pem_loaded || !der_loaded) {
    gs_error = "Can't load or generate certificate";
    return GS_FAILED;
  }

  // Sunshine/GFE Protocol: Use hex-encoded PEM (text) certificate for Step 1
  // pairing. (The user found that DER binary hex-encoding was a root cause of 400 errors).
  int length = 0;
  if (pem_loaded) {
    for (int i = 0; i < pem_text_len && length < (int)sizeof(cert_hex) - 2; i++) {
      snprintf(cert_hex + length, 3, "%02x", (unsigned char)pem_text[i]);
      length += 2;
    }
    cert_hex[length] = '\0';
    LOG_INFO(COMPONENT_NETWORK,
             "cert_hex built from PEM text (%d hex chars from %d PEM bytes)",
             length, pem_text_len);
  } else {
    // DER (binary) hex fallback for old GFE/Sunshine versions, though modern requires PEM.
    for (int i = 0; i < der_len && length < (int)sizeof(cert_hex) - 2;
         i++) {
      snprintf(cert_hex + length, 3, "%02x", (unsigned char)der_buf[i]);
      length += 2;
    }
    cert_hex[length] = '\0';
    LOG_INFO(COMPONENT_NETWORK,
             "cert_hex built from DER binary (%d hex chars from %d DER bytes)",
             length, der_len);
  }

  // Parse DER (or PEM) into the global 'cert' for internal TLS use
  if (cert == NULL) {
    cert = malloc(sizeof(mbedtls_x509_crt));
    mbedtls_x509_crt_init(cert);

  } else {
    mbedtls_x509_crt_free(cert);
    mbedtls_x509_crt_init(cert);
  }

  int crt_parse_res;
  if (der_loaded) {
    crt_parse_res =
        mbedtls_x509_crt_parse(cert, (unsigned char *)der_buf, der_len);
  } else {
    // Fallback: parse PEM directly (needs null terminator in length)
    crt_parse_res = mbedtls_x509_crt_parse(cert, (unsigned char *)pem_text,
                                           pem_text_len + 1);
  }
  if (crt_parse_res != 0) {
    char err_buf[128];
    mbedtls_strerror(crt_parse_res, err_buf, sizeof(err_buf));
    LOG_ERROR(COMPONENT_NETWORK, "mbedtls_x509_crt_parse FAILED: -0x%04x (%s)",
              -crt_parse_res, err_buf);
    gs_error = "Error parsing certificate buffer";
    return GS_FAILED;
  }

  char subject_name[256];
  mbedtls_x509_dn_gets(subject_name, sizeof(subject_name), &cert->subject);
  LOG_INFO(COMPONENT_NETWORK, "Loaded Certificate Subject: %s", subject_name);

  // Now load the key from the file using sceIo to avoid fopen issues
  SceUID key_fd = sceIoOpen(keyPath, PSP_O_RDONLY, 0);
  if (key_fd >= 0) {
    char key_pem[8192];
    memset(key_pem, 0, sizeof(key_pem));
    int key_read = sceIoRead(key_fd, key_pem, sizeof(key_pem) - 1);
    sceIoClose(key_fd);

    if (key_read > 0) {
      if (privateKey == NULL) {
        privateKey = malloc(sizeof(mbedtls_pk_context));
        mbedtls_pk_init(privateKey);
      }
      // Note: mbedtls_pk_parse_key needs the null terminator for PEM, which we
      // have from memset
      int pk_res = mbedtls_pk_parse_key(privateKey, (unsigned char *)key_pem,
                                        key_read + 1, NULL, 0,
                                        mbedtls_ctr_drbg_random, NULL);
      if (pk_res != 0) {
        char err_buf[128];
        mbedtls_strerror(pk_res, err_buf, sizeof(err_buf));
        LOG_ERROR(COMPONENT_NETWORK,
                  "mbedtls_pk_parse_key FAILED: -0x%04x (%s)", -pk_res,
                  err_buf);
        gs_error = "Error parsing private key buffer";
        return GS_FAILED;
      }
    } else {
      LOG_ERROR(COMPONENT_NETWORK, "Error reading key file: %d", key_read);
      gs_error = "Error reading key file";
      return GS_FAILED;
    }
  } else {
    gs_error = "Can't open key file";
    return GS_FAILED;
  }

  return GS_OK;
}
static int load_serverinfo(PSERVER_DATA server, bool https) {
  char url[4096];
  int ret = GS_INVALID;
  char *pairedText = NULL;
  char *currentGameText = NULL;
  char *stateText = NULL;
  char *serverCodecModeSupportText = NULL;
  char *httpsPortText = NULL;
  FILE *flog = NULL;

  snprintf(url, sizeof(url), "%s://%s:%d/serverinfo?uniqueid=%s&uuid=%s",
           https ? "https" : "http", server->serverInfo.address,
           https ? server->httpsPort : server->httpPort, unique_id, uuid_str);

  PHTTP_DATA data = http_create_data();
  if (data == NULL) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (http_request(url, data) != GS_OK) {
    if ((flog = fopen("moonlight_debug.log", "a"))) {
      fprintf(flog, "[CLIENT] http_request failed for: %s\n", url);
      fclose(flog);
    }
    ret = GS_IO_ERROR;
    gs_error = "Server connection failed (IO Error)";
    goto cleanup;
  }

  flog = fopen("moonlight_debug.log", "a");
  if (flog) {
    fprintf(flog, "XML payload received:\n%s\n", data->memory);
    fclose(flog);
  }

  if (xml_status(data->memory, data->size) == GS_ERROR) {
    if ((flog = fopen("moonlight_debug.log", "a"))) {
      fprintf(flog, "[CLIENT] xml_status check failed for: %s\n", url);
      fclose(flog);
    }
    ret = GS_ERROR;
    gs_error = "Invalid server response (XML Status)";
    goto cleanup;
  }

  if (xml_search(data->memory, data->size, "currentgame", &currentGameText) !=
      GS_OK) {
    goto cleanup;
  }

  if (xml_search(data->memory, data->size, "PairStatus", &pairedText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "appversion",
                 (char **)&server->serverInfo.serverInfoAppVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "state", &stateText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "ServerCodecModeSupport",
                 &serverCodecModeSupportText) != GS_OK)
    goto cleanup;

  if (server->gpuType) {
    free(server->gpuType);
    server->gpuType = NULL;
  }
  if (xml_search(data->memory, data->size, "gputype", &server->gpuType) !=
      GS_OK)
    goto cleanup;

  if (server->gsVersion) {
    free(server->gsVersion);
    server->gsVersion = NULL;
  }
  if (xml_search(data->memory, data->size, "GsVersion", &server->gsVersion) !=
      GS_OK)
    goto cleanup;

  if (server->serverInfo.serverInfoGfeVersion) {
    free((void *)server->serverInfo.serverInfoGfeVersion);
    server->serverInfo.serverInfoGfeVersion = NULL;
  }
  if (xml_search(data->memory, data->size, "GfeVersion",
                 (char **)&server->serverInfo.serverInfoGfeVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "HttpsPort", &httpsPortText) !=
      GS_OK)
    goto cleanup;

  if (server->modes) {
    xml_free_mode_list(server->modes);
    server->modes = NULL;
  }
  if (xml_modelist(data->memory, data->size, &server->modes) != GS_OK)
    goto cleanup;

  FILE *flog2 = fopen("moonlight_debug.log", "a");
  if (flog2) {
    fprintf(flog2,
            "currentGameText: '%s', pairedText: '%s', appVersion: '%s', "
            "stateText: '%s'\n",
            currentGameText, pairedText,
            server->serverInfo.serverInfoAppVersion, stateText);
    fclose(flog2);
  }

  // These fields are present on all version of GFE that this client supports
  if (!strlen(currentGameText) || !strlen(pairedText) ||
      !strlen(server->serverInfo.serverInfoAppVersion) || !strlen(stateText))
    goto cleanup;

  if (strstr(stateText, "MJOLNIR") == NULL) {
    // Sunshine detection - force a fake version that triggers modern features
    // and IS_SUNSHINE(). We use -1 for the last quad to trigger IS_SUNSHINE()
    // in moonlight-common-c.
    LOG_INFO(
        COMPONENT_NETWORK,
        "Sunshine detected! Forcing version 7.1.431.-1 for compatibility.");
    free((void *)server->serverInfo.serverInfoAppVersion);
    server->serverInfo.serverInfoAppVersion = strdup("7.1.431.-1");
    // Standard Forensic Repair: Explicitly set the Sunshine flag for
    // IS_SUNSHINE() macro
    AppVersionQuad[3] = -1;
  }

  server->paired = pairedText != NULL && strcmp(pairedText, "1") == 0;
  server->currentGame = currentGameText == NULL ? 0 : atoi(currentGameText);
  server->serverInfo.serverCodecModeSupport =
      serverCodecModeSupportText == NULL ? SCM_H264
                                         : atoi(serverCodecModeSupportText);
  server->serverMajorVersion = atoi(server->serverInfo.serverInfoAppVersion);
  server->isNvidiaSoftware = strstr(stateText, "MJOLNIR") != NULL;

  server->httpsPort = atoi(httpsPortText);
  if (!server->httpsPort)
    server->httpsPort = 47984;

  /* Note: Sunshine doesn't always signal MJOLNIR_SERVER_BUSY when an app is
   * open but not streaming. We preserve server->currentGame if it's non-zero in
   * XML. */
  if (strstr(stateText, "_SERVER_BUSY") == NULL && server->currentGame == 0) {
    // Force to zero only if BOTH server says FREE AND currentgame tag was 0
    server->currentGame = 0;
  }
  ret = GS_OK;

cleanup:
  if (data != NULL)
    http_free_data(data);

  if (pairedText != NULL)
    free(pairedText);

  if (currentGameText != NULL)
    free(currentGameText);

  if (serverCodecModeSupportText != NULL)
    free(serverCodecModeSupportText);

  if (httpsPortText != NULL)
    free(httpsPortText);

  return ret;
}

static int load_server_status(PSERVER_DATA server) {
  int ret;
  int i;

  /* Default HTTPS port to 47984 if not yet discovered.
     The first load_serverinfo(http) call will populate server->httpsPort
     from the XML. If it is still 0 after that, keep 47984 as fallback. */
  if (!server->httpsPort) {
    server->httpsPort = 47984;
  }

  // First try HTTP to get the real HTTPS port and basic server info.
  // Modern GFE/Sunshine don't allow serverinfo over HTTPS until paired.
  // We try HTTP first, then HTTPS (for paired status accuracy).
  // Modern GFE versions don't allow serverinfo to be fetched over HTTPS if the
  // client is not already paired. Since we can't pair without knowing the
  // server version, we make another request over HTTP if the HTTPS request
  // fails. We can't just use HTTP for everything because it doesn't accurately
  // tell us if we're paired.
  ret = GS_INVALID;
  for (i = 0; i < 2 && ret != GS_OK; i++) {
    bool use_https = (i == 1); /* Try HTTP first (fast), then HTTPS */
    LOG_INFO(COMPONENT_NETWORK, "Attempting serverinfo via %s...",
             use_https ? "HTTPS" : "HTTP");
    ret = load_serverinfo(server, use_https);
    if (ret != GS_OK) {
      LOG_INFO(COMPONENT_NETWORK, "  %s attempt failed (err=%d)",
               use_https ? "HTTPS" : "HTTP", ret);
    } else {
      LOG_INFO(COMPONENT_NETWORK, "  %s attempt SUCCESS!",
               use_https ? "HTTPS" : "HTTP");
    }
  }

  if (ret == GS_OK && !server->unsupported) {
    if (server->serverMajorVersion > MAX_SUPPORTED_GFE_VERSION) {
      gs_error = "Ensure you're running the latest version of Moonlight "
                 "Embedded or downgrade GeForce Experience and try again";
      ret = GS_UNSUPPORTED_VERSION;
    } else if (server->serverMajorVersion < MIN_SUPPORTED_GFE_VERSION) {
      gs_error = "Moonlight Embedded requires a newer version of GeForce "
                 "Experience. Please upgrade GFE on your PC and try again.";
      ret = GS_UNSUPPORTED_VERSION;
    }
  }

  return ret;
}

static void bytes_to_hex(unsigned char *in, char *out, size_t len) {
  size_t i;
  for (i = 0; i < len; i++) {
    sprintf(out + i * 2, "%02x", in[i]);
  }
  out[len * 2] = 0;
}

static unsigned char hex_char_to_val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return 0;
}

static void hex_to_bytes(const char *in, unsigned char *out, size_t len) {
  size_t count;
  for (count = 0; count < len; count += 2) {
    out[count / 2] =
        (hex_char_to_val(in[count]) << 4) | hex_char_to_val(in[count + 1]);
  }
}

static int sign_it(const unsigned char *msg, size_t mlen, unsigned char **sig,
                   size_t *slen, mbedtls_pk_context *pkey) {
  unsigned char hash[32];
  mbedtls_sha256(msg, mlen, hash, 0);

  *sig = malloc(MBEDTLS_PK_SIGNATURE_MAX_SIZE);
  if (*sig == NULL)
    return GS_FAILED;

  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_entropy_init(&entropy);

  // Forensic Repair: Use internal entropy source to ensure it works on PSP
  if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_rand, &entropy,
                            (const unsigned char *)"PSPMoonlightSign",
                            16) != 0) {
    free(*sig);
    *sig = NULL;
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return GS_FAILED;
  }

  if (mbedtls_pk_sign(pkey, MBEDTLS_MD_SHA256, hash, 32, *sig,
                      MBEDTLS_PK_SIGNATURE_MAX_SIZE, slen,
                      mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
    free(*sig);
    *sig = NULL;
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return GS_FAILED;
  }

  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return GS_OK;
}

static bool verifySignature(const unsigned char *data, int dataLength,
                            const unsigned char *signature, int signatureLength,
                            const char *cert, size_t certLen) {
  mbedtls_x509_crt x509;
  mbedtls_x509_crt_init(&x509);

  int parse_ret =
      mbedtls_x509_crt_parse(&x509, (const unsigned char *)cert, certLen);
  if (parse_ret != 0) {
    // Try parsing as PEM by including the null terminator in the length, as
    // required by mbedtls
    parse_ret =
        mbedtls_x509_crt_parse(&x509, (const unsigned char *)cert, certLen + 1);
  }

  if (parse_ret != 0) {
    FILE *fdbg = fopen("moonlight_debug.log", "a");
    if (fdbg) {
      fprintf(fdbg, "verifySignature: crt_parse failed: -0x%x\n", -parse_ret);
      fclose(fdbg);
    }
    mbedtls_x509_crt_free(&x509);
    return false;
  }

  unsigned char hash[32];
  mbedtls_sha256(data, dataLength, hash, 0);

  int result = mbedtls_pk_verify(&x509.pk, MBEDTLS_MD_SHA256, hash, 32,
                                 signature, signatureLength);
  if (result != 0) {
    FILE *fdbg = fopen("moonlight_debug.log", "a");
    if (fdbg) {
      fprintf(fdbg, "verifySignature: pk_verify failed: -0x%x\n", -result);
      fclose(fdbg);
    }
  }

  mbedtls_x509_crt_free(&x509);
  return result == 0;
}

static void encrypt(const unsigned char *plaintext, int plaintextLen,
                    const unsigned char *key, unsigned char *ciphertext) {
  mbedtls_aes_context ctx;
  int i;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);

  for (i = 0; i < plaintextLen; i += 16) {
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, plaintext + i,
                          ciphertext + i);
  }

  mbedtls_aes_free(&ctx);
}

static void decrypt(const unsigned char *ciphertext, int ciphertextLen,
                    const unsigned char *key, unsigned char *plaintext) {
  mbedtls_aes_context ctx;
  int i;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, key, 128);

  for (i = 0; i < ciphertextLen; i += 16) {
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, ciphertext + i,
                          plaintext + i);
  }

  mbedtls_aes_free(&ctx);
}

int gs_unpair(PSERVER_DATA server) {
  int ret = GS_OK;
  char url[4096];
  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  bool use_https = server->serverMajorVersion >= 7;
  snprintf(url, sizeof(url),
           "%s://%s:%u/unpair?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s",
           use_https ? "https" : "http", server->serverInfo.address,
           use_https ? server->httpsPort : server->httpPort, unique_id,
           uuid_str, unique_id, unique_id);
  ret = http_request(url, data);

  http_free_data(data);
  return ret;
}

int gs_pair(PSERVER_DATA server, char *pin) {
  int ret = GS_OK;
  char *result = NULL;
  size_t url_max_len = 16384;
  char *url = malloc(url_max_len);
  char *plaincert = NULL;
  char *challenge_response = NULL;
  char *pairing_secret = NULL;
  char *client_pairing_secret = NULL;
  char *client_pairing_secret_hex = NULL;
  PHTTP_DATA data = NULL;
  unsigned char salt_data[16];
  char salt_hex[33];
  unsigned char salt_pin[20];
  unsigned char aes_key[32];
  unsigned char challenge_data[16];
  unsigned char challenge_enc[16];
  char challenge_hex[33];
  char challenge_response_data_enc[65];
  char challenge_response_data[65];
  char client_secret_data[16];
  char challenge_response_hash[32];
  char challenge_response_hash_enc[32];
  char challenge_response_hex[65];
  char sig_start_hex[33];

  if (server->paired) {
    gs_error = "Already paired";
    ret = GS_WRONG_STATE;
    goto cleanup;
  }

  RAND_bytes(salt_data, sizeof(salt_data));
  bytes_to_hex(salt_data, salt_hex, sizeof(salt_data));

  snprintf(url, url_max_len,
           "http://%s:%u/"
           "pair?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s&updateState=1&"
           "phrase=getservercert&salt=%s&clientcert=%s",
           server->serverInfo.address, server->httpPort, unique_id, uuid_str,
           unique_id, unique_id, salt_hex, cert_hex);
  LOG_INFO(COMPONENT_NETWORK,
           "gs_pair: Step 1 - Sending getservercert (salt + clientcert)...");
  data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;
  else if ((ret = http_request(url, data)) != GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK, "gs_pair: Step 1 http_request FAILED ret=%d",
              ret);
    goto cleanup;
  }
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 1 response received (%d bytes)",
           data->size);

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) !=
           GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if ((ret = xml_search(data->memory, data->size, "plaincert", &result)) !=
      GS_OK)
    goto cleanup;

  int r_w = 0;
  int r_r;
  for (r_r = 0; result[r_r] != '\0'; r_r++) {
    if (!isspace((unsigned char)result[r_r])) {
      result[r_w++] = result[r_r];
    }
  }
  result[r_w] = '\0';

  size_t plaincertlen = strlen(result) / 2;
  plaincert = malloc(plaincertlen + 1);
  hex_to_bytes(result, (unsigned char *)plaincert, plaincertlen * 2);
  plaincert[plaincertlen] = 0;

  LOG_INFO(COMPONENT_NETWORK, "gs_pair: Server Major Version %d", server->serverMajorVersion);
  int is_gfe7 = (server->serverMajorVersion >= 7 || server->serverMajorVersion == 0); // Force 7+ for Sunshine (often reports 0 or 7)
  int hash_length = is_gfe7 ? 32 : 20;
  memcpy(salt_pin, salt_data, sizeof(salt_data));
  memcpy(salt_pin + sizeof(salt_data), pin, 4);
  if (is_gfe7) {
    LOG_INFO(COMPONENT_NETWORK, "gs_pair: Using GFE 7+ (SHA256/AES256) pairing logic");
    mbedtls_sha256((unsigned char *)salt_pin, sizeof(salt_pin), aes_key, 0);
  } else {
    LOG_INFO(COMPONENT_NETWORK, "gs_pair: Using GFE 3-6 (SHA1/AES128) pairing logic");
    mbedtls_sha1((unsigned char *)salt_pin, sizeof(salt_pin), aes_key);
  }

  RAND_bytes(challenge_data, sizeof(challenge_data));
  encrypt(challenge_data, sizeof(challenge_data), aes_key, challenge_enc);
  bytes_to_hex(challenge_enc, challenge_hex, sizeof(challenge_enc));

  snprintf(url, url_max_len,
           "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s"
           "&updateState=1&clientchallenge=%s",
           server->serverInfo.address, server->httpPort, unique_id, uuid_str,
           device_name, device_name, challenge_hex);
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 2 - Sending clientchallenge...");
  if ((ret = http_request(url, data)) != GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK, "gs_pair: Step 2 http_request FAILED ret=%d", ret);
    goto cleanup;
  }
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 2 response received (%d bytes)", data->size);
  if ((ret = xml_status(data->memory, data->size)) != GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK, "gs_pair: Step 2 XML status FAILED ret=%d", ret);
    goto cleanup;
  }
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 2 SUCCESS");
  if (data->size > 0 && data->size < 1024) {
    LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 2 XML: %s",
             (char *)data->memory);
  }

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) !=
           GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if ((ret = xml_search(data->memory, data->size, "challengeresponse",
                        &result)) != GS_OK) {
    ret = GS_INVALID;
    goto cleanup;
  }

  if (strlen(result) / 2 > sizeof(challenge_response_data_enc)) {
    gs_error = "Server challenge response too big";
    ret = GS_FAILED;
    goto cleanup;
  }

  hex_to_bytes(result, (unsigned char *)challenge_response_data_enc,
               strlen(result));
  decrypt((unsigned char *)challenge_response_data_enc, 64, aes_key,
          (unsigned char *)challenge_response_data);

  RAND_bytes((unsigned char *)client_secret_data, sizeof(client_secret_data));

  challenge_response = malloc(16 + cert->sig.len + sizeof(client_secret_data));
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: cert->sig.len = %d",
           (int)cert->sig.len);
  bytes_to_hex(cert->sig.p, sig_start_hex, 16);
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: cert->sig.p[0..15] = %s",
           sig_start_hex);

  /* serverchallenge = challenge_response_data[hash_length..hash_length+16] */
  char serverchallenge_hex[33] = {0};
  bytes_to_hex((unsigned char *)(challenge_response_data + hash_length),
               serverchallenge_hex, 16);
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: serverchallenge[0..15] = %s",
           serverchallenge_hex);

  memcpy(challenge_response, challenge_response_data + hash_length, 16);
  memcpy(challenge_response + 16, cert->sig.p, cert->sig.len);
  memcpy(challenge_response + 16 + cert->sig.len,
         (unsigned char *)client_secret_data, sizeof(client_secret_data));
  size_t cr_total = 16 + cert->sig.len + sizeof(client_secret_data);
  LOG_INFO(COMPONENT_NETWORK,
           "gs_pair: challenge_response total=%d (16 + %d + 16)", (int)cr_total,
           (int)cert->sig.len);
  if (server->serverMajorVersion >= 7)
    mbedtls_sha256((unsigned char *)challenge_response, cr_total,
                   (unsigned char *)challenge_response_hash, 0);
  else
    mbedtls_sha1((unsigned char *)challenge_response, cr_total,
                 (unsigned char *)challenge_response_hash);

  char hash_hex[65] = {0};
  bytes_to_hex((unsigned char *)challenge_response_hash, hash_hex, 32);
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: challenge_response_hash = %s",
           hash_hex);

  encrypt((unsigned char *)challenge_response_hash,
          sizeof(challenge_response_hash), aes_key,
          (unsigned char *)challenge_response_hash_enc);
  bytes_to_hex((unsigned char *)challenge_response_hash_enc,
               challenge_response_hex, sizeof(challenge_response_hash_enc));

  snprintf(url, url_max_len,
           "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s"
           "&updateState=1&phrase=pairsecretreply&serverchallengeresp=%s",
           server->serverInfo.address, server->httpPort, unique_id, uuid_str,
           device_name, device_name, challenge_response_hex);

  /* PSP WiFi: brief settle time after Step 2 TCP close before new connection */
  sceKernelDelayThread(200000); /* 200ms */

  LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 3 - Sending serverchallengeresp (PIN already accepted in Step 1)...");
  /* NOTE: Sunshine holds the Step 1 HTTP response until PIN is entered.
   * By the time we reach here, the PIN has already been verified.
   * Step 3 is a single-shot request — no polling needed. */
  if ((ret = http_request(url, data)) != GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK, "gs_pair: Step 3 http_request FAILED ret=%d", ret);
    goto cleanup;
  }

  LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 3 response received (%d bytes)",
           data->size);

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) !=
           GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if (xml_search(data->memory, data->size, "pairingsecret", &result) != GS_OK) {
    ret = GS_INVALID;
    goto cleanup;
  }

  size_t pairing_secret_len = strlen(result) / 2;
  if (pairing_secret_len <= 16) {
    ret = GS_INVALID;
    goto cleanup;
  }

  pairing_secret = malloc(pairing_secret_len);
  hex_to_bytes(result, (unsigned char *)pairing_secret, pairing_secret_len * 2);
  if (!verifySignature((unsigned char *)pairing_secret, 16,
                       (unsigned char *)pairing_secret + 16,
                       pairing_secret_len - 16, plaincert, plaincertlen)) {
    gs_error = "MITM attack detected";
    ret = GS_FAILED;
    goto cleanup;
  }

  unsigned char *signature = NULL;
  size_t s_len;
  if (sign_it((unsigned char *)client_secret_data, sizeof(client_secret_data),
              &signature, &s_len, privateKey) != GS_OK) {
    gs_error = "Failed to sign data";
    ret = GS_FAILED;
    goto cleanup;
  }

  client_pairing_secret = malloc(sizeof(client_secret_data) + s_len);
  client_pairing_secret_hex =
      malloc(LEN_AS_HEX_STR(sizeof(client_secret_data) + s_len));
  memcpy(client_pairing_secret, (unsigned char *)client_secret_data,
         sizeof(client_secret_data));
  memcpy(client_pairing_secret + sizeof(client_secret_data), signature, s_len);
  bytes_to_hex((unsigned char *)client_pairing_secret,
               client_pairing_secret_hex, sizeof(client_secret_data) + s_len);

  LOG_INFO(COMPONENT_NETWORK,
           "gs_pair: Step 4 - client_secret_data: %02X%02X%02X%02X...",
           (unsigned char)client_secret_data[0],
           (unsigned char)client_secret_data[1],
           (unsigned char)client_secret_data[2],
           (unsigned char)client_secret_data[3]);
  LOG_INFO(COMPONENT_NETWORK,
           "gs_pair: Step 4 - Signature length: %u, head: %02X%02X%02X%02X",
           (unsigned int)s_len, signature[0], signature[1], signature[2],
           signature[3]);

  snprintf(url, url_max_len,
           "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s"
           "&updateState=1&clientpairingsecret=%s",
           server->serverInfo.address, server->httpPort, unique_id, uuid_str,
           device_name, device_name, client_pairing_secret_hex);
  LOG_INFO(COMPONENT_NETWORK,
           "gs_pair: Step 4 - Sending clientpairingsecret...");
  if ((ret = http_request(url, data)) != GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK, "gs_pair: Step 4 http_request FAILED ret=%d",
              ret);
    goto cleanup;
  }
  LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 4 response received (%d bytes)",
           data->size);
  if (data->size > 0) {
    char log_snippet[512];
    memset(log_snippet, 0, sizeof(log_snippet));
    int copy_len = data->size < 511 ? data->size : 511;
    memcpy(log_snippet, data->memory, copy_len);
    LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 4 Response Payload: %s",
             log_snippet);
  }

  char *status_msg = NULL;
  xml_search(data->memory, data->size, "status_message", &status_msg);
  if (status_msg) {
    LOG_ERROR(COMPONENT_NETWORK, "Sunshine Error: %s", status_msg);
    gs_error = status_msg; // Pass to UI
  }

  free(result);
  result = NULL;
  if (data->size > 0 && data->size < 4096) {
    LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 4 XML: %s",
             (char *)data->memory);
  }
  if ((ret = xml_status(data->memory, data->size)) != GS_OK) {
    if (status_msg && strlen(status_msg) > 0) {
      LOG_INFO(COMPONENT_NETWORK,
               "gs_pair: Step 4 XML status failed (%d), Sunshine Error: %s",
               ret, status_msg);
      gs_error = status_msg;

      if (IS_SUNSHINE()) {
        ret = GS_FAILED;
        goto cleanup;
      } else {
        ret = GS_FAILED;
        goto cleanup;
      }
    } else {
      LOG_INFO(COMPONENT_NETWORK,
               "gs_pair: Step 4 XML status failed (%d), checking for Sunshine "
               "manual approval",
               ret);
      if (IS_SUNSHINE()) {
        for (int i = 120; i > 0; i--) {
          snprintf(countdown_msg, sizeof(countdown_msg),
                   "Sunshine needs approval. Waiting... (%ds)", i);
          gs_error = countdown_msg;

          HTTP_DATA tmp_data;
          tmp_data.memory = malloc(1);
          tmp_data.size = 0;

          const char *saved_error = gs_error;
          if (http_request(url, &tmp_data) == GS_OK) {
            if (tmp_data.size > 0) {
              LOG_INFO(COMPONENT_NETWORK, "Manual Poll Response: %s",
                       (char *)tmp_data.memory);
            }
            char *tmp_result = NULL;
            if (xml_search(tmp_data.memory, tmp_data.size, "paired",
                           &tmp_result) == GS_OK &&
                tmp_result && !strcmp(tmp_result, "1")) {
              LOG_INFO(COMPONENT_NETWORK,
                       "Manual approval detected! Pairing SUCCESS.");
              ret = GS_OK;
              result = "1"; // Force success for the next check
              free(tmp_data.memory);
              goto step5;
            }
          }
          gs_error = saved_error; // Restore the countdown message
          if (tmp_data.memory)
            free(tmp_data.memory);
          sceKernelDelayThread(1000 * 1000); // 1 second
        }
        LOG_ERROR(COMPONENT_NETWORK, "Manual approval TIMEOUT after 120s.");
        ret = GS_FAILED;
        goto cleanup;
      }
    }

  } else if ((ret = xml_search(data->memory, data->size, "paired", &result)) !=
             GS_OK) {
    LOG_INFO(COMPONENT_NETWORK,
             "gs_pair: Step 4 paired tag not found, but ignoring for Sunshine");
    ret = GS_OK;
  }

step5:; // Label now outside the block or with dummy statement
  if (result == NULL || strcmp(result, "1") != 0) {
    LOG_INFO(COMPONENT_NETWORK, "gs_pair: Step 4 returned paired=%s",
             result ? result : "NULL");
    if (!IS_SUNSHINE()) {
      ret = GS_FAILED;
    }
  }

  snprintf(url, url_max_len,
           "https://%s:%u/"
           "pair?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s&updateState=1&"
           "phrase=pairchallenge",
           server->serverInfo.address, server->httpsPort, unique_id, uuid_str,
           device_name, device_name);
  LOG_INFO(COMPONENT_NETWORK,
           "gs_pair: Final HTTPS pairchallenge URL: "
           "https://%s:%u/pair?...phrase=pairchallenge",
           server->serverInfo.address, server->httpsPort);
  LOG_INFO(COMPONENT_NETWORK,
           "gs_pair: Step 5 - Sending HTTPS pairchallenge (confirmation)...");
  if ((ret = http_request(url, data)) != GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK,
              "gs_pair: HTTPS pairchallenge FAILED (ret=%d). (non-fatal for "
              "Sunshine)",
              ret);
    /* Step 5 is confirmation. On Sunshine, Step 4 is often enough. */
    ret = GS_OK;
    server->paired = true;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK,
              "gs_pair: Step 5 XML status failed (non-fatal)");
    ret = GS_OK;
    server->paired = true;
    goto cleanup;
  } else if ((ret = xml_search(data->memory, data->size, "paired", &result)) !=
             GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK,
              "gs_pair: Step 5 paired tag not found (non-fatal)");
    ret = GS_OK;
    server->paired = true;
    goto cleanup;
  }

  if (result == NULL || strcmp(result, "1") != 0) {
    LOG_INFO(COMPONENT_NETWORK,
             "gs_pair: Step 5 returned paired=%s (non-fatal on Sunshine)",
             result ? result : "NULL");
    ret = GS_OK;
  }

  server->paired = true;

cleanup:
  /* Only call unpair if we actually failed Step 1-4. Step 5 is confirmation. */
  if (ret != GS_OK)
    gs_unpair(server);

  free(url);
  free(plaincert);
  free(challenge_response);
  free(pairing_secret);
  free(client_pairing_secret);
  free(client_pairing_secret_hex);
  free(result);

  http_free_data(data);

  // If we failed when attempting to pair with a game running, that's likely the
  // issue. Sunshine supports pairing with an active session, but GFE does not.
  if (ret != GS_OK && server->currentGame != 0) {
    gs_error = "The computer is currently in a game. You must close the game "
               "before pairing.";
    ret = GS_WRONG_STATE;
  }

  return ret;
}

int gs_applist(PSERVER_DATA server, PAPP_LIST *list) {
  int ret = GS_OK;
  char url[4096];
  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  snprintf(
      url, sizeof(url),
      "https://%s:%u/applist?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s",
      server->serverInfo.address, server->httpsPort, unique_id, uuid_str,
      unique_id, unique_id);
  if (http_request(url, data) != GS_OK)
    ret = GS_IO_ERROR;
  else {
    LOG_INFO(COMPONENT_NETWORK, "gs_applist response: %.256s",
             (char *)data->memory);
    if (xml_status(data->memory, data->size) == GS_ERROR) {
      LOG_INFO(COMPONENT_NETWORK,
               "gs_applist: XML status check failed (non-fatal for Sunshine)");
      if (xml_applist(data->memory, data->size, list) != GS_OK)
        ret = GS_INVALID;
      else
        ret = GS_OK;
    } else if (xml_applist(data->memory, data->size, list) != GS_OK) {
      ret = GS_INVALID;
    }
  }
  http_free_data(data);
  return ret;
}

int gs_start_app(PSERVER_DATA server, STREAM_CONFIGURATION *config, int appId,
                 bool sops, bool localaudio, int gamepad_mask) {
  int ret = GS_OK;
  char *result = NULL;

  PDISPLAY_MODE mode = server->modes;
  bool correct_mode = false;
  while (mode != NULL) {
    if (mode->width == (unsigned int)config->width &&
        mode->height == (unsigned int)config->height) {
      if (mode->refresh == (unsigned int)config->fps)
        correct_mode = true;
    }

    mode = mode->next;
  }

  if (!correct_mode && !server->unsupported) {
    // Moonlight-embedded normally rejects non-standard resolutions here.
    // However, we want to allow the PSP to request arbitrary resolutions (like
    // 360x204) to utilize Sunshine's "Virtual Display" feature. So we log it
    // and proceed anyway.
    correct_mode = true;
  }

  RAND_bytes((unsigned char *)config->remoteInputAesKey,
             sizeof(config->remoteInputAesKey));
  memset(config->remoteInputAesIv, 0, sizeof(config->remoteInputAesIv));

  char url[4096];
  uint32_t rikeyid = 0;
  RAND_bytes((unsigned char *)&rikeyid, sizeof(rikeyid));
  memcpy(config->remoteInputAesIv, &rikeyid, sizeof(rikeyid));
  rikeyid = htonl(rikeyid);
  char rikey_hex[SIZEOF_AS_HEX_STR(config->remoteInputAesKey)];
  bytes_to_hex((unsigned char *)config->remoteInputAesKey, rikey_hex,
               sizeof(config->remoteInputAesKey));

  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  int fps = config->fps;

  int surround_info =
      SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(config->audioConfiguration);

  /* Use the resolution requested by the caller (PSP defaults to
   * 480x272/360x204). Hardcoding 1280x720 here was a legacy hack that causes
   * PSP 1000 to crash due to bitstream overhead and RTSP timeouts. */
  int request_w = config->width;
  int request_h = config->height;

  snprintf(
      url, sizeof(url),
      "https://%s:%u/"
      "%s?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s&appid=%d&mode=%dx%dx%"
      "d&additionalStates=1&sops=%d&rikey=%s&rikeyid=%d&localAudioPlayMode=%d&"
      "surroundAudioInfo=%d&remoteControllersBitmap=%d&gcmap=%d%s",
      server->serverInfo.address, server->httpsPort,
      server->currentGame ? "resume" : "launch", unique_id, uuid_str, unique_id,
      unique_id, appId, request_w, request_h, fps, sops, rikey_hex, rikeyid,
      localaudio, surround_info, gamepad_mask, gamepad_mask,
      (config->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT)
          ? "&hdrMode=1&clientHdrCapVersion=0&"
            "clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_"
            "STATIC_METADATA_TYPE_1&clientHdrCapDisplayData="
            "0x0x0x0x0x0x0x0x0x0x0"
          : "");
  if ((ret = http_request(url, data)) == GS_OK)
    server->currentGame = appId;
  else
    goto cleanup;

  LOG_INFO(COMPONENT_NETWORK, "gs_start_app response: %s",
           (char *)data->memory);
  if ((ret = xml_status(data->memory, data->size)) != GS_OK) {
    LOG_INFO(COMPONENT_NETWORK, "gs_start_app: XML status check failed (%d).",
             ret);
    /* We don't force GS_OK here anymore to prevent dummy RTSP connections.
       We will still try to find sessionUrl0 below, and if found, we set ret =
       GS_OK. */
  }

  if ((ret = xml_search(data->memory, data->size, "gamesession", &result)) !=
          GS_OK &&
      (ret = xml_search(data->memory, data->size, "resume", &result)) !=
          GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK,
              "gs_start_app: Failed to find gamesession/resume tag");
    goto cleanup;
  }

  if (result == NULL || !strcmp(result, "0")) {
    LOG_ERROR(COMPONENT_NETWORK,
              "gs_start_app: gamesession/resume check failed (result=%s)",
              result ? result : "NULL");
    ret = GS_FAILED;
    if (result)
      free(result);
    goto cleanup;
  }

  free(result);
  result = NULL;

  if (xml_search(data->memory, data->size, "sessionUrl0", &result) == GS_OK &&
      result != NULL) {
    if (server->serverInfo.rtspSessionUrl) {
      free((char *)server->serverInfo.rtspSessionUrl);
    }
    server->serverInfo.rtspSessionUrl = result;
    LOG_INFO(COMPONENT_NETWORK, "gs_start_app: RTSP Session URL identified: %s",
             (char *)server->serverInfo.rtspSessionUrl);
    result = NULL;
  } else {
    /* If we have no session URL, we can't connect, so this is a failure even if
     * resume was 1 */
    LOG_ERROR(COMPONENT_NETWORK,
              "gs_start_app: Missing sessionUrl0 in response");
    ret = GS_FAILED;
  }

cleanup:

  if (result != NULL)
    free(result);

  http_free_data(data);
  return ret;
}

int gs_quit_app(PSERVER_DATA server) {
  int ret = GS_OK;
  char url[4096];
  char *result = NULL;
  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  snprintf(
      url, sizeof(url),
      "https://%s:%u/cancel?uniqueid=%s&uuid=%s&devicename=%s&clientname=%s",
      server->serverInfo.address, server->httpsPort, unique_id, uuid_str,
      "PSPMoonlight", "PSPMoonlight");
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "cancel", &result)) !=
           GS_OK)
    goto cleanup;

  if (result == NULL || strcmp(result, "0") == 0) {
    ret = GS_FAILED;
    if (result)
      free(result);
    goto cleanup;
  }

cleanup:
  if (result != NULL)
    free(result);

  http_free_data(data);
  return ret;
}

int gs_init(PSERVER_DATA server, char *address, unsigned short httpPort,
            const char *keyDirectory, int log_level, bool unsupported) {
  gs_cleanup(server);
  LOG_INFO(COMPONENT_NETWORK, "gs_init: keyDir=%s", keyDirectory);
  mkdirtree(keyDirectory);

  load_unique_id(keyDirectory);
  load_uuid();
  load_device_name(keyDirectory);

  if (load_cert(keyDirectory)) {
    LOG_ERROR(COMPONENT_NETWORK, "gs_init: load_cert FAILED");
    return GS_FAILED;
  }

  http_init(keyDirectory, log_level);

  LiInitializeServerInformation(&server->serverInfo);
  server->serverInfo.address = address;
  server->unsupported = unsupported;
  server->httpPort = httpPort ? httpPort : 47989;
  server->httpsPort = 0; /* Populated by load_server_status() */

  int ret = load_server_status(server);
  LOG_INFO(COMPONENT_NETWORK, "gs_init: server status=%d (paired=%d)", ret,
           server->paired);
  return ret;
}

void gs_cleanup(PSERVER_DATA server) {
  // NOTE: Do NOT free cert or privateKey here.
  // They must persist across gs_init retries so Sunshine recognizes
  // the same TLS identity that was used during pairing.
  // Freeing them causes load_cert to regenerate keys on the next
  // gs_init call, which invalidates the pairing and triggers
  // "Permission denied" from Sunshine.

  if (server == NULL)
    return;

  if (server->modes) {
    xml_free_mode_list(server->modes);
    server->modes = NULL;
  }

  if (server->gpuType != NULL) {
    free(server->gpuType);
    server->gpuType = NULL;
  }
  if (server->gsVersion != NULL) {
    free(server->gsVersion);
    server->gsVersion = NULL;
  }

  if (server->serverInfo.serverInfoAppVersion != NULL) {
    free((void *)server->serverInfo.serverInfoAppVersion);
    server->serverInfo.serverInfoAppVersion = NULL;
  }
  if (server->serverInfo.serverInfoGfeVersion != NULL) {
    free((void *)server->serverInfo.serverInfoGfeVersion);
    server->serverInfo.serverInfoGfeVersion = NULL;
  }
  if (server->serverInfo.rtspSessionUrl != NULL) {
    free((void *)server->serverInfo.rtspSessionUrl);
    server->serverInfo.rtspSessionUrl = NULL;
  }

  http_cleanup();
}

void gs_free_applist(PAPP_LIST app_list) { xml_free_app_list(app_list); }

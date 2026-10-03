#pragma once
// Smaarthi upload API (the four calls from references/Api_endpoints):
//   1. requestAssetUploadPresignedUrl  2. PUT to R2  3. createAsset  4. createConversation
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    API_OK,
    API_NET,        // no connection / timeout: retry next window
    API_SERVER,     // 5xx / 429: retry next window
    API_AUTH,       // 401/403: token problem, stop uploading
    API_REJECT,     // explicit validation error for this chunk: counts towards quarantine
    API_LOCAL,      // device-side problem (SD read, memory): skip this chunk this window
} api_result_t;

api_result_t api_request_upload_url(const char *token, const char *file_name, const char *mime,
                                    char **url_out, char *key_out, size_t key_len);
// Streams `path` to the presigned URL. Verifies R2's ETag against the MD5 of what was sent.
api_result_t api_put_file(const char *url, const char *path, const char *mime);
api_result_t api_create_asset(const char *token, const char *key, const char *mime, const char *file_name,
                              char *asset_id_out, size_t len);
// media_url_out receives the conversation's media URL when the backend returns one (else "").
api_result_t api_create_conversation(const char *token, const char *asset_id, char *conv_id_out, size_t len,
                                     char *media_url_out, size_t url_len);
const char *api_result_name(api_result_t r);

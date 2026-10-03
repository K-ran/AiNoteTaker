#include "smaarthi_api.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mbedtls/md5.h"
#include "cJSON.h"
#include "app_config.h"
#include "timekeep.h"

static const char *TAG = "api";

#define BODY_CAP 16384               // presigned-URL responses are ~1 KB; anything bigger is unexpected

const char *api_result_name(api_result_t r)
{
    static const char *n[] = {"ok", "network", "server", "auth", "rejected", "local"};
    return n[r];
}

typedef struct { char *buf; size_t len, cap; bool overflow; } body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Date")) {
        timekeep_from_http_date(e->header_value);     // certificate already verified at this point
    } else if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->len + e->data_len < b->cap) {
            memcpy(b->buf + b->len, e->data, e->data_len);
            b->len += e->data_len;
            b->buf[b->len] = 0;
        } else {
            b->overflow = true;
        }
    }
    return ESP_OK;
}

// Only explicit validation errors mean "this chunk is bad"; anything unknown is
// treated as a server problem and retried, so a backend hiccup never quarantines audio.
static api_result_t classify_graphql_error(cJSON *root, const char *field)
{
    cJSON *e0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "errors"), 0);
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e0, "message"));
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(e0, "extensions"), "code"));
    ESP_LOGW(TAG, "%s: GraphQL error code=%s msg=%.120s", field, code ? code : "-", msg ? msg : "(no data)");
    if (code && (!strcmp(code, "UNAUTHENTICATED") || !strcmp(code, "FORBIDDEN"))) return API_AUTH;
    if (code && (!strcmp(code, "BAD_USER_INPUT") || !strcmp(code, "GRAPHQL_VALIDATION_FAILED"))) return API_REJECT;
    return API_SERVER;
}

// POSTs a GraphQL request. On API_OK, *root owns the response and *obj is data.<field>.
static api_result_t graphql(const char *token, cJSON *req, const char *field, cJSON **root, cJSON **obj)
{
    *root = NULL;
    char *payload = cJSON_PrintUnformatted(req);
    body_t body = {.cap = BODY_CAP};
    body.buf = heap_caps_malloc(body.cap, MALLOC_CAP_SPIRAM);
    size_t auth_len = strlen(token) + 8;
    char *auth = malloc(auth_len);
    api_result_t r = API_NET;
    if (!payload || !body.buf || !auth) goto out;
    body.buf[0] = 0;
    snprintf(auth, auth_len, "Bearer %s", token);

    esp_http_client_config_t cfg = {
        .url = SMAARTHI_GRAPHQL_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,     // verify the server certificate
        .event_handler = on_http,
        .user_data = &body,
        .buffer_size_tx = 2048,                         // the bearer token is a long header
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) goto out;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_post_field(c, payload, strlen(payload));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) r = API_NET;
    else if (status == 401 || status == 403) r = API_AUTH;
    else if (status < 200 || status >= 300 || body.overflow) r = API_SERVER;   // incl. 4xx: retry, don't quarantine
    else {
        *root = cJSON_Parse(body.buf);
        *obj = cJSON_GetObjectItem(cJSON_GetObjectItem(*root, "data"), field);
        if (cJSON_IsObject(*obj)) r = API_OK;
        else {
            r = *root ? classify_graphql_error(*root, field) : API_SERVER;
            cJSON_Delete(*root);
            *root = NULL;
        }
    }
    if (r != API_OK && r != API_REJECT)
        ESP_LOGW(TAG, "%s: %s (HTTP %d, %s%s)", field, api_result_name(r), status, esp_err_to_name(err),
                 body.overflow ? ", body too large" : "");
out:
    if (auth) { explicit_bzero(auth, auth_len); free(auth); }
    free(payload);
    free(body.buf);
    return r;
}

static cJSON *request(const char *op, const char *query)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "operationName", op);
    cJSON_AddStringToObject(req, "query", query);
    return req;
}

static bool copy_str(cJSON *obj, const char *name, char *out, size_t len)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(obj, name));
    return v && strlcpy(out, v, len) < len;
}

api_result_t api_request_upload_url(const char *token, const char *file_name, const char *mime,
                                    char **url_out, char *key_out, size_t key_len)
{
    cJSON *req = request("RequestAssetUploadPresignedUrl",
        "query RequestAssetUploadPresignedUrl($mimeType: String!, $fileName: String!) {"
        " requestAssetUploadPresignedUrl(mimeType: $mimeType, fileName: $fileName) { expiresInSeconds key url } }");
    cJSON *vars = cJSON_AddObjectToObject(req, "variables");
    cJSON_AddStringToObject(vars, "fileName", file_name);
    cJSON_AddStringToObject(vars, "mimeType", mime);
    cJSON *root, *obj = NULL;
    api_result_t r = graphql(token, req, "requestAssetUploadPresignedUrl", &root, &obj);
    cJSON_Delete(req);
    if (r != API_OK) return r;
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "url"));
    if (!url || strncmp(url, "https://", 8) != 0) {           // never send audio in plaintext
        ESP_LOGW(TAG, "presigned URL missing or not HTTPS");
        r = API_SERVER;
    } else if (!copy_str(obj, "key", key_out, key_len)) {
        r = API_SERVER;
    } else if (!(*url_out = strdup(url))) {
        r = API_LOCAL;
    }
    cJSON_Delete(root);
    return r;
}

api_result_t api_create_asset(const char *token, const char *key, const char *mime, const char *file_name,
                              char *asset_id_out, size_t len)
{
    cJSON *req = request("CreateAsset",
        "mutation CreateAsset($key: String!, $mimeType: String!, $fileName: String!) {"
        " createAsset(key: $key, mimeType: $mimeType, fileName: $fileName) { id key } }");
    cJSON *vars = cJSON_AddObjectToObject(req, "variables");
    cJSON_AddStringToObject(vars, "key", key);
    cJSON_AddStringToObject(vars, "mimeType", mime);
    cJSON_AddStringToObject(vars, "fileName", file_name);
    cJSON *root, *obj = NULL;
    api_result_t r = graphql(token, req, "createAsset", &root, &obj);
    cJSON_Delete(req);
    if (r == API_OK && !copy_str(obj, "id", asset_id_out, len)) r = API_SERVER;
    cJSON_Delete(root);
    return r;
}

api_result_t api_create_conversation(const char *token, const char *asset_id, char *conv_id_out, size_t len,
                                     char *media_url_out, size_t url_len)
{
    cJSON *req = request("CreateConversation",
        "mutation CreateConversation($input: CreateConversationInput!) {"
        " createConversation(input: $input) { id media { url } } }");
    cJSON *input = cJSON_AddObjectToObject(cJSON_AddObjectToObject(req, "variables"), "input");
    cJSON_AddStringToObject(input, "mediaId", asset_id);
    cJSON *root, *obj = NULL;
    api_result_t r = graphql(token, req, "createConversation", &root, &obj);
    cJSON_Delete(req);
    if (r == API_OK && !copy_str(obj, "id", conv_id_out, len)) r = API_SERVER;
    if (r == API_OK && !copy_str(cJSON_GetObjectItem(obj, "media"), "url", media_url_out, url_len)) media_url_out[0] = 0;
    cJSON_Delete(root);
    return r;
}

// esp_http_client_get_header() reads request headers, so capture the response ETag here.
static esp_err_t on_put(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "ETag"))
        strlcpy(e->user_data, e->header_value, 48);
    return ESP_OK;
}

api_result_t api_put_file(const char *url, const char *path, const char *mime)
{
    struct stat st;
    if (stat(path, &st) != 0) return API_LOCAL;
    FILE *f = fopen(path, "rb");
    if (!f) return API_LOCAL;
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);  // SD reads DMA straight into it
    if (!buf) { fclose(f); return API_LOCAL; }

    char etag[48] = "";
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_PUT,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size_tx = 2048,                          // presigned URLs are ~800 bytes
        .event_handler = on_put,
        .user_data = etag,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    api_result_t r = API_NET;
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    long sent = 0;
    int status = 0;
    if (c && esp_http_client_open(c, st.st_size) == ESP_OK) {
        size_t n;
        while ((n = fread(buf, 1, 4096, f)) > 0) {
            if (esp_http_client_write(c, buf, n) != (int)n) break;
            mbedtls_md5_update(&md5, (const unsigned char *)buf, n);
            sent += n;
        }
        if (sent == st.st_size && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            if (status == 401 || status == 403) r = API_SERVER;   // presigned URL expired/invalid: get a new one
            else if (status >= 200 && status < 300) r = API_OK;
            else r = API_SERVER;
        }
    }
    if (r == API_OK) {
        // For single PUTs R2's ETag is the hex MD5 of the body: proves it stored exactly what we sent.
        unsigned char d[16];
        char hex[33];
        mbedtls_md5_finish(&md5, d);
        for (int i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02x", d[i]);
        if (!strcasestr(etag, hex)) {
            ESP_LOGW(TAG, "ETag mismatch: got '%s', want %s", etag, hex);
            r = API_NET;                                 // re-upload next window
        } else {
            ESP_LOGI(TAG, "PUT %ld bytes, ETag verified (%s)", sent, hex);
        }
    } else {
        ESP_LOGW(TAG, "PUT failed: HTTP %d, sent %ld/%ld", status, sent, (long)st.st_size);
    }
    mbedtls_md5_free(&md5);
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    free(buf);
    fclose(f);
    return r;
}

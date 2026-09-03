#include "gmail_api.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define GOOGLE_AUTH_URL "https://accounts.google.com/o/oauth2/v2/auth"
#define GOOGLE_TOKEN_URL "https://oauth2.googleapis.com/token"
#define GMAIL_API_BASE "https://gmail.googleapis.com/gmail/v1/users/me"
#define GMAIL_SCOPE "https://www.googleapis.com/auth/gmail.readonly"

/* Memory buffer for cURL write callback */
typedef struct {
    gchar *data;
    gsize size;
} CurlBuffer;

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    CurlBuffer *mem = (CurlBuffer *)userp;

    gchar *ptr = g_realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

static gchar *url_decode(const gchar *src) {
    if (!src) return NULL;
    GString *out = g_string_new(NULL);
    const gchar *p = src;
    while (*p) {
        if (*p == '%' && p[1] && p[2] && g_ascii_isxdigit(p[1]) && g_ascii_isxdigit(p[2])) {
            gchar hex[3] = { p[1], p[2], 0 };
            gchar ch = (gchar)g_ascii_strtoull(hex, NULL, 16);
            g_string_append_c(out, ch);
            p += 3;
        } else if (*p == '+') {
            g_string_append_c(out, ' ');
            p++;
        } else {
            g_string_append_c(out, *p);
            p++;
        }
    }
    return g_string_free(out, FALSE);
}

static gboolean idle_notify_cb(gpointer user_data) {
    GmailState *state = (GmailState *)user_data;
    if (state && state->on_update_cb) {
        state->on_update_cb(state->cb_user_data);
    }
    return G_SOURCE_REMOVE;
}

static void trigger_ui_update(GmailState *state) {
    g_idle_add(idle_notify_cb, state);
}

void gmail_free_label_stats_list(GList *list) {
    if (!list) return;
    GList *l;
    for (l = list; l != NULL; l = l->next) {
        g_free(l->data);
    }
    g_list_free(list);
}

void gmail_state_init(GmailState *state, void (*update_cb)(gpointer), gpointer user_data) {
    memset(state, 0, sizeof(GmailState));
    g_mutex_init(&state->mutex);
    g_cond_init(&state->cond);

    state->poll_interval = DEFAULT_POLL_INTERVAL;
    g_strlcpy(state->labels_str, DEFAULT_LABELS, sizeof(state->labels_str));
    g_strlcpy(state->click_command, DEFAULT_CLICK_CMD, sizeof(state->click_command));
    g_strlcpy(state->last_status_msg, "Not initialized", sizeof(state->last_status_msg));
    state->total_is_inbox = TRUE;
    state->display_layout = 0; /* 2-line */
    state->oauth_port = DEFAULT_OAUTH_PORT;

    state->on_update_cb = update_cb;
    state->cb_user_data = user_data;
}

void gmail_state_cleanup(GmailState *state) {
    gmail_stop_worker(state);
    gmail_cancel_oauth_flow(state);

    g_mutex_lock(&state->mutex);
    gmail_free_label_stats_list(state->monitored_labels_stats);
    state->monitored_labels_stats = NULL;
    gmail_free_label_stats_list(state->all_available_labels);
    state->all_available_labels = NULL;
    g_mutex_unlock(&state->mutex);

    g_mutex_clear(&state->mutex);
    g_cond_clear(&state->cond);
}

/* HTTP request helper */
static gchar *http_request(const gchar *url, const gchar *method, const gchar *post_data,
                          const gchar *auth_bearer, long *out_http_code, gchar *err_buf, gsize err_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err_buf) g_strlcpy(err_buf, "Failed to initialize curl", err_size);
        return NULL;
    }

    CurlBuffer buffer = { NULL, 0 };
    struct curl_slist *headers = NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GKrellM-Gmail-Plugin/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buffer);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (auth_bearer && *auth_bearer) {
        gchar *auth_hdr = g_strdup_printf("Authorization: Bearer %s", auth_bearer);
        headers = curl_slist_append(headers, auth_hdr);
        g_free(auth_hdr);
    }

    if (g_strcmp0(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (post_data) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
            headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        }
    }

    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (out_http_code) *out_http_code = http_code;

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (err_buf) g_snprintf(err_buf, err_size, "Network error: %s", curl_easy_strerror(res));
        g_free(buffer.data);
        return NULL;
    }

    return buffer.data;
}

gboolean gmail_refresh_access_token(GmailState *state, gchar *err_buf, gsize err_size) {
    gchar client_id[256], client_secret[256], refresh_token[512];

    g_mutex_lock(&state->mutex);
    g_strlcpy(client_id, state->client_id, sizeof(client_id));
    g_strlcpy(client_secret, state->client_secret, sizeof(client_secret));
    g_strlcpy(refresh_token, state->refresh_token, sizeof(refresh_token));
    g_mutex_unlock(&state->mutex);

    if (!*client_id || !*client_secret || !*refresh_token) {
        if (err_buf) g_strlcpy(err_buf, "Missing OAuth credentials or refresh token", err_size);
        return FALSE;
    }

    gchar *escaped_client_id = g_uri_escape_string(client_id, NULL, TRUE);
    gchar *escaped_client_secret = g_uri_escape_string(client_secret, NULL, TRUE);
    gchar *escaped_refresh_token = g_uri_escape_string(refresh_token, NULL, TRUE);

    gchar *post_data = g_strdup_printf(
        "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
        escaped_client_id, escaped_client_secret, escaped_refresh_token
    );

    g_free(escaped_client_id);
    g_free(escaped_client_secret);
    g_free(escaped_refresh_token);

    long http_code = 0;
    gchar *response = http_request(GOOGLE_TOKEN_URL, "POST", post_data, NULL, &http_code, err_buf, err_size);
    g_free(post_data);

    if (!response) return FALSE;

    gboolean success = FALSE;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, response, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "access_token")) {
                const gchar *token = json_object_get_string_member(obj, "access_token");
                gint64 expires_in = 3600;
                if (json_object_has_member(obj, "expires_in")) {
                    expires_in = json_object_get_int_member(obj, "expires_in");
                }

                g_mutex_lock(&state->mutex);
                g_strlcpy(state->access_token, token, sizeof(state->access_token));
                state->token_expiry = time(NULL) + (time_t)expires_in;
                state->authorized = TRUE;
                g_mutex_unlock(&state->mutex);

                success = TRUE;
            } else if (json_object_has_member(obj, "error_description")) {
                if (err_buf) g_strlcpy(err_buf, json_object_get_string_member(obj, "error_description"), err_size);
            } else if (json_object_has_member(obj, "error")) {
                if (err_buf) g_strlcpy(err_buf, json_object_get_string_member(obj, "error"), err_size);
            }
        }
    } else {
        if (err_buf) g_strlcpy(err_buf, "Failed to parse token response JSON", err_size);
    }

    g_object_unref(parser);
    g_free(response);
    return success;
}

gboolean gmail_exchange_auth_code(GmailState *state, const gchar *code, gchar *err_buf, gsize err_size) {
    if (!code || !*code) {
        if (err_buf) g_strlcpy(err_buf, "Invalid authorization code", err_size);
        return FALSE;
    }

    gchar client_id[256], client_secret[256];
    gint port = DEFAULT_OAUTH_PORT;

    g_mutex_lock(&state->mutex);
    g_strlcpy(client_id, state->client_id, sizeof(client_id));
    g_strlcpy(client_secret, state->client_secret, sizeof(client_secret));
    if (state->oauth_port > 0) port = state->oauth_port;
    g_mutex_unlock(&state->mutex);

    gchar *escaped_client_id = g_uri_escape_string(client_id, NULL, TRUE);
    gchar *escaped_client_secret = g_uri_escape_string(client_secret, NULL, TRUE);
    gchar *escaped_code = g_uri_escape_string(code, NULL, TRUE);
    gchar *redirect_uri = g_strdup_printf("http://127.0.0.1:%d", port);
    gchar *escaped_redirect_uri = g_uri_escape_string(redirect_uri, NULL, TRUE);

    gchar *post_data = g_strdup_printf(
        "code=%s&client_id=%s&client_secret=%s&redirect_uri=%s&grant_type=authorization_code",
        escaped_code, escaped_client_id, escaped_client_secret, escaped_redirect_uri
    );

    g_free(escaped_client_id);
    g_free(escaped_client_secret);
    g_free(escaped_code);
    g_free(redirect_uri);
    g_free(escaped_redirect_uri);

    long http_code = 0;
    gchar *response = http_request(GOOGLE_TOKEN_URL, "POST", post_data, NULL, &http_code, err_buf, err_size);
    g_free(post_data);

    if (!response) return FALSE;

    gboolean success = FALSE;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, response, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "refresh_token") || json_object_has_member(obj, "access_token")) {
                const gchar *atoken = json_object_get_string_member(obj, "access_token");
                const gchar *rtoken = json_object_has_member(obj, "refresh_token") ?
                                     json_object_get_string_member(obj, "refresh_token") : NULL;
                gint64 expires_in = 3600;
                if (json_object_has_member(obj, "expires_in")) {
                    expires_in = json_object_get_int_member(obj, "expires_in");
                }

                g_mutex_lock(&state->mutex);
                if (atoken) g_strlcpy(state->access_token, atoken, sizeof(state->access_token));
                if (rtoken) g_strlcpy(state->refresh_token, rtoken, sizeof(state->refresh_token));
                state->token_expiry = time(NULL) + (time_t)expires_in;
                state->authorized = TRUE;
                g_strlcpy(state->last_status_msg, "Authorized", sizeof(state->last_status_msg));
                g_mutex_unlock(&state->mutex);

                success = TRUE;
            } else if (json_object_has_member(obj, "error_description")) {
                if (err_buf) g_strlcpy(err_buf, json_object_get_string_member(obj, "error_description"), err_size);
            } else if (json_object_has_member(obj, "error")) {
                if (err_buf) g_strlcpy(err_buf, json_object_get_string_member(obj, "error"), err_size);
            }
        }
    } else {
        if (err_buf) g_strlcpy(err_buf, "Failed to parse token response JSON", err_size);
    }

    g_object_unref(parser);
    g_free(response);
    return success;
}

void gmail_disconnect_account(GmailState *state) {
    g_mutex_lock(&state->mutex);
    state->refresh_token[0] = '\0';
    state->access_token[0] = '\0';
    state->email_address[0] = '\0';
    state->authorized = FALSE;
    state->total_unread = 0;
    state->total_messages = 0;
    state->inbox_total = 0;
    state->inbox_unread = 0;
    g_strlcpy(state->last_status_msg, "Disconnected", sizeof(state->last_status_msg));

    gmail_free_label_stats_list(state->monitored_labels_stats);
    state->monitored_labels_stats = NULL;
    gmail_free_label_stats_list(state->all_available_labels);
    state->all_available_labels = NULL;
    g_mutex_unlock(&state->mutex);

    trigger_ui_update(state);
}

gboolean gmail_load_client_secrets_file(const gchar *filepath, gchar *out_id, gsize id_len,
                                        gchar *out_secret, gsize secret_len, gchar *err_buf, gsize err_size) {
    if (!filepath || !*filepath) {
        if (err_buf) g_strlcpy(err_buf, "No file path provided", err_size);
        return FALSE;
    }

    gchar *content = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (!g_file_get_contents(filepath, &content, &length, &error)) {
        if (err_buf) g_strlcpy(err_buf, error ? error->message : "Failed to read file", err_size);
        if (error) g_error_free(error);
        return FALSE;
    }

    gboolean success = FALSE;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, content, length, &error)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *root_obj = json_node_get_object(root);
            JsonObject *app_obj = NULL;

            if (json_object_has_member(root_obj, "installed")) {
                app_obj = json_object_get_object_member(root_obj, "installed");
            } else if (json_object_has_member(root_obj, "web")) {
                app_obj = json_object_get_object_member(root_obj, "web");
            } else {
                app_obj = root_obj;
            }

            if (app_obj && json_object_has_member(app_obj, "client_id") && json_object_has_member(app_obj, "client_secret")) {
                const gchar *cid = json_object_get_string_member(app_obj, "client_id");
                const gchar *csec = json_object_get_string_member(app_obj, "client_secret");
                if (out_id && cid) g_strlcpy(out_id, cid, id_len);
                if (out_secret && csec) g_strlcpy(out_secret, csec, secret_len);
                success = TRUE;
            } else {
                if (err_buf) g_strlcpy(err_buf, "JSON missing client_id or client_secret fields", err_size);
            }
        }
    } else {
        if (err_buf) g_strlcpy(err_buf, error ? error->message : "Invalid JSON syntax", err_size);
        if (error) g_error_free(error);
    }

    g_object_unref(parser);
    g_free(content);
    return success;
}

/* Fetch list of all labels from Gmail */
GList *gmail_fetch_labels_list(GmailState *state, gchar *err_buf, gsize err_size) {
    gchar token[512];
    g_mutex_lock(&state->mutex);
    g_strlcpy(token, state->access_token, sizeof(token));
    g_mutex_unlock(&state->mutex);

    if (!*token) {
        if (err_buf) g_strlcpy(err_buf, "No access token", err_size);
        return NULL;
    }

    gchar *url = g_strdup_printf("%s/labels", GMAIL_API_BASE);
    long http_code = 0;
    gchar *response = http_request(url, "GET", NULL, token, &http_code, err_buf, err_size);
    g_free(url);

    if (!response) return NULL;

    GList *labels = NULL;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, response, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "labels")) {
                JsonArray *arr = json_object_get_array_member(obj, "labels");
                guint len = json_array_get_length(arr);
                guint i;
                for (i = 0; i < len; i++) {
                    JsonObject *lbl = json_array_get_object_element(arr, i);
                    if (lbl) {
                        GmailLabelStats *st = g_new0(GmailLabelStats, 1);
                        if (json_object_has_member(lbl, "id"))
                            g_strlcpy(st->id, json_object_get_string_member(lbl, "id"), sizeof(st->id));
                        if (json_object_has_member(lbl, "name"))
                            g_strlcpy(st->name, json_object_get_string_member(lbl, "name"), sizeof(st->name));
                        if (json_object_has_member(lbl, "type"))
                            g_strlcpy(st->type, json_object_get_string_member(lbl, "type"), sizeof(st->type));
                        labels = g_list_append(labels, st);
                    }
                }
            }
        }
    }
    g_object_unref(parser);
    g_free(response);
    return labels;
}

/* Fetch single label stats */
static gboolean fetch_label_details(const gchar *token, const gchar *label_id, GmailLabelStats *out_stats, gchar *err_buf, gsize err_size) {
    if (!token || !*token || !label_id || !*label_id || !out_stats) return FALSE;

    gchar *escaped_id = g_uri_escape_string(label_id, NULL, TRUE);
    gchar *url = g_strdup_printf("%s/labels/%s", GMAIL_API_BASE, escaped_id);
    g_free(escaped_id);

    long http_code = 0;
    gchar *response = http_request(url, "GET", NULL, token, &http_code, err_buf, err_size);
    g_free(url);

    if (!response) return FALSE;

    gboolean success = FALSE;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, response, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "id"))
                g_strlcpy(out_stats->id, json_object_get_string_member(obj, "id"), sizeof(out_stats->id));
            if (json_object_has_member(obj, "name"))
                g_strlcpy(out_stats->name, json_object_get_string_member(obj, "name"), sizeof(out_stats->name));
            if (json_object_has_member(obj, "type"))
                g_strlcpy(out_stats->type, json_object_get_string_member(obj, "type"), sizeof(out_stats->type));
            if (json_object_has_member(obj, "messagesTotal"))
                out_stats->messages_total = (gint)json_object_get_int_member(obj, "messagesTotal");
            if (json_object_has_member(obj, "messagesUnread"))
                out_stats->messages_unread = (gint)json_object_get_int_member(obj, "messagesUnread");
            if (json_object_has_member(obj, "threadsTotal"))
                out_stats->threads_total = (gint)json_object_get_int_member(obj, "threadsTotal");
            if (json_object_has_member(obj, "threadsUnread"))
                out_stats->threads_unread = (gint)json_object_get_int_member(obj, "threadsUnread");
            success = TRUE;
        }
    }

    g_object_unref(parser);
    g_free(response);
    return success;
}

/* Fetch Profile (emailAddress, messagesTotal) */
static gboolean fetch_user_profile(const gchar *token, gchar *out_email, gsize email_size, gint *out_total_messages, gchar *err_buf, gsize err_size) {
    if (!token || !*token) return FALSE;

    gchar *url = g_strdup_printf("%s/profile", GMAIL_API_BASE);
    long http_code = 0;
    gchar *response = http_request(url, "GET", NULL, token, &http_code, err_buf, err_size);
    g_free(url);

    if (!response) return FALSE;

    gboolean success = FALSE;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, response, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "emailAddress")) {
                if (out_email) g_strlcpy(out_email, json_object_get_string_member(obj, "emailAddress"), email_size);
                if (out_total_messages && json_object_has_member(obj, "messagesTotal")) {
                    *out_total_messages = (gint)json_object_get_int_member(obj, "messagesTotal");
                }
                success = TRUE;
            }
        }
    }

    g_object_unref(parser);
    g_free(response);
    return success;
}

/* Main check routine */
gboolean gmail_fetch_all(GmailState *state) {
    gchar err_buf[256] = {0};

    g_mutex_lock(&state->mutex);
    gboolean has_refresh = (state->refresh_token[0] != '\0');
    time_t now = time(NULL);
    gboolean need_refresh = (!state->access_token[0] || (now >= state->token_expiry - 60));
    gchar labels_conf[256];
    g_strlcpy(labels_conf, state->labels_str, sizeof(labels_conf));
    gboolean total_is_inbox = state->total_is_inbox;
    state->is_checking = TRUE;
    g_mutex_unlock(&state->mutex);

    if (!has_refresh) {
        g_mutex_lock(&state->mutex);
        state->is_checking = FALSE;
        g_strlcpy(state->last_status_msg, "Not authorized", sizeof(state->last_status_msg));
        g_mutex_unlock(&state->mutex);
        trigger_ui_update(state);
        return FALSE;
    }

    if (need_refresh) {
        if (!gmail_refresh_access_token(state, err_buf, sizeof(err_buf))) {
            g_mutex_lock(&state->mutex);
            state->is_checking = FALSE;
            g_snprintf(state->last_status_msg, sizeof(state->last_status_msg), "Auth error: %s", err_buf);
            g_mutex_unlock(&state->mutex);
            trigger_ui_update(state);
            return FALSE;
        }
    }

    gchar token[512];
    g_mutex_lock(&state->mutex);
    g_strlcpy(token, state->access_token, sizeof(token));
    g_mutex_unlock(&state->mutex);

    /* Fetch profile */
    gchar email[128] = {0};
    gint mailbox_total = 0;
    if (!fetch_user_profile(token, email, sizeof(email), &mailbox_total, err_buf, sizeof(err_buf))) {
        /* Token might have been revoked or expired */
        if (gmail_refresh_access_token(state, err_buf, sizeof(err_buf))) {
            g_mutex_lock(&state->mutex);
            g_strlcpy(token, state->access_token, sizeof(token));
            g_mutex_unlock(&state->mutex);
            fetch_user_profile(token, email, sizeof(email), &mailbox_total, err_buf, sizeof(err_buf));
        }
    }

    /* Fetch available labels list */
    GList *avail_labels = gmail_fetch_labels_list(state, err_buf, sizeof(err_buf));

    /* Always fetch INBOX */
    GmailLabelStats inbox_stats;
    memset(&inbox_stats, 0, sizeof(inbox_stats));
    g_strlcpy(inbox_stats.name, "INBOX", sizeof(inbox_stats.name));
    fetch_label_details(token, "INBOX", &inbox_stats, err_buf, sizeof(err_buf));

    /* Parse configured labels */
    gchar **tokens = g_strsplit_set(labels_conf, ",; \t\n", -1);
    GList *mon_stats_list = NULL;
    gint sum_unread = 0;

    if (tokens) {
        int i;
        for (i = 0; tokens[i] != NULL; i++) {
            gchar *raw_tok = g_strstrip(tokens[i]);
            if (!*raw_tok) continue;

            /* Check if already added */
            gboolean exists = FALSE;
            GList *ml;
            for (ml = mon_stats_list; ml != NULL; ml = ml->next) {
                GmailLabelStats *st = (GmailLabelStats *)ml->data;
                if (g_ascii_strcasecmp(st->name, raw_tok) == 0 || g_ascii_strcasecmp(st->id, raw_tok) == 0) {
                    exists = TRUE;
                    break;
                }
            }
            if (exists) continue;

            GmailLabelStats *st = g_new0(GmailLabelStats, 1);
            g_strlcpy(st->name, raw_tok, sizeof(st->name));

            /* Resolve ID if needed */
            gchar target_id[64];
            g_strlcpy(target_id, raw_tok, sizeof(target_id));

            if (avail_labels) {
                GList *al;
                for (al = avail_labels; al != NULL; al = al->next) {
                    GmailLabelStats *avail = (GmailLabelStats *)al->data;
                    if (g_ascii_strcasecmp(avail->name, raw_tok) == 0 || g_ascii_strcasecmp(avail->id, raw_tok) == 0) {
                        g_strlcpy(target_id, avail->id, sizeof(target_id));
                        g_strlcpy(st->name, avail->name, sizeof(st->name));
                        break;
                    }
                }
            }

            if (g_ascii_strcasecmp(target_id, "INBOX") == 0) {
                *st = inbox_stats;
            } else {
                fetch_label_details(token, target_id, st, err_buf, sizeof(err_buf));
            }

            sum_unread += st->messages_unread;
            mon_stats_list = g_list_append(mon_stats_list, st);
        }
        g_strfreev(tokens);
    }

    /* Fallback if no valid labels configured: default to INBOX */
    if (!mon_stats_list) {
        GmailLabelStats *st = g_new0(GmailLabelStats, 1);
        *st = inbox_stats;
        sum_unread = st->messages_unread;
        mon_stats_list = g_list_append(mon_stats_list, st);
    }

    /* Update state with fresh results */
    g_mutex_lock(&state->mutex);
    if (email[0]) g_strlcpy(state->email_address, email, sizeof(state->email_address));
    state->total_unread = sum_unread;
    state->inbox_total = inbox_stats.messages_total;
    state->inbox_unread = inbox_stats.messages_unread;
    state->total_messages = total_is_inbox ? inbox_stats.messages_total : mailbox_total;
    state->last_check_time = time(NULL);
    state->is_checking = FALSE;
    state->authorized = TRUE;
    g_strlcpy(state->last_status_msg, "OK", sizeof(state->last_status_msg));

    gmail_free_label_stats_list(state->monitored_labels_stats);
    state->monitored_labels_stats = mon_stats_list;

    if (avail_labels) {
        gmail_free_label_stats_list(state->all_available_labels);
        state->all_available_labels = avail_labels;
    }
    g_mutex_unlock(&state->mutex);

    trigger_ui_update(state);
    return TRUE;
}

/* Background Polling Worker Thread */
static gpointer worker_thread_func(gpointer data) {
    GmailState *state = (GmailState *)data;

    while (TRUE) {
        g_mutex_lock(&state->mutex);
        if (state->abort_requested) {
            g_mutex_unlock(&state->mutex);
            break;
        }

        gint interval = state->poll_interval;
        if (interval < 10) interval = 10; /* minimum 10s */

        if (!state->check_requested) {
            gint64 end_time = g_get_monotonic_time() + (gint64)interval * G_TIME_SPAN_SECOND;
            g_cond_wait_until(&state->cond, &state->mutex, end_time);
        }

        if (state->abort_requested) {
            g_mutex_unlock(&state->mutex);
            break;
        }

        state->check_requested = FALSE;
        gboolean is_auth = state->authorized || (state->refresh_token[0] != '\0');
        g_mutex_unlock(&state->mutex);

        if (is_auth) {
            gmail_fetch_all(state);
        }
    }

    return NULL;
}

void gmail_start_worker(GmailState *state) {
    g_mutex_lock(&state->mutex);
    if (!state->thread_running) {
        state->abort_requested = FALSE;
        state->check_requested = TRUE;
        state->thread_running = TRUE;
        state->worker_thread = g_thread_new("gmail-worker", worker_thread_func, state);
    }
    g_mutex_unlock(&state->mutex);
}

void gmail_stop_worker(GmailState *state) {
    g_mutex_lock(&state->mutex);
    if (state->thread_running) {
        state->abort_requested = TRUE;
        g_cond_signal(&state->cond);
        g_mutex_unlock(&state->mutex);

        if (state->worker_thread) {
            g_thread_join(state->worker_thread);
            state->worker_thread = NULL;
        }

        g_mutex_lock(&state->mutex);
        state->thread_running = FALSE;
    }
    g_mutex_unlock(&state->mutex);
}

void gmail_request_immediate_check(GmailState *state) {
    g_mutex_lock(&state->mutex);
    state->check_requested = TRUE;
    g_cond_signal(&state->cond);
    g_mutex_unlock(&state->mutex);
}

/* OAuth Loopback Server Thread */
static gpointer oauth_listener_func(gpointer data) {
    GmailState *state = (GmailState *)data;
    gint port = state->oauth_port;
    if (port <= 0) port = DEFAULT_OAUTH_PORT;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        g_mutex_lock(&state->mutex);
        state->auth_in_progress = FALSE;
        g_snprintf(state->auth_error, sizeof(state->auth_error), "Cannot open socket: %s", strerror(errno));
        g_mutex_unlock(&state->mutex);
        trigger_ui_update(state);
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        g_mutex_lock(&state->mutex);
        state->auth_in_progress = FALSE;
        g_snprintf(state->auth_error, sizeof(state->auth_error), "Port %d is already in use: %s", port, strerror(errno));
        g_mutex_unlock(&state->mutex);
        close(server_fd);
        trigger_ui_update(state);
        return NULL;
    }

    if (listen(server_fd, 1) < 0) {
        g_mutex_lock(&state->mutex);
        state->auth_in_progress = FALSE;
        g_snprintf(state->auth_error, sizeof(state->auth_error), "Listen failed: %s", strerror(errno));
        g_mutex_unlock(&state->mutex);
        close(server_fd);
        trigger_ui_update(state);
        return NULL;
    }

    /* Wait up to 3 minutes for browser redirect */
    fd_set fds;
    struct timeval tv;
    tv.tv_sec = 180;
    tv.tv_usec = 0;

    FD_ZERO(&fds);
    FD_SET(server_fd, &fds);

    int activity = select(server_fd + 1, &fds, NULL, NULL, &tv);

    g_mutex_lock(&state->mutex);
    if (state->oauth_abort || activity <= 0) {
        if (!state->oauth_abort && activity == 0) {
            g_strlcpy(state->auth_error, "Authentication timed out (no response from browser)", sizeof(state->auth_error));
        }
        state->auth_in_progress = FALSE;
        g_mutex_unlock(&state->mutex);
        close(server_fd);
        trigger_ui_update(state);
        return NULL;
    }
    g_mutex_unlock(&state->mutex);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    close(server_fd);

    if (client_fd < 0) {
        g_mutex_lock(&state->mutex);
        state->auth_in_progress = FALSE;
        g_strlcpy(state->auth_error, "Failed to accept browser connection", sizeof(state->auth_error));
        g_mutex_unlock(&state->mutex);
        trigger_ui_update(state);
        return NULL;
    }

    char req_buf[2048] = {0};
    ssize_t bytes_read = read(client_fd, req_buf, sizeof(req_buf) - 1);
    gchar *extracted_code = NULL;

    if (bytes_read > 0) {
        req_buf[bytes_read] = '\0';
        /* Look for "code=" in GET request */
        char *code_pos = strstr(req_buf, "code=");
        if (code_pos) {
            code_pos += 5;
            char *end_pos = strpbrk(code_pos, "& \r\n");
            if (end_pos) *end_pos = '\0';
            extracted_code = url_decode(code_pos);
        }
    }

    const char *html_success =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><title>Gmail GKrellM Plugin</title>"
        "<style>body{font-family:Segoe UI,Roboto,sans-serif;text-align:center;padding:60px;background:#f8f9fa;color:#202124;}"
        ".card{background:white;padding:30px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.1);max-width:480px;margin:0 auto;}"
        "h1{color:#ea4335;font-size:24px;margin-bottom:12px;}"
        "p{font-size:15px;color:#5f6368;line-height:1.5;}</style></head>"
        "<body><div class='card'>"
        "<h1>Authentication Successful!</h1>"
        "<p>GKrellM Gmail Plugin has received your OAuth authorization.<br>You may now close this browser tab and return to GKrellM.</p>"
        "</div></body></html>";

    const char *html_fail =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><title>Gmail GKrellM Plugin</title>"
        "<style>body{font-family:sans-serif;text-align:center;padding:60px;background:#f8f9fa;color:#202124;}"
        ".card{background:white;padding:30px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.1);max-width:480px;margin:0 auto;}"
        "h1{color:#d93025;}</style></head><body><div class='card'>"
        "<h1>Authentication Failed</h1><p>No authorization code was found in the redirect request.</p>"
        "</div></body></html>";

    if (extracted_code && *extracted_code) {
        write(client_fd, html_success, strlen(html_success));
    } else {
        write(client_fd, html_fail, strlen(html_fail));
    }
    close(client_fd);

    gchar err_buf[256] = {0};
    gboolean ok = FALSE;
    if (extracted_code && *extracted_code) {
        ok = gmail_exchange_auth_code(state, extracted_code, err_buf, sizeof(err_buf));
        g_free(extracted_code);
    } else {
        g_strlcpy(err_buf, "No authorization code received from Google", sizeof(err_buf));
    }

    g_mutex_lock(&state->mutex);
    state->auth_in_progress = FALSE;
    if (ok) {
        state->auth_error[0] = '\0';
    } else {
        g_strlcpy(state->auth_error, err_buf, sizeof(state->auth_error));
    }
    g_mutex_unlock(&state->mutex);

    if (ok) {
        gmail_fetch_all(state);
    }
    trigger_ui_update(state);
    return NULL;
}

gboolean gmail_start_oauth_flow(GmailState *state, gint port) {
    gchar client_id[256];
    gchar client_secret[256];

    g_mutex_lock(&state->mutex);
    g_strlcpy(client_id, state->client_id, sizeof(client_id));
    g_strlcpy(client_secret, state->client_secret, sizeof(client_secret));

    if (!*client_id || !*client_secret) {
        g_strlcpy(state->auth_error, "Please enter both Client ID and Client Secret before authorizing", sizeof(state->auth_error));
        g_mutex_unlock(&state->mutex);
        return FALSE;
    }

    if (state->auth_in_progress) {
        g_mutex_unlock(&state->mutex);
        return FALSE;
    }

    if (port <= 0) port = DEFAULT_OAUTH_PORT;
    state->oauth_port = port;
    state->oauth_abort = FALSE;
    state->auth_in_progress = TRUE;
    state->auth_error[0] = '\0';

    state->oauth_thread = g_thread_new("gmail-oauth", oauth_listener_func, state);
    g_mutex_unlock(&state->mutex);

    /* Construct Google OAuth URL */
    gchar *escaped_client_id = g_uri_escape_string(client_id, NULL, TRUE);
    gchar *redirect_uri = g_strdup_printf("http://127.0.0.1:%d", port);
    gchar *escaped_redirect_uri = g_uri_escape_string(redirect_uri, NULL, TRUE);
    gchar *escaped_scope = g_uri_escape_string(GMAIL_SCOPE, NULL, TRUE);

    gchar *auth_url = g_strdup_printf(
        "%s?client_id=%s&redirect_uri=%s&response_type=code&scope=%s&access_type=offline&prompt=consent",
        GOOGLE_AUTH_URL, escaped_client_id, escaped_redirect_uri, escaped_scope
    );

    g_free(escaped_client_id);
    g_free(redirect_uri);
    g_free(escaped_redirect_uri);
    g_free(escaped_scope);

    /* Launch user's default web browser */
    gchar *open_cmd = g_strdup_printf("xdg-open '%s' &", auth_url);
    int res = system(open_cmd);
    (void)res;
    g_free(open_cmd);
    g_free(auth_url);

    return TRUE;
}

void gmail_cancel_oauth_flow(GmailState *state) {
    g_mutex_lock(&state->mutex);
    if (state->auth_in_progress) {
        state->oauth_abort = TRUE;
    }
    g_mutex_unlock(&state->mutex);
}

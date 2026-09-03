#ifndef GMAIL_API_H
#define GMAIL_API_H

#include <glib.h>
#include <time.h>
#include <stdio.h>

#define DEFAULT_OAUTH_PORT 8085
#define DEFAULT_POLL_INTERVAL 300 /* 5 minutes */
#define DEFAULT_LABELS "INBOX"
#define DEFAULT_CLICK_CMD "xdg-open https://mail.google.com/"

typedef struct {
    gchar id[64];
    gchar name[128];
    gchar type[32]; /* "system" or "user" */
    gint messages_total;
    gint messages_unread;
    gint threads_total;
    gint threads_unread;
} GmailLabelStats;

typedef struct {
    /* OAuth Configuration */
    gchar client_id[256];
    gchar client_secret[256];
    gchar refresh_token[512];
    gchar access_token[512];
    time_t token_expiry;

    /* User Settings */
    gint poll_interval;        /* in seconds */
    gchar labels_str[256];     /* comma/space separated label names */
    gboolean total_is_inbox;   /* TRUE: total from INBOX; FALSE: total from whole mailbox */
    gint display_layout;       /* 0: 2-line (Unread / Total), 1: 1-line compact */
    gchar click_command[256];  /* Browser command */

    /* Runtime Data */
    gboolean authorized;
    gchar email_address[128];
    gint total_unread;         /* Sum of unread across monitored labels */
    gint total_messages;       /* Total emails (inbox or mailbox) */
    gint inbox_total;
    gint inbox_unread;
    time_t last_check_time;
    gchar last_status_msg[256];
    gboolean is_checking;
    gboolean auth_in_progress;
    gchar auth_error[256];

    /* Label stats list (elements of GmailLabelStats*) */
    GList *monitored_labels_stats;
    GList *all_available_labels;

    /* Synchronization */
    GMutex mutex;
    GCond cond;
    gboolean thread_running;
    gboolean check_requested;
    gboolean abort_requested;
    GThread *worker_thread;

    /* OAuth loopback thread */
    GThread *oauth_thread;
    gint oauth_port;
    gboolean oauth_abort;

    /* Callback when data updates (called in GLib main loop) */
    void (*on_update_cb)(gpointer user_data);
    gpointer cb_user_data;
} GmailState;

/* Initialization and Lifecycle */
void gmail_state_init(GmailState *state, void (*update_cb)(gpointer), gpointer user_data);
void gmail_state_cleanup(GmailState *state);

/* Worker Thread Controls */
void gmail_start_worker(GmailState *state);
void gmail_stop_worker(GmailState *state);
void gmail_request_immediate_check(GmailState *state);

/* OAuth 2.0 Flow */
gboolean gmail_start_oauth_flow(GmailState *state, gint port);
void gmail_cancel_oauth_flow(GmailState *state);
gboolean gmail_exchange_auth_code(GmailState *state, const gchar *code, gchar *err_buf, gsize err_size);
gboolean gmail_refresh_access_token(GmailState *state, gchar *err_buf, gsize err_size);
void gmail_disconnect_account(GmailState *state);

/* Helper to import client_secret JSON from Google Cloud Console */
gboolean gmail_load_client_secrets_file(const gchar *filepath, gchar *out_id, gsize id_len, gchar *out_secret, gsize secret_len, gchar *err_buf, gsize err_size);

/* Data fetching */
gboolean gmail_fetch_all(GmailState *state);
GList *gmail_fetch_labels_list(GmailState *state, gchar *err_buf, gsize err_size);

/* Label list helpers */
void gmail_free_label_stats_list(GList *list);

#endif /* GMAIL_API_H */

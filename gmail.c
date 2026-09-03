#include <gkrellm2/gkrellm.h>
#include <curl/curl.h>
#include "gmail_api.h"
#include "gmail_icon.h"

#define PLUGIN_VERSION "1.0.0"
#define CONFIG_KEYWORD "gkrellm_gmail"

static GkrellmMonitor plugin_mon;
static GkrellmPanel   *panel = NULL;
static GkrellmDecal   *decal_icon = NULL;
static GkrellmDecal   *decal_unread = NULL;
static GkrellmDecal   *decal_total = NULL;
static GkrellmStyle   *style = NULL;
static gint            style_id = 0;

static GmailState gmail_state;
static gboolean need_ui_redraw = TRUE;
static GtkTooltips *tooltips = NULL;

/* Configuration widget pointers */
static GtkWidget *entry_client_id = NULL;
static GtkWidget *entry_client_secret = NULL;
static GtkWidget *label_auth_status = NULL;
static GtkWidget *entry_labels = NULL;
static GtkWidget *spin_poll_interval = NULL;
static GtkWidget *chk_total_is_inbox = NULL;
static GtkWidget *combo_layout = NULL;
static GtkWidget *entry_click_cmd = NULL;

/* Forward declarations */
static void cb_create_monitor(GtkWidget *vbox, gint first_create);
static void cb_update_monitor(void);
static void cb_create_config(GtkWidget *tab_vbox);
static void cb_apply_config(void);
static void cb_save_config(FILE *f);
static void cb_load_config(gchar *line);
static void cb_on_gmail_data_update(gpointer user_data);
static void update_panel_display(void);
static void update_tooltip(void);

/* Click handler on panel */
static gboolean cb_panel_button_press(GtkWidget *widget G_GNUC_UNUSED, GdkEventButton *event, gpointer data G_GNUC_UNUSED) {
    if (event->type == GDK_BUTTON_PRESS) {
        if (event->button == 1) { /* Left click: open browser */
            gchar cmd[256];
            g_mutex_lock(&gmail_state.mutex);
            g_strlcpy(cmd, gmail_state.click_command, sizeof(cmd));
            g_mutex_unlock(&gmail_state.mutex);

            if (cmd[0]) {
                gchar *exec_cmd = g_strdup_printf("%s &", cmd);
                int res = system(exec_cmd);
                (void)res;
                g_free(exec_cmd);
            }
            return TRUE;
        } else if (event->button == 2) { /* Middle click: force refresh */
            gmail_request_immediate_check(&gmail_state);
            return TRUE;
        }
    }
    return FALSE;
}

static void cb_on_gmail_data_update(gpointer user_data G_GNUC_UNUSED) {
    need_ui_redraw = TRUE;

    /* Update config dialog auth status label if visible */
    if (label_auth_status && GTK_IS_LABEL(label_auth_status)) {
        gchar status_text[512];
        g_mutex_lock(&gmail_state.mutex);
        if (gmail_state.auth_in_progress) {
            g_snprintf(status_text, sizeof(status_text),
                       "<b>Status:</b> <span foreground=\"#e37400\">Waiting for Google authorization in browser...</span>");
        } else if (gmail_state.auth_error[0]) {
            g_snprintf(status_text, sizeof(status_text),
                       "<b>Status:</b> <span foreground=\"#d93025\">Auth Error: %s</span>", gmail_state.auth_error);
        } else if (gmail_state.authorized && gmail_state.email_address[0]) {
            g_snprintf(status_text, sizeof(status_text),
                       "<b>Status:</b> <span foreground=\"#188038\">Connected (%s)</span>", gmail_state.email_address);
        } else if (gmail_state.authorized) {
            g_snprintf(status_text, sizeof(status_text),
                       "<b>Status:</b> <span foreground=\"#188038\">Authorized</span>");
        } else {
            g_snprintf(status_text, sizeof(status_text),
                       "<b>Status:</b> <span foreground=\"#5f6368\">Not Authorized</span>");
        }
        g_mutex_unlock(&gmail_state.mutex);
        gtk_label_set_markup(GTK_LABEL(label_auth_status), status_text);
    }
}

static void update_panel_display(void) {
    if (!panel) return;

    gint unread = 0, total = 0;
    gboolean auth = FALSE, checking = FALSE;
    gint layout = 0;
    gchar status_msg[256];

    g_mutex_lock(&gmail_state.mutex);
    unread = gmail_state.total_unread;
    total = gmail_state.total_messages;
    auth = gmail_state.authorized;
    checking = gmail_state.is_checking;
    layout = gmail_state.display_layout;
    g_strlcpy(status_msg, gmail_state.last_status_msg, sizeof(status_msg));
    g_mutex_unlock(&gmail_state.mutex);

    gchar unread_str[64];
    gchar total_str[64];
    unread_str[0] = '\0';
    total_str[0] = '\0';

    if (!auth) {
        if (layout == 1) {
            g_snprintf(unread_str, sizeof(unread_str), "No Auth");
        } else {
            g_snprintf(unread_str, sizeof(unread_str), "No Auth");
            g_snprintf(total_str, sizeof(total_str), "Setup in cfg");
        }
    } else if (checking && total == 0 && unread == 0) {
        g_snprintf(unread_str, sizeof(unread_str), "Checking...");
    } else {
        if (layout == 1) { /* 1-line compact: "5 / 120" */
            g_snprintf(unread_str, sizeof(unread_str), "%d / %d", unread, total);
        } else { /* 2-line */
            g_snprintf(unread_str, sizeof(unread_str), "%d unread", unread);
            g_snprintf(total_str, sizeof(total_str), "%d total", total);
        }
    }

    if (decal_icon) {
        gkrellm_draw_decal_pixmap(panel, decal_icon, 0);
    }
    if (decal_unread) {
        gkrellm_draw_decal_text(panel, decal_unread, unread_str, -1);
    }
    if (decal_total && layout == 0) {
        gkrellm_draw_decal_text(panel, decal_total, total_str, -1);
    }
    gkrellm_draw_panel_layers_force(panel);

    update_tooltip();
}

static void update_tooltip(void) {
    if (!panel || !panel->drawing_area) return;

    if (!tooltips) {
        tooltips = gtk_tooltips_new();
    }

    GString *tip = g_string_new(NULL);

    g_mutex_lock(&gmail_state.mutex);
    if (gmail_state.email_address[0]) {
        g_string_append_printf(tip, "Gmail: %s\n", gmail_state.email_address);
    } else {
        g_string_append(tip, "Gmail Monitor\n");
    }

    if (gmail_state.authorized) {
        g_string_append_printf(tip, "Unread: %d\n", gmail_state.total_unread);
        g_string_append_printf(tip, "Total: %d emails\n", gmail_state.total_messages);
        g_string_append_printf(tip, "Status: %s\n", gmail_state.last_status_msg);

        if (gmail_state.last_check_time > 0) {
            struct tm *tm_info = localtime(&gmail_state.last_check_time);
            gchar time_buf[64];
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
            g_string_append_printf(tip, "Last checked: %s\n", time_buf);
        }

        if (gmail_state.monitored_labels_stats) {
            g_string_append(tip, "-- Monitored Labels --\n");
            GList *l;
            for (l = gmail_state.monitored_labels_stats; l != NULL; l = l->next) {
                GmailLabelStats *st = (GmailLabelStats *)l->data;
                g_string_append_printf(tip, "• %s: %d unread (%d total)\n",
                                       st->name, st->messages_unread, st->messages_total);
            }
        }
    } else {
        g_string_append(tip, "Not Authorized. Open Configuration to connect Google Account.\n");
    }
    g_mutex_unlock(&gmail_state.mutex);

    g_string_append(tip, "Left-click: Open Gmail | Middle-click: Check now");

    gtk_tooltips_set_tip(tooltips, panel->drawing_area, tip->str, NULL);
    g_string_free(tip, TRUE);
}

static void cb_create_monitor(GtkWidget *vbox, gint first_create) {
    if (first_create) {
        panel = gkrellm_panel_new0();
    } else {
        gkrellm_destroy_decal_list(panel);
    }

    style = gkrellm_meter_style(style_id);

    gint layout = gmail_state.display_layout;
    gint panel_height = (layout == 1) ? 18 : 28;

    /* IMPORTANT: configure style first, THEN set the explicit height */
    gkrellm_panel_configure(panel, NULL, style);
    gkrellm_panel_configure_set_height(panel, panel_height);
    gkrellm_panel_create(vbox, &plugin_mon, panel);

    /* Load Gmail Logo Decal */
    GdkPixbuf *icon_pixbuf = gdk_pixbuf_new_from_xpm_data(gmail_icon_xpm);
    if (icon_pixbuf) {
        GdkPixmap *icon_pixmap = NULL;
        GdkBitmap *icon_mask = NULL;
        gdk_pixbuf_render_pixmap_and_mask(icon_pixbuf, &icon_pixmap, &icon_mask, 128);

        gint y_icon = (panel_height - 14) / 2;
        if (y_icon < 1) y_icon = 1;

        decal_icon = gkrellm_create_decal_pixmap(panel, icon_pixmap, icon_mask, 1, style, 3, y_icon);
        g_object_unref(icon_pixbuf);
    }

    /* Text Decals */
    GkrellmTextstyle *ts_unread = gkrellm_meter_textstyle(style_id);
    GkrellmTextstyle *ts_total = gkrellm_meter_alt_textstyle(style_id);

    if (layout == 1) { /* 1-line compact */
        decal_unread = gkrellm_create_decal_text(panel, "9999 / 99999", ts_unread, style, 24, 2, -1);
        decal_total = NULL;
    } else { /* 2-line */
        decal_unread = gkrellm_create_decal_text(panel, "99999 unread", ts_unread, style, 24, 2, -1);
        decal_total = gkrellm_create_decal_text(panel, "99999 total", ts_total, style, 24, 14, -1);
    }

    /* Connect mouse click handlers */
    if (panel->drawing_area) {
        gtk_widget_add_events(panel->drawing_area, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(G_OBJECT(panel->drawing_area), "button-press-event",
                         G_CALLBACK(cb_panel_button_press), NULL);
    }

    update_panel_display();

    /* Automatically trigger immediate check on monitor creation / plugin load */
    gmail_request_immediate_check(&gmail_state);
}

static void cb_update_monitor(void) {
    if (GK.second_tick || need_ui_redraw) {
        need_ui_redraw = FALSE;
        update_panel_display();
    }
}

/* Config UI Callbacks */
static void cb_show_setup_help(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    const gchar *help_text =
        "<b>Self-Hosted Google OAuth 2.0 Setup Guide</b>\n\n"
        "To protect your privacy and retain 100% control over your account permissions, "
        "each user sets up their own free OAuth Client ID directly in Google Cloud Console.\n\n"
        "<b>1. Create Project & Enable API:</b>\n"
        "• Go to <i>https://console.cloud.google.com/</i>\n"
        "• Create a project (e.g. 'gkrellm-gmail') and enable the <b>Gmail API</b>.\n\n"
        "<b>2. Configure OAuth Consent Screen & Scopes:</b>\n"
        "• Go to <b>APIs & Services > OAuth consent screen</b>.\n"
        "• Select <b>External</b>, set App Name to 'gkrellm-gmail', and enter your email.\n"
        "• Under <b>Scopes</b>, add <code>https://www.googleapis.com/auth/gmail.readonly</code>.\n\n"
        "<b>3. Create Credentials:</b>\n"
        "• Go to <b>APIs & Services > Credentials</b>.\n"
        "• Click <b>Create Credentials > OAuth client ID</b>.\n"
        "• Select <b>Desktop App</b> (or Web App with redirect <code>http://127.0.0.1:8085</code>).\n"
        "• Download the JSON file or copy the Client ID and Secret.\n\n"
        "<b>4. Authorize:</b>\n"
        "• Click <b>Import JSON...</b> (or paste ID and Secret), then click <b>Authorize with Google</b>.\n\n"
        "<i>Tip: Publish the app on the 'Audience' tab to make tokens permanent (avoiding 7-day expiration).</i>";

    GtkWidget *dialog = gtk_message_dialog_new_with_markup(
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "%s", help_text
    );
    gtk_window_set_title(GTK_WINDOW(dialog), "Google OAuth Setup Guide");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void cb_browse_secrets_file(GtkWidget *widget, gpointer data G_GNUC_UNUSED) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Select Google Client Secret JSON File",
        GTK_WINDOW(gtk_widget_get_toplevel(widget)),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
        GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
        NULL
    );

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "JSON files (*.json)");
    gtk_file_filter_add_pattern(filter, "*.json");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            gchar cid[256] = {0}, csec[256] = {0}, err[256] = {0};
            if (gmail_load_client_secrets_file(filename, cid, sizeof(cid), csec, sizeof(csec), err, sizeof(err))) {
                if (entry_client_id) gtk_entry_set_text(GTK_ENTRY(entry_client_id), cid);
                if (entry_client_secret) gtk_entry_set_text(GTK_ENTRY(entry_client_secret), csec);
                gkrellm_message_dialog("Google Secrets Loaded", "Client ID and Client Secret imported successfully!\nClick 'Authorize with Google' to log in.");
            } else {
                gchar *msg = g_strdup_printf("Failed to parse JSON file:\n%s", err);
                gkrellm_message_dialog("Import Error", msg);
                g_free(msg);
            }
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
}

static void cb_authorize_clicked(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    if (entry_client_id) {
        const gchar *cid = gtk_entry_get_text(GTK_ENTRY(entry_client_id));
        g_mutex_lock(&gmail_state.mutex);
        g_strlcpy(gmail_state.client_id, cid, sizeof(gmail_state.client_id));
        g_mutex_unlock(&gmail_state.mutex);
    }
    if (entry_client_secret) {
        const gchar *csec = gtk_entry_get_text(GTK_ENTRY(entry_client_secret));
        g_mutex_lock(&gmail_state.mutex);
        g_strlcpy(gmail_state.client_secret, csec, sizeof(gmail_state.client_secret));
        g_mutex_unlock(&gmail_state.mutex);
    }

    if (!gmail_state.client_id[0] || !gmail_state.client_secret[0]) {
        gkrellm_message_dialog("Missing Credentials", "Please enter both Client ID and Client Secret first.");
        return;
    }

    if (gmail_start_oauth_flow(&gmail_state, DEFAULT_OAUTH_PORT)) {
        if (label_auth_status) {
            gtk_label_set_markup(GTK_LABEL(label_auth_status),
                "<b>Status:</b> <span foreground=\"#e37400\">Browser opened. Authorize access in your browser...</span>");
        }
    } else {
        gkrellm_message_dialog("OAuth Error", gmail_state.auth_error);
    }
}

static void cb_manual_code_clicked(GtkWidget *widget, gpointer data G_GNUC_UNUSED) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Enter Authorization Code",
        GTK_WINDOW(gtk_widget_get_toplevel(widget)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
        GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
        NULL
    );

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_box_pack_start(GTK_BOX(content), vbox, TRUE, TRUE, 0);

    GtkWidget *lbl = gtk_label_new("Paste Google Authorization Code below:");
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);

    GtkWidget *code_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(code_entry), 40);
    gtk_box_pack_start(GTK_BOX(vbox), code_entry, FALSE, FALSE, 0);

    gtk_widget_show_all(vbox);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *code = gtk_entry_get_text(GTK_ENTRY(code_entry));
        gchar err[256] = {0};
        if (gmail_exchange_auth_code(&gmail_state, code, err, sizeof(err))) {
            gkrellm_message_dialog("Authorization Success", "Successfully authorized with Google!");
            gmail_fetch_all(&gmail_state);
        } else {
            gchar *msg = g_strdup_printf("Authorization failed:\n%s", err);
            gkrellm_message_dialog("Auth Error", msg);
            g_free(msg);
        }
    }
    gtk_widget_destroy(dialog);
}

static void cb_disconnect_clicked(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    gmail_disconnect_account(&gmail_state);
    gkrellm_message_dialog("Disconnected", "Google Account disconnected and tokens cleared.");
}

static void cb_check_now_clicked(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    gmail_request_immediate_check(&gmail_state);
}

static void cb_show_labels_clicked(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    gchar err[256] = {0};
    GList *labels = gmail_fetch_labels_list(&gmail_state, err, sizeof(err));

    if (!labels) {
        gchar *msg = g_strdup_printf("Could not fetch labels from Gmail:\n%s", err[0] ? err : "Not authorized or network error");
        gkrellm_message_dialog("Labels Error", msg);
        g_free(msg);
        return;
    }

    GString *text = g_string_new("Available Gmail Labels in your account:\n\n");
    GList *l;
    for (l = labels; l != NULL; l = l->next) {
        GmailLabelStats *st = (GmailLabelStats *)l->data;
        g_string_append_printf(text, "• %s  (ID: %s, type: %s)\n", st->name, st->id, st->type);
    }
    g_string_append(text, "\nEnter label names separated by commas in the 'Monitored Labels' box.");

    gkrellm_message_dialog("Gmail Labels", text->str);
    g_string_free(text, TRUE);
    gmail_free_label_stats_list(labels);
}

static void cb_create_config(GtkWidget *tab_vbox) {
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(tab_vbox), notebook, TRUE, TRUE, 0);

    /* --- Tab 1: Account & OAuth --- */
    GtkWidget *vbox_tab1 = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_tab1), 8);

    /* Status display */
    GtkWidget *frame_status = gtk_frame_new("Account Status");
    gtk_box_pack_start(GTK_BOX(vbox_tab1), frame_status, FALSE, FALSE, 0);
    GtkWidget *box_status = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box_status), 8);
    gtk_container_add(GTK_CONTAINER(frame_status), box_status);

    label_auth_status = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(label_auth_status), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(box_status), label_auth_status, FALSE, FALSE, 0);
    cb_on_gmail_data_update(NULL);

    /* OAuth Credentials Frame */
    GtkWidget *frame_oauth = gtk_frame_new("Google OAuth 2.0 Credentials (User Self-Hosted)");
    gtk_box_pack_start(GTK_BOX(vbox_tab1), frame_oauth, FALSE, FALSE, 0);
    GtkWidget *box_oauth = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box_oauth), 8);
    gtk_container_add(GTK_CONTAINER(frame_oauth), box_oauth);

    GtkWidget *table_oauth = gtk_table_new(2, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table_oauth), 4);
    gtk_table_set_col_spacings(GTK_TABLE(table_oauth), 8);
    gtk_box_pack_start(GTK_BOX(box_oauth), table_oauth, FALSE, FALSE, 0);

    GtkWidget *lbl_cid = gtk_label_new("Client ID:");
    gtk_misc_set_alignment(GTK_MISC(lbl_cid), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table_oauth), lbl_cid, 0, 1, 0, 1, GTK_FILL, GTK_FILL, 0, 0);
    entry_client_id = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_client_id), gmail_state.client_id);
    gtk_table_attach(GTK_TABLE(table_oauth), entry_client_id, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);

    GtkWidget *lbl_csec = gtk_label_new("Client Secret:");
    gtk_misc_set_alignment(GTK_MISC(lbl_csec), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table_oauth), lbl_csec, 0, 1, 1, 2, GTK_FILL, GTK_FILL, 0, 0);
    entry_client_secret = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry_client_secret), FALSE);
    gtk_entry_set_text(GTK_ENTRY(entry_client_secret), gmail_state.client_secret);
    gtk_table_attach(GTK_TABLE(table_oauth), entry_client_secret, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);

    /* Action Buttons Box */
    GtkWidget *hbox_auth_btns = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box_oauth), hbox_auth_btns, FALSE, FALSE, 4);

    GtkWidget *btn_help = gtk_button_new_with_label("Setup Guide...");
    g_signal_connect(G_OBJECT(btn_help), "clicked", G_CALLBACK(cb_show_setup_help), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_auth_btns), btn_help, FALSE, FALSE, 0);

    GtkWidget *btn_import = gtk_button_new_with_label("Import JSON...");
    g_signal_connect(G_OBJECT(btn_import), "clicked", G_CALLBACK(cb_browse_secrets_file), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_auth_btns), btn_import, FALSE, FALSE, 0);

    GtkWidget *btn_auth = gtk_button_new_with_label("Authorize with Google");
    g_signal_connect(G_OBJECT(btn_auth), "clicked", G_CALLBACK(cb_authorize_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_auth_btns), btn_auth, FALSE, FALSE, 0);

    GtkWidget *btn_manual = gtk_button_new_with_label("Manual Code...");
    g_signal_connect(G_OBJECT(btn_manual), "clicked", G_CALLBACK(cb_manual_code_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_auth_btns), btn_manual, FALSE, FALSE, 0);

    GtkWidget *btn_disc = gtk_button_new_with_label("Disconnect");
    g_signal_connect(G_OBJECT(btn_disc), "clicked", G_CALLBACK(cb_disconnect_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(hbox_auth_btns), btn_disc, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox_tab1, gtk_label_new("Account & OAuth"));

    /* --- Tab 2: Mail & Labels Settings --- */
    GtkWidget *vbox_tab2 = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_tab2), 8);

    /* Labels settings frame */
    GtkWidget *frame_labels = gtk_frame_new("Labels Configuration");
    gtk_box_pack_start(GTK_BOX(vbox_tab2), frame_labels, FALSE, FALSE, 0);
    GtkWidget *box_labels = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box_labels), 8);
    gtk_container_add(GTK_CONTAINER(frame_labels), box_labels);

    GtkWidget *lbl_labels_desc = gtk_label_new("Monitored Labels (comma-separated, e.g. 'INBOX, Work, Urgent'):");
    gtk_misc_set_alignment(GTK_MISC(lbl_labels_desc), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(box_labels), lbl_labels_desc, FALSE, FALSE, 0);

    GtkWidget *hbox_lbl_row = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box_labels), hbox_lbl_row, FALSE, FALSE, 0);

    entry_labels = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_labels), gmail_state.labels_str);
    gtk_box_pack_start(GTK_BOX(hbox_lbl_row), entry_labels, TRUE, TRUE, 0);

    GtkWidget *btn_list_labels = gtk_button_new_with_label("Available Labels...");
    g_signal_connect(G_OBJECT(btn_list_labels), "clicked", G_CALLBACK(cb_show_labels_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_lbl_row), btn_list_labels, FALSE, FALSE, 0);

    /* Total count preference */
    chk_total_is_inbox = gtk_check_button_new_with_label("Show total count for INBOX only (unchecked: show entire mailbox total)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_total_is_inbox), gmail_state.total_is_inbox);
    gtk_box_pack_start(GTK_BOX(box_labels), chk_total_is_inbox, FALSE, FALSE, 4);

    /* Polling interval */
    GtkWidget *frame_poll = gtk_frame_new("Polling & Checking");
    gtk_box_pack_start(GTK_BOX(vbox_tab2), frame_poll, FALSE, FALSE, 0);
    GtkWidget *box_poll = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box_poll), 8);
    gtk_container_add(GTK_CONTAINER(frame_poll), box_poll);

    GtkWidget *hbox_poll = gtk_hbox_new(FALSE, 8);
    gtk_box_pack_start(GTK_BOX(box_poll), hbox_poll, FALSE, FALSE, 0);

    GtkWidget *lbl_interval = gtk_label_new("Check interval (seconds):");
    gtk_box_pack_start(GTK_BOX(hbox_poll), lbl_interval, FALSE, FALSE, 0);

    spin_poll_interval = gtk_spin_button_new_with_range(10.0, 3600.0, 30.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_poll_interval), (gdouble)gmail_state.poll_interval);
    gtk_box_pack_start(GTK_BOX(hbox_poll), spin_poll_interval, FALSE, FALSE, 0);

    GtkWidget *btn_check_now = gtk_button_new_with_label("Check Mail Now");
    g_signal_connect(G_OBJECT(btn_check_now), "clicked", G_CALLBACK(cb_check_now_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(hbox_poll), btn_check_now, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox_tab2, gtk_label_new("Labels & Polling"));

    /* --- Tab 3: Display & Actions --- */
    GtkWidget *vbox_tab3 = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_tab3), 8);

    GtkWidget *frame_disp = gtk_frame_new("Panel Display Style");
    gtk_box_pack_start(GTK_BOX(vbox_tab3), frame_disp, FALSE, FALSE, 0);
    GtkWidget *box_disp = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box_disp), 8);
    gtk_container_add(GTK_CONTAINER(frame_disp), box_disp);

    GtkWidget *hbox_layout = gtk_hbox_new(FALSE, 8);
    gtk_box_pack_start(GTK_BOX(box_disp), hbox_layout, FALSE, FALSE, 0);
    GtkWidget *lbl_layout = gtk_label_new("Display Layout:");
    gtk_box_pack_start(GTK_BOX(hbox_layout), lbl_layout, FALSE, FALSE, 0);

    combo_layout = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_layout), "Two-line (Unread on top, Total on bottom)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_layout), "Single-line Compact (Unread / Total)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_layout), gmail_state.display_layout);
    gtk_box_pack_start(GTK_BOX(hbox_layout), combo_layout, TRUE, TRUE, 0);

    GtkWidget *frame_action = gtk_frame_new("Click Actions");
    gtk_box_pack_start(GTK_BOX(vbox_tab3), frame_action, FALSE, FALSE, 0);
    GtkWidget *box_action = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box_action), 8);
    gtk_container_add(GTK_CONTAINER(frame_action), box_action);

    GtkWidget *lbl_cmd = gtk_label_new("Command to launch on click:");
    gtk_misc_set_alignment(GTK_MISC(lbl_cmd), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(box_action), lbl_cmd, FALSE, FALSE, 0);

    entry_click_cmd = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_click_cmd), gmail_state.click_command);
    gtk_box_pack_start(GTK_BOX(box_action), entry_click_cmd, TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox_tab3, gtk_label_new("Display & Click"));

    gtk_widget_show_all(tab_vbox);
}

static void cb_apply_config(void) {
    gboolean need_recreate = FALSE;

    g_mutex_lock(&gmail_state.mutex);
    if (entry_client_id) {
        g_strlcpy(gmail_state.client_id, gtk_entry_get_text(GTK_ENTRY(entry_client_id)), sizeof(gmail_state.client_id));
    }
    if (entry_client_secret) {
        g_strlcpy(gmail_state.client_secret, gtk_entry_get_text(GTK_ENTRY(entry_client_secret)), sizeof(gmail_state.client_secret));
    }
    if (entry_labels) {
        g_strlcpy(gmail_state.labels_str, gtk_entry_get_text(GTK_ENTRY(entry_labels)), sizeof(gmail_state.labels_str));
    }
    if (chk_total_is_inbox) {
        gmail_state.total_is_inbox = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_total_is_inbox));
    }
    if (spin_poll_interval) {
        gmail_state.poll_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_poll_interval));
    }
    if (entry_click_cmd) {
        g_strlcpy(gmail_state.click_command, gtk_entry_get_text(GTK_ENTRY(entry_click_cmd)), sizeof(gmail_state.click_command));
    }
    if (combo_layout) {
        gint new_layout = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_layout));
        if (new_layout != gmail_state.display_layout) {
            gmail_state.display_layout = new_layout;
            need_recreate = TRUE;
        }
    }
    g_mutex_unlock(&gmail_state.mutex);

    if (need_recreate && panel && plugin_mon.privat) {
        cb_create_monitor(plugin_mon.privat->vbox, FALSE);
    }

    gmail_request_immediate_check(&gmail_state);
}

static void cb_save_config(FILE *f) {
    g_mutex_lock(&gmail_state.mutex);
    fprintf(f, "%s client_id %s\n", CONFIG_KEYWORD, gmail_state.client_id);
    fprintf(f, "%s client_secret %s\n", CONFIG_KEYWORD, gmail_state.client_secret);
    fprintf(f, "%s refresh_token %s\n", CONFIG_KEYWORD, gmail_state.refresh_token);
    fprintf(f, "%s email_address %s\n", CONFIG_KEYWORD, gmail_state.email_address);
    fprintf(f, "%s labels %s\n", CONFIG_KEYWORD, gmail_state.labels_str);
    fprintf(f, "%s poll_interval %d\n", CONFIG_KEYWORD, gmail_state.poll_interval);
    fprintf(f, "%s total_is_inbox %d\n", CONFIG_KEYWORD, gmail_state.total_is_inbox ? 1 : 0);
    fprintf(f, "%s display_layout %d\n", CONFIG_KEYWORD, gmail_state.display_layout);
    fprintf(f, "%s click_command %s\n", CONFIG_KEYWORD, gmail_state.click_command);
    g_mutex_unlock(&gmail_state.mutex);
}

static void cb_load_config(gchar *line) {
    gchar item[CFG_BUFSIZE], val[CFG_BUFSIZE];
    item[0] = '\0';
    val[0] = '\0';

    if (sscanf(line, "%s %[^\n]", item, val) >= 1) {
        g_mutex_lock(&gmail_state.mutex);
        if (strcmp(item, "client_id") == 0) {
            g_strlcpy(gmail_state.client_id, val, sizeof(gmail_state.client_id));
        } else if (strcmp(item, "client_secret") == 0) {
            g_strlcpy(gmail_state.client_secret, val, sizeof(gmail_state.client_secret));
        } else if (strcmp(item, "refresh_token") == 0) {
            g_strlcpy(gmail_state.refresh_token, val, sizeof(gmail_state.refresh_token));
            if (val[0]) gmail_state.authorized = TRUE;
        } else if (strcmp(item, "email_address") == 0) {
            g_strlcpy(gmail_state.email_address, val, sizeof(gmail_state.email_address));
        } else if (strcmp(item, "labels") == 0) {
            g_strlcpy(gmail_state.labels_str, val, sizeof(gmail_state.labels_str));
        } else if (strcmp(item, "poll_interval") == 0) {
            gmail_state.poll_interval = atoi(val);
        } else if (strcmp(item, "total_is_inbox") == 0) {
            gmail_state.total_is_inbox = (atoi(val) != 0);
        } else if (strcmp(item, "display_layout") == 0) {
            gmail_state.display_layout = atoi(val);
        } else if (strcmp(item, "click_command") == 0) {
            g_strlcpy(gmail_state.click_command, val, sizeof(gmail_state.click_command));
        }
        g_mutex_unlock(&gmail_state.mutex);

        /* As soon as credentials/tokens are loaded from config, trigger check */
        gmail_request_immediate_check(&gmail_state);
    }
}

/* Plugin initialization entry point */
GkrellmMonitor *gkrellm_init_plugin(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    memset(&plugin_mon, 0, sizeof(GkrellmMonitor));
    plugin_mon.name = "Gmail";
    plugin_mon.id = MON_PLUGIN;
    plugin_mon.create_monitor = cb_create_monitor;
    plugin_mon.update_monitor = cb_update_monitor;
    plugin_mon.create_config = cb_create_config;
    plugin_mon.apply_config = cb_apply_config;
    plugin_mon.save_user_config = cb_save_config;
    plugin_mon.load_user_config = cb_load_config;
    plugin_mon.config_keyword = CONFIG_KEYWORD;
    plugin_mon.insert_before_id = MON_MAIL;

    style_id = gkrellm_add_meter_style(&plugin_mon, "gmail");

    gmail_state_init(&gmail_state, cb_on_gmail_data_update, NULL);
    gmail_start_worker(&gmail_state);

    return &plugin_mon;
}

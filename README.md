# GKrellM Gmail Monitor Plugin

[![CI Build & Test](https://github.com/skamphax0r/gkrellm_gmail/actions/workflows/ci.yml/badge.svg)](https://github.com/skamphax0r/gkrellm_gmail/actions)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![GKrellM: 2.x](https://img.shields.io/badge/GKrellM-2.x-green.svg)](http://gkrellm.srcbox.net/)

A modern, secure **Gmail Monitor plugin for GKrellM** that monitors your Gmail inbox and custom labels using the official **Google Gmail REST API and OAuth 2.0** (no insecure IMAP, Mailwatch, or plain passwords required).

---

## Features

- **Gmail Logo Decal**: Displays the iconic Gmail logo directly on the panel.
- **Unread & Total Email Counts**: Shows unread count and total emails in real-time.
- **Label-Specific Monitoring**: Monitor `INBOX`, `UNREAD`, `IMPORTANT`, `STARRED`, or any custom user-created Gmail labels (e.g., `Work`, `Personal`, `Urgent`). Supports comma-separated multiple labels.
- **OAuth 2.0 Desktop Authentication**:
  - Seamless automatic browser login via local loopback redirect (`http://127.0.0.1:8085`).
  - Fallback manual authorization code entry dialog for headless or restricted environments.
  - One-click import for Google Cloud `client_secret_*.json` credential files.
  - Automatic background token refresh (access tokens refreshed seamlessly without prompting).
- **Auto-Poll on Startup**: Immediately checks your mailbox when GKrellM launches or when saved credentials load.
- **Non-Blocking Background Worker**: Network requests and API calls are handled on dedicated background threads so GKrellM's UI never freezes or stutters.
- **Interactive Actions**:
  - **Left-Click**: Launches Gmail in your default web browser (`xdg-open https://mail.google.com/`).
  - **Middle-Click**: Forces an immediate mail check.
- **Detailed Tooltip**: Hovering over the panel reveals:
  - Connected account email address
  - Total mailbox count
  - Timestamp of last successful check
  - Per-label breakdown of unread and total messages
- **Customizable Display Layouts**:
  - **Two-Line Mode** (default): Top: `X unread`, Bottom: `Y total`.
  - **Single-Line Compact Mode**: `X / Y`.
- **CLI Utility**: Includes [`gkrellm-gmail-auth`](./gkrellm-gmail-auth) tool for testing authentication, listing labels, and checking mailbox status directly from the command line.

---

## Architecture Overview

```
+-----------------------------------------------------------+
|                        GKrellM                            |
|  +-----------------------------------------------------+  |
|  |             GKrellM Gmail Plugin (gmail.so)         |  |
|  |  [Gmail Icon] [Unread: 5] / [Total: 1,420]          |  |
|  +-----------------------------------------------------+  |
+-----------------------------+-----------------------------+
                              | (Non-blocking worker thread)
                              v
                +----------------------------+
                |   OAuth 2.0 Token Manager  |
                |   (Auto-refresh via token) |
                +-------------+--------------+
                              |
                              v (HTTPS / REST)
                +----------------------------+
                |      Google Gmail API      |
                |  - /profile                |
                |  - /labels                 |
                |  - /labels/{id}            |
                +----------------------------+
```

---

## 1. Google Cloud OAuth 2.0 Setup (One-time)

Because this plugin uses secure OAuth 2.0 without IMAP, you need a free Google Cloud OAuth Client ID:

1. Open the [Google Cloud Console](https://console.cloud.google.com/).
2. Create a new project (e.g., `GKrellM-Gmail`) or select an existing one.
3. Enable the **Gmail API**:
   - Go to **APIs & Services** > **Library**.
   - Search for **Gmail API** and click **Enable**.
4. Configure the **OAuth Consent Screen**:
   - Go to **APIs & Services** > **OAuth consent screen**.
   - Select **External** (or Internal if using Google Workspace) and click **Create**.
   - Fill in App Name (e.g., `GKrellM Gmail`), support email, and developer contact email.
   - In **Scopes**, add `https://www.googleapis.com/auth/gmail.readonly`.
   - In **Test users**, add your Gmail address (important while app is in Testing status).
5. Create **Credentials**:
   - Go to **APIs & Services** > **Credentials**.
   - Click **Create Credentials** > **OAuth client ID**.
   - Select **Desktop App** as the Application Type (or **Web Application** with Authorized Redirect URI `http://127.0.0.1:8085`).
   - Name it `GKrellM Plugin` and click **Create**.
6. Download the JSON file (`client_secret_*.json`) or copy the **Client ID** and **Client Secret**.

---

## 2. Building & Installing

### Dependencies

#### Fedora / RHEL
```bash
sudo dnf install -y gkrellm-devel gtk2-devel libcurl-devel json-glib-devel gcc make
```

#### Debian / Ubuntu / Mint
```bash
sudo apt-get install -y gkrellm libgtk2.0-dev libcurl4-openssl-dev libjson-glib-dev build-essential
```

#### Arch Linux
```bash
sudo pacman -S gkrellm gtk2 curl json-glib base-devel
```

### Build and Install
```bash
git clone https://github.com/skamphax0r/gkrellm_gmail.git
cd gkrellm-gmail

# Build the plugin and CLI tool:
make

# Run the unit test suite:
make test

# Install for the current user:
make install-user
# (Installs to ~/.gkrellm2/plugins/gmail.so)

# Or install system-wide:
sudo make install
```

---

## 3. Configuring the Plugin in GKrellM

1. Open **GKrellM**.
2. Right-click the GKrellM window frame and select **Configuration** (or press `F1`).
3. Expand **Plugins** and select **Gmail** (ensure the checkbox next to Gmail is enabled).
4. In the **Account & OAuth** tab:
   - Click **Import JSON...** and select your downloaded `client_secret_*.json` file (or paste your **Client ID** and **Client Secret**).
   - Click **Authorize with Google**.
   - Your default browser will open to Google's authorization page. Log in and click **Allow**.
   - The status in GKrellM will update to `Connected (your_email@gmail.com)`.
5. In the **Labels & Polling** tab:
   - **Monitored Labels**: Enter the labels you want to check, separated by commas (e.g., `INBOX`, `INBOX, Work, Alerts`, or `UNREAD`).
   - Click **Available Labels...** to view all system and custom labels in your Gmail account.
   - Choose whether the total count represents the `INBOX` only or the entire mailbox.
   - Set your preferred check interval in seconds (default: 300s = 5 minutes).
6. In the **Display & Click** tab:
   - Choose between **Two-line** or **Single-line compact** layout.
   - Set the click launcher command (defaults to `xdg-open https://mail.google.com/`).
7. Click **Apply** and **OK**.

---

## 4. Standalone CLI Utility (`gkrellm-gmail-auth`)

You can also use the included command-line tool to manage authorization or test API access directly:

```bash
# Authenticate via browser:
./gkrellm-gmail-auth --import client_secret.json --auth

# Check status and fetch unread / total counts:
./gkrellm-gmail-auth --check

# List all labels in your Gmail account:
./gkrellm-gmail-auth --list-labels
```

---

## 5. Running Tests

The test suite validates JSON parsing, OAuth token payload extraction, client secrets loaders, and label deduplication:

```bash
make test
```

---

## 6. Troubleshooting

- **"Port 8085 is already in use" during authorization**:
  Ensure no other local server is running on port 8085, or use the **Manual Code...** button to paste the OAuth code from Google.
- **"Waiting for Google authorization in browser..." hangs**:
  Ensure you completed the authorization prompt in your browser and your firewall allows local loopback on `127.0.0.1`.
- **"Auth Error: Token has been expired or revoked"**:
  In Google Cloud Console, refresh tokens for apps in "Testing" status expire after 7 days unless moved to "Production". Re-click **Authorize with Google** to refresh your grant.

---

## License

GNU General Public License v3.0 ([GPL-3.0](LICENSE)).

---

## 7. Permanent Token Setup (In Production Mode)

By default, Google puts new OAuth apps in **Testing** status, which causes refresh tokens to expire every 7 days. To make your token permanent:

1. In the Google Cloud Console, go to **Branding**:
   - Set **Application home page**: `https://github.com/skamphax0r/gkrellm_gmail`
   - Set **Application privacy policy link**: `https://github.com/skamphax0r/gkrellm_gmail`
   - Under **Authorized domains**, click **+ Add domain** and add `github.com`.
   - Click **Save**.
2. Go to **Audience**:
   - Click **Publish app** and confirm. The status will change to **In production**.
3. In GKrellM:
   - Go to configuration > **Plugins** > **Gmail**.
   - Click **Disconnect** and then **Authorize with Google** once more.
   - If prompted with *"Google hasn't verified this app"*, click **Advanced** > **Go to GKrellM Gmail (unsafe)** > **Allow**.

Your refresh token is now permanent and will never expire automatically.

# Changelog

All notable changes to the GKrellM Gmail Plugin will be documented in this file.

## [1.0.0] - 2026-09-03

### Initial Release
- **Gmail REST API Integration**: Complete OAuth 2.0 desktop authentication with refresh token auto-renewal (no IMAP required).
- **Embedded Loopback Server**: Browser-based one-click login on `http://127.0.0.1:8085` with manual code entry fallback.
- **Client Secrets Importer**: Direct JSON import for Google Cloud `client_secret_*.json` credentials.
- **Visual Display**:
  - Embedded Gmail Logo decal.
  - Live Unread count and Total emails count.
  - Two display layout modes: Two-Line (`X unread / Y total`) and Compact Single-Line (`X / Y`).
- **Label Monitoring**: Multi-label support with case-insensitive tokenization and label query inspector.
- **Auto-Poll on Startup**: Automatically triggers an asynchronous inbox check on load and credential restore.
- **Interactive Features**: Left-click browser launcher, middle-click refresh, and informative multi-line tooltip.
- **CLI Utility**: Included `gkrellm-gmail-auth` tool for testing and headless setup.
- **Unit Test Suite**: Full test coverage for JSON parsing, token exchange logic, and label tokenization.

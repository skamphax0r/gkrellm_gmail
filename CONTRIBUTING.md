# Contributing to GKrellM Gmail Plugin

Thank you for your interest in contributing to the **GKrellM Gmail Plugin**!

## How to Contribute

1. **Fork the repository** on GitHub.
2. **Clone your fork** locally:
   ```bash
   git clone https://github.com/your-username/gkrellm-gmail.git
   cd gkrellm-gmail
   ```
3. **Install build dependencies**:
   - **Fedora / RHEL**: `sudo dnf install -y gkrellm-devel gtk2-devel libcurl-devel json-glib-devel gcc make`
   - **Debian / Ubuntu**: `sudo apt install -y libgkrellm-dev libgtk2.0-dev libcurl4-openssl-dev libjson-glib-dev build-essential`
4. **Create a topic branch**:
   ```bash
   git checkout -b feature/my-new-feature
   ```
5. **Run the test suite**:
   ```bash
   make test
   ```
6. **Submit a Pull Request**:
   - Provide a clear explanation of the changes.
   - Ensure `make test` and `make` complete with zero warnings and errors.

## Coding Guidelines

- Written in standard C99/C11 adhering to GLib/GTK2 and GKrellM conventions.
- Maintain thread safety: any shared state between background polling threads and the GTK main loop must be synchronized using `GMutex` and dispatched with `g_idle_add`.
- Avoid synchronous blocking operations on the GTK main thread.

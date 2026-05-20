"""
PlatformIO pre-build script: patch WebSockets for deprecated flush() call.

Problem:
  WebSocketsClient.cpp calls client->tcp->flush() before stopping a connection.
  NetworkClient::flush() is deprecated in the Arduino ESP32 framework and
  replaced by clear(), which discards the receive buffer — the correct
  operation before a disconnect.

Fix:
  Replace the single flush() call with clear() in clientDisconnect().

Applied idempotently — safe to run on every build.
"""

Import("env")
import os


def patch_websockets(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        target = os.path.join(
            libdeps_dir, env_dir, "WebSockets", "src", "WebSocketsClient.cpp"
        )
        if os.path.isfile(target):
            _apply_flush_fix(target)


def _apply_flush_fix(filepath):
    MARKER = "// CrossPoint patch: clear() replaces deprecated flush()"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    OLD = "            client->tcp->flush();"
    NEW = "            " + MARKER + "\n            client->tcp->clear();"

    if OLD not in content:
        print(
            "WARNING: WebSockets flush() patch target not found in %s "
            "— library may have been updated" % filepath
        )
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched WebSockets: flush() -> clear() in clientDisconnect: %s" % filepath)


patch_websockets(env)

"""Host model for the one-shot FNK0104S boot animation lifecycle."""

INTRO_DURATION_MS = 1400
MIN_DISPLAY_MS = 1750
MAX_DISPLAY_MS = 8000
EXIT_DURATION_MS = 300


class BootAnimationLifecycle:
    def __init__(self):
        self.connected = False
        self.exiting = False
        self.destroyed = False
        self.exit_reason = None
        self.exit_started_ms = None

    def server_connected(self, elapsed_ms):
        if not self.destroyed:
            self.connected = True
        self.update(elapsed_ms)

    def update(self, elapsed_ms):
        if self.destroyed:
            return
        if not self.exiting:
            if self.connected and elapsed_ms >= MIN_DISPLAY_MS:
                self._start_exit("connected", elapsed_ms)
            elif elapsed_ms >= MAX_DISPLAY_MS:
                self._start_exit("timeout", elapsed_ms)
        if self.exiting and elapsed_ms - self.exit_started_ms >= EXIT_DURATION_MS:
            self.destroyed = True

    def _start_exit(self, reason, elapsed_ms):
        self.exiting = True
        self.exit_reason = reason
        self.exit_started_ms = elapsed_ms

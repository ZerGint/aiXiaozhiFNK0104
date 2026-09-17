import unittest

from scripts.boot_animation_lifecycle import (INTRO_DURATION_MS, MIN_DISPLAY_MS,
                                              BootAnimationLifecycle)


class BootAnimationLifecycleTests(unittest.TestCase):
    def test_early_connection_waits_for_minimum(self):
        lifecycle = BootAnimationLifecycle()
        lifecycle.server_connected(300)
        self.assertFalse(lifecycle.exiting)
        lifecycle.update(MIN_DISPLAY_MS - 1)
        self.assertFalse(lifecycle.exiting)
        lifecycle.update(MIN_DISPLAY_MS)
        self.assertEqual(lifecycle.exit_reason, "connected")
        self.assertGreater(MIN_DISPLAY_MS, INTRO_DURATION_MS)

    def test_connection_after_minimum_exits_immediately(self):
        lifecycle = BootAnimationLifecycle()
        lifecycle.server_connected(3000)
        self.assertEqual(lifecycle.exit_started_ms, 3000)

    def test_timeout_always_reveals_main_ui(self):
        lifecycle = BootAnimationLifecycle()
        lifecycle.update(8000)
        self.assertEqual(lifecycle.exit_reason, "timeout")
        lifecycle.update(8300)
        self.assertTrue(lifecycle.destroyed)

    def test_late_and_duplicate_connections_do_not_restart(self):
        lifecycle = BootAnimationLifecycle()
        lifecycle.update(8000)
        lifecycle.update(8300)
        lifecycle.server_connected(15000)
        lifecycle.server_connected(16000)
        self.assertTrue(lifecycle.destroyed)
        self.assertEqual(lifecycle.exit_started_ms, 8000)

    def test_timeout_connection_race_exits_once(self):
        lifecycle = BootAnimationLifecycle()
        lifecycle.server_connected(8000)
        lifecycle.update(8000)
        self.assertEqual(lifecycle.exit_reason, "connected")
        self.assertEqual(lifecycle.exit_started_ms, 8000)
        lifecycle.server_connected(8050)
        self.assertEqual(lifecycle.exit_started_ms, 8000)


if __name__ == "__main__":
    unittest.main()

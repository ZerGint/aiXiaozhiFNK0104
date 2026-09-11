import random
import unittest


class ShuffleNavigationModel:
    def __init__(self, count):
        self.count = count
        self.selected = -1
        self.enabled = False
        self.order = []
        self.position = -1

    def select(self, index):
        if not self.count:
            self.selected = -1
            return
        self.selected = max(0, min(index, self.count - 1))
        if self.enabled and self.selected in self.order:
            self.position = self.order.index(self.selected)

    def shuffle(self, enabled):
        self.enabled = enabled
        self.order = []
        self.position = -1
        if enabled and self.count:
            self.order = list(range(self.count))
            random.shuffle(self.order)
            current = self.selected if 0 <= self.selected < self.count else 0
            self.order.remove(current)
            self.order.insert(0, current)
            self.selected, self.position = current, 0

    def navigate(self, step):
        if not self.count:
            return -1
        if self.enabled and len(self.order) == self.count:
            self.position = (self.position + step) % self.count
            self.selected = self.order[self.position]
        else:
            self.enabled = False
            self.selected = (self.selected + step) % self.count
        return self.selected


class SdShuffleNavigationTests(unittest.TestCase):
    def test_normal_next_prev_and_wrap(self):
        m = ShuffleNavigationModel(3); m.select(0)
        self.assertEqual(m.navigate(1), 1)
        self.assertEqual(m.navigate(-1), 0)
        self.assertEqual(m.navigate(-1), 2)
        self.assertEqual(m.navigate(1), 0)

    def test_shuffle_preserves_selected_and_is_permutation(self):
        m = ShuffleNavigationModel(6); m.select(3); m.shuffle(True)
        self.assertEqual(m.selected, 3)
        self.assertEqual(sorted(m.order), list(range(6)))

    def test_shuffle_navigation_and_wrap(self):
        m = ShuffleNavigationModel(4); m.select(2); m.shuffle(True)
        order = m.order[:]
        self.assertEqual(m.navigate(1), order[1])
        self.assertEqual(m.navigate(-1), order[0])
        for _ in range(len(order)):
            m.navigate(1)
        self.assertEqual(m.selected, order[0])

    def test_manual_selection_syncs_shuffle_position(self):
        m = ShuffleNavigationModel(5); m.select(0); m.shuffle(True); m.select(4)
        self.assertEqual(m.selected, 4)
        self.assertEqual(m.order[m.position], 4)

    def test_disable_preserves_real_index_and_linear_navigation(self):
        m = ShuffleNavigationModel(5); m.select(3); m.shuffle(True); m.select(4); m.shuffle(False)
        self.assertEqual(m.selected, 4)
        self.assertEqual(m.navigate(1), 0)

    def test_empty_and_single_track_are_safe(self):
        empty = ShuffleNavigationModel(0); empty.shuffle(True)
        self.assertEqual(empty.navigate(1), -1); self.assertEqual(empty.navigate(-1), -1)
        one = ShuffleNavigationModel(1); one.shuffle(True)
        self.assertEqual(one.order, [0]); self.assertEqual(one.navigate(1), 0); self.assertEqual(one.navigate(-1), 0)


if __name__ == "__main__":
    unittest.main()

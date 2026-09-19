import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from weather_service import (Cache, LOCATION, UPDATE_INTERVAL_MS, build_url, icon_for_code,
                             parse, wind_direction)


def payload():
    return json.dumps({
        "current": {"temp_c": 16.2, "condition": {"code": 1006}, "wind_kph": 14.4,
                    "wind_degree": 271, "is_day": 1},
        "forecast": {"forecastday": [
            {"day": {"condition": {"code": 1006}, "maxtemp_c": 16, "mintemp_c": 10,
                     "daily_chance_of_rain": 20}},
            {"day": {"condition": {"code": 1183}, "maxtemp_c": 17, "mintemp_c": 12,
                     "daily_chance_of_rain": 65}},
            {"day": {"condition": {"code": 1000}, "maxtemp_c": 19, "mintemp_c": 11,
                     "daily_chance_of_rain": 10}},
        ]},
    })


class WeatherServiceTest(unittest.TestCase):
    def test_icon_mapping(self):
        self.assertEqual([icon_for_code(code) for code in
                          (1000, 1003, 1006, 1030, 1183, 1273, 1213)],
                         ["clear", "partly_cloudy", "cloudy", "fog", "rain", "storm", "snow"])

    def test_russian_wind_directions(self):
        self.assertEqual([wind_direction(value) for value in range(0, 360, 45)],
                         ["С", "СВ", "В", "ЮВ", "Ю", "ЮЗ", "З", "СЗ"])
        self.assertEqual((wind_direction(337), wind_direction(359)), ("СЗ", "С"))

    def test_valid_json_and_day_indexing(self):
        data = parse(payload())
        self.assertEqual(data["temperature"], 16.2)
        self.assertEqual((data["days"][1]["min"], data["days"][2]["max"]), (12, 19))

    def test_wind_is_converted_to_metres_per_second(self):
        self.assertAlmostEqual(parse(payload())["wind"], 4.0)

    def test_invalid_json(self):
        with self.assertRaises((json.JSONDecodeError, KeyError, IndexError, TypeError)):
            parse("not-json")

    def test_failure_keeps_cache_and_success_replaces_it(self):
        cache = Cache(); cache.refresh(0); cache.complete(payload())
        old = cache.data
        cache.refresh(100); cache.complete(None)
        self.assertIs(cache.data, old)
        changed = json.loads(payload()); changed["current"]["temp_c"] = 21
        cache.refresh(200); cache.complete(json.dumps(changed))
        self.assertEqual(cache.data["temperature"], 21)

    def test_interval_stale_manual_and_duplicate(self):
        cache = Cache(); self.assertTrue(cache.refresh(0)); self.assertFalse(cache.refresh(1))
        cache.complete(payload()); self.assertFalse(cache.due(UPDATE_INTERVAL_MS - 1))
        self.assertTrue(cache.due(UPDATE_INTERVAL_MS)); self.assertTrue(cache.stale(UPDATE_INTERVAL_MS))

    def test_refresh_is_deferred_until_radio_fully_stops(self):
        cache = Cache(); cache.refresh(0); cache.complete(payload())
        self.assertTrue(cache.refresh(100, radio_active=True))
        self.assertTrue(cache.pending)
        self.assertFalse(cache.active)
        self.assertTrue(cache.due(101))
        self.assertTrue(cache.refresh(101, radio_active=False))
        self.assertFalse(cache.pending)
        self.assertTrue(cache.active)

    def test_startup_refresh_waits_for_valid_system_time(self):
        cache = Cache()
        self.assertTrue(cache.refresh(0, time_valid=False))
        self.assertTrue(cache.pending)
        self.assertFalse(cache.active)
        self.assertFalse(cache.due(100, time_valid=False))
        self.assertTrue(cache.due(101, time_valid=True))
        self.assertTrue(cache.refresh(101, time_valid=True))
        self.assertFalse(cache.pending)
        self.assertTrue(cache.active)

    def test_location_reaches_request(self):
        url = build_url()
        self.assertIn("q=54.3520,18.6466", url)
        self.assertIn("days=3", url)
        self.assertEqual(LOCATION["name"], "Гданьск")
        self.assertNotIn("hourly=", url)

    def test_date_format_contract(self):
        import datetime
        self.assertEqual(datetime.date(2026, 9, 17).strftime("%d.%m.%Y"), "17.09.2026")


if __name__ == "__main__":
    unittest.main()

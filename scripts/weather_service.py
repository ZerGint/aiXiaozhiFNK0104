"""Host model for validating the embedded weather service policy."""

import json

LOCATION = {"latitude": 54.3520, "longitude": 18.6466, "name": "Гданьск"}
UPDATE_INTERVAL_MS = 30 * 60 * 1000


def icon_for_code(code):
    if code == 1000:
        return "clear"
    if code == 1003:
        return "partly_cloudy"
    if code in (1006, 1009):
        return "cloudy"
    if code in (1030, 1135, 1147):
        return "fog"
    if code == 1087 or code >= 1273:
        return "storm"
    if (code in (1066, 1069, 1072, 1114, 1117) or 1204 <= code <= 1237 or
            1255 <= code <= 1264):
        return "snow"
    return "rain"


def wind_direction(degrees):
    directions = ("С", "СВ", "В", "ЮВ", "Ю", "ЮЗ", "З", "СЗ")
    return directions[((degrees + 22) // 45) % 8]


def build_url(api_key="test-key", location=LOCATION):
    return ("https://api.weatherapi.com/v1/forecast.json?"
            f"key={api_key}&q={location['latitude']:.4f},{location['longitude']:.4f}&"
            "days=3&aqi=no&alerts=no&lang=ru")


def parse(payload):
    root = json.loads(payload)
    current = root["current"]
    forecast = root["forecast"]["forecastday"]
    days = []
    for index in range(3):
        day = forecast[index]["day"]
        days.append({"code": day["condition"]["code"], "min": day["mintemp_c"],
                     "max": day["maxtemp_c"], "rain": day["daily_chance_of_rain"]})
    return {"temperature": current["temp_c"], "code": current["condition"]["code"],
            "wind": current["wind_kph"] / 3.6, "direction": current["wind_degree"],
            "is_day": bool(current["is_day"]), "days": days}


class Cache:
    def __init__(self):
        self.data = None
        self.active = False
        self.pending = False
        self.last_attempt_ms = 0

    def refresh(self, now_ms, radio_active=False, time_valid=True):
        if self.active:
            return False
        if radio_active or not time_valid:
            self.pending = True
            return True
        self.pending = False
        self.active = True
        self.last_attempt_ms = now_ms
        return True

    def complete(self, payload=None):
        self.active = False
        if payload is not None:
            self.data = parse(payload)

    def due(self, now_ms, time_valid=True):
        return time_valid and not self.active and (self.pending or self.data is None or
                                                   now_ms - self.last_attempt_ms >= UPDATE_INTERVAL_MS)

    def stale(self, now_ms):
        return self.data is not None and now_ms - self.last_attempt_ms >= UPDATE_INTERVAL_MS

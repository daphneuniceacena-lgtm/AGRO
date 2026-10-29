"""
AGRO soil drying forecast.

Model (after watering, soil moisture dries toward an equilibrium):
    theta(t) = theta_eq + (theta0 - theta_eq) * exp(-t / tau)

Time until the soil reaches the watering threshold theta_thr:
    t = tau * ln((theta_now - theta_eq) / (theta_thr - theta_eq))
valid when theta_eq < theta_thr < theta_now.

Fitting without scipy: sample the curve every dt seconds. Then
    theta[k+1] = a * theta[k] + b      with a = exp(-dt / tau), theta_eq = b / (1 - a)
which is an ordinary straight-line fit (np.polyfit).

tau changes with temperature and humidity (evaporation). After a few drying episodes,
ln(tau) = c0 + c1*T + c2*RH is fitted so a new episode can get an early estimate.
"""

import json
import math
import os

import numpy as np


class SoilForecaster:
    def __init__(self, threshold=35.0, dt_s=600, min_points=6, jump=8.0):
        self.threshold = threshold      # % moisture that triggers watering
        self.dt = dt_s                  # grid spacing used for the fit
        self.min_points = min_points    # readings needed before an episode can be fitted
        self.jump = jump                # a rise bigger than this means watering happened
        self.episodes = []              # each: {"t": [], "soil": [], "temp": [], "hum": []}
        self.tau_coef = None

    # ---------- data ----------
    def add_reading(self, t, soil, temp=None, hum=None):
        ep = self.episodes[-1] if self.episodes else None
        if ep is None or soil - ep["soil"][-1] > self.jump:
            ep = {"t": [], "soil": [], "temp": [], "hum": []}
            self.episodes.append(ep)
        ep["t"].append(float(t))
        ep["soil"].append(float(soil))
        ep["temp"].append(None if temp is None else float(temp))
        ep["hum"].append(None if hum is None else float(hum))

    def save(self, path):
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w") as f:
            json.dump(self.episodes, f)

    def load(self, path):
        if os.path.exists(path):
            with open(path) as f:
                self.episodes = json.load(f)

    # ---------- fitting ----------
    def fit_episode(self, ep):
        """Return dict(tau, theta_eq, rms, n) in seconds / %, or None if the data can't be fitted."""
        t, y = np.array(ep["t"]), np.array(ep["soil"])
        if len(t) < self.min_points:
            return None
        grid = np.arange(t[0], t[-1] + 1e-9, self.dt)
        if len(grid) < self.min_points:
            return None
        yg = np.interp(grid, t, y)
        a, b = np.polyfit(yg[:-1], yg[1:], 1)
        if not (0.0 < a < 1.0):
            return None                         # not a decaying curve (flat, noisy, or rising)
        tau = -self.dt / math.log(a)
        theta_eq = b / (1.0 - a)
        resid = yg[1:] - (a * yg[:-1] + b)
        return {"tau": tau, "theta_eq": theta_eq,
                "rms": float(np.sqrt(np.mean(resid ** 2))), "n": len(grid)}

    def fit_tau_model(self):
        """Fit ln(tau) = c0 + c1*T + c2*RH over finished episodes. Needs at least 4 good episodes."""
        rows = []
        for ep in self.episodes[:-1]:           # skip the episode still in progress
            f = self.fit_episode(ep)
            temps = [v for v in ep["temp"] if v is not None]
            hums = [v for v in ep["hum"] if v is not None]
            if f and temps and hums:
                rows.append((np.mean(temps), np.mean(hums), math.log(f["tau"]), f["theta_eq"]))
        if len(rows) < 4:
            return None
        r = np.array(rows)
        A = np.column_stack([np.ones(len(r)), r[:, 0], r[:, 1]])
        self.tau_coef, *_ = np.linalg.lstsq(A, r[:, 2], rcond=None)
        self.eq_median = float(np.median(r[:, 3]))
        return self.tau_coef

    # ---------- prediction ----------
    def _time_to_threshold(self, soil_now, tau, theta_eq):
        if soil_now <= self.threshold:
            return 0.0
        if theta_eq >= self.threshold:
            return None                         # settles above the threshold, no watering needed
        return tau * math.log((soil_now - theta_eq) / (self.threshold - theta_eq))

    def predict(self):
        """Seconds until the current episode reaches the threshold, from its own fit.
        Returns None if there isn't enough data yet."""
        if not self.episodes:
            return None
        ep = self.episodes[-1]
        f = self.fit_episode(ep)
        if not f:
            return None
        return self._time_to_threshold(ep["soil"][-1], f["tau"], f["theta_eq"])

    def early_forecast(self, soil_now, temp, hum):
        """Estimate for a fresh episode from the temperature/humidity model (needs fit_tau_model first)."""
        if self.tau_coef is None:
            return None
        tau = math.exp(self.tau_coef[0] + self.tau_coef[1] * temp + self.tau_coef[2] * hum)
        return self._time_to_threshold(soil_now, tau, self.eq_median)


if __name__ == "__main__":
    # Synthetic self-test of the math only. Not real sensor data.
    rng = np.random.default_rng(1)
    TAU, EQ, START, THR = 3600.0, 15.0, 60.0, 35.0
    true_total = TAU * math.log((START - EQ) / (THR - EQ))
    fc = SoilForecaster(threshold=THR, dt_s=300)
    for k in range(9):                          # 9 readings, one every 5 min
        t = k * 300
        fc.add_reading(t, EQ + (START - EQ) * math.exp(-t / TAU) + rng.normal(0, 0.3))
    pred = fc.predict()
    true_left = true_total - 8 * 300
    print(f"synthetic test: predicted {pred/60:.1f} min left, true {true_left/60:.1f} min left")

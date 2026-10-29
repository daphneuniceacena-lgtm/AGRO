# AGRO soil drying forecast: the math

AGRO currently waters when the soil reading drops below a threshold. The forecast adds a prediction layer: it estimates how long until the soil reaches that threshold, so watering can be scheduled before it gets there.

## Model

After watering, soil moisture dries toward an equilibrium value:

```
theta(t) = theta_eq + (theta0 - theta_eq) * exp(-t / tau)
```

`theta` is soil moisture in %, `theta_eq` the level the soil settles at, `tau` the drying time constant in seconds. A larger `tau` means slower drying.

Time until the soil reaches the watering threshold `theta_thr`, starting from the current reading `theta_now`:

```
t = tau * ln((theta_now - theta_eq) / (theta_thr - theta_eq))
```

This works when `theta_eq < theta_thr < theta_now`. If `theta_eq >= theta_thr` the soil settles above the threshold and no watering is needed. If `theta_now <= theta_thr` the answer is 0.

## Fitting `tau` and `theta_eq` from readings

Sample the soil every `dt` seconds. Then:

```
theta[k+1] = a * theta[k] + b
a = exp(-dt / tau)           ->   tau = -dt / ln(a)
theta_eq = b / (1 - a)
```

That is an ordinary straight-line fit of each reading against the next one (`np.polyfit`). A fit is accepted only when `0 < a < 1`, meaning the data really is a decaying curve. Readings come at irregular times (the probe only measures when AGRO stops), so they are interpolated onto a uniform grid first.

A new drying episode starts whenever the soil jumps up by more than 8 points, which means watering or rain happened.

## Temperature and humidity

Evaporation speeds up with heat and slows with humidity, so `tau` is not constant. After several finished episodes, fit:

```
ln(tau) = c0 + c1 * T + c2 * RH
```

with `T` and `RH` averaged over each episode from the DHT22. A new episode then gets an early estimate before it has enough points to fit its own curve. This needs at least 4 finished episodes, so it fills in as the experiment runs.

## How far ahead can it be trusted?

The drying curve is a stable system: errors in `theta` shrink over time, they don't blow up. So a Lyapunov exponent estimated from this series would come out negative, and it can't be used to cap the horizon the way it can for a chaotic system. The real limits are outside changes the model doesn't see: sunlight, rain, wind, and the fit noise itself. Check the horizon empirically instead (below).

## Testing it (for the research)

1. Log the forecast with each reading (`next_water_min` column in `logs/inventory.csv`) and the soil readings.
2. When the soil actually crosses the threshold, record the true time.
3. For each prediction, error = predicted time left minus true time left. Report mean absolute error and how it shrinks as an episode gains readings.
4. Compare against a baseline: a straight-line extrapolation of the last two readings. The exponential model should win when the curve bends.
5. Report separately the early forecast (temperature/humidity model) and the in-episode forecast.

The built-in self-test (`python3 soil_forecast.py`) only checks the code against a synthetic curve. It is not evidence for the research. Only real logged readings count.

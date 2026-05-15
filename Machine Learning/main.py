import os
import numpy as np
import pandas as pd
from flask import Flask, jsonify
from google.cloud import firestore
from datetime import datetime, timedelta, timezone
from sklearn.ensemble import RandomForestRegressor

# ---------------- APP SETUP ----------------
app = Flask(__name__)

# ---------------- CONFIGURATION ----------------
# IMPORTANT: To authenticate with Firestore locally, you must set the 
# GOOGLE_APPLICATION_CREDENTIALS environment variable pointing to your 
# Firebase Service Account JSON key. 
# NEVER commit that JSON file to GitHub!
SENSOR_ID = "esp32"
MIN_REQUIRED_POINTS = 100  # Minimum data points to attempt training
FORECAST_DAYS = 7          # How many days to predict

# 48 steps per day (every 30 mins) instead of 24
FORECAST_STEPS = FORECAST_DAYS * 48  

# Timezone: Sri Lanka (IST)
IST = timezone(timedelta(hours=5, minutes=30))

# Initialize Firestore client (relies on environment credentials)
db = firestore.Client()

# ---------------- TIME HELPERS ----------------
def now_ist() -> datetime:
    return datetime.now(IST)

def minute_doc_id(dt: datetime) -> str:
    return dt.strftime("%Y%m%d_%H%M")

def firestore_ts(dt: datetime):
    return dt

# ---------------- DATABASE CLEANUP ----------------
def cleanup_old_forecasts(now: datetime):
    """Deletes forecast entries that are in the past to save space."""
    ref = db.collection("forecast").document(SENSOR_ID).collection("timeline")
    # Clean up slightly more aggressively since we have double the data now
    expired = ref.where("predicted_time", "<", now).limit(500).stream()
    for doc in expired:
        doc.reference.delete()

# ---------------- DATA FETCHING ----------------
def get_historical_data():
    """
    Fetches the last ~14 days of data (20,000 points) to capture weekly cycles.
    """
    docs = (
        db.collection("sensor_data")
        .document(SENSOR_ID)
        .collection("readings")
        .order_by("timestamp", direction=firestore.Query.DESCENDING)
        .limit(20000) 
        .stream()
    )
    
    records = []
    for doc in docs:
        d = doc.to_dict()
        try:
            records.append({
                "timestamp": d["timestamp"].astimezone(IST),
                "temperature": float(d.get("temperature", 0)),
                "humidity": float(d.get("humidity", 0)),
                "dust": float(d.get("dust", 0)),
                "aqi": int(d.get("aqi", 0)),
                "tvoc": int(d.get("tvoc", 0)),
                "eco2": int(d.get("eco2", 0)),
                "gas_polution_percent": float(d.get("gas_polution_percent", 0)),
            })
        except:
            continue
            
    records.reverse()
    return pd.DataFrame(records)

# ---------------- ML ENGINE ----------------
def train_and_predict(df):
    """
    Trains a Random Forest model using ALL variables as history.
    """
    if df.empty or len(df) < MIN_REQUIRED_POINTS:
        return None, "Not enough data"

    # 1. RESAMPLING (UPDATED to 30min)
    df.set_index('timestamp', inplace=True)
    df = df.resample('30min').mean().interpolate(method='linear')
    df.reset_index(inplace=True)

    # 2. FEATURE ENGINEERING
    df['hour'] = df['timestamp'].dt.hour
    # Add minute feature so model knows if it is :00 or :30
    df['minute'] = df['timestamp'].dt.minute 
    df['day_of_week'] = df['timestamp'].dt.dayofweek
    
    targets = [
        "temperature", "humidity", "dust", "aqi", 
        "tvoc", "eco2", "gas_polution_percent"
    ]
    
    X = []
    y = {k: [] for k in targets}
    
    for i in range(1, len(df)):
        prev_row = df.iloc[i-1]
        curr_row = df.iloc[i]
        
        row_features = [
            prev_row['temperature'], 
            prev_row['humidity'], 
            prev_row['dust'],
            prev_row['aqi'],
            prev_row['tvoc'],
            prev_row['eco2'],
            prev_row['gas_polution_percent'],
            curr_row['hour'], 
            curr_row['minute'], # Added minute
            curr_row['day_of_week']
        ]
        X.append(row_features)
        
        for k in targets:
            y[k].append(curr_row[k])

    # 3. MODEL TRAINING
    models = {}
    for k in targets:
        rf = RandomForestRegressor(n_estimators=50, max_depth=10, random_state=42)
        rf.fit(X, y[k])
        models[k] = rf

    # 4. PREDICTION LOOP
    future_preds = []
    
    last_known = df.iloc[-1]
    
    current_vals = {
        'temperature': last_known['temperature'],
        'humidity': last_known['humidity'],
        'dust': last_known['dust'],
        'aqi': last_known['aqi'],
        'tvoc': last_known['tvoc'],
        'eco2': last_known['eco2'],
        'gas_polution_percent': last_known['gas_polution_percent']
    }
    next_time = last_known['timestamp']
    now = now_ist()

    for _ in range(FORECAST_STEPS):
        # Increment by 30 minutes
        next_time += timedelta(minutes=30)
        
        features = [
            current_vals['temperature'], 
            current_vals['humidity'], 
            current_vals['dust'],
            current_vals['aqi'],
            current_vals['tvoc'],
            current_vals['eco2'],
            current_vals['gas_polution_percent'],
            next_time.hour, 
            next_time.minute, # Added minute
            next_time.weekday()
        ]
        
        step_pred = {}
        for k in targets:
            val = models[k].predict([features])[0]
            step_pred[k] = val
        
        current_vals = step_pred 
        
        if next_time < now:
            continue

        doc_data = {
            "predicted_time": firestore_ts(next_time),
            "predicted_at": firestore_ts(now),
            "temperature": round(step_pred["temperature"], 2),
            "humidity": round(step_pred["humidity"], 2),
            "dust": round(step_pred["dust"], 3),
            "aqi": int(round(step_pred["aqi"])),
            "tvoc": int(round(step_pred["tvoc"])),
            "eco2": int(round(step_pred["eco2"])),
            "gas_polution_percent": round(step_pred["gas_polution_percent"], 2),
        }
        future_preds.append((minute_doc_id(next_time), doc_data))

    return future_preds, "Success"

# ---------------- MAIN FUNCTION ----------------
def generate_forecast_internal():
    now = now_ist()
    cleanup_old_forecasts(now)
    
    # 1. Get Data
    df = get_historical_data()
    
    # 2. Run AI
    preds, status = train_and_predict(df)
    
    if preds is None:
        return {"status": "error", "message": status}
    
    # 3. Write to Firestore
    batch = db.batch()
    ref = db.collection("forecast").document(SENSOR_ID).collection("timeline")
    
    count = 0
    for doc_id, data in preds:
        doc_ref = ref.document(doc_id)
        batch.set(doc_ref, data)
        count += 1
        
        if count >= 450:
            batch.commit()
            batch = db.batch()
            count = 0
            
    if count > 0:
        batch.commit()

    return {"status": "success", "steps_predicted": len(preds)}

# ---------------- FLASK ROUTES ----------------
@app.route("/")
def health():
    return "AI Forecast Engine Online (30-min Intervals)", 200

@app.route("/generate_forecast")
def generate_forecast():
    try:
        result = generate_forecast_internal()
        return jsonify(result), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8080))
    app.run(host="0.0.0.0", port=port)
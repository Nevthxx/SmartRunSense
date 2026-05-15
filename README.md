🏃‍♂️ SmartRunSense
IoT-Based Environmental Monitoring and Prediction System
C++ · Python · scikit-learn · Google Cloud · Firebase · ESP32
📖 Overview
Outdoor running offers major physical and mental health benefits, but environmental conditions such as heat, humidity, particulate matter, and gas pollutants can introduce serious physiological stress.
SmartRunSense is an IoT-based environmental intelligence system designed to help runners make safer and more informed training decisions. It combines real-time sensor data, cloud infrastructure, and machine learning forecasting to provide both live and predictive environmental insights.
The system integrates wearable and stationary IoT nodes, cloud-based data storage, ML-driven forecasting, and a mobile application for visualization and decision support.
🗺️ System Architecture
SmartRunSense is built across four logical layers:
Edge Layer (IoT Devices):
Wearable devices and stationary Smart Poles equipped with environmental sensors.
Cloud Ingestion & Storage:
Google Firestore acts as the central real-time database.
Cloud Processing Layer:
Google Cloud Run microservice running a scikit-learn-based forecasting engine.
Application Layer:
Mobile app for live monitoring, historical analysis, and predictive insights.
✨ Key Features
📡 Real-Time Environmental Monitoring
Continuous data collection from wearable and Smart Pole IoT nodes.
🤖 Machine Learning Forecasting
Predicts environmental conditions up to 7 days ahead using a Random Forest Regressor.
📱 Mobile Application Dashboard
Provides live sensor data, analytics, and personalized run recommendations.
📅 Smart Scheduling System
Suggests optimal time slots for outdoor activity based on predicted environmental risk.
🛠️ Hardware Architecture
⌚ Wearable Device
Portable ESP32-based node (ESP32 DevKitC / ESP32-C3) designed for runners in motion.
🗼 Smart Pole (Stationary Node)
Fixed monitoring stations powered by ESP32-S3 for continuous environmental tracking in public areas.
🌡️ Sensor Suite
Both nodes integrate a shared set of sensors:
🌬️ ENS160 – Air quality (AQI, eCO₂, TVOC)
💧 AHT21 – Temperature & humidity
🏭 MQ135 – Gas pollution estimation (NOx, NH₃, smoke, benzene, etc.)
💨 SHARP GP2Y1010 – Dust/particulate matter detection
🧠 Machine Learning & Cloud Pipeline
⚙️ Execution: Cloud Run microservice triggered every 30 minutes via Cloud Scheduler
📊 Model: Random Forest Regressor (scikit-learn)
📈 Training Data: Last 14 days of sensor data (~20,000 records)
🔮 Forecast Horizon: 7 days ahead in 30-minute intervals (336 steps)
🎯 Outputs: Temperature, humidity, dust density, AQI, TVOC, CO₂, gas pollution %
🗄️ Database
🔥 Google Firestore
Stores real-time sensor readings and ML-generated forecasts with timestamped environmental metrics.
📱 Mobile Application
📊 Live Monitor: Real-time environmental conditions with a “Run Score” risk indicator
📅 Smart Schedule: Highlights safe vs risky time slots for outdoor running
📉 Analytics: Historical trends for air quality and pollutants
🌤️ Forecast: 7-day environmental predictions from the ML engine

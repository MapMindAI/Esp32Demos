FROM python:3.12-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
    libglib2.0-0 \
    libgl1 \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

# ORB-SLAM3 Python bindings are optional but recommended.
RUN pip install --no-cache-dir orbslam3-python || true

COPY app.py /app/app.py
COPY orbslam_engine.py /app/orbslam_engine.py

EXPOSE 8090

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8090"]
